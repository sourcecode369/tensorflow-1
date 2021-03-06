/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_util.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/kernels/batching_util/periodic_function.h"
#include "tensorflow/core/kernels/batching_util/shared_batch_scheduler.h"
#include "tensorflow/core/kernels/concat_lib.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/kernels/split_lib.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/lib/monitoring/percentile_sampler.h"
#include "tensorflow/core/lib/random/random.h"
#include "tensorflow/core/platform/context.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/util/incremental_barrier.h"
#include "tensorflow/core/util/ptr_util.h"

namespace tensorflow {

namespace {

void RecordPaddingSize(int32 padding_size, const string& model_name,
                       int32 execution_batch_size) {
  static auto* cell = tensorflow::monitoring::PercentileSampler<2>::New(
      {"/tensorflow/serving/batching/padding_size",
       "Tracks the padding size distribution on batches by model_name (if "
       "available).",
       "model_name", "execution_batch_size"},
      /*percentiles=*/{25.0, 50.0, 75.0, 90.0, 95.0, 99.0},
      /*max_samples=*/1024, tensorflow::monitoring::UnitOfMeasure::kNumber);
  cell->GetCell(model_name, absl::StrCat(execution_batch_size))
      ->Add(static_cast<double>(padding_size));
}

void RecordInputBatchSize(int32 batch_size, const string& model_name) {
  static auto* cell = tensorflow::monitoring::PercentileSampler<1>::New(
      {"/tensorflow/serving/batching/input_batch_size",
       "Tracks the batch size distribution on the inputs by model_name (if "
       "available).",
       "model_name"},
      /*percentiles=*/{25.0, 50.0, 75.0, 90.0, 95.0, 99.0},
      /*max_samples=*/1024, tensorflow::monitoring::UnitOfMeasure::kNumber);
  cell->GetCell(model_name)->Add(static_cast<double>(batch_size));
}

void RecordProcessedBatchSize(int32 batch_size, const string& model_name) {
  static auto* cell = tensorflow::monitoring::PercentileSampler<1>::New(
      {"/tensorflow/serving/batching/processed_batch_size",
       "Tracks the batch size distribution on processing by model_name (if "
       "available).",
       "model_name"},
      /*percentiles=*/{25.0, 50.0, 75.0, 90.0, 95.0, 99.0},
      /*max_samples=*/1024, tensorflow::monitoring::UnitOfMeasure::kNumber);
  cell->GetCell(model_name)->Add(static_cast<double>(batch_size));
}

void RecordBatchDelayMs(int64 batch_delay_ms, const string& model_name) {
  static auto* cell = monitoring::PercentileSampler<1>::New(
      {"/tensorflow/serving/batching/batch_delay_ms",
       "Tracks the batching delay for inputs by model_name (if "
       "available).",
       "model_name"},
      /*percentiles=*/{25.0, 50.0, 75.0, 90.0, 95.0, 99.0},
      /*max_samples=*/1024, monitoring::UnitOfMeasure::kTime);
  cell->GetCell(model_name)->Add(static_cast<double>(batch_delay_ms));
}

const string& GetModelName(OpKernelContext* ctx) {
  static string* kModelNameUnset = new string("model_name_unset");
  if (!ctx->session_metadata()) return *kModelNameUnset;
  if (ctx->session_metadata()->name().empty()) return *kModelNameUnset;
  return ctx->session_metadata()->name();
}

}  // namespace

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;
#ifdef TENSORFLOW_USE_SYCL
typedef Eigen::SyclDevice SYCLDevice;
#endif  // TENSORFLOW_USE_SYCL

// Concatenates 'inputs' into a single tensor along the zeroth dimension.
// Requires that all elements of 'inputs' have element type T. Writes to
// 'output' using 'context' for the allocation to ensure proper device
// placement.
template <typename T>
Status Concat(OpKernelContext* context, const gtl::ArraySlice<Tensor> inputs,
              Tensor* output) {
  const int input_dims = inputs[0].dims();
  const TensorShape& input_shape = inputs[0].shape();

  // Note that we reduce the concat of k-dimensional tensors into a two
  // dimensional concat. Assuming the dimensions of any input tensor are
  // {y0, y1,...,ym-1}, we flatten it to {1, y}, where y = Prod_i(yi).
  std::vector<std::unique_ptr<typename TTypes<T, 2>::ConstMatrix>> inputs_flat;
  inputs_flat.reserve(inputs.size());
  int64 output_dim0 = 0;
  for (size_t i = 0; i < inputs.size(); ++i) {
    const Tensor& input = inputs[i];
    if (input.dims() != input_dims) {
      return errors::InvalidArgument(
          "Ranks of all input tensors should match: shape[0] = ",
          input_shape.DebugString(), " vs. shape[", i,
          "] = ", input.shape().DebugString());
    }
    for (int j = 1; j < input_dims; ++j) {
      if (input.dim_size(j) != input_shape.dim_size(j)) {
        return errors::InvalidArgument(
            "Dimensions of inputs should match: shape[0] = ",
            input_shape.DebugString(), " vs. shape[", i,
            "] = ", input.shape().DebugString());
      }
    }
    if (input.NumElements() > 0) {
      inputs_flat.emplace_back(new typename TTypes<T, 2>::ConstMatrix(
          input.shaped<T, 2>({1, input.NumElements()})));
    }
    output_dim0 += input.dim_size(0);
  }

  TensorShape output_shape(input_shape);
  output_shape.set_dim(0, output_dim0);
  TF_RETURN_IF_ERROR(
      context->allocate_temp(DataTypeToEnum<T>::value, output_shape, output));
  if (output->NumElements() > 0) {
    auto output_flat = output->shaped<T, 2>({1, output->NumElements()});
#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA) || \
    (defined(TENSORFLOW_USE_ROCM) && TENSORFLOW_USE_ROCM)
    if (std::is_same<Device, GPUDevice>::value) {
      ConcatGPU<T>(context, inputs_flat, output, &output_flat);
      return Status::OK();
    }
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
    ConcatCPU<T>(context->device(), inputs_flat, &output_flat);
  }

  return Status::OK();
}

// Same as 'Concat' above, but handles Tensor dtype deduction automatically.
Status Concat(OpKernelContext* context, const gtl::ArraySlice<Tensor> inputs,
              Tensor* output) {
  const DataType type = inputs[0].dtype();
  Status concat_status;
  switch (type) {
#define CASE(type)                                         \
  case DataTypeToEnum<type>::value:                        \
    concat_status = Concat<type>(context, inputs, output); \
    break;
    TF_CALL_ALL_TYPES(CASE);
#undef CASE
    default:
      concat_status = errors::InvalidArgument("Unsupported data type: ", type);
      break;
  }
  return concat_status;
}

// The Split*() functions split 'input' with element type T into 'sizes.size()'
// tensors along the zeroth dimension, with the ith split having zeroth-
// dimension size 'sizes[i]'. They allocate the output tensors using 'context',
// for proper device placement.

// Handles special cases that are cheap. Sets 'done==true' iff it found an
// applicable special case and wrote to the outputs. Otherwise acts as a no-op.
template <typename T>
Status SplitEasyCases(OpKernelContext* context, const Tensor& input,
                      const gtl::ArraySlice<int64> sizes,
                      std::vector<Tensor>* outputs, bool* done) {
  *done = false;

  int64 total_size = 0;
  for (const int64 size : sizes) {
    total_size += size;
  }
  if (total_size > input.shape().dim_size(0)) {
    return errors::InvalidArgument(
        "Sum of split sizes must not exceed dim0-size of input tensor");
  }

  // Special case 0: trivial 1-way split.
  if (sizes.size() == 1 && sizes.at(0) == input.shape().dim_size(0)) {
    outputs->push_back(input);
    *done = true;
    return Status::OK();
  }

  // Special case 1: input is aligned.
  if (IsInnerDimsSizeAligned<T>(input.shape())) {
    int64 position = 0;
    for (const int64 size : sizes) {
      outputs->emplace_back(input.Slice(position, position + size));
      position += size;
    }
    *done = true;
    return Status::OK();
  }

  return Status::OK();
}

// Handles the general case, on CPU.
template <typename T>
Status SplitCPU(OpKernelContext* context, const Tensor& input,
                const gtl::ArraySlice<int64> sizes,
                std::vector<Tensor>* outputs) {
  int64 suffix_dim_size = 1;
  for (int i = 1; i < input.shape().dims(); ++i) {
    suffix_dim_size *= input.shape().dim_size(i);
  }
  auto input_reshaped =
      input.shaped<T, 2>({input.shape().dim_size(0), suffix_dim_size});

  int64 position = 0;
  for (const int64 size : sizes) {
    TensorShape output_shape = input.shape();
    output_shape.set_dim(0, size);
    Tensor output;
    TF_RETURN_IF_ERROR(
        context->allocate_temp(input.dtype(), output_shape, &output));
    auto output_shaped = output.shaped<T, 2>({size, suffix_dim_size});

    Eigen::DSizes<Eigen::DenseIndex, 2> slice_indices{position, 0};
    Eigen::DSizes<Eigen::DenseIndex, 2> slice_sizes{size, suffix_dim_size};
    functor::Split<CPUDevice, T, 2>()(context->eigen_device<CPUDevice>(),
                                      output_shaped, input_reshaped,
                                      slice_indices, slice_sizes);

    outputs->emplace_back(output);

    position += size;
  }

  return Status::OK();
}

#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA) || \
    (defined(TENSORFLOW_USE_ROCM) && TENSORFLOW_USE_ROCM)

// Handles the general case, on GPU.
template <typename T>
Status SplitGPU(OpKernelContext* context, const Tensor& input,
                const gtl::ArraySlice<int64>& sizes,
                std::vector<Tensor>* outputs) {
  // TODO(olston, apassos): Implement this.
  LOG(FATAL) << "Not yet implemented";  // Crash ok
}

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

// The outer function that dispatches to the various Split*() functions above.
template <typename T>
Status Split(OpKernelContext* context, const Tensor& input,
             const gtl::ArraySlice<int64> sizes, std::vector<Tensor>* outputs) {
  bool easy_cases_done;
  TF_RETURN_IF_ERROR(
      SplitEasyCases<T>(context, input, sizes, outputs, &easy_cases_done));
  if (easy_cases_done) {
    return Status::OK();
  }

#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA) || \
    (defined(TENSORFLOW_USE_ROCM) && TENSORFLOW_USE_ROCM)
// TODO(olston, apassos): Handle non-CPU cases.
// return SplitGPU<T>(context, input, sizes, outputs);
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
  return SplitCPU<T>(context, input, sizes, outputs);
}

// Same as 'Split' above, but handles Tensor dtype automatically.
Status Split(OpKernelContext* context, const Tensor& input,
             const gtl::ArraySlice<int64> sizes, std::vector<Tensor>* outputs) {
  const DataType type = input.dtype();
  Status split_status;
  switch (type) {
#define CASE(type)                                              \
  case DataTypeToEnum<type>::value:                             \
    split_status = Split<type>(context, input, sizes, outputs); \
    break;
    TF_CALL_ALL_TYPES(CASE);
#undef CASE
    default:
      split_status = errors::InvalidArgument("Unsupported data type: ", type);
      break;
  }
  return split_status;
}

// Wrapper class to allow both lock-free construction and concurrent updates on
// a shared 'status'.
class ThreadSafeStatus {
 public:
  const Status& status() const& TF_LOCKS_EXCLUDED(mutex_) {
    tf_shared_lock lock(mutex_);
    return status_;
  }
  Status status() && TF_LOCKS_EXCLUDED(mutex_) {
    tf_shared_lock lock(mutex_);
    return std::move(status_);
  }

