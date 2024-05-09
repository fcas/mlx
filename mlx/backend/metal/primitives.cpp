// Copyright © 2023-2024 Apple Inc.
#include <algorithm>
#include <cassert>
#include <numeric>
#include <sstream>

#include "mlx/backend/metal/copy.h"
#include "mlx/backend/metal/device.h"
#include "mlx/backend/metal/utils.h"
#include "mlx/primitives.h"
#include "mlx/utils.h"

namespace mlx::core {

template <typename T>
void arange_set_scalars(T start, T next, CommandEncoder& enc) {
  enc->setBytes(&start, sizeof(T), 0);
  T step = next - start;
  enc->setBytes(&step, sizeof(T), 1);
}

void Arange::eval_gpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 0);
  out.set_data(allocator::malloc_or_wait(out.nbytes()));
  if (out.size() == 0) {
    return;
  }
  auto& s = stream();
  auto& d = metal::device(s.device);
  auto kernel = d.get_kernel("arange" + type_to_name(out));
  size_t nthreads = out.size();
  MTL::Size grid_dims = MTL::Size(nthreads, 1, 1);
  MTL::Size group_dims = MTL::Size(
      std::min(nthreads, kernel->maxTotalThreadsPerThreadgroup()), 1, 1);
  auto& compute_encoder = d.get_command_encoder(s.index);
  compute_encoder->setComputePipelineState(kernel);

  switch (out.dtype()) {
    case bool_: // unsupported
      throw std::runtime_error("[Arange::eval_gpu] Does not support bool");
    case uint8:
      arange_set_scalars<uint8_t>(start_, start_ + step_, compute_encoder);
      break;
    case uint16:
      arange_set_scalars<uint16_t>(start_, start_ + step_, compute_encoder);
      break;
    case uint32:
      arange_set_scalars<uint32_t>(start_, start_ + step_, compute_encoder);
      break;
    case uint64:
      arange_set_scalars<uint64_t>(start_, start_ + step_, compute_encoder);
      break;
    case int8:
      arange_set_scalars<int8_t>(start_, start_ + step_, compute_encoder);
      break;
    case int16:
      arange_set_scalars<int16_t>(start_, start_ + step_, compute_encoder);
      break;
    case int32:
      arange_set_scalars<int32_t>(start_, start_ + step_, compute_encoder);
      break;
    case int64:
      arange_set_scalars<int64_t>(start_, start_ + step_, compute_encoder);
      break;
    case float16:
      arange_set_scalars<float16_t>(start_, start_ + step_, compute_encoder);
      break;
    case float32:
      arange_set_scalars<float>(start_, start_ + step_, compute_encoder);
      break;
    case bfloat16:
      arange_set_scalars<bfloat16_t>(start_, start_ + step_, compute_encoder);
      break;
    case complex64:
      throw std::runtime_error("[Arange::eval_gpu] Does not support complex64");
  }

  compute_encoder.set_output_array(out, 2);
  compute_encoder.dispatchThreads(grid_dims, group_dims);
}

