#include "caffe2/operators/spatial_batch_norm_op.h"

namespace caffe2 {

template <>
bool SpatialBNGradientOp<CPUContext>::RunOnDevice() {
  const auto& X = Input(INPUT);
  const auto& dY = Input(OUTPUT_GRAD);
  const auto& scale = Input(SCALE);

  DCHECK_EQ(X.ndim(), 4);
  const int N = X.dim32(0);
  const int C = (order_ == StorageOrder::NCHW ? X.dim32(1) : X.dim32(3));
  const int H = (order_ == StorageOrder::NCHW ? X.dim32(2) : X.dim32(1));
  const int W = (order_ == StorageOrder::NCHW ? X.dim32(3) : X.dim32(2));
  DCHECK_EQ(scale.ndim(), 1);
  DCHECK_EQ(scale.dim32(0), C);

  ConstEigenVectorArrayMap<float> scale_arr(scale.data<float>(), C);
  ConstEigenVectorArrayMap<float> mean_arr(Input(SAVED_MEAN).data<float>(), C);
  ConstEigenVectorArrayMap<float> inv_var_arr(
      Input(SAVED_INV_VAR).data<float>(), C);

  auto* dX = Output(INPUT_GRAD);
  auto* dScale = Output(SCALE_GRAD);
  auto* dBias = Output(BIAS_GRAD);
  dX->ResizeLike(X);
  dScale->ResizeLike(scale);
  dBias->ResizeLike(scale);

  // dBias = np.sum(dY, axis=0)
  // dScale = np.sum((X - mean) / inv_std * dy, axis=0)
  // dX = (1. / N) * scale * inv_var * (N * dY - np.sum(dY, axis=0) - (X - mean)
  //   * inv_var * inv_var * np.sum(dY * (X - mean), axis=0))

  EigenVectorArrayMap<float> dBias_arr(dBias->mutable_data<float>(), C);
  EigenVectorArrayMap<float> dScale_arr(dScale->mutable_data<float>(), C);

  dBias_arr.setZero();
  dScale_arr.setZero();

  const auto scaleInvVarNHW = scale_arr * inv_var_arr / (N * H * W);

  switch (order_) {
    case StorageOrder::NCHW: {
      ConstEigenArrayMap<float> X_arr(X.data<float>(), H * W, N * C);
      ConstEigenArrayMap<float> dY_arr(dY.data<float>(), H * W, N * C);
      EigenArrayMap<float> dX_arr(dX->mutable_data<float>(), H * W, N * C);
      dX_arr.setZero();

      for (int nc = 0; nc < N * C; ++nc) {
        int c = nc % C;
        dBias_arr(c) += dY_arr.col(nc).sum();
        dScale_arr(c) +=
            ((X_arr.col(nc) - mean_arr(c)) * inv_var_arr(c) * dY_arr.col(nc))
                .sum();
      }
      for (int nc = 0; nc < N * C; ++nc) {
        int c = nc % C;
        dX_arr.col(nc) += scaleInvVarNHW(c) *
            (dY_arr.col(nc) * N * H * W - dBias_arr(c) -
             (X_arr.col(nc) - mean_arr[c]) * dScale_arr(c) * inv_var_arr(c));
      }
      break;
    }
    case StorageOrder::NHWC: {
      ConstEigenArrayMap<float> X_arr(X.data<float>(), C, N * H * W);
      ConstEigenArrayMap<float> dY_arr(dY.data<float>(), C, N * H * W);
      EigenArrayMap<float> dX_arr(dX->mutable_data<float>(), C, N * H * W);
      dX_arr.setZero();

      const auto dYRowSum = dY_arr.rowwise().sum();
      const auto XMinusMean = X_arr.colwise() - mean_arr;
      const auto dYMulXMinusMeanRowSum = (dY_arr * XMinusMean).rowwise().sum();
      const auto invVarSqr = inv_var_arr * inv_var_arr;
      for (int nhw = 0; nhw < N * H * W; ++nhw) {
        dBias_arr += dY_arr.col(nhw);
        dScale_arr +=
            (X_arr.col(nhw) - mean_arr) * inv_var_arr * dY_arr.col(nhw);
        dX_arr.col(nhw) += scaleInvVarNHW *
            (dY_arr.col(nhw) * N * H * W - dYRowSum -
             XMinusMean.col(nhw) * invVarSqr * dYMulXMinusMeanRowSum);
      }
      break;
    }
    default:
      CAFFE_THROW("Unknown storage order: ", order_);
  }
  return true;
}

REGISTER_CPU_OPERATOR(SpatialBNGradient, SpatialBNGradientOp<CPUContext>);

// Input: X, scale, dY, mean, variance
// Output: dX, dscale, dbias
OPERATOR_SCHEMA(SpatialBNGradient).NumInputs(5).NumOutputs(3);

// Spatial batch normalization's gradient, depending on the various input sizes,
// is a bit more complex than usual gradient operators.
class GetSpatialBNGradient : public GradientMakerBase {
  using GradientMakerBase::GradientMakerBase;
  vector<OperatorDef> GetGradientDefs() override {
    // Check if we are in training or testing mode.
    bool is_test = false;
    if (HasArgument(def_, "is_test")) {
      const auto& arg = GetArgument(def_, "is_test");
      CAFFE_ENFORCE(arg.has_i());
      is_test = arg.i();
    }
    vector<string> grad_outputs{GI(0), GI(1), GI(2)};
    vector<string> grad_inputs;
    if (is_test) {
      // This is in testing mode. The operator should have five input:
      //     X, scale, bias, estimated_mean, estimated_variance
      // The gradient inputs are:
      //     X, scale, dY, estimated_mean, estimated_variance
      CAFFE_ENFORCE_EQ(def_.input_size(), 5);
      CAFFE_ENFORCE_EQ(def_.output_size(), 1);
      grad_inputs = vector<string>{I(0), I(1), GO(0), I(3), I(4)};
    } else {
      CAFFE_ENFORCE_EQ(def_.input_size(), 5);
      CAFFE_ENFORCE_EQ(def_.output_size(), 5);
      grad_inputs = vector<string>{I(0), I(1), GO(0), O(3), O(4)};
    }
    return SingleGradientDef(
        "SpatialBNGradient", "", grad_inputs, grad_outputs);
  }
};
REGISTER_GRADIENT(SpatialBN, GetSpatialBNGradient);
}