  // Retains the first error status: replaces the current status with
  // `new_status` if `new_status` is not OK and the previous status is OK.
  void Update(const Status& new_status) TF_LOCKS_EXCLUDED(mutex_) {
    if (new_status.ok()) {
      return;
    }

    mutex_lock lock(mutex_);
    status_.Update(new_status);
  }
  void Update(Status&& new_status) TF_LOCKS_EXCLUDED(mutex_) {
    if (new_status.ok()) {
      return;
    }

    mutex_lock lock(mutex_);
    status_.Update(std::forward<Status>(new_status));
  }

 private:
  mutable mutex mutex_;
  Status status_ TF_GUARDED_BY(mutex_);
};

// A class encapsulating the state and logic for batching tensors.
class BatchResource : public ResourceBase {
 public:
  // Given a BatchTask (from one op invocation) with 'num_outputs'== M and
  // splitted into N sub tasks, TensorMatrix is a N X M matrix.
  // Namely, TensorMatrix[i][j] indicates the i-th split tensor of j-th output;
  // concatenating tensors along the 2nd dimension gives a output tensor.
  typedef std::vector<std::vector<Tensor>> TensorMatrix;

  static Status Create(int32 num_batch_threads, int32 max_batch_size,
                       int32 batch_timeout_micros, int32 max_enqueued_batches,
                       const std::vector<int32>& allowed_batch_sizes,
                       FunctionLibraryRuntime::Handle fhandle,
                       bool enable_large_batch_splitting,
                       std::unique_ptr<BatchResource>* resource) {
    std::unique_ptr<BatchResource> new_resource(new BatchResource);

    Batcher::Options batcher_options;
    batcher_options.num_batch_threads = num_batch_threads;
    TF_RETURN_IF_ERROR(
        Batcher::Create(batcher_options, &new_resource->batcher_));

    new_resource->batcher_queue_options_.input_batch_size_limit =
        max_batch_size;
    new_resource->batcher_queue_options_.max_enqueued_batches =
        max_enqueued_batches;
    new_resource->batcher_queue_options_.batch_timeout_micros =
        batch_timeout_micros;
    // Support for splitting large batch is still in progress.
    new_resource->batcher_queue_options_.enable_large_batch_splitting =
        enable_large_batch_splitting;
    new_resource->allowed_batch_sizes_ = allowed_batch_sizes;
    if (enable_large_batch_splitting) {
      new_resource->batcher_queue_options_.split_input_task_func =
          [](std::unique_ptr<BatchTask>* input_task,
             int open_batch_remaining_slot, int max_batch_size,
             std::vector<std::unique_ptr<BatchTask>>* output_tasks) -> Status {
        return SplitInputTask(input_task, open_batch_remaining_slot,
                              max_batch_size, output_tasks);
      };

      if (allowed_batch_sizes.empty()) {
        new_resource->batcher_queue_options_.max_execution_batch_size =
            max_batch_size;
      } else {
        new_resource->batcher_queue_options_.max_execution_batch_size =
            *allowed_batch_sizes.rbegin();
      }
    }

    new_resource->fhandle_ = fhandle;

    *resource = std::move(new_resource);
    return Status::OK();
  }

  string DebugString() const final { return "BatchResource"; }

