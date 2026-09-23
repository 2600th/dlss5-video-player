#pragma once

#include "AudioTrackPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Subtitles: which track, which file, when, and the ffmpeg command that draws
// them. The decisions live here, free of processes and GPUs, so they can be
// tested; SubtitleOverlay runs the commands and D3D12Renderer composites the
// frames they produce.
//
// Subtitles are drawn AFTER the network, over the presented picture, and never
// reach the cache or the neural input: they are HUD, and the model warps and
// shimmers static text the way it would any fine high-contrast detail. They are
// drawn at the size the picture has on screen, not at the video's size, so text
// is rasterised once at the resolution it is seen at rather than upscaled.
//
// The staged FFmpeg carries libass (--enable-libass, DirectWrite font provider),
// so there is no new dependency: an ffmpeg child renders onto a transparent
// canvas and hands back premultiplied BGRA frames, the way the player already
// decodes video in ffmpeg children. The subtitles filter sets libass's storage
// size (original_size below) and loads fonts attached to a Matroska file, the
// two details libass integrations most often get wrong.
namespace subtitle {

// How a stream is drawn. Text goes through libass; a bitmap stream (PGS,
// VobSub, DVB) is already a picture and is scaled onto the canvas.
enum class Kind { Text, Bitmap, Unsupported };

inline Kind KindForCodec(std::string_view codec)
{
    static constexpr std::string_view kText[] = {
        "subrip", "srt", "ass", "ssa", "webvtt", "mov_text", "text", "microdvd", "subviewer",
        "subviewer1", "sami", "realtext", "jacosub", "mpl2", "pjs", "vplayer", "stl", "ttml"};
    static constexpr std::string_view kBitmap[] = {
        "hdmv_pgs_subtitle", "dvd_subtitle", "dvb_subtitle", "xsub"};
    for (const std::string_view text : kText)
        if (codec == text) return Kind::Text;
    for (const std::string_view bitmap : kBitmap)
        if (codec == bitmap) return Kind::Bitmap;
    return Kind::Unsupported;
}

struct Track {
    // Index among the source's subtitle streams: what `0:s:N` and the
    // subtitles filter's `si=N` take.
    int subtitleIndex = 0;
    std::string language;
    std::string title;
    std::string codec;
    bool isDefault = false;
    bool forced = false;
    bool hearingImpaired = false;
    bool visualImpaired = false;
    bool comment = false;

    Kind Drawn() const { return KindForCodec(codec); }
};

inline constexpr size_t kNoTrack = static_cast<size_t>(-1);

// The track to start on, or none: subtitles are off unless the file asks for
// them. A track the container marks as its default is what its author wants
// shown; failing that, a forced track, which carries only the lines a viewer
// cannot follow otherwise (a foreign-language scene). A stream the player
// cannot draw is never chosen.
inline size_t SelectDefault(std::span<const Track> tracks)
{
    size_t forced = kNoTrack;
    for (size_t index = 0; index < tracks.size(); ++index) {
        if (tracks[index].Drawn() == Kind::Unsupported) continue;
        if (tracks[index].isDefault) return index;
        if (tracks[index].forced && forced == kNoTrack) forced = index;
    }
    return forced;
}

namespace detail {

inline std::string CodecName(std::string_view codec)
{
    struct Entry { std::string_view codec, name; };
    static constexpr Entry kNames[] = {
        {"subrip", "SRT"},         {"srt", "SRT"},     {"ass", "ASS"},          {"ssa", "SSA"},
        {"webvtt", "WebVTT"},      {"mov_text", "Timed Text"},                  {"hdmv_pgs_subtitle", "PGS"},
        {"dvd_subtitle", "VobSub"}, {"dvb_subtitle", "DVB"}, {"dvb_teletext", "Teletext"},
        {"eia_608", "CEA-608"},
    };
    for (const Entry& entry : kNames)
        if (entry.codec == codec) return std::string(entry.name);
    return audio_track::detail::CodecName(codec);
}

} // namespace detail

// What the menu shows, in the audio track list's shape so the two read alike:
// "2. English - SDH - SRT (forced)".
inline std::string Describe(const Track& track)
{
    std::string label = std::to_string(track.subtitleIndex + 1) + ".";
    const std::string language = audio_track::detail::LanguageName(track.language);
    if (!language.empty()) label += " " + language;
    if (!track.title.empty()) label += (language.empty() ? " " : " - ") + track.title;
    const std::string format = track.codec.empty() ? std::string{} : detail::CodecName(track.codec);
    if (!format.empty())
        label += (language.empty() && track.title.empty()) ? " " + format : " - " + format;
    if (track.forced) label += " (forced)";
    else if (track.hearingImpaired) label += " (for the hard of hearing)";
    else if (track.comment) label += " (commentary)";
    if (track.Drawn() == Kind::Unsupported) label += " (not supported)";
    return label;
}

// ---- Files beside the video -------------------------------------------------

namespace detail {

inline wchar_t Lower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? wchar_t(c - L'A' + L'a') : c; }

inline bool EqualNoCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t index = 0; index < a.size(); ++index)
        if (Lower(a[index]) != Lower(b[index])) return false;
    return true;
}

