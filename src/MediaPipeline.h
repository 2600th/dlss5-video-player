#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

enum class EncoderKind {
    HevcNvenc,
    H264Software,
};

enum class EncodeError {
    None,
    InvalidSpecification,
    HelperMissing,
    StartFailed,
    WriteFailed,
    FinishFailed,
    Cancelled,
    InvalidFrame,
};

enum class MaterializeError {
    None,
    InvalidRequest,
    HelperMissing,
    StartFailed,
    ProcessFailed,
    Cancelled,
};

struct MaterializeRequest {
    std::wstring videoUrl;
    std::wstring audioUrl;
    std::filesystem::path output;
    double expectedDurationSeconds{};
};

struct CachedExportRequest {
    std::filesystem::path neuralVideo;
    std::filesystem::path sourceMedia;
    std::filesystem::path output;
    // Trim applied to the source's audio/subtitles/chapters so they match a
    // range-rendered neural video: the source input is seeked to the start and
    // the muxed output bounded to the duration. Start 0 and duration 0 keep
    // the whole source; a positive start with duration 0 runs to its end.
    double rangeStartSeconds{};
    double rangeDurationSeconds{};
};

// Capacity requested for a child process's stdin pipe. Large enough that the encoder
// feeder is not woken for every partial frame, small enough that it does not hold tens of
// megabytes of nonpaged pool. Exposed because a write only blocks, and therefore only
// becomes cancellable, once it exceeds this.
inline constexpr size_t kChildStdinPipeBytes = 16u * 1024u * 1024u;

// Layout of the raw frames fed to RawVideoEncoder::WriteFrame. The neural capture path
// reads back a B8G8R8A8 render target, so letting ffmpeg consume BGRA directly removes
// a full-frame channel swizzle on the CPU. Everything else still supplies BGRA.
//
// Nv12 is the same capture with the colour conversion already done on the GPU: one
// full-resolution Y plane followed by an interleaved half-resolution UV plane, BT.709
// with limited range. It exists because BGRA leaves ffmpeg converting every frame on
// the CPU, which measured as the export's slowest stage, and it also cuts the readback
// and the pipe write from four bytes per pixel to one and a half.
enum class EncoderPixelFormat { Bgra, Nv12 };

// Bytes one frame of `format` occupies at this size. NV12 needs even dimensions; the
// caller is responsible for not selecting it otherwise.
constexpr uint64_t EncoderFrameBytes(EncoderPixelFormat format, uint32_t width, uint32_t height)
{
    const uint64_t pixels = uint64_t{width} * height;
    return format == EncoderPixelFormat::Nv12 ? pixels + (pixels / 2u) : pixels * 4u;
}

struct EncoderSpec {
    uint32_t width{};
    uint32_t height{};
    double fps{};
    EncoderKind kind{EncoderKind::HevcNvenc};
    EncoderPixelFormat pixelFormat{EncoderPixelFormat::Bgra};
    // NVENC preset p1..p7, 7 being the slowest and 1 the fastest; ignored by the
    // software encoder.
    //
    // p5 is the default on a measured trade, not on taste. Measured 2026-09-18
    // on an RTX 5090 / driver 616.64, hevc_nvenc at the shipping settings
    // (-tune hq -rc vbr -cq 16 -b:v 0 -split_encode_mode auto), 120 frames of
    // 2560x1440 NV12, quality scored against the raw input:
    //
    //   preset   encode time   VMAF (soft)   VMAF (noise)
    //   p7       1.51 s        98.17         95.78
    //   p6       1.38 s        -             -
    //   p5       0.75 s        98.05         95.26
    //   p4       0.71 s        -             -
    //   p1       0.45 s        -             -
    //
    // So p7 costs TWICE the encode time of p5 to buy 0.12 VMAF on ordinary
    // content and 0.53 on the noise-heavy worst case, at 95-98 VMAF where
    // neither is a difference anyone can see. Both figures are far below the
    // ~6 VMAF usually quoted as the just-noticeable difference.
    //
    // What that is worth depends on whether the encoder is the long pole, and
    // for this player it is. A frame-generation conversion was measured
    // end to end through the shipped pass on the same machine - 2560x1440,
    // 2x, 480 output frames - at 9.86 ms per output frame on p7 against
    // 5.08 ms on p5, a fit over 120 and 480 frames that also puts the fixed
    // cost (device, NGX init, mux, two probes) at 3.8 s. The per-frame figure
    // MATCHES the standalone encode above, which is the finding: decode,
    // upload, evaluate and readback already hide under FFmpeg's own
    // concurrency through the pipe, so the encoder preset is the only thing
    // on that path worth changing. Whole conversions went 8.8 s -> 6.3 s.
    //
    // The same encoder writes the neural render's segments, so the render path
    // takes the same trade. Anyone who wants p7 back has it in Encoder
    // settings, and the tooltip there quotes these numbers.
    uint32_t nvencPreset{5};
};

