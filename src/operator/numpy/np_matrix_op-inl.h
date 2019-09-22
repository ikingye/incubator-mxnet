/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 *  Copyright (c) 2019 by Contributors
 * \file np_matrix_op-inl.h
 * \brief Function definition of matrix related operators
 */
#ifndef MXNET_OPERATOR_NUMPY_NP_MATRIX_OP_INL_H_
#define MXNET_OPERATOR_NUMPY_NP_MATRIX_OP_INL_H_

#include <vector>
#include <algorithm>
#include "../tensor/matrix_op-inl.h"
#include "../nn/concat-inl.h"

namespace mxnet {
namespace op {

struct NumpyTransposeParam : public dmlc::Parameter<NumpyTransposeParam> {
  mxnet::TShape axes;
  DMLC_DECLARE_PARAMETER(NumpyTransposeParam) {
    DMLC_DECLARE_FIELD(axes).set_default(mxnet::TShape(-1, 0))
    .describe("By default, reverse the dimensions, otherwise permute "
              "the axes according to the values given.");
  }
};

struct NumpyVstackParam : public dmlc::Parameter<NumpyVstackParam> {
  int num_args;
  DMLC_DECLARE_PARAMETER(NumpyVstackParam) {
    DMLC_DECLARE_FIELD(num_args).set_lower_bound(1)
    .describe("Number of inputs to be vstacked.");
  }
};

template<typename xpu>
void NumpyTranspose(const nnvm::NodeAttrs& attrs,
                    const OpContext& ctx,
                    const std::vector<TBlob>& inputs,
                    const std::vector<OpReqType>& req,
                    const std::vector<TBlob>& outputs) {
  const NumpyTransposeParam& param = nnvm::get<NumpyTransposeParam>(attrs.parsed);
  CHECK_EQ(req[0], kWriteTo) << "Transpose does not support inplace";
  if (ndim_is_known(param.axes)) {
    TransposeImpl<xpu>(ctx.run_ctx, inputs[0], outputs[0], param.axes);
  } else {
    mxnet::TShape axes(inputs[0].ndim(), -1);
    for (int i = 0; i < axes.ndim(); ++i) {
      axes[i] = axes.ndim() - 1 - i;
    }
    TransposeImpl<xpu>(ctx.run_ctx, inputs[0], outputs[0], axes);
  }
}

template<typename xpu>
void NumpyVstackForward(const nnvm::NodeAttrs& attrs,
                        const OpContext& ctx,
                        const std::vector<TBlob>& inputs,
                        const std::vector<OpReqType>& req,
                        const std::vector<TBlob>& outputs) {
  using namespace mshadow;
  using namespace mshadow_op;

  const NumpyVstackParam& param = nnvm::get<NumpyVstackParam>(attrs.parsed);
  CHECK_EQ(inputs.size(), param.num_args);
  CHECK_EQ(outputs.size(), 1);
  CHECK_EQ(req.size(), 1);

  // reshape if necessary
  std::vector<TBlob> data(param.num_args);
  for (int i = 0; i < param.num_args; i++) {
    if (inputs[i].shape_.ndim() == 0 || inputs[i].shape_.ndim() == 1) {
      TShape shape = Shape2(1, inputs[i].shape_.Size());
      data[i] = inputs[i].reshape(shape);
    } else {
      data[i] = inputs[i];
    }
  }

  // initialize ConcatOp
  ConcatParam cparam;
  cparam.num_args = param.num_args;
  cparam.dim = 0;
  MSHADOW_TYPE_SWITCH(inputs[0].type_flag_, DType, {
    ConcatOp<xpu, DType> op;
    op.Init(cparam);
    op.Forward(ctx, data, req, outputs);
  });
}

template<typename xpu>
void NumpyVstackBackward(const nnvm::NodeAttrs& attrs,
                         const OpContext& ctx,
                         const std::vector<TBlob>& inputs,
                         const std::vector<OpReqType>& req,
                         const std::vector<TBlob>& outputs) {
  using namespace mshadow;
  using namespace mshadow_op;

  const NumpyVstackParam& param = nnvm::get<NumpyVstackParam>(attrs.parsed);
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(outputs.size(), param.num_args);
  CHECK_EQ(req.size(), param.num_args);

  // reshape if necessary
  std::vector<TBlob> data(param.num_args);
  for (int i = 0; i < param.num_args; i++) {
    if (outputs[i].shape_.ndim() == 0 || outputs[i].shape_.ndim() == 1) {
      TShape shape = Shape2(1, outputs[i].shape_.Size());
      data[i] = outputs[i].reshape(shape);
    } else {
      data[i] = outputs[i];
    }
  }

  // initialize ConcatOp
  ConcatParam cparam;
  cparam.num_args = param.num_args;
  cparam.dim = 0;
  MSHADOW_TYPE_SWITCH(inputs[0].type_flag_, DType, {
    ConcatOp<xpu, DType> op;
    op.Init(cparam);
    op.Backward(ctx, inputs[0], req, data);
  });
}

struct NumpyRollParam : public dmlc::Parameter<NumpyRollParam> {
  dmlc::optional<mxnet::TShape> shift;
  dmlc::optional<mxnet::TShape> axis;
  DMLC_DECLARE_PARAMETER(NumpyRollParam) {
    DMLC_DECLARE_FIELD(shift)
    .set_default(dmlc::optional<mxnet::TShape>())
    .describe("The number of places by which elements are shifted. If a tuple,"
              "then axis must be a tuple of the same size, and each of the given axes is shifted"
              "by the corresponding number. If an int while axis is a tuple of ints, "
              "then the same value is used for all given axes.");
    DMLC_DECLARE_FIELD(axis)
    .set_default(dmlc::optional<mxnet::TShape>())
    .describe("Axis or axes along which elements are shifted. By default, the array is flattened"
              "before shifting, after which the original shape is restored.");
  }
};

template<int req>
struct RollAxisNone_forward {
  template<typename DType>
  MSHADOW_XINLINE static void Map(int i, DType* out_data, const DType* in_data,
                                  const int size, const int shift) {
    int new_index = i - shift < 0 ? i - shift + size : i - shift;
    KERNEL_ASSIGN(out_data[i], req, in_data[new_index]);
  }
};

template<int req>
struct RollAxis_forward {
  template<typename DType>
  MSHADOW_XINLINE static void Map(int i, DType* out_data, const DType* in_data,
                                  const size_t* new_index) {
    KERNEL_ASSIGN(out_data[i], req, in_data[new_index[i]]);
  }
};

inline void RollDfs(const std::vector<std::vector<size_t>>& new_axes,
                    const std::vector<size_t>& value,
                    std::vector<size_t>* new_index,
                    int index, int ndim, int mid) {
  for (int a : new_axes[index]) {
    if (index == ndim - 1) {
      std::vector<size_t>& out = (*new_index);
      out.push_back(mid + a);
    } else {
      mid += a * value[ndim - 1 - index];
      RollDfs(new_axes, value, new_index, index + 1, ndim, mid);
      mid -= a * value[ndim - 1 - index];
    }
  }
}

template<typename xpu>
void NumpyRollCompute(const nnvm::NodeAttrs& attrs,
                      const OpContext& ctx,
                      const std::vector<TBlob>& inputs,
                      const std::vector<OpReqType>& req,
                      const std::vector<TBlob>& outputs) {
  using namespace mxnet_op;
  CHECK_EQ(inputs.size(), 1U);
  CHECK_EQ(outputs.size(), 1U);
  CHECK_EQ(req.size(), 1U);
  if (inputs[0].Size() == 0U) return;
  const NumpyRollParam& param = nnvm::get<NumpyRollParam>(attrs.parsed);
  const index_t ndim(inputs[0].shape_.ndim());
  Stream<xpu> *s = ctx.get_stream<xpu>();
  std::vector<int> shifts(ndim, 0);
  index_t input_size = inputs[0].Size();
  if (!param.axis.has_value()) {
    int shift = param.shift.value()[0];
    shift = shift % input_size;
    if (shift < 0) {
      shift += inputs[0].shape_.Size();
    }
    MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
      MXNET_ASSIGN_REQ_SWITCH(req[0], req_type, {
        Kernel<RollAxisNone_forward<req_type>, xpu>::Launch(
            s, outputs[0].Size(), outputs[0].dptr<DType>(), inputs[0].dptr<DType>(),
            inputs[0].Size(), shift);
      });
    });
  } else {
    mxnet::TShape axes(param.axis.value());
    for (int i = 0; i < axes.ndim(); ++i) {
      if (axes[i] < 0) {
        axes[i] += ndim;
      }
    }
    for (int i = 0; i < axes.ndim(); ++i) {
      CHECK_LT(axes[i], ndim)
        << "axis " << axes[i]
        << " Exceeds input dimensions " << inputs[0].shape_;
      CHECK_GE(axes[0], 0)
        << "Reduction axis " << param.axis.value()
        << " Exceeds input dimensions " << inputs[0].shape_;
    }
    if (param.shift.value().ndim() == 1) {
      for (int i = 0; i < axes.ndim(); ++i) {
        shifts[axes[i]] = param.shift.value()[0];
      }
    } else {
      if (param.shift.value().ndim() != axes.ndim()) {
        LOG(FATAL) << "shift and `axis` must be a tuple of the same size,";
      }
      for (int i = 0; i < axes.ndim(); ++i) {
        shifts[axes[i]] = param.shift.value()[i];
      }
    }
    // keep shift in a legal range
    for (int i = 0; i < ndim; ++i) {
      int trans_shift = shifts[i] % inputs[0].shape_[i];
      if (trans_shift < 0) {
        trans_shift = shifts[i] + inputs[0].shape_[i];
      }
      shifts[i] = trans_shift;
    }
    // the result of new axis after shift.
    std::vector<std::vector<size_t>> new_axes;
    std::vector<size_t> new_index;
    std::vector<size_t> temp;
    std::vector<size_t> value(ndim, 0);
    int mid_val = 1;
    for (int i = 0; i < ndim; ++i) {
      if (shifts[i] != 0) {
        for (int j = 0; j < inputs[0].shape_[i]; ++j) {
          int new_axis = (j + inputs[0].shape_[i] - shifts[i]) % inputs[0].shape_[i];
          temp.push_back(new_axis);
        }
      } else {
        for (int j = 0; j < inputs[0].shape_[i]; ++j) {
          temp.push_back(j);
        }
      }
      new_axes.push_back(temp);
      temp.clear();
      value[i] = mid_val;
      mid_val *= inputs[0].shape_[ndim - 1 - i];
    }
    RollDfs(new_axes, value, &new_index, 0, ndim, 0);
    size_t workspace_size = new_index.size() * sizeof(size_t);
    Tensor<xpu, 1, char> workspace =
        ctx.requested[0].get_space_typed<xpu, 1, char>(Shape1(workspace_size), s);
    Tensor<cpu, 1, size_t> index_cpu_tensor(new_index.data(), Shape1(new_index.size()));
    Tensor<xpu, 1, size_t> index_xpu_tensor(
        reinterpret_cast<size_t*>(workspace.dptr_), Shape1(new_index.size()));
    mshadow::Copy(index_xpu_tensor, index_cpu_tensor, s);
    MSHADOW_TYPE_SWITCH(outputs[0].type_flag_, DType, {
      MXNET_ASSIGN_REQ_SWITCH(req[0], req_type, {
        Kernel<RollAxis_forward<req_type>, xpu>::Launch(
            s, outputs[0].Size(), outputs[0].dptr<DType>(), inputs[0].dptr<DType>(),
            index_xpu_tensor.dptr_);
      });
    });
  }
}

}  // namespace op
}  // namespace mxnet

#endif  // MXNET_OPERATOR_NUMPY_NP_MATRIX_OP_INL_H_