inline std::pair<std::wstring_view, std::wstring_view> SplitExtension(std::wstring_view name)
{
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos || dot == 0) return {name, {}};
    return {name.substr(0, dot), name.substr(dot)};
}

} // namespace detail

// The subtitle files the player picks up by themselves, best first. Text before
// bitmap because text is drawn sharp at any size; .sub is left out on purpose:
// it is both MicroDVD text and VobSub pictures, and the .idx beside a VobSub
// .sub is the file that names it.
inline constexpr std::wstring_view kSidecarExtensions[] = {L".ass", L".ssa", L".srt", L".vtt", L".sup", L".idx"};

inline bool IsSidecarExtension(std::wstring_view extension)
{
    for (const std::wstring_view known : kSidecarExtensions)
        if (detail::EqualNoCase(known, extension)) return true;
    return false;
}

// The file to load automatically for `videoFileName`, chosen from the names in
// its folder: the video's own name with a subtitle extension ("Film.srt"), or
// failing that with one more tag before it ("Film.en.srt", "Film.forced.ass").
// An exact name beats a tagged one, then the extension order above, then the
// name, so the answer does not depend on the order the folder was listed in.
inline std::optional<std::wstring> FindSidecar(std::wstring_view videoFileName,
                                               std::span<const std::wstring> folderNames)
{
    const std::wstring_view stem = detail::SplitExtension(videoFileName).first;
    if (stem.empty()) return std::nullopt;
    struct Candidate { int tagged; size_t extension; std::wstring name; };
    std::optional<Candidate> best;
    for (const std::wstring& name : folderNames) {
        const auto [base, extension] = detail::SplitExtension(name);
        size_t rank = std::size(kSidecarExtensions);
        for (size_t index = 0; index < std::size(kSidecarExtensions); ++index)
            if (detail::EqualNoCase(kSidecarExtensions[index], extension)) rank = index;
        if (rank == std::size(kSidecarExtensions)) continue;
        int tagged = -1;
        if (detail::EqualNoCase(base, stem)) tagged = 0;
        else if (base.size() > stem.size() + 1 && base[stem.size()] == L'.' &&
                 detail::EqualNoCase(base.substr(0, stem.size()), stem) &&
                 base.find(L'.', stem.size() + 1) == std::wstring_view::npos)
            tagged = 1;
        if (tagged < 0) continue;
        const Candidate candidate{tagged, rank, name};
        if (!best || std::tie(candidate.tagged, candidate.extension, candidate.name) <
                         std::tie(best->tagged, best->extension, best->name))
            best = candidate;
    }
    if (!best) return std::nullopt;
    return best->name;
}

// ---- Text encoding ------------------------------------------------------------

// libass takes UTF-8. FFmpeg's subtitle demuxers recognise a UTF-8 or UTF-16
// byte-order mark and convert UTF-16 themselves (checked against the staged
// 9.0.1: a UTF-16LE .srt with a BOM renders correctly with no option), so those
// need nothing. UTF-16 without a mark is recognised here by its zero bytes. Any
// other file that is not valid UTF-8 is taken to be in the machine's ANSI code
// page: an old .srt on a Windows machine was almost always saved by a tool that
// wrote that code page, and without a charenc FFmpeg refuses the file outright
// ("Invalid UTF-8 in decoded subtitles text") rather than showing it garbled.
// A file in some other legacy code page is shown with the wrong accents, which
// is the limit of a heuristic this cheap.
enum class TextEncoding { Utf8, Utf8Bom, Utf16LeBom, Utf16BeBom, Utf16Le, Utf16Be, Legacy };

