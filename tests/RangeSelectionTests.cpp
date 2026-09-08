#include "RangeSelection.h"
#include "TestSupport.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

namespace {

constexpr double k23976 = 24000.0 / 1001.0;
constexpr double k2997 = 30000.0 / 1001.0;
constexpr double kRates[]{k23976, k2997, 60.0};
constexpr int64_t kTenSeconds = 100000000;

std::string Narrow(std::wstring_view text)
{
    std::string narrow;
    for (const wchar_t character : text) narrow.push_back(character < 128 ? static_cast<char>(character) : '?');
    return narrow;
}

void CheckParse(std::wstring_view text, double fps, std::optional<int64_t> expected)
{
    const auto parsed = ParseTimecode(text, fps);
    if (parsed != expected) {
        ++test_support::failure_count;
        std::cerr << "ParseTimecode(\"" << Narrow(text) << "\", " << fps << ") -> "
                  << (parsed ? std::to_string(*parsed) : "nullopt") << ", expected "
                  << (expected ? std::to_string(*expected) : "nullopt") << '\n';
    }
}

void TimecodeRoundTripTests()
{
    for (const double fps : kRates) {
        // Every frame of the first five seconds plus a far index with a
        // non-trivial hour/minute decomposition and a non-integral pts.
        const uint64_t indices[]{0, 1, 2, 23, 24, 25, 29, 30, 59, 60, 61, 119, 120,
            static_cast<uint64_t>(std::llround(fps * 3661.0)) + 5, 1234567};
        for (uint64_t index = 0; index < static_cast<uint64_t>(fps * 5.0); ++index) {
            const int64_t pts = FramePts(index, fps);
            CHECK_EQ(index, FrameIndexNearest(pts, fps));
            CheckParse(FormatTimecode(pts, fps, true), fps, pts);
            CheckParse(FormatTimecode(pts, fps, false), fps, pts);
        }
        for (const uint64_t index : indices) {
            const int64_t pts = FramePts(index, fps);
            CheckParse(FormatTimecode(pts, fps, true), fps, pts);
            CheckParse(FormatTimecode(pts, fps, false), fps, pts);
            CheckParse(L"f" + std::to_wstring(index), fps, pts);
        }
    }
    // Non-drop frames: 24 labelled frames per second at 23.976.
    CHECK_EQ(std::wstring(L"01:01:01:05"), FormatTimecode(FramePts(24 * 3661 + 5, k23976), k23976, true));
    CHECK_EQ(std::wstring(L"00:00:01:00"), FormatTimecode(FramePts(24, k23976), k23976, true));
    CHECK_EQ(std::wstring(L"00:00:01:29"), FormatTimecode(FramePts(59, k2997), k2997, true));
    CHECK_EQ(std::wstring(L"00:00:00:59"), FormatTimecode(FramePts(59, 60.0), 60.0, true));
    // Milliseconds round to nearest and carry into the higher fields.
    CHECK_EQ(std::wstring(L"0:00:01.001"), FormatTimecode(FramePts(24, k23976), k23976, false));
    CHECK_EQ(std::wstring(L"0:01:00.000"), FormatTimecode(599996000, 60.0, false));
    CHECK_EQ(std::wstring(L"1:01:01.500"), FormatTimecode(36615000000, 60.0, false));
    CHECK_EQ(std::wstring(L"0:00:00.000"), FormatTimecode(-5, 60.0, false));
}

void TimecodeParseTests()
{
    // Seconds-based forms snap to the nearest grid frame.
    CheckParse(L"1:02:03.5", 60.0, FramePts(223410, 60.0));
    CheckParse(L"02:03", 60.0, FramePts(7380, 60.0));
    CheckParse(L"90:00", 60.0, FramePts(324000, 60.0));
    CheckParse(L"12.25", 60.0, FramePts(735, 60.0));
    CheckParse(L"125.5", 60.0, FramePts(7530, 60.0));
    CheckParse(L"0.0417083", k23976, FramePts(1, k23976));
    CheckParse(L" 00:00:01:12 ", k23976, FramePts(36, k23976));
    CheckParse(L"00:00:01:23", k23976, FramePts(47, k23976));
    CheckParse(L"00:00:01:29", k2997, FramePts(59, k2997));
    CheckParse(L"F10", 60.0, FramePts(10, 60.0));
    CheckParse(L"0", 60.0, 0);
    CheckParse(L"0:00:00.0", 60.0, 0);
    // Frames beyond the nominal rate, out-of-range or malformed fields, and
    // negatives are rejected rather than wrapped.
    for (const wchar_t* rejected : {L"", L"   ", L"abc", L"-1", L"-0:01", L"1:-2", L"+1", L"1:60", L"1:2:60",
             L"1:60:00", L"1::2", L"1:2:3:4:5", L"1.", L".5", L"1..5", L"1:2:3.12345678", L"f", L"f12a", L"f-1",
             L"1 2", L"1,5", L"00:00:01:24.5", L"1e3", L"1:123:00", L"::"})
        CheckParse(rejected, 60.0, std::nullopt);
    CheckParse(L"00:00:01:24", k23976, std::nullopt);
    CheckParse(L"00:00:01:30", k2997, std::nullopt);
    CheckParse(L"00:00:01:60", 60.0, std::nullopt);
    CheckParse(L"1", 0.0, std::nullopt);
    CheckParse(L"1", -24.0, std::nullopt);
}

void MarkerTests()
{
    const auto range = [](std::optional<int64_t> in, std::optional<int64_t> out, int64_t duration = kTenSeconds) {
        return RangeFromMarkers({in, out}, duration);
    };
    const auto inside = range(20000000, 50000000);
    CHECK(inside.has_value());
    if (inside) { CHECK_EQ(int64_t{20000000}, inside->start100ns); CHECK_EQ(int64_t{50000000}, inside->end100ns); }
    CHECK(!range(std::nullopt, 50000000).has_value());
    CHECK(!range(20000000, std::nullopt).has_value());
    CHECK(!range(std::nullopt, std::nullopt).has_value());
    CHECK(!range(50000000, 50000000).has_value());
    CHECK(!range(60000000, 50000000).has_value());
    CHECK(!range(-1, 50000000).has_value());
    CHECK(!range(kTenSeconds, 2 * kTenSeconds).has_value());
    CHECK(!range(20000000, 50000000, 0).has_value());
    // Out beyond the source clamps; a range covering the whole source is Whole().
    const auto clamped = range(50000000, 2 * kTenSeconds);
    CHECK(clamped.has_value());
    if (clamped) { CHECK_EQ(int64_t{50000000}, clamped->start100ns); CHECK_EQ(kTenSeconds, clamped->end100ns); }
    const auto whole = range(0, kTenSeconds);
    CHECK(whole.has_value() && whole->Whole());
    const auto overshoot = range(0, 2 * kTenSeconds);
    CHECK(overshoot.has_value() && overshoot->Whole());
    // Markers produced by ParseTimecode stay on the grid.
    const auto typed = range(ParseTimecode(L"00:00:01:00", k23976), ParseTimecode(L"0:00:02.002", k23976));
    CHECK(typed.has_value());
    if (typed) { CHECK_EQ(FramePts(24, k23976), typed->start100ns); CHECK_EQ(FramePts(48, k23976), typed->end100ns); }
}

void SingleFrameTests()
{
    for (const double fps : kRates) {
        const int64_t frame = static_cast<int64_t>(10000000.0 / fps);
        for (const uint64_t index : {uint64_t{0}, uint64_t{1}, uint64_t{100}}) {
            // Anywhere inside frame N selects exactly [N, N+1).
            for (const int64_t offset : {int64_t{0}, int64_t{1000}, frame - 2}) {
                const auto range = SingleFrameRange(FramePts(index, fps) + offset, fps, kTenSeconds);
                CHECK_EQ(FramePts(index, fps), range.start100ns);
                CHECK_EQ(FramePts(index + 1, fps), range.end100ns);
                CHECK_EQ(index + 1, FrameIndexNearest(range.end100ns, fps));
            }
        }
        // At or beyond the end the last frame that starts before the duration is used.
        const uint64_t lastIndex = static_cast<uint64_t>(std::ceil(10.0 * fps)) - 1;
        CHECK(FramePts(lastIndex, fps) < kTenSeconds);
        CHECK(FramePts(lastIndex + 1, fps) >= kTenSeconds);
        for (const int64_t at : {kTenSeconds, kTenSeconds + 1, 5 * kTenSeconds}) {
            const auto range = SingleFrameRange(at, fps, kTenSeconds);
            CHECK_EQ(FramePts(lastIndex, fps), range.start100ns);
            CHECK_EQ(FramePts(lastIndex + 1, fps), range.end100ns);
        }
    }
    // Frame zero of a 1 fps still image is the whole source.
    CHECK(SingleFrameRange(0, 1.0, 10000000).Whole());
    CHECK(!SingleFrameRange(0, 5.0, 10000000).Whole());
    CHECK_EQ(int64_t{2000000}, SingleFrameRange(0, 5.0, 10000000).end100ns);
}

void ClipPreviewTests()
{
    const auto fromStart = ClipPreviewRange(0, 60.0, kTenSeconds);
    CHECK_EQ(int64_t{0}, fromStart.start100ns);
    CHECK_EQ(int64_t{40000000}, fromStart.end100ns);
    CHECK(!fromStart.Whole());
    const auto seconds = ClipPreviewRange(0, 60.0, kTenSeconds, 2.0);
    CHECK_EQ(int64_t{20000000}, seconds.end100ns);
    // Clamps at the source end and never produces an empty clip.
    const auto tail = ClipPreviewRange(80000000, 60.0, kTenSeconds);
    CHECK_EQ(int64_t{80000000}, tail.start100ns);
    CHECK_EQ(kTenSeconds, tail.end100ns);
    const auto beyond = ClipPreviewRange(3 * kTenSeconds, 60.0, kTenSeconds);
    CHECK_EQ(FramePts(599, 60.0), beyond.start100ns);
    CHECK_EQ(kTenSeconds, beyond.end100ns);
    const auto lastFrame = ClipPreviewRange(FramePts(599, 60.0), 60.0, kTenSeconds);
    CHECK_EQ(FramePts(599, 60.0), lastFrame.start100ns);
    CHECK_EQ(kTenSeconds, lastFrame.end100ns);
    // A clip longer than the source from frame zero is the whole source; the
    // grid end of a 23.976 source lies just beyond its container duration.
    CHECK(ClipPreviewRange(0, 60.0, kTenSeconds, 20.0).Whole());
    CHECK(ClipPreviewRange(0, k23976, kTenSeconds, 10.0).Whole());
    const auto ragged = ClipPreviewRange(FramePts(200, k23976), k23976, kTenSeconds);
    CHECK_EQ(FramePts(200, k23976), ragged.start100ns);
    CHECK_EQ(FramePts(240, k23976), ragged.end100ns);
    CHECK(ragged.end100ns >= kTenSeconds);
    CHECK(FramePts(239, k23976) < kTenSeconds);
    CHECK(ClipPreviewRange(0, 60.0, kTenSeconds, 0.0).end100ns == FramePts(1, 60.0));
}

void PrerollTests()
{
    CHECK_EQ(0u, PrerollFramesFor({}, 60.0));
    CHECK_EQ(0u, PrerollFramesFor({0, 10000000}, 60.0));
    CHECK_EQ(1u, PrerollFramesFor({FramePts(1, 60.0), 10000000}, 60.0));
    CHECK_EQ(10u, PrerollFramesFor({FramePts(10, 60.0), 10000000}, 60.0));
    CHECK_EQ(59u, PrerollFramesFor({FramePts(59, 60.0), 10000000}, 60.0));
    CHECK_EQ(kDefaultPrerollFrames, PrerollFramesFor({FramePts(60, 60.0), 10000000}, 60.0));
    CHECK_EQ(kDefaultPrerollFrames, PrerollFramesFor({FramePts(100000, 60.0), 0}, 60.0));
    CHECK_EQ(37u, PrerollFramesFor({FramePts(37, k23976), 0}, k23976));
    // A start between grid points counts every frame that begins before it.
    CHECK_EQ(3u, PrerollFramesFor({5000000, 10000000}, 5.0));
    CHECK_EQ(0u, PrerollFramesFor({FramePts(10, 60.0), 10000000}, 0.0));
}

} // namespace

int main()
{
    TimecodeRoundTripTests();
    TimecodeParseTests();
    MarkerTests();
    SingleFrameTests();
    ClipPreviewTests();
    PrerollTests();
    if (test_support::failure_count != 0) return EXIT_FAILURE;
    std::cout << "Range selection tests passed.\n";
    return EXIT_SUCCESS;
}
