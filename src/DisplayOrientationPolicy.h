#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// How a video's stored picture is turned upright for display.
//
// A phone films portrait by storing landscape frames and a display matrix
// saying "rotate me a quarter turn": a 1080x1920 portrait clip is coded as
// 1920x1080, tagged rotation=90 or 270. ffprobe's stream width and height are
// the CODED ones, and ffmpeg's autorotation applies the matrix only on the
// software path - with `-hwaccel cuda` or `d3d11va` and a hardware output
// format it leaves the frames as stored. Measured on the bundled 9.0.1 with a
// 320x176 clip tagged rotation=90: the CUDA chain emitted the sideways picture,
// and the software chain emitted upright 176x320 frames that the decoder,
// expecting 320x176, read at the wrong stride as a sheared picture. Both went
// into the cache and the export.
//
// So the decoder turns autorotation off (`-noautorotate`) and applies the
// matrix itself, as one explicit filter on every path, from the one reading
// of the matrix below. The geometry it reports is the upright one. The
// reading is ffmpeg's own (fftools' get_rotation and the filters its
// autorotation inserts), so the software path's pixels are exactly the ones
// ffmpeg would have produced, and the CUDA path's are the same permutation
// done by transpose_cuda on the GPU.
namespace display_orientation {

// The eight ways a picture can be stored, named after the filter that stands
// it up. The quarter turns change the frame's shape; the rest do not.
enum class Orientation : uint8_t {
    Upright,
    Clockwise,             // transpose=clock
    CounterClockwise,      // transpose=cclock
    ClockwiseFlip,         // transpose=clock_flip: a quarter turn of a mirrored picture
    CounterClockwiseFlip,  // transpose=cclock_flip
    HalfTurn,              // hflip,vflip
    MirrorHorizontal,      // hflip
    MirrorVertical,        // vflip
};

using Matrix = std::array<int32_t, 9>;

// The first display matrix in ffprobe's `default` output for
// `-show_entries stream_side_data=displaymatrix`: a `displaymatrix=` line and
// then three rows of the form `00000000:  a  b  u`. Everything else in the
// text is ignored, so it can be read out of the same probe that printed the
// geometry. Nullopt when there is none, or when a row does not hold three
// integers - a matrix read in part would turn the picture the wrong way.
inline std::optional<Matrix> ParseDisplayMatrix(std::string_view text)
{
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line != "displaymatrix=") continue;
        Matrix matrix{};
        for (size_t row = 0; row < 3; ++row) {
            if (at >= text.size()) return std::nullopt;
            end = text.find('\n', at);
            if (end == std::string_view::npos) end = text.size();
            std::string_view values = text.substr(at, end - at);
            at = end + 1;
            const size_t colon = values.find(':');
            if (colon == std::string_view::npos) return std::nullopt;
            values.remove_prefix(colon + 1);
            for (size_t column = 0; column < 3; ++column) {
                while (!values.empty() && (values.front() == ' ' || values.front() == '\t')) values.remove_prefix(1);
                int32_t value = 0;
                const auto parsed = std::from_chars(values.data(), values.data() + values.size(), value);
                if (parsed.ec != std::errc{}) return std::nullopt;
                values.remove_prefix(static_cast<size_t>(parsed.ptr - values.data()));
                matrix[row * 3 + column] = value;
            }
        }
        return matrix;
    }
    return std::nullopt;
}

