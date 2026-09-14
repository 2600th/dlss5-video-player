#pragma once

#include "NeuralRenderTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Range selection on the decoder's constant-frame-rate timeline. Frame index i
// starts at pts = trunc((i / fps) * 1e7) exactly like VideoDecoder, so ranges
// built here line up with decoded frame timestamps and frame numbers.

struct RangeMarkers {
    std::optional<int64_t> in100ns;
    std::optional<int64_t> out100ns;
};

// Timestamp of frame `index` on the decoder's CFR grid.
int64_t FramePts(uint64_t index, double fps);
// Frame whose timestamp is nearest to pts (the decoder's frameNumber rule).
uint64_t FrameIndexNearest(int64_t pts100ns, double fps);

// Half-open [in, out) snapped to the frame grid: the in marker snaps down to
// the frame containing it, the out marker snaps up so any partially covered
// frame is rendered. The end is clamped to the source's last grid boundary. A
// range covering every frame returns Whole(). nullopt when a marker is missing,
// in >= out, in lies at or beyond the source's last frame, or fps or the
// duration is invalid. A range must name at least one frame that exists: a
// marker inside the sub-frame remainder past the last frame is rejected, not
// rendered as an empty range.
std::optional<NeuralRenderRange> RangeFromMarkers(const RangeMarkers& markers, double fps, int64_t sourceDuration100ns);

// Timestamp of the last frame a source of this duration contains; 0 when fps
// or the duration is invalid. Markers are clamped to it so they always name a
// frame that can be decoded.
int64_t LastFramePts(double fps, int64_t sourceDuration100ns);

// Exactly the frame containing at100ns (clamped to the last source frame).
NeuralRenderRange SingleFrameRange(int64_t at100ns, double fps, int64_t sourceDuration100ns);

// `seconds` of frames starting at the frame containing at100ns, clamped to the
// source end. Returns Whole() when the clip covers the entire source.
NeuralRenderRange ClipPreviewRange(int64_t at100ns, double fps, int64_t sourceDuration100ns, double seconds = 4.0);

// Accepts "h:mm:ss.mmm", "mm:ss.mmm", "ss.mmm" (fraction optional, up to seven
// digits), non-drop "hh:mm:ss:ff" with ff < ceil(fps), and "f<frames>". The
// result is the timestamp of the nearest grid frame. Minute/second fields must
// be below 60 and no field may be negative or empty; anything else is rejected.
std::optional<int64_t> ParseTimecode(std::wstring_view text, double fps);

// withFrames: "hh:mm:ss:ff" (non-drop, ceil(fps) frames per second, exact
// frame round trip). Otherwise "h:mm:ss.mmm" rounded to the millisecond, which
// ParseTimecode maps back to the same frame for any fps below 1000.
std::wstring FormatTimecode(int64_t pts100ns, double fps, bool withFrames);

// min(kDefaultPrerollFrames, frames before range.start100ns); 0 for Whole().
uint32_t PrerollFramesFor(const NeuralRenderRange& range, double fps);
