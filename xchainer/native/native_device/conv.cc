#include "xchainer/native/native_device.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <vector>

#include <gsl/gsl>
#include <nonstd/optional.hpp>

#include "xchainer/array.h"
#include "xchainer/axes.h"
#include "xchainer/device.h"
#include "xchainer/dtype.h"
#include "xchainer/indexable_array.h"
#include "xchainer/indexer.h"
#include "xchainer/native/col2im.h"
#include "xchainer/native/im2col.h"
#include "xchainer/native/tensor_dot.h"
#include "xchainer/routines/connection.h"
#include "xchainer/routines/creation.h"
#include "xchainer/routines/manipulation.h"
#include "xchainer/shape.h"
#include "xchainer/stack_vector.h"

namespace xchainer {
namespace native {

Array NativeDevice::Conv(
        const Array& x,
        const Array& w,
        const nonstd::optional<Array>& b,
        const StackVector<int64_t, kMaxNdim>& stride,
        const StackVector<int64_t, kMaxNdim>& pad,
        bool cover_all) {
    int8_t ndim = w.ndim() - 2;  // Number of spatial dimensions

    // Compute the kernel size from the weight array.
    StackVector<int64_t, kMaxNdim> kernel_size;
    std::copy_n(w.shape().begin() + 2, ndim, std::back_inserter(kernel_size));

    // Convert to colum representation of shape (batch_size, channel, k_1, k_2, ..., k_n, out_1, out_2, ..., out_n).
    Array col = internal::Im2Col(x, kernel_size, stride, pad, cover_all, 0);

    // Compute the tensor dot product of col and w, reducing (channel, k_1, k_2, ..., k_n).
    Axes axes;
    axes.resize(ndim + 1);
    std::iota(axes.begin(), axes.end(), 1);
    Array y = TensorDot(col, w, axes, axes);  // (batch_size, out_1, out_2, ..., out_n, out_channel)

    // Add bias, if given.
    if (b.has_value()) {
        y += b->AsGradStopped();
    }

    // Move the out channel axis to the second
    Axes roll_axes;
    roll_axes.resize(y.ndim());
    roll_axes[0] = 0;
    roll_axes[1] = ndim + 1;
    std::iota(roll_axes.begin() + 2, roll_axes.end(), 1);
    Array out = y.Transpose(roll_axes);

    return out;
}

Array NativeDevice::ConvGradWeight(
        Dtype w_dtype,
        const Shape& w_shape,
        const Array& x,
        const Array& gy,
        const StackVector<int64_t, kMaxNdim>& stride,
        const StackVector<int64_t, kMaxNdim>& pad,
        bool cover_all) {
    assert(x.ndim() == w_shape.ndim());
    int8_t ndim = x.ndim() - 2;  // Number of spatial dimensions

    // Compute the kernel size
    StackVector<int64_t, kMaxNdim> kernel_size{w_shape.begin() + 2, w_shape.end()};

    // Im2Col
    Array col = internal::Im2Col(x, kernel_size, stride, pad, cover_all, 0);

    // TensorDot
    Axes out_axes{0};
    Axes col_axes{0};
    for (int8_t i = 0; i < ndim; ++i) {
        out_axes.emplace_back(int64_t{2 + i});
        col_axes.emplace_back(int64_t{2 + ndim + i});
    }
    return TensorDot(gy, col, out_axes, col_axes).AsType(w_dtype, false);
}

Array NativeDevice::ConvTranspose(
        const Array& x,
        const Array& w,
        const nonstd::optional<Array>& b,
        const StackVector<int64_t, kMaxNdim>& stride,
        const StackVector<int64_t, kMaxNdim>& pad,
        const StackVector<int64_t, kMaxNdim>& out_size) {
    Array col = TensorDot(w, x, {0}, {1});  // shape: out_channel, k_1, ..., k_n, batch_size, out_1, ..., out_n
    col = RollAxis(col, x.ndim() - 1);  // batch axis is rolled to the top

    Array y = internal::Col2Im(col, stride, pad, out_size);  // shape: batch_size, out_channel, out_size...

    // Add bias, if given.
    if (b.has_value()) {
        std::vector<ArrayIndex> slice{NewAxis{}, Slice{}};
        for (size_t i = 0; i < out_size.size(); ++i) {
            slice.emplace_back(NewAxis{});
        }
        y += b->At(slice);
    }

    return y;
}

}  // namespace native
}  // namespace xchainer