  // Ingests data from one invocation of the batch op. The data is enqueued to
  // be combined with others into a batch, asynchronously.
  Status RegisterInput(int64 guid, OpKernelContext* context,
                       const string& batcher_queue_name,
                       AsyncOpKernel::DoneCallback done_callback) {
    auto batch_components = MakeUnique<BatchTask>();
    batch_components->start_time = EnvTime::NowNanos();
    batch_components->guid = guid;
    batch_components->propagated_context = Context(ContextKind::kThread);
    OpInputList tensors;
    TF_RETURN_IF_ERROR(context->input_list("in_tensors", &tensors));
    batch_components->inputs.reserve(tensors.size());
    for (const Tensor& tensor : tensors) {
      if (tensor.shape().dims() == 0) {
        return errors::InvalidArgument(
            "Batching input tensors must have at least one dimension");
      }
      if (tensors.size() >= 2 &&
          tensor.shape().dim_size(0) != tensors[0].shape().dim_size(0)) {
        return errors::InvalidArgument(
            "Batching input tensors supplied in a given op invocation must "
            "have equal 0th-dimension size");
      }
      batch_components->inputs.push_back(tensor);
    }
    RecordInputBatchSize(tensors[0].shape().dim_size(0), GetModelName(context));
    OpInputList captured_tensors;
    const auto captured_status =
        context->input_list("captured_tensors", &captured_tensors);
    if (captured_status.ok()) {
      batch_components->captured_inputs.reserve(captured_tensors.size());
      for (const Tensor& captured_tensor : captured_tensors) {
        batch_components->captured_inputs.push_back(captured_tensor);
      }
    }
    batch_components->context = context;
    batch_components->done_callback = std::move(done_callback);
    batch_components->split_index = 0;
    batch_components->output = std::make_shared<TensorMatrix>();
    batch_components->status = std::make_shared<ThreadSafeStatus>();

    BatcherQueue* batcher_queue;
    TF_RETURN_IF_ERROR(
        LookupOrCreateBatcherQueue(batcher_queue_name, &batcher_queue));
    return batcher_queue->Schedule(&batch_components);
  }

 private:
  BatchResource() = default;

  // One task to be batched, corresponds to a `slice` of input from one batch-op
  // invocation.
  //
  // Given input from one batch-op invocation, a `slice` of this input is:
  // 1) Split each Tensor in `BatchTask::inputs` along the 0th dimension.
  // 2) 'split_index' is calculated along the 0-th dimension.
  //
  // Note input from one batch-op invocation is valid and considered a
  // specialized `slice`.
  struct BatchTask : public serving::BatchTask {
    // A unique ID to identify this invocation of Batch.
    int64 guid;

    Context propagated_context;

    std::vector<Tensor> inputs;
    std::vector<Tensor> captured_inputs;
    OpKernelContext* context;
    AsyncOpKernel::DoneCallback done_callback;

    // The index of this split, along the 0-th dimension of input from op
    // invocation.
    int split_index = 0;

    // Two-dimensional tensor matrix, ownership shared by:
    // 1) each split of task (to fill one row in this matrix)
    // and
    // 2) callback that runs to merge output of individual splits for an op
    // invocation, after all splits complete.
    std::shared_ptr<TensorMatrix> output;

    // 'status' records error (could be from any split) if at least one split
    // returns error, OK otherwise.
    // Ownership is shared by individual splits and callback.
    std::shared_ptr<ThreadSafeStatus> status;

    bool is_partial = false;

    size_t size() const override { return inputs[0].shape().dim_size(0); }

    uint64 start_time;
  };

  using Batcher = serving::SharedBatchScheduler<BatchTask>;
  using BatcherQueue = serving::BatchScheduler<BatchTask>;
  using Batch = serving::Batch<BatchTask>;

  // Validates that it's legal to combine the tasks in 'batch' into a batch.
  // Assumes the batch is non-empty.
  static Status ValidateBatch(const Batch& batch) {
    for (int task_idx = 0; task_idx < batch.num_tasks(); ++task_idx) {
      const BatchTask& task = batch.task(task_idx);

      if (task.inputs.size() != batch.task(0).inputs.size()) {
        return errors::InvalidArgument(
            "Batching inputs must have equal number of edges");
      }
    }

    return Status::OK();
  }

  // Returns the smallest entry in 'allowed_batch_sizes_' that is greater than
  // or equal to 'batch_size'. If 'allowed_batch_sizes_' is empty, simply
  // returns 'batch_size'.
  int RoundToLowestAllowedBatchSize(int batch_size) const {
    if (allowed_batch_sizes_.empty()) {
      return batch_size;
    }
    for (int allowed_size : allowed_batch_sizes_) {
      if (allowed_size >= batch_size) {
        return allowed_size;
      }
    }
    LOG(ERROR) << "Maximum batch size greater than largest allowed size; "
                  "ignoring allowed sizes constraint";
    return batch_size;
  }

  Status ConcatInputTensors(const Batch& batch, OpKernelContext* context,
                            std::vector<Tensor>* concatenated_tensors) const {
    if (batch.num_tasks() == 0) {
      return errors::InvalidArgument("Empty batch.");
    }

    const int padded_batch_size = RoundToLowestAllowedBatchSize(batch.size());
    const int padding_amount = padded_batch_size - batch.size();
    RecordPaddingSize(padding_amount, GetModelName(context), padded_batch_size);
    RecordProcessedBatchSize(padded_batch_size, GetModelName(context));

    // All tasks should have the same number of input edges.
    const int num_inputs = batch.task(0).inputs.size();
    concatenated_tensors->reserve(num_inputs);

    // Process each input one at a time (the typical case has just one).
    for (int i = 0; i < num_inputs; ++i) {
      // Concatenate the tasks ith input tensors into a big output tensor.
      std::vector<Tensor> to_concatenate;
      to_concatenate.reserve(batch.num_tasks());
      for (int task_idx = 0; task_idx < batch.num_tasks(); ++task_idx) {
        to_concatenate.push_back(batch.task(task_idx).inputs.at(i));
      }

      // Add padding as needed. Use the first row of the first task's tensor as
      // the data for padding.
      if (padding_amount > 0) {
        const Tensor& padding_source = batch.task(0).inputs.at(i);
        Tensor padding;
        if (padding_source.shape().dim_size(0) == 0) {
          return errors::InvalidArgument(
              "Cannot use an empty tensor with zero rows as padding when "
              "batching. (Input ",
              i, " got shape ", padding_source.shape().DebugString(), ".)");
        }
        if (padding_source.shape().dim_size(0) == 1) {
          padding = padding_source;
        } else {
          padding = padding_source.Slice(0, 1);
        }
        for (int i = 0; i < padding_amount; ++i) {
          to_concatenate.push_back(padding);
        }
      }

      Tensor concatenated_tensor;
      Status concat_status =
          Concat(context, to_concatenate, &concatenated_tensor);
      TF_RETURN_IF_ERROR(concat_status);
      concatenated_tensors->push_back(concatenated_tensor);
    }
    return Status::OK();
  }

