// Copyright 2019 Google LLC
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <xnnpack.h>
#include <xnnpack/allocator.h>
#include <xnnpack/log.h>
#include <xnnpack/operator.h>
#include <xnnpack/params-init.h>
#include <xnnpack/params.h>


enum xnn_status xnn_create_multiply_nd_f32(
    float output_min,
    float output_max,
    uint32_t flags,
    xnn_operator_t* multiply_op_out)
{
  xnn_operator_t multiply_op = NULL;
  enum xnn_status status = xnn_status_uninitialized;

  if (!xnn_params.initialized) {
    xnn_log_error("failed to create Multiply operator: XNNPACK is not initialized");
    goto error;
  }

  status = xnn_status_invalid_parameter;

  if (isnan(output_min)) {
    xnn_log_error(
      "failed to create Multiply operator with NaN output lower bound: lower bound must be non-NaN");
    goto error;
  }

  if (isnan(output_max)) {
    xnn_log_error(
      "failed to create Multiply operator with NaN output upper bound: upper bound must be non-NaN");
    goto error;
  }

  if (output_min >= output_max) {
    xnn_log_error(
      "failed to create Multiply operator with [%.7g, %.7g] output range: lower bound must be below upper bound",
      output_min, output_max);
    goto error;
  }

  status = xnn_status_out_of_memory;

  multiply_op = xnn_allocate_zero_memory(sizeof(struct xnn_operator));
  if (multiply_op == NULL) {
    xnn_log_error("failed to allocate %zu bytes for Multiply operator descriptor", sizeof(struct xnn_operator));
    goto error;
  }

  multiply_op->f32_output_params = xnn_init_f32_output_params(output_min, output_max);

  multiply_op->type = xnn_operator_type_multiply_f32;
  multiply_op->ukernel.type = xnn_ukernel_type_multiply;

  multiply_op->state = xnn_run_state_invalid;

  *multiply_op_out = multiply_op;
  return xnn_status_success;

error:
  xnn_delete_operator(multiply_op);
  return status;
}