void ArgReduce::eval_gpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 1);
  auto& in = inputs[0];
  out.set_data(allocator::malloc_or_wait(out.nbytes()));
  auto& s = stream();
  auto& d = metal::device(s.device);
  std::string op_name;
  switch (reduce_type_) {
    case ArgReduce::ArgMin:
      op_name = "argmin_";
      break;
    case ArgReduce::ArgMax:
      op_name = "argmax_";
      break;
  }

  // Prepare the shapes, strides and axis arguments.
  std::vector<size_t> in_strides = in.strides();
  std::vector<int> shape = in.shape();
  std::vector<size_t> out_strides = out.strides();
  size_t axis_stride = in_strides[axis_];
  size_t axis_size = shape[axis_];
  if (out_strides.size() == in_strides.size()) {
    out_strides.erase(out_strides.begin() + axis_);
  }
  in_strides.erase(in_strides.begin() + axis_);
  shape.erase(shape.begin() + axis_);
  size_t ndim = shape.size();

  // ArgReduce
  int simd_size = 32;
  int n_reads = 4;
  auto& compute_encoder = d.get_command_encoder(s.index);
  {
    auto kernel = d.get_kernel(op_name + type_to_name(in));
    NS::UInteger thread_group_size = std::min(
        (axis_size + n_reads - 1) / n_reads,
        kernel->maxTotalThreadsPerThreadgroup());
    // round up to the closest number divisible by simd_size
    thread_group_size =
        (thread_group_size + simd_size - 1) / simd_size * simd_size;
    assert(thread_group_size <= kernel->maxTotalThreadsPerThreadgroup());

    size_t n_threads = out.size() * thread_group_size;
    MTL::Size grid_dims = MTL::Size(n_threads, 1, 1);
    MTL::Size group_dims = MTL::Size(thread_group_size, 1, 1);
    compute_encoder->setComputePipelineState(kernel);
    compute_encoder.set_input_array(in, 0);
    compute_encoder.set_output_array(out, 1);
    if (ndim == 0) {
      // Pass place holders so metal doesn't complain
      int shape_ = 0;
      size_t stride_ = 0;
      compute_encoder->setBytes(&shape_, sizeof(int), 2);
      compute_encoder->setBytes(&stride_, sizeof(size_t), 3);
      compute_encoder->setBytes(&stride_, sizeof(size_t), 4);
    } else {
      compute_encoder->setBytes(shape.data(), ndim * sizeof(int), 2);
      compute_encoder->setBytes(in_strides.data(), ndim * sizeof(size_t), 3);
      compute_encoder->setBytes(out_strides.data(), ndim * sizeof(size_t), 4);
    }
    compute_encoder->setBytes(&ndim, sizeof(size_t), 5);
    compute_encoder->setBytes(&axis_stride, sizeof(size_t), 6);
    compute_encoder->setBytes(&axis_size, sizeof(size_t), 7);
    compute_encoder.dispatchThreads(grid_dims, group_dims);
  }
}

void AsType::eval_gpu(const std::vector<array>& inputs, array& out) {
  CopyType ctype =
      inputs[0].flags().contiguous ? CopyType::Vector : CopyType::General;
  copy_gpu(inputs[0], out, ctype);
}

void AsStrided::eval_gpu(const std::vector<array>& inputs, array& out) {
  eval(inputs, out);
}

void Broadcast::eval_gpu(const std::vector<array>& inputs, array& out) {
  eval(inputs, out);
}

void Concatenate::eval_gpu(const std::vector<array>& inputs, array& out) {
  std::vector<int> sizes;
  sizes.push_back(0);
  for (auto& p : inputs) {
    sizes.push_back(p.shape(axis_));
  }
  std::partial_sum(sizes.cbegin(), sizes.cend(), sizes.begin());

  out.set_data(allocator::malloc_or_wait(out.nbytes()));

  auto strides = out.strides();
  auto flags = out.flags();
  flags.row_contiguous = false;
  flags.col_contiguous = false;
  flags.contiguous = false;
  auto& d = metal::device(stream().device);
  auto& compute_encoder = d.get_command_encoder(stream().index);
  auto concurrent_ctx = compute_encoder.start_concurrent();
  for (int i = 0; i < inputs.size(); i++) {
    array out_slice(inputs[i].shape(), out.dtype(), nullptr, {});
    size_t data_offset = strides[axis_] * sizes[i];
    out_slice.copy_shared_buffer(
        out, strides, flags, out_slice.size(), data_offset);
    copy_gpu_inplace(inputs[i], out_slice, CopyType::GeneralGeneral, stream());
  }
}

void Conjugate::eval_gpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 1);
  const auto& in = inputs[0];
  if (out.dtype() == complex64) {
    unary_op(inputs, out, "conj");
  } else {
    throw std::invalid_argument(
        "[conjugate] conjugate must be called on complex input.");
  }
}