  // Split 'input' of 'input_task_ptr' along 0th dimension, into a list of
  // 'output_tasks'.
  // Task sizes are determined by
  // 1) open_batch_remaining_slot
  // 2) max_batch_size
  // 3) size-of-input-task
  // in a way that
  // 1) Task sizes add up to `size-of-input-task`.
  // 2) Task sizes from left to right are like
  //    [open_batch_remaining_slot, max_batch_size, max_batch_size, ...,
  //    `size-of-input-task` - `sum-of-previous-elements`].
  //
  // REQUIRES:
  // Caller should make sure size-of-input-task is greater than
  // open_batch_remaining_slot.
  static Status SplitInputTask(
      std::unique_ptr<BatchTask>* input_task_ptr, int open_batch_remaining_slot,
      int max_batch_size,
      std::vector<std::unique_ptr<BatchTask>>* output_tasks) {
    BatchTask& input_task = *(*input_task_ptr);
    const int64 input_task_size = input_task.size();

    DCHECK_GT(input_task_size, open_batch_remaining_slot);

    std::shared_ptr<ThreadSafeStatus> shared_status = input_task.status;

    // `split_task_done_callback` runs only after all splitted tasks are
    // complete.
    std::function<void()> split_task_done_callback =
        [done_callback = input_task.done_callback, output = input_task.output,
         op_kernel_context = input_task.context, status = shared_status]() {
          const int num_output = op_kernel_context->num_outputs();
          for (int i = 0; i < num_output; ++i) {
            Tensor output_tensor;

            // Concat would memcpy each input tensor to one output tensor.
            // In this context, Concat can be further optimized to get rid of
            // some (probably all) memcpy when input tensors are slices of
            // another copy.
            // TODO(b/154140947):
            // Add a custom implementation of Split and then optimize Concat.
            std::vector<Tensor> to_concatenate;
            to_concatenate.reserve(output->size());
            for (int j = 0; j < output->size(); ++j) {
              to_concatenate.push_back(std::move((*output)[j][i]));
            }
            const auto concat_status =
                Concat(op_kernel_context, to_concatenate, &output_tensor);
            if (!concat_status.ok()) {
              status->Update(concat_status);
            }

            op_kernel_context->set_output(i, std::move(output_tensor));
          }
          op_kernel_context->SetStatus(status->status());
          done_callback();
        };
    IncrementalBarrier barrier(split_task_done_callback);

    std::vector<int64> output_task_sizes;

    if (open_batch_remaining_slot > 0) {
      output_task_sizes.push_back(open_batch_remaining_slot);
    }

    for (int left_task_size = input_task_size - open_batch_remaining_slot;
         left_task_size > 0; left_task_size -= max_batch_size) {
      int next_task_size = std::min(left_task_size, max_batch_size);
      output_task_sizes.push_back(next_task_size);
    }

    const int output_task_num = output_task_sizes.size();
    input_task.output->resize(output_task_num);

    for (int i = 0; i < output_task_num; ++i) {
      (*input_task.output)[i].resize(input_task.context->num_outputs());
    }

    output_tasks->reserve(output_task_num);
    for (int i = 0; i < output_task_num; i++) {
      auto task = absl::make_unique<BatchTask>();
      task->guid = input_task.guid;
      task->propagated_context = Context(ContextKind::kThread);
      task->captured_inputs = input_task.captured_inputs;
      task->context = input_task.context;
      task->done_callback = barrier.Inc();
      task->start_time = input_task.start_time;
      task->split_index = i;
      task->inputs.reserve(input_task.inputs.size());
      task->is_partial = true;
      task->status = input_task.status;

      task->output = input_task.output;
      output_tasks->push_back(std::move(task));
    }

    const int num_input_tensors = input_task.inputs.size();

    // Splits each input tensor according to `output_task_sizes`, and
    // initializes input of `output_tasks` with split results.
    for (int i = 0; i < num_input_tensors; ++i) {
      std::vector<Tensor> split_tensors;
      const Tensor& input_tensor = input_task.inputs[i];
      // TODO(b/154140947):
      // Figure out the optimal implementation of Split, by using
      // 'Tensor::Slice' and eliminating unnecessary memcpy as much as possible.
      const Status split_status = Split(input_task.context, input_tensor,
                                        output_task_sizes, &split_tensors);
      if (!split_status.ok()) {
        return errors::Internal(
            "When splitting input, Tensor split operation failed: ",
            split_status.ToString());
      }
      if (split_tensors.size() != output_task_sizes.size()) {
        return errors::Internal(
            "When splitting input, tensor split operation did not work as "
            "expected; got ",
            split_tensors.size(), " splits; expected ",
            output_task_sizes.size());
      }
      for (int j = 0; j < output_tasks->size(); ++j) {
        BatchTask& output_task = *((*output_tasks)[j]);
        auto moved_tensor_iter = std::next(split_tensors.begin(), j);
        std::move(moved_tensor_iter, moved_tensor_iter + 1,
                  std::back_inserter(output_task.inputs));
      }
    }
    return Status::OK();
  }

  Status SplitOutputTensors(const std::vector<Tensor>& combined_outputs,
                            Batch* batch) const {
    DCHECK_GE(batch->num_tasks(), 1);
    if (batch->num_tasks() < 1) {
      return errors::Internal("Batch size expected to be positive; was ",
                              batch->num_tasks());
    }

    std::vector<int64> task_sizes_plus_optional_padding;
    task_sizes_plus_optional_padding.reserve(batch->num_tasks());
    for (int i = 0; i < batch->num_tasks(); ++i) {
      task_sizes_plus_optional_padding.push_back(batch->task(i).size());
    }
    const int padding_size =
        RoundToLowestAllowedBatchSize(batch->size()) - batch->size();
    if (padding_size > 0) {
      task_sizes_plus_optional_padding.push_back(padding_size);
    }

    // For each output tensor name, a divided-up tensor with one entry per task.
    std::map<string, std::vector<Tensor>> split_tensors;

    DCHECK_EQ(batch->task(0).context->num_outputs(), combined_outputs.size());
    int combined_outputs_size = combined_outputs.size();
    if (combined_outputs_size != batch->task(0).context->num_outputs()) {
      return errors::Internal("Wrong number of batched output tensors");
    }

    // Generate 'split_tensors' and populate the context outputs.
    for (int i = 0, iter_limit = combined_outputs.size(); i < iter_limit; ++i) {
      const Tensor& output_tensor = combined_outputs[i];
      if (output_tensor.shape().dims() == 0) {
        return errors::FailedPrecondition(
            "Batched output tensor has 0 dimensions");
      }
      if (output_tensor.shape().dim_size(0) !=
          static_cast<long long int>(batch->size() + padding_size)) {
        return errors::FailedPrecondition(
            "Batched output tensor's 0th dimension does not equal the sum of "
            "the 0th dimension sizes of the input tensors");
      }

      std::vector<Tensor> split_tensor;
      const Status split_status = tensor::Split(
          output_tensor, task_sizes_plus_optional_padding, &split_tensor);
      DCHECK(split_status.ok()) << split_status.ToString();
      if (!split_status.ok()) {
        return errors::Internal("Tensor split operation failed: ",
                                split_status.ToString());
      }
      DCHECK_EQ(split_tensor.size(), task_sizes_plus_optional_padding.size());
      if (split_tensor.size() != task_sizes_plus_optional_padding.size()) {
        return errors::Internal(
            "Tensor split operation did not work as expected; got ",
            split_tensor.size(), " splits; expected ",
            task_sizes_plus_optional_padding.size());
      }

      // Ignore a possible final split_tensors entry containing the padding.
      for (int j = 0; j < batch->num_tasks(); ++j) {
        BatchTask& task = *(batch->mutable_task(j));
        if (task.is_partial) {
          std::vector<Tensor>& tensor_vector = (*task.output)[task.split_index];
          tensor_vector[i] = std::move(split_tensor[j]);
        } else {
          task.context->set_output(i, split_tensor[j]);
        }
      }
    }

    return Status::OK();
  }