inline TextEncoding DetectTextEncoding(std::span<const uint8_t> head)
{
    if (head.size() >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) return TextEncoding::Utf8Bom;
    if (head.size() >= 2 && head[0] == 0xFF && head[1] == 0xFE) return TextEncoding::Utf16LeBom;
    if (head.size() >= 2 && head[0] == 0xFE && head[1] == 0xFF) return TextEncoding::Utf16BeBom;
    // Subtitle text is mostly ASCII, so UTF-16 without a mark has a zero in
    // about every other byte, on the odd side for little endian.
    size_t evenZeros = 0, oddZeros = 0;
    for (size_t index = 0; index < head.size(); ++index)
        if (head[index] == 0) ++((index % 2) != 0 ? oddZeros : evenZeros);
    const size_t pairs = head.size() / 2;
    if (pairs >= 4) {
        if (oddZeros * 10 >= pairs * 3 && evenZeros * 10 < pairs) return TextEncoding::Utf16Le;
        if (evenZeros * 10 >= pairs * 3 && oddZeros * 10 < pairs) return TextEncoding::Utf16Be;
    }
    // Strict UTF-8, except that a sequence cut off by the end of the sample is
    // not held against the file.
    for (size_t index = 0; index < head.size();) {
        const uint8_t lead = head[index];
        size_t length = 0;
        uint32_t minimum = 0;
        if (lead < 0x80) { ++index; continue; }
        if ((lead & 0xE0) == 0xC0) { length = 2; minimum = 0x80; }
        else if ((lead & 0xF0) == 0xE0) { length = 3; minimum = 0x800; }
        else if ((lead & 0xF8) == 0xF0) { length = 4; minimum = 0x10000; }
        else return TextEncoding::Legacy;
        if (index + length > head.size()) {
            for (size_t next = index + 1; next < head.size(); ++next)
                if ((head[next] & 0xC0) != 0x80) return TextEncoding::Legacy;
            break;
        }
        uint32_t code = lead & (0xFFu >> (length + 1));
        for (size_t next = 1; next < length; ++next) {
            if ((head[index + next] & 0xC0) != 0x80) return TextEncoding::Legacy;
            code = (code << 6) | (head[index + next] & 0x3Fu);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return TextEncoding::Legacy;
        index += length;
    }
    return TextEncoding::Utf8;
}

// The subtitles filter's charenc for a file in `encoding`, or empty for none.
inline std::wstring CharencFor(TextEncoding encoding, unsigned ansiCodePage)
{
    switch (encoding) {
    case TextEncoding::Utf16Le: return L"UTF-16LE";
    case TextEncoding::Utf16Be: return L"UTF-16BE";
    case TextEncoding::Legacy: return ansiCodePage ? L"CP" + std::to_wstring(ansiCodePage) : L"CP1252";
    default: return {};
    }
}

// ---- The ffmpeg command lines ---------------------------------------------------

// A value inside a filtergraph passes two parsers, each with its own escaping:
// the filter's option parser (`\`, `'` and `:`, which separates options) and
// then the graph parser (`\`, `'`, and `[ ] , ;`, which delimit filters). A
// Windows path is full of both, so it is written with forward slashes (which
// FFmpeg accepts) and escaped for the option level, then the graph level.
inline std::wstring EscapeFilterValue(std::wstring_view value)
{
    std::wstring option;
    for (wchar_t c : value) {
        if (c == L'\\') c = L'/';
        if (c == L'\'' || c == L':') option += L'\\';
        option += c;
    }
    std::wstring graph;
    for (const wchar_t c : option) {
        if (c == L'\\' || c == L'\'' || c == L'[' || c == L']' || c == L',' || c == L';') graph += L'\\';
        graph += c;
    }
    return graph;
}

// Frames on the canvas per second. Aligned with the video's own when it is 30
// or below, so an event lands on the frame it was timed to; capped at 30
// because a subtitle is text that changes every few seconds, and a 60 fps
// canvas would only double the frames the child has to compare.
inline double CanvasRate(double videoFps)
{
    if (!std::isfinite(videoFps) || videoFps <= 0.0) return 24.0;
    return std::clamp(videoFps, 10.0, 30.0);
}

// A canvas the child can draw and the renderer can hold.
inline bool CanvasUsable(uint32_t width, uint32_t height)
{
    return width >= 16 && height >= 16 && width <= 16384 && height <= 16384;
}

// Which frames the child writes: every one of them, with its time, on the
// stats line the reader pairs with it (see SubtitleOverlay).
inline constexpr std::wstring_view kStatsPrefix = L"SUBT ";

struct RenderCommand {
    std::wstring input;              // the file with the subtitles
    int stream = 0;                  // subtitle-relative stream index in it
    Kind kind = Kind::Text;
    std::wstring charenc;            // text in a legacy encoding, see CharencFor
    uint32_t width = 0, height = 0;  // the canvas: the picture's size on screen
    uint32_t videoWidth = 0, videoHeight = 0;  // libass's storage size
    double rate = 24.0;              // canvas frames per second (text)
    double start = 0.0;              // subtitle time of the first frame
    double origin = 0.0;             // the input's own time at subtitle time zero
    double duration = 0.0;           // of the video, 0 when unknown
};

// How long before the start a bitmap child begins reading. A PGS or VobSub
// picture is sent once, when it appears, so a child that starts reading at the
// seek point misses one that is already on screen; it reads from here instead,
// and the frames before the start are only there to leave the right one showing.
inline constexpr double kBitmapPrerollSeconds = 30.0;

namespace detail {

inline std::wstring Seconds(double seconds)
{
    std::wostringstream text;
    text << std::fixed << std::setprecision(6) << seconds;
    return text.str();
}

} // namespace detail

// The arguments after "ffmpeg.exe ". Raw premultiplied BGRA frames of exactly
// width*height*4 bytes on stdout, only when the picture changes, each with a
// "SUBT <pts> <num>/<den>" line on stderr (in the input's own time, which the
// reader converts back with `origin`).
//
// Text: a transparent canvas, stamped with the subtitle clock, drawn by libass.
// Every canvas frame is drawn once onto an opaque canvas of an odd colour and
// compared with the last one kept (mpdecimate with every threshold at zero, so
// any changed pixel keeps it); only a kept frame is drawn again, onto a
// transparent canvas with alpha=1, which leaves premultiplied colour and
// coverage alpha - the same bytes a black-and-white difference matte gives,
// measured. The detection draw is opaque because mpdecimate reads no alpha, and
// odd-coloured so that a black box drawn on black still counts as a change. At
// 1920x1080 a canvas frame that did not change costs about 1.3 ms, and 6 ms at
// 3840x2160, against 5 ms for a pipeline that mattes every frame.
//
// Bitmap: the stream's own pictures, which ffmpeg turns into frames only when
// one appears or is taken away, premultiplied and then scaled to the canvas.
// A picture made for a 1920x1080 frame over a video cropped to 1920x800 is
// stretched with the frame, not cut off.
inline std::wstring RenderArguments(const RenderCommand& command)
{
    std::wostringstream args;
    args << L"-hide_banner -loglevel error -nostdin -copyts ";
    const std::wstring size = std::to_wstring(command.width) + L"x" + std::to_wstring(command.height);
    if (command.kind == Kind::Bitmap) {
        const double from = std::max(0.0, command.start - kBitmapPrerollSeconds);
        if (from > 0.0) args << L"-ss " << detail::Seconds(from) << L" ";
        args << L"-i \"" << command.input << L"\" -filter_complex \"[0:s:" << command.stream
             << L"]format=gbrap,premultiply=inplace=1,scale=" << size << L":flags=bilinear,format=bgra[subs]\"";
    } else {
        std::wstring source = L"f=" + EscapeFilterValue(command.input) + L":si=" + std::to_wstring(command.stream);
        if (!command.charenc.empty()) source += L":charenc=" + command.charenc;
        if (command.videoWidth && command.videoHeight)
            source += L":original_size=" + std::to_wstring(command.videoWidth) + L"x" + std::to_wstring(command.videoHeight);
        // Ends a second after the video does; with no duration, after six hours.
        // Canvas frames nothing changes on cost little, but not nothing, and a
        // canvas without an end would be compared for ever past the last line.
        const double remaining = command.duration > 0.0 ? std::max(1.0, command.duration - command.start + 1.0) : 21600.0;
        std::wostringstream rate;
        rate << std::setprecision(8) << command.rate;
        args << L"-filter_complex \"color=c=0x10EF60:s=" << size << L":r=" << rate.str() << L":d="
             << detail::Seconds(remaining) << L",format=gbrp,setpts=PTS+" << detail::Seconds(command.start + command.origin)
             << L"/TB,subtitles=" << source << L",mpdecimate=hi=0:lo=0:frac=0,format=gbrap,"
             << L"drawbox=x=0:y=0:w=iw:h=ih:color=black@0:t=fill:replace=1,subtitles=" << source
             << L":alpha=1,format=bgra[subs]\"";
    }
    args << L" -map \"[subs]\" -fps_mode passthrough -stats_mux_pre pipe:2 -stats_mux_pre_fmt \""
         << kStatsPrefix << L"{pts} {tb}\" -f rawvideo pipe:1";
    return args.str();
}

// A text stream inside a video, copied out once into a small Matroska file with
// the fonts the video carries. The subtitles filter reads its whole input when
// it starts, and a child starts at every seek: pointed at a 4 GB film it would
// read 4 GB each time. Tracks Matroska cannot hold as they are (mov_text from
// an MP4) are converted to ASS on the way.
inline std::wstring ExtractArguments(const std::wstring& input, int stream, std::string_view codec,
                                     const std::wstring& output)
{
    const bool copy = codec == "subrip" || codec == "ass" || codec == "ssa" || codec == "webvtt";
    std::wostringstream args;
    args << L"-hide_banner -loglevel error -nostdin -y -i \"" << input << L"\" -map 0:s:" << stream
         << L" -map 0:t? -c:s " << (copy ? L"copy" : L"ass") << L" -c:t copy -f matroska \"" << output << L"\"";
    return args.str();
}

// One stats line: "SUBT <pts> <num>/<den>", in seconds.
inline std::optional<double> ParseStatsLine(std::string_view line)
{
    constexpr std::string_view kPrefix = "SUBT ";
    if (line.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    line.remove_prefix(kPrefix.size());
    const auto integer = [](std::string_view text, int64_t& value) {
        if (text.empty()) return false;
        bool negative = false;
        if (text.front() == '-') { negative = true; text.remove_prefix(1); }
        if (text.empty() || text.size() > 18) return false;
        value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        if (negative) value = -value;
        return true;
    };
    const size_t space = line.find(' ');
    const size_t slash = line.find('/');
    if (space == std::string_view::npos || slash == std::string_view::npos || slash < space) return std::nullopt;
    int64_t pts = 0, num = 0, den = 0;
    std::string_view tail = line.substr(slash + 1);
    while (!tail.empty() && (tail.back() == '\r' || tail.back() == ' ')) tail.remove_suffix(1);
    if (!integer(line.substr(0, space), pts) || !integer(line.substr(space + 1, slash - space - 1), num) ||
        !integer(tail, den) || num <= 0 || den <= 0)
        return std::nullopt;
    return double(pts) * double(num) / double(den);
}

// ---- Time --------------------------------------------------------------------

// The delay control: a positive delay shows each line later.
inline constexpr int kDelayStepMs = 100;
inline constexpr int kDelayLimitMs = 600000;

inline int StepDelay(int delayMs, int direction)
{
    const int64_t next = int64_t(delayMs) + int64_t(direction) * kDelayStepMs;
    return int(std::clamp<int64_t>(next, -kDelayLimitMs, kDelayLimitMs));
}

// The subtitle time to show at video time `videoSeconds`.
inline double SubtitleClock(double videoSeconds, int delayMs) { return videoSeconds - double(delayMs) / 1000.0; }

// Frames arrive only when the picture changes, so the one on screen at `t` is
// the newest with pts <= t, and it stays until the next one's pts. kNoTrack
// when every frame is still ahead.
inline size_t FrameOnScreen(std::span<const double> pts, double t)
{
    size_t shown = kNoTrack;
    for (size_t index = 0; index < pts.size() && pts[index] <= t; ++index) shown = index;
    return shown;
}

// Whether frames already read can say what is on screen at `t`, so a seek there
// needs no new child: one is at or before t, and either one is after it or the
// child has finished. A child only reads a little ahead, so a seek anywhere else
// - back past what it kept, or forward past what it has drawn - starts over.
inline bool QueueAnswers(std::span<const double> pts, bool ended, double t)
{
    const size_t shown = FrameOnScreen(pts, t);
    if (shown == kNoTrack) return false;
    return ended || shown + 1 < pts.size();
}

// A time jump the player made - a seek, a delay step backwards - as against
// the clock simply running between two looks at it.
inline bool ClockJumped(double previous, double now)
{
    return now < previous - 0.001 || now > previous + 1.0;
}

// ---- What a source remembers -----------------------------------------------------

// A viewer's choice for one source, kept in the settings file like the mask:
// a track, a file, or off, and the delay. "Auto" is what nothing remembered
// means: the file beside the video, else the container's default.
struct Choice {
    enum class Mode { Auto, Off, Track, File };
    Mode mode = Mode::Auto;
    int track = -1;
    std::wstring file;
    int delayMs = 0;
    uint64_t sequence = 0;

    friend bool operator==(const Choice&, const Choice&) = default;
};

inline constexpr size_t kMaxRemembered = 200;

// "<sequence>|off|<delay>|", "|track:<n>|", "|file|<delay>|<path>": the path
// last, because it is the one field that may itself contain a '|'.
inline std::wstring FormatChoice(const Choice& choice)
{
    std::wstring mode = L"auto";
    if (choice.mode == Choice::Mode::Off) mode = L"off";
    else if (choice.mode == Choice::Mode::Track) mode = L"track:" + std::to_wstring(choice.track);
    else if (choice.mode == Choice::Mode::File) mode = L"file";
    return std::to_wstring(choice.sequence) + L"|" + mode + L"|" + std::to_wstring(choice.delayMs) + L"|" +
           (choice.mode == Choice::Mode::File ? choice.file : std::wstring{});
}

inline std::optional<Choice> ParseChoice(std::wstring_view text)
{
    const auto number = [](std::wstring_view digits, int64_t& value) {
        bool negative = false;
        if (!digits.empty() && digits.front() == L'-') { negative = true; digits.remove_prefix(1); }
        if (digits.empty() || digits.size() > 18) return false;
        value = 0;
        for (const wchar_t c : digits) {
            if (c < L'0' || c > L'9') return false;
            value = value * 10 + (c - L'0');
        }
        if (negative) value = -value;
        return true;
    };
    std::wstring_view fields[3];
    for (std::wstring_view& field : fields) {
        const size_t bar = text.find(L'|');
        if (bar == std::wstring_view::npos) return std::nullopt;
        field = text.substr(0, bar);
        text.remove_prefix(bar + 1);
    }
    Choice choice;
    int64_t sequence = 0, delay = 0;
    if (!number(fields[0], sequence) || sequence < 0 || !number(fields[2], delay) ||
        delay < -kDelayLimitMs || delay > kDelayLimitMs)
        return std::nullopt;
    choice.sequence = uint64_t(sequence);
    choice.delayMs = int(delay);
    const std::wstring_view mode = fields[1];
    if (mode == L"auto") choice.mode = Choice::Mode::Auto;
    else if (mode == L"off") choice.mode = Choice::Mode::Off;
    else if (mode == L"file") {
        if (text.empty()) return std::nullopt;
        choice.mode = Choice::Mode::File;
        choice.file = std::wstring(text);
    } else if (mode.substr(0, 6) == L"track:") {
        int64_t track = 0;
        if (!number(mode.substr(6), track) || track < 0 || track > 1000) return std::nullopt;
        choice.mode = Choice::Mode::Track;
        choice.track = int(track);
    } else return std::nullopt;
    return choice;
}

// The oldest entries past `keep`, to delete from the settings file.
inline std::vector<std::wstring> Evict(const std::map<std::wstring, Choice>& choices, size_t keep = kMaxRemembered)
{
    std::vector<std::pair<uint64_t, std::wstring>> order;
    for (const auto& [key, choice] : choices) order.emplace_back(choice.sequence, key);
    std::sort(order.begin(), order.end());
    std::vector<std::wstring> evicted;
    for (size_t index = 0; index + keep < order.size(); ++index) evicted.push_back(order[index].second);
    return evicted;
}

} // namespace subtitle