void Copy::eval_gpu(const std::vector<array>& inputs, array& out) {
  eval(inputs, out);
}

void CustomVJP::eval_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  eval(inputs, outputs);
}

void Depends::eval_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  eval(inputs, outputs);
}

void Full::eval_gpu(const std::vector<array>& inputs, array& out) {
  auto in = inputs[0];
  CopyType ctype;
  if (in.data_size() == 1) {
    ctype = CopyType::Scalar;
  } else if (in.flags().contiguous) {
    ctype = CopyType::Vector;
  } else {
    ctype = CopyType::General;
  }
  copy_gpu(in, out, ctype);
}

void Load::eval_gpu(const std::vector<array>& inputs, array& out) {
  eval(inputs, out);
}

void NumberOfElements::eval_gpu(const std::vector<array>& inputs, array& out) {
  eval(inputs, out);
}

void Pad::eval_gpu(const std::vector<array>& inputs, array& out) {
  // Inputs must be base input array and scalar val array
  assert(inputs.size() == 2);
  auto& in = inputs[0];
  auto& val = inputs[1];

  // Padding value must be a scalar
  assert(val.size() == 1);

  // Padding value, input and output must be of the same type
  assert(val.dtype() == in.dtype() && in.dtype() == out.dtype());

  // Fill output with val
  copy_gpu(val, out, CopyType::Scalar, stream());

  // Find offset for start of input values
  size_t data_offset = 0;
  for (int i = 0; i < axes_.size(); i++) {
    auto ax = axes_[i] < 0 ? out.ndim() + axes_[i] : axes_[i];
    data_offset += out.strides()[ax] * low_pad_size_[i];
  }

  // Extract slice from output where input will be pasted
  array out_slice(in.shape(), out.dtype(), nullptr, {});
  out_slice.copy_shared_buffer(
      out, out.strides(), out.flags(), out_slice.size(), data_offset);

  // Copy input values into the slice
  copy_gpu_inplace(in, out_slice, CopyType::GeneralGeneral, stream());
}

void RandomBits::eval_gpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 1);

  // keys has shape (N1, ..., NK, 2)
  // out has shape (N1, ..., NK, M1, M2, ...)
  auto& keys = inputs[0];
  size_t num_keys = keys.size() / 2;

  size_t elems_per_key = out.size() / num_keys;
  size_t bytes_per_key = out.itemsize() * elems_per_key;
  out.set_data(allocator::malloc_or_wait(out.nbytes()));
  if (out.size() == 0) {
    return;
  }

  size_t out_per_key = (bytes_per_key + 4 - 1) / 4;
  size_t half_size = out_per_key / 2;
  bool odd = out_per_key % 2;

  auto& s = stream();
  auto& d = metal::device(s.device);
  std::string kname = keys.flags().row_contiguous ? "rbitsc" : "rbits";
  auto kernel = d.get_kernel(kname);

  // organize into grid nkeys x elem_per_key
  MTL::Size grid_dims = MTL::Size(num_keys, half_size + odd, 1);
  NS::UInteger thread_group_size = kernel->maxTotalThreadsPerThreadgroup();
  MTL::Size group_dims = MTL::Size(thread_group_size, 1, 1);
  auto& compute_encoder = d.get_command_encoder(s.index);
  compute_encoder->setComputePipelineState(kernel);
  compute_encoder.set_input_array(keys, 0);
  compute_encoder.set_output_array(out, 1);
  compute_encoder->setBytes(&odd, sizeof(bool), 2);
  compute_encoder->setBytes(&bytes_per_key, sizeof(size_t), 3);

  if (!keys.flags().row_contiguous) {
    int ndim = keys.ndim();
    compute_encoder->setBytes(&ndim, sizeof(int), 4);
    compute_encoder->setBytes(
        keys.shape().data(), keys.ndim() * sizeof(int), 5);
    compute_encoder->setBytes(
        keys.strides().data(), keys.ndim() * sizeof(size_t), 6);
  }

  compute_encoder.dispatchThreads(grid_dims, group_dims);
}