  void ProcessFuncBatch(std::unique_ptr<Batch> batch) const {
    if (batch->empty()) {
      return;
    }

    // We use the 'propagated_context' from one of the threads which setup one
    // of the tasks. This will propagate any common context over all the threads
    // which are running this Session, of which this BatchOp is a part.
    WithContext wc(batch->task(batch->num_tasks() - 1).propagated_context);

    OpKernelContext* last_task_context =
        batch->task(batch->num_tasks() - 1).context;

    // Regardless of the outcome, we need to propagate the status to the
    // individual tasks and signal that they are done. We use MakeCleanup() to
    // ensure that this happens no matter how we exit the method below.
    Status status;
    bool cleanup_done = false;
    auto cleanup_fn = [&cleanup_done, &batch](const Status& status) {
      if (cleanup_done) {
        return;
      }
      for (int i = 0; i < batch->num_tasks(); ++i) {
        if (batch->task(i).is_partial) {
          batch->mutable_task(i)->status->Update(status);
        } else {
          batch->mutable_task(i)->context->SetStatus(status);
        }

        batch->mutable_task(i)->done_callback();
      }
      cleanup_done = true;
    };

    auto finally =
        gtl::MakeCleanup([&cleanup_fn, &status] { cleanup_fn(status); });

    status = ValidateBatch(*batch);
    if (!status.ok()) {
      return;
    }

    std::vector<Tensor> concatenated_tensors;
    status =
        ConcatInputTensors(*batch, last_task_context, &concatenated_tensors);
    if (!status.ok()) {
      return;
    }
    FunctionLibraryRuntime::Options opts;
    opts.step_container = last_task_context->step_container();
    opts.cancellation_manager = last_task_context->cancellation_manager();
    opts.collective_executor = last_task_context->collective_executor();
    opts.stats_collector = last_task_context->stats_collector();
    opts.rendezvous = last_task_context->rendezvous();
    opts.runner = last_task_context->runner();
    opts.run_all_kernels_inline = last_task_context->run_all_kernels_inline();

    auto* flib = last_task_context->function_library();
    std::vector<Tensor> combined_outputs;
    Notification done;
    std::vector<Tensor> args(concatenated_tensors.begin(),
                             concatenated_tensors.end());
    const auto& captured_inputs =
        batch->task(batch->num_tasks() - 1).captured_inputs;
    args.insert(args.end(), captured_inputs.begin(), captured_inputs.end());

    uint64 current_time = EnvTime::NowNanos();
    const string& model_name = GetModelName(last_task_context);
    for (int i = 0; i < batch->num_tasks(); ++i) {
      RecordBatchDelayMs((current_time - batch->task(i).start_time) * 1e-6,
                         model_name);
    }
    // Releases the cleanup method here, because the callback of the function
    // library runtime will handle it now.
    finally.release();
    flib->Run(
        opts, fhandle_, args, &combined_outputs, [&](const Status& run_status) {
          Status final_status;
          auto run_finally = gtl::MakeCleanup([&]() {
            // We do the cleanup here as an optimization, so that it runs in
            // the underlying TF inter-op threadpool. Running it in the
            // threadpool, let's the ensuing ops be scheduled faster,
            // because the executor will add them to the front of the
            // threadpool's task queue rather than the end.
            cleanup_fn(final_status);
            done.Notify();
          });
          final_status = run_status;
          if (!final_status.ok()) {
            return;
          }
          final_status = SplitOutputTensors(combined_outputs, batch.get());
        });
    // By waiting for the notification we are ensuring that this thread isn't
    // used for processing other batches, which gives the batches time to
    // coalesce upstream. So overall the number of batches going through the
    // devices goes down, improving latency and throughput in most cases.
    done.WaitForNotification();
  }

  // Processes a batch of one or more BatchTask entries.
  void ProcessBatch(std::unique_ptr<Batch> batch) const {
    if (batch->empty()) {
      return;
    }

    WithContext wc(batch->task(batch->num_tasks() - 1).propagated_context);

    OpKernelContext* last_task_context =
        batch->task(batch->num_tasks() - 1).context;
    AsyncOpKernel::DoneCallback last_task_callback =
        batch->task(batch->num_tasks() - 1).done_callback;

    OP_REQUIRES_OK_ASYNC(last_task_context, ValidateBatch(*batch),
                         last_task_callback);

    // All tasks should have the same number of input edges.
    const int num_input_edges = batch->task(0).inputs.size();
    std::vector<Tensor> concatenated_tensors;
    const Status concat_status =
        ConcatInputTensors(*batch, last_task_context, &concatenated_tensors);
    OP_REQUIRES_OK_ASYNC(last_task_context, concat_status, last_task_callback);

    // Process each input edge one at a time (the typical case has just one).
    for (int i = 0; i < num_input_edges; ++i) {
      last_task_context->set_output(i, concatenated_tensors[i]);

      // Emit batch->num_tasks() - 1 empty output tensors.
      for (int task_idx = 0; task_idx < batch->num_tasks() - 1; ++task_idx) {
        const BatchTask& task = batch->task(task_idx);
        TensorShape output_shape(task.inputs[i].shape());
        output_shape.set_dim(0, 0);
        Tensor* output = nullptr;
        OP_REQUIRES_OK_ASYNC(
            task.context,
            task.context->allocate_output(i, output_shape, &output),
            task.done_callback);
      }
    }
    // Emit batch->num_tasks() - 1 empty index tensors.
    for (int task_idx = 0; task_idx < batch->num_tasks() - 1; ++task_idx) {
      const BatchTask& task = batch->task(task_idx);
      TensorShape index_shape({0, 3});
      Tensor* output = nullptr;
      OP_REQUIRES_OK_ASYNC(
          task.context,
          task.context->allocate_output(num_input_edges, index_shape, &output),
          task.done_callback);
    }
    // Emit all ID tensors.
    for (int task_idx = 0; task_idx < batch->num_tasks(); ++task_idx) {
      const BatchTask& task = batch->task(task_idx);
      Tensor* id;
      OP_REQUIRES_OK_ASYNC(task.context,
                           task.context->allocate_output(num_input_edges + 1,
                                                         TensorShape({}), &id),
                           task.done_callback);
      id->scalar<int64>()() = task.guid;
    }
    OP_REQUIRES_OK_ASYNC(
        last_task_context,
        EmitIndexTensor(last_task_context, *batch, num_input_edges),
        last_task_callback);

    // Signal done for each element of the batch. (At this point, the contexts
    // are no longer guaranteed to remain live.)
    for (int task_idx = 0; task_idx < batch->num_tasks(); ++task_idx) {
      batch->mutable_task(task_idx)->done_callback();
    }
  }

  // Emits an index tensor, which the Unbatch op will use to un-concatenate
  // the tensor and attribute the pieces to the right batch keys. The index
  // tensor contains, for each input: [batch_key, start_offset, end_offset]
  // where start_offset and end_offset represent the range of entries in the
  // concatenated tensors that belong to that input.
  //
  // Emits the result to the output at 'output_index' using 'context'.
  static Status EmitIndexTensor(OpKernelContext* context, const Batch& batch,
                                int output_index) {
    const TensorShape index_shape({batch.num_tasks(), 3});
    Tensor* index = nullptr;
    TF_RETURN_IF_ERROR(
        context->allocate_output(output_index, index_shape, &index));
    auto index_flat = index->shaped<int64, 2>({batch.num_tasks(), 3});
    size_t offset = 0;
    for (int task_idx = 0; task_idx < batch.num_tasks(); ++task_idx) {
      const BatchTask& task = batch.task(task_idx);
      index_flat(task_idx, 0) = task.guid;
      index_flat(task_idx, 1) = offset;
      index_flat(task_idx, 2) = offset + task.size();
      offset += task.size();
    }
    return Status::OK();
  }

  // Looks up the batcher queue for 'queue_name'. If it did't previously exist,
  // creates it.
  Status LookupOrCreateBatcherQueue(const string& queue_name,
                                    BatcherQueue** queue) {
    mutex_lock l(batcher_queues_mu_);

    auto it = batcher_queues_.find(queue_name);
    if (it != batcher_queues_.end()) {
      *queue = it->second.get();
      return Status::OK();
    }

    std::unique_ptr<BatcherQueue> new_queue;
    auto process_batch_callback = [this](std::unique_ptr<Batch> batch) {
      if (fhandle_ == kInvalidHandle) {
        ProcessBatch(std::move(batch));
      } else {
        ProcessFuncBatch(std::move(batch));
      }
    };
    TF_RETURN_IF_ERROR(batcher_->AddQueue(batcher_queue_options_,
                                          process_batch_callback, &new_queue));
    *queue = new_queue.get();
    batcher_queues_[queue_name] = std::move(new_queue);
    return Status::OK();
  }

  // A batch scheduler, and options for creating queues.
  std::shared_ptr<Batcher> batcher_;
  Batcher::QueueOptions batcher_queue_options_;

