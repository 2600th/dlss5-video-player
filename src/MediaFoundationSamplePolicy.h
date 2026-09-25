#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

// How many bytes of a Media Foundation sample buffer the BGRA copy may read,
// decided before it reads any of them.
//
// The Media Foundation reader is the decoder behind every file FFmpeg cannot
// open, and the only one in the core package. Its frames arrive as one locked
// buffer holding `height` rows `stride` bytes apart - bottom-up when the stride
// is negative - and the copy used to trust that. It fell back to packed rows
// when the declared stride did not fit, but never asked whether the packed rows
// fit either: a stream that dropped resolution mid-file handed over a buffer
// sized for the smaller picture, and the copy walked the opening geometry's
// rows straight past its end, into whatever the heap held next.
namespace mf_sample {

// How to walk the buffer, or that it cannot be walked. `stride` is signed as
// the rows are laid out; `firstRow` is the byte offset of the top row, the
// last one in the buffer for a bottom-up frame; `rowBytes` is what is copied
// from each row, at most one BGRA row of the frame.
struct CopyPlan {
    bool copy{};
    int32_t stride{};
    size_t firstRow{};
    size_t rowBytes{};
};

// The furthest byte a walk of `height` rows `absStride` apart reads, whichever
// way it walks: the top row of a bottom-up frame is the last in the buffer.
constexpr uint64_t BytesRead(uint64_t absStride, uint64_t rowBytes, uint32_t height)
{
    return absStride * (height - 1u) + rowBytes;
}

// `width` x `height` is the geometry the session was opened at, `declaredStride`
// the stride the current media type states (MF_MT_DEFAULT_STRIDE, or packed
// when it states none), and `bufferBytes` what the locked buffer says it holds.
// The declared stride is used when every row it implies fits; otherwise the
// buffer is read as packed rows, as it always was; and when neither fits, the
// frame is refused - a short buffer is an error, never a copy.
constexpr CopyPlan PlanCopy(uint32_t width, uint32_t height, int32_t declaredStride, size_t bufferBytes)
{
    if (width == 0 || height == 0) return {};
    const uint64_t packed = uint64_t(width) * 4u;
    // A stride this large describes no buffer the reader hands out, and its
    // negation would not fit the signed field either.
    if (packed > 0x7fffffffu) return {};
    const auto fits = [&](uint64_t absStride) {
        const uint64_t rowBytes = absStride < packed ? absStride : packed;
        return rowBytes != 0 && BytesRead(absStride, rowBytes, height) <= bufferBytes;
    };
    // INT32_MIN has no positive twin; widening first keeps its magnitude.
    const uint64_t declared = declaredStride < 0 ? uint64_t(-int64_t(declaredStride)) : uint64_t(declaredStride);
    int32_t stride = declaredStride;
    uint64_t absStride = declared;
    if (!fits(declared)) {
        stride = static_cast<int32_t>(packed);
        absStride = packed;
        if (!fits(packed)) return {};
    }
    CopyPlan plan{};
    plan.copy = true;
    plan.stride = stride;
    plan.rowBytes = static_cast<size_t>(absStride < packed ? absStride : packed);
    plan.firstRow = stride < 0 ? static_cast<size_t>(absStride * (height - 1u)) : 0u;
    return plan;
}

// Whether the session can follow a media type change. The player's geometry is
// fixed per open - the pipe, the textures, the render and its cache key are all
// sized from it - so a change of frame size ends the decode; anything else (a
// new stride, a new frame rate) is followed.
constexpr bool FollowsTypeChange(uint32_t openWidth, uint32_t openHeight, uint32_t width, uint32_t height)
{
    return width == openWidth && height == openHeight;
}

} // namespace mf_sample