void Reshape::eval_gpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 1);
  const auto& in = inputs[0];

  auto [copy_necessary, out_strides] = prepare_reshape(in, out);

  if (copy_necessary) {
    copy_gpu(in, out, CopyType::General);
  } else {
    shared_buffer_reshape(in, out_strides, out);
  }
}

void Split::eval_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  eval(inputs, outputs);
}

void Slice::eval_gpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 1);
  if (out.size() == 0) {
    out.set_data(nullptr);
    return;
  }

  auto& in = inputs[0];

  // Calculate out strides, initial offset and if copy needs to be made
  auto [copy_needed, data_offset, inp_strides] = prepare_slice(in);

  // Do copy if needed
  if (copy_needed) {
    out.set_data(allocator::malloc_or_wait(out.nbytes()));
    std::vector<int64_t> ostrides{out.strides().begin(), out.strides().end()};
    copy_gpu_inplace(
        /* const array& in = */ in,
        /* array& out = */ out,
        /* const std::vector<int>& data_shape = */ out.shape(),
        /* const std::vector<stride_t>& i_strides = */ inp_strides,
        /* const std::vector<stride_t>& o_strides = */ ostrides,
        /* int64_t i_offset = */ data_offset,
        /* int64_t o_offset = */ 0,
        /* CopyType ctype = */ CopyType::General,
        /* const Stream& s = */ stream());
  } else {
    std::vector<size_t> ostrides{inp_strides.begin(), inp_strides.end()};
    shared_buffer_slice(in, ostrides, data_offset, out);
  }
}

void SliceUpdate::eval_gpu(const std::vector<array>& inputs, array& out) {
  assert(inputs.size() == 2);
  if (out.size() == 0) {
    out.set_data(nullptr);
    return;
  }

  auto& in = inputs[0];
  auto& upd = inputs[1];

  if (upd.size() == 0) {
    out.copy_shared_buffer(in);
    return;
  }

  // Check if materialization is needed
  auto ctype = in.flags().contiguous && in.size() == in.data_size()
      ? CopyType::Vector
      : CopyType::General;
  copy_gpu(in, out, in.data_size() == 1 ? CopyType::Scalar : ctype, stream());

  // Calculate out strides, initial offset and if copy needs to be made
  auto [data_offset, out_strides] = prepare_slice(out);

  // Do copy
  std::vector<int64_t> upd_strides{upd.strides().begin(), upd.strides().end()};
  copy_gpu_inplace<int64_t>(
      /* const array& src = */ upd,
      /* array& dst = */ out,
      /* const std::vector<int>& data_shape = */ upd.shape(),
      /* const std::vector<stride_t>& i_strides = */ upd_strides,
      /* const std::vector<stride_t>& o_strides = */ out_strides,
      /* int64_t i_offset = */ 0,
      /* int64_t o_offset = */ data_offset,
      /* CopyType ctype = */ CopyType::GeneralGeneral,
      /* const Stream& s = */ stream());
}

void StopGradient::eval_gpu(const std::vector<array>& inputs, array& out) {
  eval(inputs, out);
}

void Transpose::eval_gpu(const std::vector<array>& inputs, array& out) {
  eval(inputs, out);
}

void QRF::eval_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  throw std::runtime_error("[QRF::eval_gpu] Metal QR factorization NYI.");
}

void SVD::eval_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs) {
  throw std::runtime_error("[SVD::eval_gpu] Metal SVD NYI.");
}

void Inverse::eval_gpu(const std::vector<array>& inputs, array& output) {
  throw std::runtime_error("[Inverse::eval_gpu] Metal inversion NYI.");
}

} // namespace mlx::core