  // A collection of batcher queues, keyed on queue name.
  // TODO(olston): Garbage-collect unused queues (perhaps simply remove empty
  // ones (with a time delay?); it's okay if they get recreated later).
  mutable mutex batcher_queues_mu_;
  std::map<string, std::unique_ptr<BatcherQueue>> batcher_queues_
      TF_GUARDED_BY(batcher_queues_mu_);

  std::vector<int32> allowed_batch_sizes_;
  FunctionLibraryRuntime::Handle fhandle_;
};

class BatchFunctionKernel : public AsyncOpKernel {
 public:
  explicit BatchFunctionKernel(OpKernelConstruction* c) : AsyncOpKernel(c) {
    OP_REQUIRES_OK(c, c->GetAttr("container", &container_));
    OP_REQUIRES_OK(c, c->GetAttr("shared_name", &shared_name_));
    // If shared_name is not supplied, use name instead (prevent collisions by
    // default).
    if (shared_name_.empty()) {
      shared_name_ = name();
    }
    OP_REQUIRES_OK(c, c->GetAttr("batching_queue", &batcher_queue_));
    OP_REQUIRES_OK(c, c->GetAttr("num_batch_threads", &num_batch_threads_));
    OP_REQUIRES_OK(c, c->GetAttr("max_batch_size", &max_batch_size_));
    OP_REQUIRES_OK(c,
                   c->GetAttr("batch_timeout_micros", &batch_timeout_micros_));
    OP_REQUIRES_OK(c,
                   c->GetAttr("max_enqueued_batches", &max_enqueued_batches_));
    OP_REQUIRES_OK(c, c->GetAttr("allowed_batch_sizes", &allowed_batch_sizes_));

    auto lib = c->function_library();
    OP_REQUIRES(c, lib != nullptr, errors::Internal("No function library"));
    NameAttrList func;
    OP_REQUIRES_OK(c, c->GetAttr("f", &func));
    OP_REQUIRES_OK(
        c, lib->Instantiate(func.name(), AttrSlice(&func.attr()), &fhandle_));

    if (c->HasAttr("enable_large_batch_splitting")) {
      OP_REQUIRES_OK(c, c->GetAttr("enable_large_batch_splitting",
                                   &enable_large_batch_splitting_));
    } else {
      enable_large_batch_splitting_ = false;
    }

    if (enable_large_batch_splitting_ && (!allowed_batch_sizes_.empty())) {
      max_execution_batch_size_ = *allowed_batch_sizes_.rbegin();
    } else {
      max_execution_batch_size_ = max_batch_size_;
    }
    OP_REQUIRES_OK(c, ValidateAllowedBatchSizes());
  }

  bool IsExpensive() override { return false; }

  void ComputeAsync(OpKernelContext* c, DoneCallback done) final {
    BatchResource* br;
    std::function<Status(BatchResource**)> creator = [this](BatchResource** r) {
      std::unique_ptr<BatchResource> new_resource;
      TF_RETURN_IF_ERROR(BatchResource::Create(
          num_batch_threads_, max_batch_size_, batch_timeout_micros_,
          max_enqueued_batches_, allowed_batch_sizes_, fhandle_,
          enable_large_batch_splitting_, &new_resource));
      *r = new_resource.release();
      return Status::OK();
    };
    OP_REQUIRES_OK_ASYNC(c,
                         c->resource_manager()->LookupOrCreate(
                             container_, shared_name_, &br, creator),
                         done);
    const Status status =
        br->RegisterInput(random::New64(), c, batcher_queue_, done);
    br->Unref();
    OP_REQUIRES_OK_ASYNC(c, status, done);
    // Assume br calls done, so nothing to do here.
  }

  // Validates 'allowed_batch_sizes_'. The entries must increase monotonically,
  // and the last one must equal 'max_batch_size_'.
  Status ValidateAllowedBatchSizes() const {
    if (allowed_batch_sizes_.empty()) {
      return Status::OK();
    }
    int32 last_size = 0;
    for (size_t i = 0; i < allowed_batch_sizes_.size(); ++i) {
      const int32 size = allowed_batch_sizes_.at(i);
      if (i > 0 && size <= last_size) {
        return errors::InvalidArgument(
            "allowed_batch_sizes entries must be monotonically increasing");
      }

      if ((!enable_large_batch_splitting_) &&
          (i == allowed_batch_sizes_.size() - 1) && (size != max_batch_size_)) {
        return errors::InvalidArgument(
            "final entry in allowed_batch_sizes must equal max_batch_size when "
            "enable_large_batch_splitting is False");
      }

      last_size = size;
    }
    return Status::OK();
  }

 private:
  string container_;
  string shared_name_;
  string batcher_queue_;
  int32 num_batch_threads_;
  int32 max_batch_size_;
  int32 max_execution_batch_size_;
  int32 batch_timeout_micros_;
  int32 max_enqueued_batches_;
  std::vector<int32> allowed_batch_sizes_;
  FunctionLibraryRuntime::Handle fhandle_;
  bool enable_large_batch_splitting_;
};

REGISTER_KERNEL_BUILDER(Name("BatchFunction").Device(DEVICE_CPU),
                        BatchFunctionKernel);

class BatchKernel : public AsyncOpKernel {
 public:
  explicit BatchKernel(OpKernelConstruction* c) : AsyncOpKernel(c) {
    OP_REQUIRES_OK(c, c->GetAttr("container", &container_));
    OP_REQUIRES_OK(c, c->GetAttr("shared_name", &shared_name_));
    // If shared_name is not supplied, use name instead (prevent collisions by
    // default).
    if (shared_name_.empty()) {
      shared_name_ = name();
    }
    OP_REQUIRES_OK(c, c->GetAttr("batching_queue", &batcher_queue_));
    OP_REQUIRES_OK(c, c->GetAttr("num_batch_threads", &num_batch_threads_));
    OP_REQUIRES_OK(c, c->GetAttr("max_batch_size", &max_batch_size_));
    OP_REQUIRES_OK(c,
                   c->GetAttr("batch_timeout_micros", &batch_timeout_micros_));
    OP_REQUIRES_OK(c,
                   c->GetAttr("max_enqueued_batches", &max_enqueued_batches_));
    OP_REQUIRES_OK(c, c->GetAttr("allowed_batch_sizes", &allowed_batch_sizes_));
    OP_REQUIRES_OK(c, ValidateAllowedBatchSizes());
  }

  void ComputeAsync(OpKernelContext* c, DoneCallback done) final {
    BatchResource* br;
    std::function<Status(BatchResource**)> creator = [this](BatchResource** r) {
      std::unique_ptr<BatchResource> new_resource;
      TF_RETURN_IF_ERROR(BatchResource::Create(
          num_batch_threads_, max_batch_size_, batch_timeout_micros_,
          max_enqueued_batches_, allowed_batch_sizes_, kInvalidHandle, false,
          &new_resource));
      *r = new_resource.release();
      return Status::OK();
    };
    OP_REQUIRES_OK_ASYNC(c,
                         c->resource_manager()->LookupOrCreate(
                             container_, shared_name_, &br, creator),
                         done);
    const Status status =
        br->RegisterInput(random::New64(), c, batcher_queue_, done);
    br->Unref();
    OP_REQUIRES_OK_ASYNC(c, status, done);
    // Assume br calls done, so nothing to do here.
  }

  // Validates 'allowed_batch_sizes_'. The entries must increase monotonically,
  // and the last one must equal 'max_batch_size_'.
  Status ValidateAllowedBatchSizes() const {
    if (allowed_batch_sizes_.empty()) {
      return Status::OK();
    }
    int32 last_size = 0;
    for (size_t i = 0; i < allowed_batch_sizes_.size(); ++i) {
      const int32 size = allowed_batch_sizes_.at(i);
      if (i > 0 && size <= last_size) {
        return errors::InvalidArgument(
            "allowed_batch_sizes entries must be monotonically increasing");
      }
      if (i == allowed_batch_sizes_.size() - 1 && size != max_batch_size_) {
        return errors::InvalidArgument(
            "final entry in allowed_batch_sizes must equal max_batch_size");
      }
      last_size = size;
    }
    return Status::OK();
  }