// What ffmpeg's autorotation would do with `matrix`, in the same order of
// tests: the angle is av_display_rotation_get's, negated, rounded and folded
// into [0, 360) as get_rotation folds it, and the sign of one element settles
// whether a quarter turn carries a mirror. Nullopt for an angle that is not a
// multiple of 90 degrees (ffmpeg would `rotate` it into a same-sized frame
// with black corners, which is not a picture worth rendering) and for a
// degenerate matrix; the decoder then decodes the frames as stored.
inline std::optional<Orientation> FromDisplayMatrix(const Matrix& matrix)
{
    const auto fixed = [&](size_t index) { return double(matrix[index]) / 65536.0; };
    const double scaleX = std::hypot(fixed(0), fixed(3));
    const double scaleY = std::hypot(fixed(1), fixed(4));
    if (scaleX == 0.0 || scaleY == 0.0) return std::nullopt;
    const double pi = 3.14159265358979323846;
    const double rotation = -std::atan2(fixed(1) / scaleY, fixed(0) / scaleX) * 180.0 / pi;
    double theta = -std::round(rotation);
    theta -= 360.0 * std::floor(theta / 360.0 + 0.9 / 360.0);
    if (std::abs(theta - 90.0) < 1.0)
        return matrix[3] > 0 ? Orientation::CounterClockwiseFlip : Orientation::Clockwise;
    if (std::abs(theta - 180.0) < 1.0) {
        const bool horizontal = matrix[0] < 0, vertical = matrix[4] < 0;
        return horizontal && vertical ? Orientation::HalfTurn :
               horizontal ? Orientation::MirrorHorizontal :
               vertical ? Orientation::MirrorVertical : Orientation::Upright;
    }
    if (std::abs(theta - 270.0) < 1.0)
        return matrix[3] < 0 ? Orientation::ClockwiseFlip : Orientation::CounterClockwise;
    if (std::abs(theta) > 1.0) return std::nullopt;
    return matrix[4] < 0 ? Orientation::MirrorVertical : Orientation::Upright;
}

// Whether standing the picture up exchanges its width and height.
constexpr bool SwapsAxes(Orientation orientation)
{
    return orientation == Orientation::Clockwise || orientation == Orientation::CounterClockwise ||
           orientation == Orientation::ClockwiseFlip || orientation == Orientation::CounterClockwiseFlip;
}

// The name the log and the cache-key term use; ffmpeg's own where it has one.
constexpr const char* Name(Orientation orientation)
{
    switch (orientation) {
    case Orientation::Clockwise: return "clock";
    case Orientation::CounterClockwise: return "cclock";
    case Orientation::ClockwiseFlip: return "clock_flip";
    case Orientation::CounterClockwiseFlip: return "cclock_flip";
    case Orientation::HalfTurn: return "reversal";
    case Orientation::MirrorHorizontal: return "hflip";
    case Orientation::MirrorVertical: return "vflip";
    case Orientation::Upright: break;
    }
    return "upright";
}

// The CPU filter that stands a decoded picture up, for the software path and
// for D3D11VA after hwdownload; empty for an upright one. These are the
// filters ffmpeg's autorotation inserts, so the pixels are its pixels.
inline std::wstring SoftwareFilter(Orientation orientation)
{
    switch (orientation) {
    case Orientation::Clockwise: return L"transpose=clock";
    case Orientation::CounterClockwise: return L"transpose=cclock";
    case Orientation::ClockwiseFlip: return L"transpose=clock_flip";
    case Orientation::CounterClockwiseFlip: return L"transpose=cclock_flip";
    case Orientation::HalfTurn: return L"hflip,vflip";
    case Orientation::MirrorHorizontal: return L"hflip";
    case Orientation::MirrorVertical: return L"vflip";
    case Orientation::Upright: break;
    }
    return {};
}

// The same, on CUDA frames before they leave the GPU. transpose_cuda takes all
// seven directions; measured on the bundled 9.0.1, its output is byte for byte
// hwdownload followed by the CPU filter above.
inline std::wstring CudaFilter(Orientation orientation)
{
    if (orientation == Orientation::Upright) return {};
    const char* name = Name(orientation);
    return L"transpose_cuda=dir=" + std::wstring(name, name + std::char_traits<char>::length(name));
}

// The cache-key term a render of such a source carries. A quarter turn already
// moves the key through its width and height, but a half turn or a mirror
// keeps both, and the renders made before this fix saw those frames as stored
// on the CUDA path - upside down or mirrored - under the same key. Empty for
// an upright source, whose key does not move.
inline std::string IdentityTerm(Orientation orientation)
{
    if (orientation == Orientation::Upright) return {};
    return std::string("|display-") + Name(orientation) + "-v1";
}

} // namespace display_orientation