struct MaterializeResult {
    bool ok{};
    MaterializeError error{MaterializeError::None};
    std::wstring detail;
};

// Reported while a remote source is copied to the cache. FFmpeg only knows what
// it has already muxed, so both fields describe written output, not the yet
// unknown total: `bytes` is the container size so far and `seconds` the source
// position reached. A caller with an expected duration turns the latter into a
// percentage; nothing here does, because a request may carry no duration.
struct MediaDownloadProgress {
    uint64_t bytes{};     // total_size, 0 until FFmpeg reports one
    double seconds{};     // out_time, the source position already written
};

struct ProbeResult {
    bool ok{};
    uint32_t width{};
    uint32_t height{};
    uint64_t frameCount{};
    int64_t duration100ns{};
    bool decodedFinalFrame{};
    std::wstring detail;
    // Decoded video frame extent; independent of container/audio duration.
    // Zero for CachedMetadata, which does not inspect frames.
    int64_t videoDuration100ns{};
};

std::vector<std::wstring> BuildMaterializeArguments(const MaterializeRequest& request);
std::vector<std::wstring> BuildEncoderArguments(const EncoderSpec& spec,
                                                const std::filesystem::path& output);
// FFmpeg arguments for CachedVideoExporter. The container follows the
// extension of request.output; the encoded file is written to `staging`.
// oddDimensions selects the 4:4:4 MP4 path; only MP4 exports inspect it.
std::vector<std::wstring> BuildCachedExportArguments(const CachedExportRequest& request,
                                                     const std::filesystem::path& staging,
                                                     bool oddDimensions);
// Bytes RawVideoEncoder requires of one frame of `spec`, or 0 when the spec cannot be
// encoded at all - which includes NV12 at an odd width or height, since that format has
// no half-pixel chroma sample to describe it.
size_t ExpectedFrameBytes(const EncoderSpec& spec);
bool ShouldRetryWithSoftware(EncoderKind attempted, EncodeError error);

class MediaMaterializer {
public:
    explicit MediaMaterializer(std::filesystem::path helperDirectory = {});
    // The progress callback is optional and runs on the calling thread inside
    // Run, at most once every 250 ms plus a final report; two-argument callers
    // that only need the result stay unchanged.
    MaterializeResult Run(const MaterializeRequest& request, std::stop_token stop,
                          std::function<void(const MediaDownloadProgress&)> onProgress = {});

private:
    std::filesystem::path helperDirectory_;
};

class CachedVideoExporter {
public:
    explicit CachedVideoExporter(std::filesystem::path helperDirectory = {});
    MaterializeResult Run(const CachedExportRequest& request, std::stop_token stop);

private:
    std::filesystem::path helperDirectory_;
};

class RawVideoEncoder {
public:
    explicit RawVideoEncoder(std::filesystem::path helperDirectory = {});
    ~RawVideoEncoder();
    RawVideoEncoder(const RawVideoEncoder&) = delete;
    RawVideoEncoder& operator=(const RawVideoEncoder&) = delete;