 private:
  string container_;
  string shared_name_;
  string batcher_queue_;
  int32 num_batch_threads_;
  int32 max_batch_size_;
  int32 batch_timeout_micros_;
  int32 max_enqueued_batches_;
  std::vector<int32> allowed_batch_sizes_;
};

REGISTER_KERNEL_BUILDER(Name("Batch").Device(DEVICE_CPU), BatchKernel);

// A class encapsulating the state and logic for unbatching tensors.
//
// UnbatchResource keeps two data structures indexed by batch-key: one which has
// the continuations for all concurrent kernels which are waiting for tensors
// and another which has tensors which are waiting for their corresponding
// kernels to run. Whenever a kernel runs, we either grab its tensor if it's
// waiting already, or we insert it in the queue and then look at its tensor to
// see if it can be used to dispatch any stored continuations.
class UnbatchResource : public ResourceBase {
 public:
  explicit UnbatchResource(int32 timeout_micros)
      : timeout_micros_(timeout_micros),
        timeout_enforcer_(new serving::PeriodicFunction(
            [this] { EnforceTimeout(); }, 1000 /* 1 ms */)) {}

  ~UnbatchResource() override {
    // Tear down 'timeout_enforcer_' first, since it accesses other state in
    // this class.
    timeout_enforcer_ = nullptr;
  }

  string DebugString() const final { return "UnbatchResource"; }

  Status Compute(OpKernelContext* context, AsyncOpKernel::DoneCallback done) {
    const Tensor& data_t = context->input(0);
    const Tensor& batch_index_t = context->input(1);

    if (batch_index_t.shape().dim_size(0) > data_t.shape().dim_size(0)) {
      return errors::InvalidArgument(
          "Wrong shape for index tensor. Expected 0th dimension size to be no "
          "greater than ",
          data_t.shape().dim_size(0),
          "; Got: ", batch_index_t.shape().dim_size(0), ".");
    }
    if (batch_index_t.shape().dim_size(1) != 3) {
      return errors::InvalidArgument(
          "Wrong shape for index tensor. Expected 1st dimension size to be 3 ; "
          "Got: ",
          batch_index_t.shape().dim_size(1), ".");
    }

    const int64 batch_key = context->input(2).scalar<int64>()();
    const bool nonempty_input = batch_index_t.dim_size(0) > 0;

    // If we have a non-empty tensor, slice it up.
    // (It is important to do this outside of the critical section below.)
    // The following variables are populated iff 'nonempty_input==true'.
    std::vector<int64> sizes;
    std::vector<int64> batch_keys;
    std::vector<Tensor> split_inputs;
    if (nonempty_input) {
      auto batch_indices =
          batch_index_t.shaped<int64, 2>({batch_index_t.dim_size(0), 3});
      for (int i = 0; i < batch_index_t.dim_size(0); ++i) {
        sizes.push_back(batch_indices(i, 2) - batch_indices(i, 1));
        batch_keys.push_back(batch_indices(i, 0));
      }

      TF_RETURN_IF_ERROR(Split(context, data_t, sizes, &split_inputs));
    }

    // Critical section.
    std::vector<AsyncOpKernel::DoneCallback> done_callbacks_to_call;
    Status status = [&]() -> Status {
      mutex_lock ml(mu_);

      // Check to see whether the tensor we want is already ready.
      auto tensor_it = waiting_tensors_.find(batch_key);
      if (tensor_it != waiting_tensors_.end()) {
        context->set_output(0, tensor_it->second.tensor);
        waiting_tensors_.erase(tensor_it);
        done_callbacks_to_call.push_back(done);
        return Status::OK();
      }

      const uint64 deadline_micros =
          Env::Default()->NowMicros() + timeout_micros_;

      // Add ourselves to the waitlist for tensors.
      if (!waiting_callbacks_
               .emplace(batch_key,
                        WaitingCallback{deadline_micros, context, done})
               .second) {
        return errors::AlreadyExists(
            "Multiple session runs with the same batch key.");
      }

      // If we have a non-empty tensor, finish the waitlisted runs,
      // and store any remaining pieces.
      if (nonempty_input) {
        for (size_t i = 0; i < batch_keys.size(); ++i) {
          auto runs_it = waiting_callbacks_.find(batch_keys[i]);
          if (runs_it != waiting_callbacks_.end()) {
            runs_it->second.context->set_output(0, split_inputs[i]);
            done_callbacks_to_call.push_back(runs_it->second.done);
            waiting_callbacks_.erase(runs_it);
          } else {
            // Note: the deadline here is in case we are arriving late and the
            // kernel that should rendezvous with this tensor has already waited
            // and timed out.
            if (!waiting_tensors_
                     .emplace(batch_keys[i],
                              WaitingTensor{deadline_micros, split_inputs[i]})
                     .second) {
              return errors::AlreadyExists(
                  "Multiple tensors returned for same batch key.");
            }
          }
        }
      }

      return Status::OK();
    }();

    for (const AsyncOpKernel::DoneCallback& done_callback :
         done_callbacks_to_call) {
      done_callback();
    }

    return status;
  }

 private:
  // Evicts waiting tensors and callbacks that have exceeded their deadline.
  void EnforceTimeout() {
    const uint64 now = Env::Default()->NowMicros();
    std::vector<WaitingCallback> evicted_callbacks;

    {
      mutex_lock ml(mu_);

      for (auto it = waiting_tensors_.begin(); it != waiting_tensors_.end();) {
        const WaitingTensor& waiting_tensor = it->second;
        if (waiting_tensor.deadline_micros < now) {
          it = waiting_tensors_.erase(it);
        } else {
          ++it;
        }
      }

      for (auto it = waiting_callbacks_.begin();
           it != waiting_callbacks_.end();) {
        const WaitingCallback& waiting_callback = it->second;
        if (waiting_callback.deadline_micros < now) {
          evicted_callbacks.push_back(waiting_callback);
          it = waiting_callbacks_.erase(it);
        } else {
          ++it;
        }
      }
    }

    for (const WaitingCallback& evicted_callback : evicted_callbacks) {
      evicted_callback.context->CtxFailureWithWarning(errors::DeadlineExceeded(
          "Batched data did not arrive within timeout window."));
      evicted_callback.done();
    }
  }

  struct WaitingTensor {
    uint64 deadline_micros;
    Tensor tensor;
  };

  struct WaitingCallback {
    uint64 deadline_micros;
    OpKernelContext* context;
    AsyncOpKernel::DoneCallback done;
  };

  const int32 timeout_micros_;

  mutex mu_;

  // Maps keyed by BatchKey of tensors waiting for callbacks and callbacks
  // waiting for tensors.
  std::unordered_map<int64, WaitingTensor> waiting_tensors_ TF_GUARDED_BY(mu_);
  std::unordered_map<int64, WaitingCallback> waiting_callbacks_
      TF_GUARDED_BY(mu_);

  // A thread that evicts waiting tensors and callbacks that have exceeded their
  // deadline.
  std::unique_ptr<serving::PeriodicFunction> timeout_enforcer_;
};

class UnbatchKernel : public AsyncOpKernel {
 public:
  explicit UnbatchKernel(OpKernelConstruction* c) : AsyncOpKernel(c) {
    OP_REQUIRES_OK(c, c->GetAttr("container", &container_));
    OP_REQUIRES_OK(c, c->GetAttr("shared_name", &shared_name_));
    // If shared_name is not supplied, use name instead (prevent collisions by
    // default).
    if (shared_name_.empty()) {
      shared_name_ = name();
    }
    OP_REQUIRES_OK(c, c->GetAttr("timeout_micros", &timeout_micros_));
  }