enum xnn_status xnn_setup_multiply_nd_f32(
    xnn_operator_t multiply_op,
    size_t num_input1_dims,
    const size_t* input1_shape,
    size_t num_input2_dims,
    const size_t* input2_shape,
    const float* input1,
    const float* input2,
    float* output,
    pthreadpool_t threadpool)
{
  if (multiply_op->type != xnn_operator_type_multiply_f32) {
    xnn_log_error("failed to setup Multiply (F32) operator: operator type mismatch");
    return xnn_status_invalid_parameter;
  }
  multiply_op->state = xnn_run_state_invalid;

  if (!xnn_params.initialized) {
    xnn_log_error("failed to setup Multiply operator: XNNPACK is not initialized");
    return xnn_status_uninitialized;
  }

  if (max(num_input1_dims, num_input2_dims) > 4) {
    xnn_log_error(
      "failed to setup Multiply operator with %zu and %zu dimensions in input shapes: "
      "the number of input dimensions must not exceed 4",
      num_input1_dims, num_input2_dims);
    return xnn_status_unsupported_parameter;
  }

  for (size_t i = 0; i < num_input1_dims; i++) {
    if (input1_shape[i] == 0) {
      xnn_log_error("failed to setup Multiply operator: shape dimension #%zu of input #1 is zero", i);
      return xnn_status_invalid_parameter;
    }
  }

  for (size_t i = 0; i < num_input2_dims; i++) {
    if (input2_shape[i] == 0) {
      xnn_log_error("failed to setup Multiply operator: shape dimension #%zu of input #2 is zero", i);
      return xnn_status_invalid_parameter;
    }
  }

  size_t num_compressed_dims = 0;
  size_t compressed_input1_shape[XNN_MAX_TENSOR_DIMS];
  size_t compressed_input2_shape[XNN_MAX_TENSOR_DIMS];
  size_t compressed_output_shape[XNN_MAX_TENSOR_DIMS];
  for (size_t i = 0; i < XNN_MAX_TENSOR_DIMS; i++) {
    compressed_input1_shape[i] = 1;
    compressed_input2_shape[i] = 1;
    compressed_output_shape[i] = 1;
  }
  bool broadcast_input1 = false;
  bool broadcast_input2 = false;
  bool first_nonunit = true;
  const size_t num_common_dims = min(num_input1_dims, num_input2_dims);
  for (size_t i = 1; i <= num_common_dims; i++) {
    const size_t input1_dim = input1_shape[num_input1_dims - i];
    const size_t input2_dim = input2_shape[num_input2_dims - i];
    if (input1_dim == 1 && input2_dim == 1) {
      continue;
    }
    assert(!broadcast_input1 || !broadcast_input2);

    if (input1_dim == 1) {
      if (!broadcast_input1) {
        broadcast_input1 = true;
        broadcast_input2 = false;
        num_compressed_dims++;
      }
      compressed_input2_shape[num_compressed_dims - 1] *= input2_dim;
      compressed_output_shape[num_compressed_dims - 1] *= input2_dim;
    } else if (input2_dim == 1) {
      if (!broadcast_input2) {
        broadcast_input1 = false;
        broadcast_input2 = true;
        num_compressed_dims++;
      }
      compressed_input1_shape[num_compressed_dims - 1] *= input1_dim;
      compressed_output_shape[num_compressed_dims - 1] *= input1_dim;
    } else if (input1_dim == input2_dim) {
      if (broadcast_input1 || broadcast_input2 || first_nonunit) {
        broadcast_input1 = false;
        broadcast_input2 = false;
        num_compressed_dims++;
      }
      compressed_input1_shape[num_compressed_dims - 1] *= input1_dim;
      compressed_input2_shape[num_compressed_dims - 1] *= input1_dim;
      compressed_output_shape[num_compressed_dims - 1] *= input1_dim;
    } else {
      xnn_log_error("failed to setup Multiply operator: "
        "shape dimension #%zu of input1 (%zu) does not match shape dimension #%zu of input2 (%zu)",
        num_input1_dims - i, input1_dim, num_input2_dims - i, input2_dim);
      return xnn_status_invalid_parameter;
    }
    first_nonunit = false;
  }
  if (num_input1_dims > num_input2_dims) {
    if (!broadcast_input2) {
      num_compressed_dims++;
    }
    for (size_t i = 0; i < num_input1_dims - num_input2_dims; i++) {
      const size_t input1_dim = input1_shape[i];
      compressed_input1_shape[num_compressed_dims - 1] *= input1_dim;
      compressed_output_shape[num_compressed_dims - 1] *= input1_dim;
    }
  } else if (num_input2_dims > num_input1_dims) {
    if (!broadcast_input1) {
      num_compressed_dims++;
    }
    for (size_t i = 0; i < num_input2_dims - num_input1_dims; i++) {
      const size_t input2_dim = input2_shape[i];
      compressed_input2_shape[num_compressed_dims - 1] *= input2_dim;
      compressed_output_shape[num_compressed_dims - 1] *= input2_dim;
    }
  }
  num_compressed_dims = max(num_compressed_dims, 1);

  multiply_op->context.elementwise_binary = (struct elementwise_binary_context) {
    .a = input1,
    .b = input2,
    .y = output,
    .elements = compressed_output_shape[0] * sizeof(float),
    .params.f32 = multiply_op->f32_output_params,
  };
  const size_t* compressed_a_shape = compressed_input1_shape;
  const size_t* compressed_b_shape = compressed_input2_shape;
  if (compressed_input1_shape[0] == 1) {
    multiply_op->context.elementwise_binary.ukernel = xnn_params.f32.vmul.ropc_ukernel;
    multiply_op->context.elementwise_binary.a = input2;
    multiply_op->context.elementwise_binary.b = input1;
    compressed_a_shape = compressed_input2_shape;
    compressed_b_shape = compressed_input1_shape;
  } else if (compressed_input2_shape[0] == 1) {
    multiply_op->context.elementwise_binary.ukernel = xnn_params.f32.vmul.opc_ukernel;
  } else if (compressed_input1_shape[0] == compressed_input2_shape[0]) {
    multiply_op->context.elementwise_binary.ukernel = xnn_params.f32.vmul.op_ukernel;
  }
  size_t a_stride = compressed_a_shape[0], b_stride = compressed_b_shape[0], y_stride = compressed_output_shape[0];
  for (size_t i = 1; i < num_compressed_dims; i++) {
    if (compressed_a_shape[i] != 1) {
      multiply_op->context.elementwise_binary.a_stride[XNN_MAX_TENSOR_DIMS - 1 - i] = a_stride * sizeof(float);
    }
    if (compressed_b_shape[i] != 1) {
      multiply_op->context.elementwise_binary.b_stride[XNN_MAX_TENSOR_DIMS - 1 - i] = b_stride * sizeof(float);
    }
    multiply_op->context.elementwise_binary.y_stride[XNN_MAX_TENSOR_DIMS - 1 - i] = y_stride * sizeof(float);
    a_stride *= compressed_a_shape[i];
    b_stride *= compressed_b_shape[i];
    y_stride *= compressed_output_shape[i];
  }

  multiply_op->compute.type = xnn_parallelization_type_3d_tile_2d;
  multiply_op->compute.task_3d_tile_2d = (pthreadpool_task_3d_tile_2d_t) xnn_compute_elementwise_binary_3d;
  multiply_op->compute.range[0] = compressed_output_shape[3];
  multiply_op->compute.range[1] = compressed_output_shape[2];
  multiply_op->compute.range[2] = compressed_output_shape[1];
  multiply_op->compute.tile[0] = 1;
  multiply_op->compute.tile[1] = 1;
  multiply_op->state = xnn_run_state_ready;

  return xnn_status_success;
}