    EncodeError Start(const EncoderSpec& spec, const std::filesystem::path& output);
    EncodeError WriteFrame(std::span<const uint8_t> bgra, std::stop_token stop = {});
    EncodeError Finish(std::stop_token stop = {});
    void Cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Losslessly joins already-encoded parts into one Matroska file, in the given
// order, through FFmpeg's concat demuxer. Every part must share the same codec
// and dimensions, which finalized neural segments of one job do.
EncodeError ConcatenateMedia(const std::filesystem::path& helperDirectory,
                             std::span<const std::filesystem::path> parts,
                             const std::filesystem::path& output,
                             std::stop_token stop = {});

// Stream-copies everything that travels with `sourceMedia` - audio, subtitles,
// chapters, attachments and metadata - alongside the first video stream of
// `video` into one Matroska file. This is CachedVideoExporter driven with no
// trim: the same single muxer the cached-range export uses, so the command,
// the optional source maps and the container choice all come from there.
// Nothing is re-encoded and nothing is re-timed: both inputs keep the
// timestamps they already have, so this is only correct where `video` covers
// the same span of time as `sourceMedia` does. A source with no audio, no
// subtitles and no chapters still produces a valid output, because every
// source map is optional.
//
// The output is written to an exclusively created staging file in its folder
// and renamed onto `output` once the mux succeeds, so a failed or cancelled
// run leaves no partial file. An existing `output` is removed first, which is
// safe only because the caller owns and guards that path - unlike the
// interactive export, which refuses to overwrite a file the user named.
EncodeError MuxVideoWithSourceStreams(const std::filesystem::path& helperDirectory,
                                      const std::filesystem::path& video,
                                      const std::filesystem::path& sourceMedia,
                                      const std::filesystem::path& output,
                                      std::stop_token stop = {});

// Counts only, or the counts plus where the first audio stream ends.
// WithAudioEnd demuxes that stream, so it is asked for only where the answer
// is used.
enum class MediaStreamMode { Counts, WithAudioEnd };

// What a file carries beside its video. Separate from ProbeResult because
// ProbeMedia answers for the video stream and is on the neural publish path,
// which must not pay for a question about audio it never asks.
struct MediaStreamSummary {
    bool ok{};
    uint32_t audioStreams{};
    uint32_t subtitleStreams{};
    // Where the first audio stream ends on the file's own timeline: its last
    // packet's timestamp plus that packet's own duration. Measured by
    // demuxing, because Matroska carries no per-stream duration at all - on
    // this FFmpeg a stream-copied MKV reports stream duration=N/A for its
    // video and its audio alike while the container says 8.021 s - so a header
    // read cannot answer where the audio in a muxed file actually stops.
    //
    // An END and not a span, because a span is not comparable with a video's
    // length: this clip's AAC begins with a priming packet at pts -0.021, so
    // end-minus-first reads 8.021 s for audio that stops exactly with the
    // 8.000 s video. The end is the number that says whether audio was
    // truncated or stretched against the picture. 0 when the file has no audio
    // stream or when mode is Counts.
    int64_t audioEnd100ns{};
    std::wstring detail;
};

MediaStreamSummary SummarizeMediaStreams(const std::filesystem::path& helperDirectory,
                                         const std::filesystem::path& media,
                                         std::stop_token stop,
                                         MediaStreamMode mode = MediaStreamMode::Counts);

// One stream of a file as ffprobe names it: its absolute index, its type
// ("video", "audio", "subtitle", "attachment", "data") and its codec.
struct MediaStreamInfo {
    uint32_t index{};
    std::string type;
    std::string codec;
};

// Every stream of `media`, in file order, or nothing when ffprobe could not
// list them. A header read.
std::optional<std::vector<MediaStreamInfo>> ListMediaStreams(const std::filesystem::path& helperDirectory,
                                                             const std::filesystem::path& media,
                                                             std::stop_token stop);

// The last step of "Export with DLSS stages": the video the passes wrote, kept
// bit for bit, in the container the output's extension names, with the
// streams of `streamSource` beside it. GIF, PNG and JPEG are encoded from the
// video alone. The container is not the carrier's: every pass writes
// Matroska, and this is what turns that into the file the user named
// (ExportContainerFor in ExportPipeline.h).
struct StageExportMuxRequest {
    std::filesystem::path video;
    std::filesystem::path streamSource;
    std::filesystem::path output;
    // The same trim CachedExportRequest applies, for a video that covers only
    // a range of `streamSource`: 0 and 0 keep the whole source.
    double rangeStartSeconds{};
    double rangeDurationSeconds{};
};

// The FFmpeg arguments for MuxStageExport, from what the two inputs were
// probed to carry. `staging` is the file FFmpeg writes. Empty when the
// output's extension names no container this export writes.
std::vector<std::wstring> BuildStageExportMuxArguments(const StageExportMuxRequest& request,
                                                       const std::filesystem::path& staging,
                                                       std::string_view videoCodec,
                                                       const std::vector<MediaStreamInfo>& sourceStreams);

// Writes the finished export. Unlike CachedVideoExporter this REPLACES an
// existing output: the user confirmed the overwrite in the Save dialog, or
// named the file with --out, whose documented contract is to replace it. The
// replacement is atomic - FFmpeg writes a staging file in the output's folder
// that is moved over the output only once it is complete - so a failed or
// cancelled mux leaves whatever was there before untouched.
MaterializeResult MuxStageExport(const std::filesystem::path& helperDirectory,
                                 const StageExportMuxRequest& request,
                                 std::stop_token stop);

enum class MediaProbeMode { FullValidation, CachedMetadata };

ProbeResult ProbeMedia(const std::filesystem::path& helperDirectory,
                       const std::filesystem::path& media,
                       std::stop_token stop,
                       MediaProbeMode mode = MediaProbeMode::FullValidation);

namespace media_pipeline_detail {

// Incremental reader for the `-progress pipe:1` key/value stream FFmpeg writes
// while it materializes a source. It exists as a named type, rather than a
// lambda inside MediaMaterializer::Run, so the awkward parts can be tested
// without a child process: the capture callback hands over whatever one ReadFile
// returned, so a key may be split across two chunks, and the stream is shared
// with FFmpeg's stderr, so every line that is not a progress key must be handed
// back untouched instead of being swallowed by the parser.
class MediaProgressReader {
public:
    using Clock = std::chrono::steady_clock;

    MediaProgressReader(std::function<void(const MediaDownloadProgress&)> onProgress,
                        std::function<void(std::string_view)> onDiagnostic);
    // `now` is injected so the rate limit is testable; callers pass Clock::now().
    void Consume(std::string_view chunk, Clock::time_point now);
    // Flushes a trailing line without a newline and reports the last block even
    // if the rate limit would have withheld it, so the final size is never lost.
    void Finish(Clock::time_point now);

private:
    void EndLine(Clock::time_point now);
    void ReadLine(std::string_view line, Clock::time_point now);
    void Report(Clock::time_point now, bool force);

    std::function<void(const MediaDownloadProgress&)> onProgress_;
    std::function<void(std::string_view)> onDiagnostic_;
    std::string partial_;
    MediaDownloadProgress latest_{};
    Clock::time_point lastReport_{};
    bool oversizedLine_{};
    bool reported_{};
    bool pending_{};
};

// FFmpeg writes a progress block roughly twice a second; a local copy of an
// already downloaded file finishes them far faster. One report per 250 ms keeps
// the UI thread free while still looking live to the user who is watching it.
inline constexpr std::chrono::milliseconds kMediaProgressInterval{250};

} // namespace media_pipeline_detail