  void ComputeAsync(OpKernelContext* c, DoneCallback done) final {
    UnbatchResource* ubr;
    std::function<Status(UnbatchResource**)> creator =
        [this](UnbatchResource** r) {
          *r = new UnbatchResource(timeout_micros_);
          return Status::OK();
        };
    OP_REQUIRES_OK_ASYNC(c,
                         c->resource_manager()->LookupOrCreate(
                             container_, shared_name_, &ubr, creator),
                         done);
    auto status = ubr->Compute(c, done);
    ubr->Unref();
    OP_REQUIRES_OK_ASYNC(c, status, done);
    // Assume ubr calls done, so nothing to do here.
  }

 private:
  string container_;
  string shared_name_;
  int32 timeout_micros_;
};
REGISTER_KERNEL_BUILDER(Name("Unbatch").Device(DEVICE_CPU), UnbatchKernel);

// A class encapsulating the state and logic for batching tensors
// deterministically for the gradient of unbatch.
class UnbatchGradResource : public ResourceBase {
 public:
  UnbatchGradResource() {}

  string DebugString() const final { return "UnbatchGradResource"; }

  // Flushes the information for one batch, given its context and done
  // callback. Clears all information about it from the available_tensors_.
  Status OutputBatch(OpKernelContext* context,
                     const AsyncOpKernel::DoneCallback& done)
      TF_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    const Tensor& batch_index_t = context->input(1);
    auto batch_index =
        batch_index_t.shaped<int64, 2>({batch_index_t.dim_size(0), 3});
    std::vector<Tensor> tensors;
    for (int i = 0; i < batch_index_t.dim_size(0); ++i) {
      auto available_it = available_tensors_.find(batch_index(i, 0));
      if (available_it == available_tensors_.end()) {
        return errors::Internal("bad bookkeeping of available tensors.");
      }
      tensors.push_back(available_it->second);
      available_tensors_.erase(available_it);
    }

    const DataType type = tensors[0].dtype();
    Tensor concatenated_tensor;
    switch (type) {
#define CASE(type)                                                            \
  case DataTypeToEnum<type>::value:                                           \
    TF_RETURN_IF_ERROR(Concat<type>(context, tensors, &concatenated_tensor)); \
    context->set_output(0, concatenated_tensor);                              \
    break;
      TF_CALL_ALL_TYPES(CASE);
#undef CASE
      default:
        return errors::InvalidArgument("Unsupported data type: ", type);
    }
    done();
    return Status::OK();
  }

  // Ingests data from one invocation of the op.
  Status Compute(OpKernelContext* context,
                 const AsyncOpKernel::DoneCallback& done) {
    const Tensor& data_t = context->input(0);
    const Tensor& batch_index_t = context->input(1);
    const Tensor& grad_t = context->input(2);

    mutex_lock ml(mu_);

    const int64 batch_key = context->input(3).scalar<int64>()();
    // Mark our tensor as available.
    if (!available_tensors_.emplace(batch_key, grad_t).second) {
      return errors::InvalidArgument("Two runs with the same batch key.");
    }

    // Check whether we have a valid input tensor and, if so, create its
    // dispatch logic.
    if (data_t.NumElements() > 0) {
      if (batch_index_t.NumElements() == 0) {
        return errors::InvalidArgument(
            "batch_index is empty while the tensor isn't.");
      }
      std::unordered_set<int64> missing_tensors;
      const auto batch_index =
          batch_index_t.shaped<int64, 2>({batch_index_t.dim_size(0), 3});
      for (int i = 0; i < batch_index_t.dim_size(0); ++i) {
        const int64 batch_key = batch_index(i, 0);
        if (available_tensors_.find(batch_key) == available_tensors_.end()) {
          missing_tensors.emplace(batch_key);
        }
      }
      if (missing_tensors.empty()) {
        return OutputBatch(context, done);
      }
      if (!available_batches_
               .emplace(batch_key, Batch{missing_tensors, context, done})
               .second) {
        return errors::InvalidArgument(
            "Batch key with valid batch used twice.");
      }
      for (const int64 i : missing_tensors) {
        if (!desired_tensor_to_batch_map_.emplace(i, batch_key).second) {
          return errors::InvalidArgument(
              "Missing tensor wanted by more than one batch.");
        }
      }
    } else {
      // If we don't have a valid input tensor we can output an empty tensor and
      // call our done closure.
      TensorShape output_shape(grad_t.shape());
      output_shape.set_dim(0, 0);
      Tensor* output = nullptr;
      TF_RETURN_IF_ERROR(context->allocate_output(0, output_shape, &output));
      done();
    }

    // Search to see whether our tensor is desired by any existing batch.
    auto desire_it = desired_tensor_to_batch_map_.find(batch_key);
    if (desire_it != desired_tensor_to_batch_map_.end()) {
      // Mark our tensor as no longer missing.
      auto batch_it = available_batches_.find(desire_it->second);
      desired_tensor_to_batch_map_.erase(desire_it);
      if (batch_it == available_batches_.end()) {
        return errors::InvalidArgument("Batch no longer exists.");
      }
      batch_it->second.missing_tensors.erase(batch_key);
      // If all tensors are available we should concatenate them and dispatch
      // the batch.
      if (batch_it->second.missing_tensors.empty()) {
        TF_RETURN_IF_ERROR(
            OutputBatch(batch_it->second.context, batch_it->second.done));
        available_batches_.erase(batch_it);
      }
    }
    return Status::OK();
  }

 private:
  mutex mu_;

  // Represents a still-incomplete batch of tensors. When all tensors become
  // available they will be concatenated in the right order and sent through the
  // context.
  struct Batch {
    // Batch keys for tensors which are still missing from this batch. When this
    // is empty the Tensors can be concatenated and forwarded.
    std::unordered_set<int64> missing_tensors;

    // Context and callback for the session responsible for finishing this
    // batch.
    OpKernelContext* context;
    AsyncOpKernel::DoneCallback done;
  };

  // Map from batch key of the session which will output the batched gradients
  // to still-incomplete batches.
  std::unordered_map<int64, Batch> available_batches_;

  // Map from batch key to tensors which are waiting for their batches to be
  // available.
  std::unordered_map<int64, Tensor> available_tensors_;

  // Map from batch key of a tensor which is not yet available to the batch key
  // of the batch to which it belongs.
  std::unordered_map<int64, int64> desired_tensor_to_batch_map_;
};

class UnbatchGradKernel : public AsyncOpKernel {
 public:
  explicit UnbatchGradKernel(OpKernelConstruction* c) : AsyncOpKernel(c) {
    OP_REQUIRES_OK(c, c->GetAttr("container", &container_));
    OP_REQUIRES_OK(c, c->GetAttr("shared_name", &shared_name_));
    // If shared_name is not supplied, use name instead (prevent collisions by
    // default).
    if (shared_name_.empty()) {
      shared_name_ = name();
    }
  }

  void ComputeAsync(OpKernelContext* c, DoneCallback done) final {
    UnbatchGradResource* ubr;
    std::function<Status(UnbatchGradResource**)> creator =
        [](UnbatchGradResource** r) {
          *r = new UnbatchGradResource();
          return Status::OK();
        };
    OP_REQUIRES_OK_ASYNC(c,
                         c->resource_manager()->LookupOrCreate(
                             container_, shared_name_, &ubr, creator),
                         done);
    Status status = ubr->Compute(c, done);
    ubr->Unref();
    OP_REQUIRES_OK_ASYNC(c, status, done);
    // Assume ubr calls done, so nothing to do here.
  }

 private:
  string container_;
  string shared_name_;
};
REGISTER_KERNEL_BUILDER(Name("UnbatchGrad").Device(DEVICE_CPU),
                        UnbatchGradKernel);

}  // namespace tensorflow
