// Stage B of docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md.
//
// Drives src/AmdLmxxfEvaluator.h through OfflineNeuralRenderer's INJECTION path:
// the real offline job (source restart at the preroll, ResetTemporal on seek,
// the pipelined capture ring, the encoder-failure retry, the result verdicts)
// with a synthetic frame source, the real LmxxfNrRuntime.dll behind the
// evaluator, and a counting encoder that keeps the bytes it was handed. Nothing
// here is a mock of the renderer, and nothing here is a mock of the backend.
//
// What a green run proves: the renderer's frame loop can drive the AMD runtime
// end to end for a whole sequence, the produced frames come back as the capture
// size, they are neither black nor the input frame, and the runtime evaluated
// once per captured frame - which is the check that catches a backend that
// silently declined the work.
//
// What it deliberately does NOT prove, and why: the renderer's NVIDIA evidence
// gates (feature 18 in the ReShade log, the armed-before-capture baseline, the
// neural-GPU-time floor) know nothing about this backend. Making them
// backend-neutral is stage C's evidence model, so this job runs with
// requireNeural = false - the only setting under which a stage B run is not
// asked to produce a ReShade log it can never have. That is why the result
// reports neural=false and verifiedNeuralFrames=0, and why this test judges the
// frames by their pixels and by the runtime's own evaluation tally instead.
//
// Exit codes:
//   0    every assertion held.
//   1    an assertion failed, or the runtime refused a call.
//   125  no runtime pack beside this checkout, or no D3D12 adapter: skipped
//        rather than failed, the ctest convention for absent hardware assets.
//
// Arguments: [runtime directory] [frames for the long run] [output directory]

// windows.h arrives through InheritedHandles.h below, and this file uses
// std::min and std::max, which its min/max macros would break.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AmdLmxxfEvaluator.h"
#include "CompareImageIO.h"
#include "GpuTestGate.h"
#include "GuideFiles.h"
#include "InheritedHandles.h"
#include "OfflineNeuralRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

#ifndef DLSS5_AMD_RUNTIME_DIR
#define DLSS5_AMD_RUNTIME_DIR "amd-neural-runtime"
#endif

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitSkipped = 125;

// The plan's stage B gate is a 10 s 1080p30 sequence; the exit criterion is that
// it comes out as a complete neural video. The runtime's admitted input box is
// 1920x1080, so that is exactly the largest frame it accepts.
constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr double kFps = 30.0;
constexpr uint32_t kShortRunFrames = 12;

// Run 7's subject: the benchmark fixture, decoded by the helper the player itself
// shells out to. Both paths are compile-time because both are repository assets -
// ffmpeg is staged by tools/fetch_ffmpeg_helpers.ps1, the clip is tracked in git -
// and run 7 skips itself rather than failing when either is missing, which is the
// same rule the runtime pack follows at the top of main.
#ifndef DLSS5_FFMPEG_PATH
#define DLSS5_FFMPEG_PATH ""
#endif
#ifndef DLSS5_REAL_CLIP_PATH
#define DLSS5_REAL_CLIP_PATH ""
#endif
const std::filesystem::path kFfmpegPath{DLSS5_FFMPEG_PATH};
const std::filesystem::path kRealClipPath{DLSS5_REAL_CLIP_PATH};

int failures = 0;

void Line(std::string_view text) { std::cout << text << '\n'; }

void Step(std::string_view title) { std::cout << "\n--- " << title << " ---\n"; }

void Check(bool condition, std::string_view what)
{
    Line(std::string(condition ? "  ok   " : "  FAIL ") + std::string(what));
    if (!condition) ++failures;
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                          nullptr, nullptr);
    return out;
}

std::string Narrow(const std::filesystem::path& path) { return Narrow(path.wstring()); }

// A moving, noisy frame. Noise is what a denoiser has to remove and motion is
// what pays for the temporal path, so "did the model run" is answerable from the
// output alone: a flat source would make a pass-through indistinguishable from a
// correct result. Deterministic in (index, seed), because the test regenerates a
// frame to measure the produced frame against it.
std::vector<uint8_t> Pattern(uint64_t index, uint32_t width, uint32_t height, uint32_t seed)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4u);
    uint32_t noise = seed ^ static_cast<uint32_t>(index * 2654435761u);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            noise = noise * 1664525u + 1013904223u;
            const int grain = static_cast<int>((noise >> 24) & 0x1Fu) - 16;
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            const int blue = static_cast<int>((x + 6u * index) & 0xFFu) + grain;
            const int green = static_cast<int>((y * 2u + 3u * index) & 0xFFu) + grain;
            const int red = static_cast<int>(((x + y) / 2u + 9u * index) & 0xFFu) + grain;
            pixels[offset + 0] = static_cast<uint8_t>(std::clamp(blue, 0, 255));
            pixels[offset + 1] = static_cast<uint8_t>(std::clamp(green, 0, 255));
            pixels[offset + 2] = static_cast<uint8_t>(std::clamp(red, 0, 255));
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}

// A still picture plus noise drawn independently per frame. The content terms are
// frozen at index 0 and only the noise seed moves, so the image is identical from
// frame to frame and every byte of variation in it is noise.
//
// This is the one scene where an accumulator can be measured without a motion
// estimator: the correct motion is exactly zero and the correct depth is a
// constant, both of which the evaluator can simply be told. Any improvement over
// the spatial path therefore comes from the temporal model and not from a flow
// field that might itself be wrong.
//
// The grain is +/-8 rather than the +/-16 Pattern uses, and that bound is not
// arbitrary. The runtime's own history guard rejects a pixel whose raw value has
// moved more than .08 from the previous frame's - 20 of 255 - because that is how
// it detects a disocclusion. Two independent +/-16 grains differ by up to 31, so
// most pixels would fail the guard and the frame would accumulate almost nothing
// no matter how correct the guides were. At +/-8 the largest possible frame-to-
// frame difference is 15, so the guard does not decide the outcome and what is
// left measuring is the temporal model itself.
std::vector<uint8_t> StillNoise(uint64_t frameIndex, uint32_t width, uint32_t height, uint32_t seed)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4u);
    uint32_t noise = seed ^ static_cast<uint32_t>(frameIndex * 2246822519u);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            noise = noise * 1664525u + 1013904223u;
            const int grain = static_cast<int>((noise >> 26) & 0x0Fu) - 8;
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            const int blue = static_cast<int>(x & 0xFFu) + grain;
            const int green = static_cast<int>((y * 2u) & 0xFFu) + grain;
            const int red = static_cast<int>(((x + y) / 2u) & 0xFFu) + grain;
            pixels[offset + 0] = static_cast<uint8_t>(std::clamp(blue, 0, 255));
            pixels[offset + 1] = static_cast<uint8_t>(std::clamp(green, 0, 255));
            pixels[offset + 2] = static_cast<uint8_t>(std::clamp(red, 0, 255));
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}

// ---------------------------------------------------------------------------
// The panning clip, which is the one subject that can tell "the motion vector
// was used" apart from "the motion vector was ignored".
//
// A still scene cannot: zero is the right answer there whether the runtime reads
// the field or never samples it. So run 6 renders moving content and compares a
// frame told the truth against the same frame told zero motion - identical
// source, identical geometry, identical noise (the grain is a function of the
// frame index and the pixel, so every arm is fed the same draw), and the only
// input that differs is the number in the motion file.
//
// The content is built for that comparison:
//
//  * every channel is a pure x-translation of a periodic 1-D table, and each
//    period divides 1920, so the wrap the pan needs is exactly as smooth as any
//    other pixel step - there is no seam column for a metric to trip over.
//  * smooth, so the image is nothing like a pathological input for a denoiser
//    trained on natural video, and so the only high-frequency energy in it is
//    the grain that was added.
//  * steep enough that an 8-pixel misregistration moves a pixel's raw value by
//    more than the runtime's own 0.08 disocclusion threshold. That is the
//    property the test needs from it: the frame told zero motion has to LOOK
//    like a disocclusion, column by column, or the runtime would fuse the
//    misregistered history anyway and the two arms would agree.
//
// The three channels carry three different periods and phases, and one of them
// is indexed with a y term, so the picture is two-dimensional rather than three
// copies of one signal.
constexpr uint32_t kPanPixelsPerFrame = 8;

const std::vector<int>& PanTable(uint32_t channel)
{
    static const std::vector<std::vector<int>> tables = [] {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kPeriods[3] = {32.0, 48.0, 64.0};
        constexpr double kPhases[3] = {0.0, 1.0, 2.0};
        std::vector<std::vector<int>> built(3, std::vector<int>(kWidth));
        for (size_t c = 0; c < built.size(); ++c) {
            for (uint32_t x = 0; x < kWidth; ++x) {
                built[c][x] = static_cast<int>(std::lround(
                    128.0 + 100.0 * std::sin(2.0 * kPi * double(x) / kPeriods[c] + kPhases[c])));
            }
        }
        return built;
    }();
    return tables[channel];
}

// The clip's content with no noise: the ground truth a produced frame can be
// measured against, which is the metric run 6 uses. The grain below cannot be
// used for that - a denoiser is supposed to remove it, so measuring against the
// noisy frame would score a perfect denoise as a perfect failure.
//
// The content moves LEFT by kPanPixelsPerFrame each frame: what was at column
// x + shift is now at column x. The motion that describes that - the offset from
// this frame's pixel back to where its content was in the previous one, which is
// the convention GuideFiles.h and TemporalGuides both use - is therefore
// POSITIVE kPanPixelsPerFrame.
//
// That convention is taken from the two headers rather than re-derived here, and
// this run does not independently verify the runtime's reprojection sign. It
// cannot: on this content the accumulated weight is so small that no arm
// separates from any other, so a reversed field would look just as plausible as
// the right one. What would settle it is a source whose content the model
// actually trusts - which is the real-video experiment this run exists to make
// possible, and there a reversed field would show up as more residual noise than
// the spatial path rather than as an error here.
std::vector<uint8_t> PanningContent(uint64_t frameIndex, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4u);
    const uint32_t shift = static_cast<uint32_t>((frameIndex * kPanPixelsPerFrame) % width);
    const std::vector<int>& blue = PanTable(0);
    const std::vector<int>& green = PanTable(1);
    const std::vector<int>& red = PanTable(2);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t u = (x + shift) % width;
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            pixels[offset + 0] = static_cast<uint8_t>(blue[u]);
            pixels[offset + 1] = static_cast<uint8_t>(green[(u + 640u) % width]);
            pixels[offset + 2] = static_cast<uint8_t>(red[(u + 2u * y) % width]);
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}

// The same clip as the decoder would hand it over, with the +/-8 grain StillNoise
// explains: small enough that two frames of aligned content pass the runtime's
// 0.08 disocclusion guard on the grain alone.
std::vector<uint8_t> PanningNoise(uint64_t frameIndex, uint32_t width, uint32_t height, uint32_t seed)
{
    std::vector<uint8_t> pixels = PanningContent(frameIndex, width, height);
    uint32_t noise = seed ^ static_cast<uint32_t>(frameIndex * 2246822519u);
    for (size_t index = 0, count = static_cast<size_t>(width) * height; index < count; ++index) {
        noise = noise * 1664525u + 1013904223u;
        const int grain = static_cast<int>((noise >> 26) & 0x0Fu) - 8;
        for (size_t channel = 0; channel < 3u; ++channel) {
            const int value = static_cast<int>(pixels[index * 4u + channel]) + grain;
            pixels[index * 4u + channel] = static_cast<uint8_t>(std::clamp(value, 0, 255));
        }
    }
    return pixels;
}

// ---------------------------------------------------------------------------
// A synthetic clip the renderer's source contract accepts: consecutive frame
// numbers, strictly increasing pts, and a seek that lands on the frame the
// preroll maths expects. The frames themselves come from `generate`, so the
// clip's contents are one argument rather than a class per scene.
class GeneratedSource final : public IFrameSource
{
public:
    // The last argument is the per-run seed, so a scene can be regenerated byte
    // for byte by the test when it wants to measure a produced frame against its
    // input.
    using Generator = std::vector<uint8_t> (*)(uint64_t, uint32_t, uint32_t, uint32_t);

    GeneratedSource(Generator generate, uint32_t width, uint32_t height, uint64_t frames, double fps,
                    uint32_t seed)
        : generate_(generate), width_(width), height_(height), frames_(frames), fps_(fps), seed_(seed)
    {
    }

    bool Open(const std::filesystem::path&, std::stop_token stop, double seekSeconds) override
    {
        if (stop.stop_requested()) return false;
        ++opens;
        seeks.push_back(seekSeconds);
        const int64_t index = static_cast<int64_t>(std::llround(seekSeconds * fps_));
        index_ = static_cast<uint64_t>(std::max<int64_t>(0, index));
        if (index_ > frames_) index_ = frames_;
        return true;
    }

    void Close() override { ++closes; }

    OfflineFrameRead Read(OfflineDecodedFrame& frame, std::stop_token stop) override
    {
        if (stop.stop_requested()) return OfflineFrameRead::Cancelled;
        if (index_ >= frames_) return OfflineFrameRead::EndOfStream;
        frame.bgra = generate_(index_, width_, height_, seed_);
        frame.timestamp100ns = static_cast<int64_t>(std::llround(double(index_) * 10000000.0 / fps_));
        frame.frameNumber = index_;
        frame.discontinuity = false;
        frame.sourceGeneration = 0;
        ++index_;
        return OfflineFrameRead::FrameReady;
    }

    int opens{};
    int closes{};
    std::vector<double> seeks;

private:
    Generator generate_{};
    uint32_t width_{};
    uint32_t height_{};
    uint64_t frames_{};
    double fps_{};
    uint32_t seed_{};
    uint64_t index_{};
};

// ---------------------------------------------------------------------------
// A clip read the way a host reads one: through a decoder, not from a generator.
//
// The frames come out of an ffmpeg child writing raw BGRA to its stdout, which is
// the shape the player's own decoder presents, so what run 7 measures is a screen
// recording of real video - real texture, real compression artefacts, real motion -
// rather than another pattern this test invented. That is the whole point of the
// run: every measurement before it was taken on content whose statistics the test
// chose.
//
// Two things about the spawn are not incidental. The child inherits its stdout and
// a NUL device and nothing else (InheritedHandles is src/'s rule, and it is here
// for the same reason: an inherited handle the child did not need is a handle that
// outlives its use). And Close() shuts the read end before it reaps the child,
// because the renderer stops reading at the end of the requested duration while
// ffmpeg is still producing: closing the consumer is what turns the child's next
// write into an error it can exit on, instead of leaving it blocked on a full pipe
// until something kills it.
class FfmpegSource final : public IFrameSource
{
public:
    FfmpegSource(const std::filesystem::path& executable, const std::filesystem::path& clip,
                 uint32_t width, uint32_t height, double fps)
        : executable_(executable), clip_(clip), width_(width), height_(height), fps_(fps)
    {
    }

    ~FfmpegSource() override { Close(); }

    // `seekSeconds` is a fast seek (before -i): ffmpeg starts at the keyframe at or
    // before it, so the frame the pts maths below names is only guaranteed for the
    // seek-to-zero this test makes. A caller that seeks elsewhere has to check the
    // timestamp of the first frame it reads against what it asked for.
    bool Open(const std::filesystem::path&, std::stop_token stop, double seekSeconds) override
    {
        Close();
        if (stop.stop_requested()) return false;

        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        attributes.bInheritHandle = TRUE;
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!::CreatePipe(&readEnd, &writeEnd, &attributes, 0)) {
            error_ = "the decoder's stdout pipe would not open";
            return false;
        }
        // The read end stays here and must not be inherited: a second copy of it in
        // the child is a copy that never closes, and the end-of-file this reader
        // waits for is the write side's handle count reaching zero.
        if (!::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0)) {
            ::CloseHandle(readEnd);
            ::CloseHandle(writeEnd);
            error_ = "the decoder's pipe could not be made private to this process";
            return false;
        }
        const HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                         OPEN_EXISTING, 0, nullptr);
        if (nul == INVALID_HANDLE_VALUE) {
            ::CloseHandle(readEnd);
            ::CloseHandle(writeEnd);
            error_ = "the NUL device would not open for the decoder's other two handles";
            return false;
        }

        // -nostdin so the child cannot eat this process's console input, -an -sn so
        // no audio or subtitle stream is decoded at all, and an explicit -map 0:v:0
        // so the geometry below is the geometry that comes out.
        std::wstring command =
            L"\"" + executable_.wstring() + L"\" -hide_banner -loglevel error -nostdin";
        if (seekSeconds > 0.0) command += L" -ss " + std::to_wstring(seekSeconds);
        command += L" -i \"" + clip_.wstring() +
                   L"\" -map 0:v:0 -an -sn -f rawvideo -pix_fmt bgra -";

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdInput = nul;
        startup.StartupInfo.hStdOutput = writeEnd;
        startup.StartupInfo.hStdError = nul;
        InheritedHandles inherit{startup.StartupInfo.hStdInput, startup.StartupInfo.hStdOutput,
                                 startup.StartupInfo.hStdError};
        startup.lpAttributeList = inherit.AttributeList();

        std::wstring application = executable_.wstring();
        PROCESS_INFORMATION info{};
        const BOOL started =
            inherit.Ready() &&
            ::CreateProcessW(application.c_str(), command.data(), nullptr, nullptr,
                             inherit.InheritHandles(), CREATE_NO_WINDOW | inherit.CreationFlags(),
                             nullptr, nullptr, &startup.StartupInfo, &info);
        // This process's copies of the two inherited handles have to go whatever
        // happened: the write end in particular, because while it is open here the
        // read end can never see end-of-file.
        ::CloseHandle(writeEnd);
        ::CloseHandle(nul);
        if (!started) {
            ::CloseHandle(readEnd);
            error_ = inherit.Ready() ? "the decoder would not start"
                                     : "the decoder's handle list would not build";
            return false;
        }
        ::CloseHandle(info.hThread);
        process_ = info.hProcess;
        pipe_ = readEnd;
        nextFrame_ = static_cast<uint64_t>(std::llround(seekSeconds * fps_));
        return true;
    }

    void Close() override
    {
        if (pipe_ != nullptr) {
            ::CloseHandle(pipe_);
            pipe_ = nullptr;
        }
        if (process_ != nullptr) {
            if (::WaitForSingleObject(process_, 2000) != WAIT_OBJECT_0) ::TerminateProcess(process_, 0);
            ::WaitForSingleObject(process_, 2000);
            ::CloseHandle(process_);
            process_ = nullptr;
        }
    }

    OfflineFrameRead Read(OfflineDecodedFrame& frame, std::stop_token stop) override
    {
        if (stop.stop_requested()) return OfflineFrameRead::Cancelled;
        if (pipe_ == nullptr) {
            error_ = "the decoder is not open";
            return OfflineFrameRead::Error;
        }
        // A pipe hands over whatever is in it, so a short read is normal and only
        // zero bytes with nothing read yet is the end of the clip.
        const size_t wanted = static_cast<size_t>(width_) * height_ * 4u;
        frame.bgra.assign(wanted, 0);
        size_t filled = 0;
        while (filled < wanted) {
            DWORD got = 0;
            if (!::ReadFile(pipe_, frame.bgra.data() + filled, static_cast<DWORD>(wanted - filled),
                            &got, nullptr)) {
                error_ = "the decoder's pipe failed mid-frame";
                return OfflineFrameRead::Error;
            }
            if (got == 0) {
                if (filled > 0) {
                    error_ = "the decoder stopped in the middle of a frame";
                    return OfflineFrameRead::Error;
                }
                // Zero bytes at a frame boundary is either the end of the clip or
                // the decoder giving up, and only its exit code tells those apart:
                // its own complaint went to the NUL device Open() gave it.
                const int code = ExitCode();
                if (code > 0) {
                    error_ = "the decoder exited with code " + std::to_string(code);
                    return OfflineFrameRead::Error;
                }
                return OfflineFrameRead::EndOfStream;
            }
            filled += got;
        }
        frame.timestamp100ns =
            static_cast<int64_t>(std::llround(double(nextFrame_) * 10000000.0 / fps_));
        frame.frameNumber = nextFrame_;
        frame.discontinuity = false;
        frame.sourceGeneration = 0;
        ++nextFrame_;
        return OfflineFrameRead::FrameReady;
    }

    const std::string& Error() const { return error_; }

private:
    // The child's exit code once it has finished, or -1 while it is still running.
    // It is the only signal left for "the decoder gave up": this source sends the
    // child's stderr to NUL, so the code is what stands in for its message.
    int ExitCode() const
    {
        if (process_ == nullptr) return -1;
        if (::WaitForSingleObject(process_, 500) != WAIT_OBJECT_0) return -1;
        DWORD code = 0;
        return ::GetExitCodeProcess(process_, &code) ? static_cast<int>(code) : -1;
    }

    std::filesystem::path executable_;
    std::filesystem::path clip_;
    uint32_t width_{};
    uint32_t height_{};
    double fps_{};
    HANDLE process_{};
    HANDLE pipe_{};
    uint64_t nextFrame_{};
    std::string error_;
};

// The clip's own bytes with grain drawn from (pixel, frame, seed), which is how a
// pair of passes is made to differ in noise and in nothing else: subtract them and
// the content and the network's response to it cancel exactly, whatever that
// response is. Same +/-8 bound run 5's StillNoise argues for - .08 of 1.0, 20 of
// 255, is what the runtime's disocclusion guard admits, and two independent draws
// at +/-8 differ by at most 15, so the guard is not what decides the measurement.
//
// The clamp at 0 and 255 biases this low wherever the clip sits at a rail, because
// both seeds pin to the same value there and the pixel reads as zero difference
// whatever the model did. Stated rather than corrected: correcting it would mean
// measuring a different set of pixels in the two arms.
void GrainInto(std::span<const uint8_t> content, uint64_t frameIndex, uint32_t seed,
               std::vector<uint8_t>& out)
{
    out.assign(content.begin(), content.end());
    uint32_t noise = seed ^ static_cast<uint32_t>(frameIndex * 2246822519u);
    for (size_t index = 0, count = content.size() / 4u; index < count; ++index) {
        noise = noise * 1664525u + 1013904223u;
        const int grain = static_cast<int>((noise >> 26) & 0x0Fu) - 8;
        for (size_t channel = 0; channel < 3u; ++channel) {
            const int value = static_cast<int>(content[index * 4u + channel]) + grain;
            out[index * 4u + channel] = static_cast<uint8_t>(std::clamp(value, 0, 255));
        }
    }
}

// Decoded frames, held in memory and replayed with grain added.
//
// `freeze` is the whole of what separates run 7's two subjects. True makes every
// frame carry the clip's first frame, so the content is still and the correct motion
// really is zero - which makes a zero field the truth about the source rather than a
// guess, and is run 5's measurement taken on real texture instead of on sines. False
// walks the clip, so the content moves and a zero field is wrong, which is the field
// a host with no motion estimator would have.
class RealFrameSource final : public IFrameSource
{
public:
    RealFrameSource(const std::vector<std::vector<uint8_t>>& frames, double fps, uint32_t seed,
                    bool freeze)
        : frames_(frames), fps_(fps), seed_(seed), freeze_(freeze)
    {
    }

    bool Open(const std::filesystem::path&, std::stop_token stop, double seekSeconds) override
    {
        if (stop.stop_requested()) return false;
        const int64_t index = static_cast<int64_t>(std::llround(seekSeconds * fps_));
        index_ = static_cast<uint64_t>(std::max<int64_t>(0, index));
        if (index_ > frames_.size()) index_ = frames_.size();
        return true;
    }

    void Close() override {}

    OfflineFrameRead Read(OfflineDecodedFrame& frame, std::stop_token stop) override
    {
        if (stop.stop_requested()) return OfflineFrameRead::Cancelled;
        if (frames_.empty() || index_ >= frames_.size()) return OfflineFrameRead::EndOfStream;
        GrainInto(frames_[freeze_ ? 0u : static_cast<size_t>(index_)], index_, seed_, frame.bgra);
        frame.timestamp100ns = static_cast<int64_t>(std::llround(double(index_) * 10000000.0 / fps_));
        frame.frameNumber = index_;
        frame.discontinuity = false;
        frame.sourceGeneration = 0;
        ++index_;
        return OfflineFrameRead::FrameReady;
    }

private:
    const std::vector<std::vector<uint8_t>>& frames_;
    double fps_{};
    uint32_t seed_{};
    bool freeze_{};
    uint64_t index_{};
};

double MeanLuminanceRgb(std::span<const uint8_t> bgra)
{
    if (bgra.empty()) return 0.0;
    double total = 0.0;
    const size_t pixels = bgra.size() / 4u;
    for (size_t index = 0; index < pixels; ++index) {
        const uint8_t* pixel = bgra.data() + index * 4u;
        total += 0.114 * pixel[0] + 0.587 * pixel[1] + 0.299 * pixel[2];
    }
    return total / static_cast<double>(pixels);
}

// RGB only: the alpha byte of a produced frame carries no information, so a
// difference measured over all four bytes could pass on alpha alone.
double MeanAbsoluteDifferenceRgb(std::span<const uint8_t> left, std::span<const uint8_t> right)
{
    if (left.size() != right.size() || left.empty()) return 0.0;
    double total = 0.0;
    for (size_t index = 0; index < left.size(); index += 4u) {
        total += std::abs(static_cast<int>(left[index + 0]) - static_cast<int>(right[index + 0]));
        total += std::abs(static_cast<int>(left[index + 1]) - static_cast<int>(right[index + 1]));
        total += std::abs(static_cast<int>(left[index + 2]) - static_cast<int>(right[index + 2]));
    }
    return total / static_cast<double>((left.size() / 4u) * 3u);
}

// Mean absolute horizontal neighbour difference over the RGB channels. On a
// still scene this measures exactly one thing: how much per-pixel noise
// survived. The content is a ramp that is smooth in x, so a spatial denoiser can
// already flatten it within a single frame and only accumulation across frames
// can take it lower - which is what makes this the quantity that separates the
// two paths, rather than a luminance or a whole-frame difference that both
// paths would move by a similar amount.
double HighFrequencyEnergyRgb(std::span<const uint8_t> bgra, uint32_t width, uint32_t height)
{
    if (bgra.empty() || width < 2u) return 0.0;
    double total = 0.0;
    uint64_t samples = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x + 1u < width; ++x) {
            const size_t left = (static_cast<size_t>(y) * width + x) * 4u;
            for (int channel = 0; channel < 3; ++channel) {
                total += std::abs(static_cast<int>(bgra[left + 4u + channel]) -
                                  static_cast<int>(bgra[left + channel]));
            }
            samples += 3u;
        }
    }
    return samples == 0 ? 0.0 : total / static_cast<double>(samples);
}

// Keeps the bytes of the first and last frame it was handed, the luminance of
// every frame, and the evaluator's own per-frame wall time. It never opens a
// file: what stage B judges is the frame the renderer published, not a container.
class CountingEncoder final : public IFrameEncoder
{
public:
    CountingEncoder(const AmdLmxxfEvaluator& evaluator, bool failNvencStart)
        : evaluator_(&evaluator), failNvencStart_(failNvencStart) {}

    EncodeError Start(const EncoderSpec& spec, const std::filesystem::path&) override
    {
        startKinds.push_back(spec.kind);
        attemptFrames.push_back(0);
        width_ = spec.width;
        height_ = spec.height;
        if (failNvencStart_ && spec.kind == EncoderKind::HevcNvenc) return EncodeError::StartFailed;
        active_ = true;
        return EncodeError::None;
    }

    EncodeError WriteFrame(std::span<const uint8_t> frame, std::stop_token stop) override
    {
        if (stop.stop_requested()) return EncodeError::Cancelled;
        if (!active_) return EncodeError::WriteFailed;
        if (attemptFrames.back() == 0) firstFrame.assign(frame.begin(), frame.end());
        lastFrame.assign(frame.begin(), frame.end());
        luminance.push_back(MeanLuminanceRgb(frame));
        // Per frame rather than once at the end: a temporal accumulator lowers
        // this as the history fills, so the sequence is what shows accumulation
        // happening, where a single frame could just be a lucky one.
        frameEnergy.push_back(HighFrequencyEnergyRgb(frame, width_, height_));
        submitMs.push_back(evaluator_->LastSubmitMs());
        ++attemptFrames.back();
        ++written_;
        return EncodeError::None;
    }

    EncodeError Finish(std::stop_token stop) override
    {
        active_ = false;
        return stop.stop_requested() ? EncodeError::Cancelled : EncodeError::None;
    }

    void Cancel() override { active_ = false; }

    std::vector<EncoderKind> startKinds;
    std::vector<uint64_t> attemptFrames;
    std::vector<double> luminance;
    std::vector<double> frameEnergy;
    std::vector<double> submitMs;
    std::vector<uint8_t> firstFrame;
    std::vector<uint8_t> lastFrame;
    uint64_t written_{};

private:
    const AmdLmxxfEvaluator* evaluator_{};
    bool failNvencStart_{};
    bool active_{};
    uint32_t width_{};
    uint32_t height_{};
};

std::filesystem::path ResolveRuntimeDirectory(int argc, char** argv)
{
    std::vector<std::filesystem::path> candidates;
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') candidates.emplace_back(argv[1]);
    candidates.emplace_back(DLSS5_AMD_RUNTIME_DIR);
    std::error_code cwdError;
    const std::filesystem::path cwd = std::filesystem::current_path(cwdError);
    if (!cwdError) candidates.emplace_back(cwd / "amd-neural-runtime");
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code existsError;
        if (std::filesystem::exists(candidate / "LmxxfNrRuntime.dll", existsError)) return candidate;
    }
    return candidates.front();
}

std::string DurationText(double milliseconds)
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", milliseconds);
    return buffer;
}

void ReportSubmitTimes(const CountingEncoder& encoder)
{
    if (encoder.submitMs.empty()) return;
    std::vector<double> sorted = encoder.submitMs;
    std::sort(sorted.begin(), sorted.end());
    const double first = encoder.submitMs.front();
    const double median = sorted[sorted.size() / 2];
    const double slowest = sorted.back();
    Line("  submit wall time: first " + DurationText(first) + ", median " + DurationText(median) +
         ", slowest " + DurationText(slowest) + " (the runtime's own network_ms is separate)");
}

std::string Number3(double value)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

// The mean of the last `count` entries. A per-frame series that is still filling
// has a trend at the front and a steady state at the back, and it is the steady
// state that the two paths should be compared on - one frame's residual is a
// sample of noise, not a measurement of it.
double TailMean(const std::vector<double>& values, size_t count)
{
    if (values.empty()) return 0.0;
    const size_t take = std::min(count, values.size());
    double total = 0.0;
    for (size_t index = values.size() - take; index < values.size(); ++index)
        total += values[index];
    return total / static_cast<double>(take);
}

// The head and the tail of a per-frame series, so a run's log shows whether the
// quantity settled instead of only reporting that it was averaged.
std::string EnergyPreview(const std::vector<double>& values, size_t head, size_t tail)
{
    if (values.empty()) return "(no frames)";
    std::string out;
    const size_t headEnd = std::min(head, values.size());
    for (size_t index = 0; index < headEnd; ++index) {
        if (!out.empty()) out += ", ";
        out += Number3(values[index]);
    }
    if (values.size() > headEnd) {
        out += " ... ";
        const size_t tailBegin = values.size() > tail ? values.size() - tail : headEnd;
        for (size_t index = tailBegin; index < values.size(); ++index) {
            if (index != tailBegin) out += ", ";
            out += Number3(values[index]);
        }
    }
    return out;
}

bool WriteFramePng(const std::filesystem::path& path, std::span<const uint8_t> bgra)
{
    std::vector<uint8_t> bgr(static_cast<size_t>(kWidth) * kHeight * 3u);
    for (size_t index = 0; index < static_cast<size_t>(kWidth) * kHeight; ++index) {
        bgr[index * 3u + 0] = bgra[index * 4u + 0];
        bgr[index * 3u + 1] = bgra[index * 4u + 1];
        bgr[index * 3u + 2] = bgra[index * 4u + 2];
    }
    return SUCCEEDED(compare_image::SavePngBgr(path, bgr.data(), kWidth, kHeight, kWidth * 3u));
}

// A device and queue the evaluator did NOT create, so run 4 can exercise the
// borrowed-device path - the one a stage C worker uses, where the worker's own
// renderer already owns both. This repeats the evaluator's own adapter choice on
// purpose: the test is not allowed to hand the runtime a GPU the runtime refuses,
// or the run would be measuring the wrong failure.
bool MakeBorrowedDevice(Microsoft::WRL::ComPtr<ID3D12Device>& device,
                        Microsoft::WRL::ComPtr<ID3D12CommandQueue>& queue)
{
    constexpr uint32_t kAmd = 0x1002u; // the only vendor with gfx1200/gfx1201 modules
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> fallbackAdapter;
    Microsoft::WRL::ComPtr<ID3D12Device> fallbackDevice;
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) continue;
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        Microsoft::WRL::ComPtr<ID3D12Device> candidate;
        if (FAILED(::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                       IID_PPV_ARGS(&candidate))))
            continue;
        if (!fallbackDevice) {
            fallbackAdapter = adapter;
            fallbackDevice = candidate;
        }
        if (description.VendorId == kAmd) {
            fallbackAdapter = adapter;
            fallbackDevice = candidate;
            break;
        }
    }
    if (!fallbackDevice) return false;

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(fallbackDevice->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))))
        return false;
    device = fallbackDevice;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    ::SetConsoleOutputCP(CP_UTF8);
    if (const int skip = gpu_test_gate::SkipWithoutGpu()) return skip;

    Line("AmdLmxxfEvaluatorSmoke - stage B: the offline renderer driving the AMD evaluator.");

    const std::filesystem::path runtimeDirectory = ResolveRuntimeDirectory(argc, argv);
    Line("  runtime dir: " + Narrow(runtimeDirectory));
    std::error_code existsError;
    if (!std::filesystem::exists(runtimeDirectory / "LmxxfNrRuntime.dll", existsError)) {
        Line("SKIPPED: no amd-neural-runtime/LmxxfNrRuntime.dll beside this checkout.");
        Line("Stage the AMD neural runtime pack there, or pass its directory as the first argument.");
        return kExitSkipped;
    }

    uint32_t longRunFrames = 300;
    if (argc > 2 && argv[2] != nullptr && argv[2][0] != '\0') {
        const long value = std::strtol(argv[2], nullptr, 10);
        if (value > 0) longRunFrames = static_cast<uint32_t>(value);
    }
    std::filesystem::path outputDirectory;
    if (argc > 3 && argv[3] != nullptr && argv[3][0] != '\0') outputDirectory = argv[3];
    else {
        std::error_code cwdError;
        outputDirectory = std::filesystem::current_path(cwdError);
    }
    Line("  long run frames: " + std::to_string(longRunFrames) + " (" +
         std::to_string(double(longRunFrames) / kFps) + " s at " + std::to_string(kFps) + " fps)");
    Line("  output directory: " + Narrow(outputDirectory));

    AmdLmxxfEvaluatorConfig config;
    config.runtimeDirectory = runtimeDirectory;
    config.log = [](std::string_view text) { Line(std::string("    ") + std::string(text)); };
    AmdLmxxfEvaluator evaluator(config);

    // ---------------------------------------------------------------------
    // Run 1: a whole sequence. This is the stage B exit criterion - a complete
    // neural pass over 10 s of 1080p30, frame for frame.
    // ---------------------------------------------------------------------
    Step("run 1  whole sequence, " + std::to_string(longRunFrames) + " frames");
    {
        GeneratedSource source(Pattern, kWidth, kHeight, longRunFrames, kFps, 0x5eedu);
        CountingEncoder encoder(evaluator, false);
        OfflineNeuralRenderer job(source, evaluator, encoder, [] { return std::string{}; });

        NeuralRenderRequest request;
        request.sourcePath = outputDirectory / L"synthetic-1080p30.mkv";
        request.stagingVideoPath = outputDirectory / L"amd-lmxxf-smoke.partial.mkv";
        request.width = kWidth;
        request.height = kHeight;
        request.fps = kFps;
        request.durationSeconds = double(longRunFrames) / kFps;
        request.jobId = 1;
        request.prerollFrames = 0;
        // Stage B boundary: the NVIDIA evidence gates are stage C's problem. See
        // the file header.
        request.requireNeural = false;

        const auto started = std::chrono::steady_clock::now();
        const NeuralRenderResult result = job.Run(request);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        Line("  adapter: " + evaluator.AdapterName());
        Line("  runtime status (bring-up): " + evaluator.RuntimeStatus());
        Line("  runtime status (live): " + evaluator.LiveRuntimeStatus());
        Line("  result: ok=" + std::string(result.ok ? "yes" : "no") + " cancelled=" +
             std::string(result.cancelled ? "yes" : "no") + " failure=" +
             std::string(NeuralRenderFailureName(result.failure)) +
             " detail=" + Narrow(result.detail));
        Line("  frames=" + std::to_string(result.frameCount) + " nativeEvaluations=" +
             std::to_string(result.nativeEvaluations) + " historyResets=" +
             std::to_string(result.historyResets) + " verifiedNeuralFrames=" +
             std::to_string(result.verifiedNeuralFrames) + " neural=" +
             std::string(result.neural ? "true" : "false"));
        Line("  wall time: " + DurationText(seconds * 1000.0) + ", " +
             DurationText(seconds * 1000.0 / std::max<uint64_t>(1, result.frameCount)) + "/frame");
        ReportSubmitTimes(encoder);

        Check(result.ok && !result.cancelled, "the whole sequence rendered");
        Check(result.failure == NeuralRenderFailure::None,
              "no failure was classified: " + std::string(NeuralRenderFailureName(result.failure)));
        Check(result.frameCount == longRunFrames,
              "every frame of the sequence was captured (" + std::to_string(result.frameCount) + "/" +
                  std::to_string(longRunFrames) + ")");
        Check(result.nativeEvaluations == longRunFrames,
              "the runtime evaluated once per captured frame (" +
                  std::to_string(result.nativeEvaluations) + "/" + std::to_string(longRunFrames) + ")");
        Check(encoder.written_ == longRunFrames,
              "the encoder was handed every frame (" + std::to_string(encoder.written_) + ")");
        Check(encoder.startKinds.size() == 1 && encoder.startKinds.front() == EncoderKind::HevcNvenc,
              "one encoder attempt was made, for NVENC");
        Check(result.historyResets >= 1, "at least the first frame reset the temporal history");

        if (encoder.firstFrame.size() == static_cast<size_t>(kWidth) * kHeight * 4u &&
            !encoder.lastFrame.empty()) {
            const std::vector<uint8_t> firstInput = Pattern(0, kWidth, kHeight, 0x5eedu);
            const std::vector<uint8_t> lastInput =
                Pattern(longRunFrames - 1u, kWidth, kHeight, 0x5eedu);
            const double firstLuminance = encoder.luminance.front();
            const double lastLuminance = encoder.luminance.back();
            const double firstDifference = MeanAbsoluteDifferenceRgb(encoder.firstFrame, firstInput);
            const double lastDifference = MeanAbsoluteDifferenceRgb(encoder.lastFrame, lastInput);
            Line("  frame 0 : luminance " + std::to_string(firstLuminance) + ", mean|produced-input| " +
                 std::to_string(firstDifference));
            Line("  frame " + std::to_string(longRunFrames - 1u) + " : luminance " +
                 std::to_string(lastLuminance) + ", mean|produced-input| " +
                 std::to_string(lastDifference));
            Check(firstLuminance > 1.0, "the first produced frame is not a black frame");
            Check(firstDifference > 1.0, "the first produced frame is not the input frame");
            Check(lastLuminance > 1.0, "the last produced frame is not a black frame");
            Check(lastDifference > 1.0, "the last produced frame is not the input frame");
            Line("  last output resource was " +
                 std::string(evaluator.LastOutputWasPrivate() ? "job.private_output" : "the host colour texture") +
                 ", not the texture that was uploaded into");
            // The strongest stage B claim: the frame handed to the encoder came
            // out of the runtime's own produced texture. If the readback had
            // landed on the host colour texture, everything else here would still
            // pass while proving nothing.
            Check(evaluator.LastOutputWasPrivate(),
                  "the produced frame was read back from job.private_output, not the upload texture");

            const std::filesystem::path firstPng = outputDirectory / L"amd-lmxxf-evaluator-first.png";
            const std::filesystem::path lastPng = outputDirectory / L"amd-lmxxf-evaluator-last.png";
            if (WriteFramePng(firstPng, encoder.firstFrame)) Line("  wrote " + Narrow(firstPng));
            if (WriteFramePng(lastPng, encoder.lastFrame)) Line("  wrote " + Narrow(lastPng));
        } else {
            Check(false, "the encoder kept the first and last frames for measurement");
        }

        // A whole-source job opens once for the range start and reopens at the
        // preroll, so the source contract was exercised on this run too.
        Check(source.opens >= 1 && source.closes >= 1, "the source was opened and closed");
    }

    // ---------------------------------------------------------------------
    // Run 2: a bounded range. This is the seek: the job restarts the source at
    // the preroll, calls ResetTemporal on the evaluator, evaluates the preroll
    // frames without capturing them, and captures only the range.
    // ---------------------------------------------------------------------
    Step("run 2  bounded range (seek + preroll + ResetTemporal)");
    {
        // The nominal range is written for the default 300-frame run. It needs a
        // source long enough to hold the preroll plus the captured range, which a
        // short diagnostic run cannot provide, so run 2 yields to run 1 there
        // rather than asking for a range the source never had.
        constexpr uint64_t kNominalRangeStartFrame = 30;
        constexpr uint64_t kNominalRangeFrames = 18;
        constexpr uint32_t kPreroll = 4;
        const uint64_t rangeFrames =
            std::min<uint64_t>(kNominalRangeFrames,
                               longRunFrames > kPreroll + 2u ? longRunFrames - kPreroll - 2u : 0u);
        if (rangeFrames < 2u) {
            Line("  skipped: a " + std::to_string(longRunFrames) +
                 "-frame source cannot hold a " + std::to_string(kPreroll) +
                 "-frame preroll plus a captured range; run at the default length to exercise the"
                 " seek path.");
        } else {
            const uint64_t rangeStartFrame =
                std::min<uint64_t>(kNominalRangeStartFrame,
                                   static_cast<uint64_t>(longRunFrames) - rangeFrames);
            GeneratedSource source(Pattern, kWidth, kHeight, longRunFrames, kFps, 0x5eedu);
            CountingEncoder encoder(evaluator, false);
            OfflineNeuralRenderer job(source, evaluator, encoder, [] { return std::string{}; });

            const int64_t frameDuration = static_cast<int64_t>(std::llround(10000000.0 / kFps));
            NeuralRenderRequest request;
            request.sourcePath = outputDirectory / L"synthetic-1080p30.mkv";
            request.stagingVideoPath = outputDirectory / L"amd-lmxxf-smoke-range.partial.mkv";
            request.width = kWidth;
            request.height = kHeight;
            request.fps = kFps;
            request.durationSeconds = double(longRunFrames) / kFps;
            request.jobId = 2;
            request.range.start100ns = static_cast<int64_t>(rangeStartFrame) * frameDuration;
            request.range.end100ns =
                static_cast<int64_t>(rangeStartFrame + rangeFrames) * frameDuration;
            request.prerollFrames = kPreroll;
            request.requireNeural = false;

            Line("  range: frames " + std::to_string(rangeStartFrame) + ".." +
                 std::to_string(rangeStartFrame + rangeFrames) + " of " +
                 std::to_string(longRunFrames) + ", preroll " + std::to_string(kPreroll));

            const NeuralRenderResult result = job.Run(request);
            Line("  result: ok=" + std::string(result.ok ? "yes" : "no") + " failure=" +
                 std::string(NeuralRenderFailureName(result.failure)) +
                 " detail=" + Narrow(result.detail));
            Line("  frames=" + std::to_string(result.frameCount) + " nativeEvaluations=" +
                 std::to_string(result.nativeEvaluations) + " historyResets=" +
                 std::to_string(result.historyResets));
            Line("  source opens=" + std::to_string(source.opens) + " seeks=" +
                 std::to_string(source.seeks.size()));
            ReportSubmitTimes(encoder);

            Check(result.ok, "the range rendered");
            Check(result.frameCount == rangeFrames,
                  "exactly the range was captured (" + std::to_string(result.frameCount) + "/" +
                      std::to_string(rangeFrames) + ")");
            Check(result.nativeEvaluations == rangeFrames,
                  "the runtime evaluated once per captured range frame");
            Check(source.opens >= 2, "the source was reopened at the preroll, which is the seek");
            Check(source.closes >= 2, "the source was closed before each reopen");
            Check(result.historyResets >= 1, "the preroll frame reset the temporal history");
        }
    }

    // ---------------------------------------------------------------------
    // Run 3: the encoder fails on the NVENC attempt, so the renderer discards
    // the pass and renders the same frames again through software encoding. The
    // runtime is driven a second time over the same sequence without a session
    // rebuild, which is the retry path.
    // ---------------------------------------------------------------------
    Step("run 3  encoder-failure retry over the same evaluator");
    {
        GeneratedSource source(Pattern, kWidth, kHeight, kShortRunFrames, kFps, 0x5eedu);
        CountingEncoder encoder(evaluator, true);
        OfflineNeuralRenderer job(source, evaluator, encoder, [] { return std::string{}; });

        NeuralRenderRequest request;
        request.sourcePath = outputDirectory / L"synthetic-short-1080p30.mkv";
        request.stagingVideoPath = outputDirectory / L"amd-lmxxf-smoke-retry.partial.mkv";
        request.width = kWidth;
        request.height = kHeight;
        request.fps = kFps;
        request.durationSeconds = double(kShortRunFrames) / kFps;
        request.jobId = 3;
        request.prerollFrames = 0;
        request.requireNeural = false;

        const NeuralRenderResult result = job.Run(request);
        Line("  result: ok=" + std::string(result.ok ? "yes" : "no") + " failure=" +
             std::string(NeuralRenderFailureName(result.failure)) +
             " encoder=" + std::to_string(static_cast<int>(result.encoder)) +
             " frames=" + std::to_string(result.frameCount));
        Line("  source opens=" + std::to_string(source.opens));
        Line("  encoder attempts=" + std::to_string(encoder.startKinds.size()));

        Check(result.ok, "the retry rendered the sequence");
        Check(encoder.startKinds.size() == 2,
              "two encoder attempts were made (" + std::to_string(encoder.startKinds.size()) + ")");
        if (encoder.startKinds.size() == 2) {
            Check(encoder.startKinds[0] == EncoderKind::HevcNvenc &&
                      encoder.startKinds[1] == EncoderKind::H264Software,
                  "the retry moved NVENC -> software, which is the policy under test");
        }
        Check(encoder.attemptFrames.size() == 2 && encoder.attemptFrames.back() == kShortRunFrames,
              "the second attempt wrote every frame of the sequence");
        Check(result.frameCount == kShortRunFrames,
              "the retried pass captured every frame (" + std::to_string(result.frameCount) + ")");
        Check(result.nativeEvaluations == kShortRunFrames,
              "the runtime evaluated once per captured frame on the retry pass");
        Check(source.opens >= 2, "the retry restarted the source, which is the same seek path");
    }

    // ---------------------------------------------------------------------
    // Run 4: a borrowed device. A stage C worker owns its device and queue and
    // hands them to the evaluator, so the evaluator has to work when it did not
    // create them - and it still has to name the adapter and measure the video
    // memory the runtime is using. That measurement needs IDXGIAdapter3, which is
    // the one thing the borrowed path cannot get from its own enumeration, and
    // getting it wrong shows up as a silent zero rather than as an error.
    // ---------------------------------------------------------------------
    Step("run 4  borrowed device and queue");
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        if (!MakeBorrowedDevice(device, queue)) {
            Check(false, "a D3D12 device could be created for the borrowed-device run");
        } else {
            constexpr uint32_t kBorrowedRunFrames = 3;
            AmdLmxxfEvaluatorConfig borrowedConfig;
            borrowedConfig.runtimeDirectory = runtimeDirectory;
            borrowedConfig.device = device.Get();
            borrowedConfig.queue = queue.Get();
            borrowedConfig.log = [](std::string_view text) {
                Line(std::string("    ") + std::string(text));
            };
            AmdLmxxfEvaluator borrowed(borrowedConfig);

            GeneratedSource source(Pattern, kWidth, kHeight, kBorrowedRunFrames, kFps, 0x5eedu);
            CountingEncoder encoder(borrowed, false);
            OfflineNeuralRenderer job(source, borrowed, encoder, [] { return std::string{}; });

            NeuralRenderRequest request;
            request.sourcePath = outputDirectory / L"synthetic-borrowed-1080p30.mkv";
            request.stagingVideoPath = outputDirectory / L"amd-lmxxf-smoke-borrowed.partial.mkv";
            request.width = kWidth;
            request.height = kHeight;
            request.fps = kFps;
            request.durationSeconds = double(kBorrowedRunFrames) / kFps;
            request.jobId = 4;
            request.prerollFrames = 0;
            request.requireNeural = false;

            const NeuralRenderResult result = job.Run(request);
            Line("  adapter: " + borrowed.AdapterName());
            Line("  runtime status (live): " + borrowed.LiveRuntimeStatus());
            Line("  video memory: " + borrowed.LocalVideoMemoryNote());
            Line("  result: ok=" + std::string(result.ok ? "yes" : "no") + " failure=" +
                 std::string(NeuralRenderFailureName(result.failure)) +
                 " detail=" + Narrow(result.detail));

            Check(result.ok, "the borrowed device rendered the sequence");
            Check(result.frameCount == kBorrowedRunFrames,
                  "every frame of the borrowed run was captured (" +
                      std::to_string(result.frameCount) + "/" +
                      std::to_string(kBorrowedRunFrames) + ")");
            Check(result.nativeEvaluations == kBorrowedRunFrames,
                  "the runtime evaluated once per frame on the borrowed device");
            Check(!borrowed.AdapterName().empty(),
                  "the adapter behind a borrowed device can still be named");
            Check(borrowed.PeakLocalVideoMemoryMiB() > 0,
                  "the video memory of a borrowed device can still be measured (" +
                      borrowed.LocalVideoMemoryNote() + ")");
            Check(borrowed.LastOutputWasPrivate(),
                  "the borrowed device produced into job.private_output too");
        }
    }

    // ---------------------------------------------------------------------
    // Run 5: the temporal model, on the one scene where it can be measured
    // without a motion estimator.
    //
    // A still frame plus noise drawn independently each frame is the cleanest
    // test an accumulator can be given: the content is frozen, the correct
    // motion is exactly zero, the correct depth is a constant, and every unit of
    // frame-to-frame variation is noise the model is supposed to average away.
    // The guides are therefore analytic rather than estimated, which is the
    // whole point of measuring here before paying for a flow field.
    //
    // The runtime only engages the model under the recording-lease lifecycle, so
    // this run also exercises the second call order end to end - the one where
    // both lists are recorded before the first submission and the job is
    // released by invalidation and collection rather than by Retire.
    // ---------------------------------------------------------------------
    Step("run 5  temporal history on a still, noisy sequence");
    {
        constexpr uint64_t kTemporalRunFrames = 24;
        constexpr uint32_t kTemporalSeed = 0x7e11u;
        // The accumulator's own settle time is not known in advance, so the two
        // paths are compared on the tail of the run rather than on one frame -
        // and the log prints the head as well, so "did it settle" is readable
        // instead of assumed.
        constexpr size_t kTailFrames = 8;
        double spatialEnergy = 0.0;
        double temporalEnergy = 0.0;
        std::vector<uint8_t> spatialFrame;

        {
            Line("  baseline: the spatial path, no guides and no lease");
            AmdLmxxfEvaluator spatial(config);
            GeneratedSource source(StillNoise, kWidth, kHeight, kTemporalRunFrames, kFps,
                                   kTemporalSeed);
            CountingEncoder encoder(spatial, false);
            OfflineNeuralRenderer job(source, spatial, encoder, [] { return std::string{}; });

            NeuralRenderRequest request;
            request.sourcePath = outputDirectory / L"synthetic-still-1080p30.mkv";
            request.stagingVideoPath =
                outputDirectory / L"amd-lmxxf-smoke-still-spatial.partial.mkv";
            request.width = kWidth;
            request.height = kHeight;
            request.fps = kFps;
            request.durationSeconds = double(kTemporalRunFrames) / kFps;
            request.jobId = 5;
            request.prerollFrames = 0;
            request.requireNeural = false;

            const NeuralRenderResult result = job.Run(request);
            Line("    result: ok=" + std::string(result.ok ? "yes" : "no") + " failure=" +
                 std::string(NeuralRenderFailureName(result.failure)) + " frames=" +
                 std::to_string(result.frameCount));

            Check(result.ok, "the spatial baseline rendered");
            Check(encoder.lastFrame.size() == static_cast<size_t>(kWidth) * kHeight * 4u,
                  "the baseline produced a full frame to measure");
            Check(spatial.HistoryStatus().empty(),
                  "an evaluator with no lease does not report a history state at all");
            Line("    residual energy by frame: " + EnergyPreview(encoder.frameEnergy, 3, 3));
            spatialEnergy = TailMean(encoder.frameEnergy, kTailFrames);
            Line("    residual energy, mean of the last " + std::to_string(kTailFrames) +
                 " frames: " + Number3(spatialEnergy));
            spatialFrame = encoder.lastFrame;
        }

        {
            Line("  subject: the lease path, told the scene is still");
            AmdLmxxfEvaluatorConfig temporalConfig = config;
            temporalConfig.temporalHistory = true;
            // Zero motion and one depth plane are the truth about this source, so
            // there is nothing here that could be an estimator's mistake.
            temporalConfig.motionPixelsPerFrameX = 0.0f;
            temporalConfig.motionPixelsPerFrameY = 0.0f;
            temporalConfig.depthValue = 1.0f;
            AmdLmxxfEvaluator temporal(temporalConfig);
            GeneratedSource source(StillNoise, kWidth, kHeight, kTemporalRunFrames, kFps,
                                   kTemporalSeed);
            CountingEncoder encoder(temporal, false);
            OfflineNeuralRenderer job(source, temporal, encoder, [] { return std::string{}; });

            NeuralRenderRequest request;
            request.sourcePath = outputDirectory / L"synthetic-still-1080p30.mkv";
            request.stagingVideoPath =
                outputDirectory / L"amd-lmxxf-smoke-still-temporal.partial.mkv";
            request.width = kWidth;
            request.height = kHeight;
            request.fps = kFps;
            request.durationSeconds = double(kTemporalRunFrames) / kFps;
            request.jobId = 6;
            request.prerollFrames = 0;
            request.requireNeural = false;

            const NeuralRenderResult result = job.Run(request);
            Line("    result: ok=" + std::string(result.ok ? "yes" : "no") + " failure=" +
                 std::string(NeuralRenderFailureName(result.failure)) + " frames=" +
                 std::to_string(result.frameCount));
            Line("    lease frames through this evaluator: " +
                 std::to_string(temporal.TemporalFrames()));
            Line("    history: " + temporal.HistoryStatus());
            Line("    runtime status (live): " + temporal.LiveRuntimeStatus());

            Check(result.ok, "the lease path rendered the sequence");
            Check(result.frameCount == kTemporalRunFrames,
                  "every frame of the lease run was captured (" +
                      std::to_string(result.frameCount) + "/" +
                      std::to_string(kTemporalRunFrames) + ")");
            Check(result.nativeEvaluations == kTemporalRunFrames,
                  "the runtime evaluated once per lease frame");
            Check(temporal.TemporalFrames() == kTemporalRunFrames,
                  "every frame went through the lease path rather than the default one");
            Check(encoder.lastFrame.size() == static_cast<size_t>(kWidth) * kHeight * 4u,
                  "the lease path produced a full frame to measure");

            const std::string historyToken = temporal.HistoryStatus();
            // "priming" is the first frame of a run, which has nothing to fuse
            // with; "active" is the model actually accumulating; "ready" is
            // PrepareHistory having accepted the guides a moment before End
            // decides whether the chain continues. All three mean the gates
            // opened. "off" means one of them did not and the frame rendered
            // spatially - and the status line is the only place that is ever
            // said, so this token is what the run is really asserting. It is
            // checked as a non-empty string as well as a non-"off" one, because
            // an empty token is a missing reading and not a good one.
            Check(!historyToken.empty() && historyToken != "off",
                  "the temporal model engaged on a still scene with analytic guides (got '" +
                      historyToken + "')");

            temporalEnergy = TailMean(encoder.frameEnergy, kTailFrames);
            Line("    residual energy by frame: " + EnergyPreview(encoder.frameEnergy, 3, 3));
            Line("    residual energy, mean of the last " + std::to_string(kTailFrames) +
                 " frames: " + Number3(temporalEnergy));
            // The two outputs differing at all is what proves the history is part
            // of the arithmetic rather than merely enabled: the source, the
            // geometry and the network are identical between the two runs, so
            // only the accumulation can account for a difference.
            const double divergence = MeanAbsoluteDifferenceRgb(spatialFrame, encoder.lastFrame);
            Line("    mean absolute difference against the spatial output: " + Number3(divergence));
            Check(!spatialFrame.empty() && divergence > 0.0,
                  "the temporal path published a different frame than the spatial one, so the "
                  "history is part of the computation");

            if (temporalEnergy > 0.0 && spatialEnergy > 0.0) {
                const double ratio = temporalEnergy / spatialEnergy;
                Line("    ratio temporal/spatial: " + Number3(ratio));
                // How much the network's own logit decided to trust the history.
                // A first-order blend v = (1-w)x + w*v_prev settles at a noise
                // variance of (1-w)/(1+w), so the measured ratio inverts to the
                // weight behind it. It is the number that explains the ratio:
                // this runtime takes no host blend strength, so the size of the
                // benefit is the model's decision and not a dial.
                const double impliedWeight = (1.0 - ratio * ratio) / (1.0 + ratio * ratio);
                Line("    implied history weight: " + Number3(impliedWeight) +
                     " (the blend is the network's learned logit, not a host setting; the same "
                     "logit on real content is what decides the benefit)");
                // Loose on purpose. What is under test here is that accumulation
                // does not make the picture worse, not a size of benefit - the
                // implied weight above is why the size is not this host's to pick.
                Check(ratio < 1.05,
                      "temporal accumulation did not leave more residual noise than the spatial "
                      "path (" + Number3(ratio) + " of it)");
            } else {
                Check(false, "both runs produced a measurable residual to compare");
            }
        }
    }

    // ---------------------------------------------------------------------
    // Run 6: guides read from FILES, on moving content.
    //
    // Run 5 told the model the truth about a still scene, from two constants.
    // That answers "does the temporal model accumulate" and nothing else: a still
    // scene cannot separate a field the runtime samples from a texture it never
    // looks at, because zero is the right answer either way.
    //
    // So this run feeds the fields a video host would actually compute - one
    // Portable Float Map per source frame, on the analysis grid, in the layout
    // src/GuideFiles.h defines and tools/benchmark's `--guide-dump` writes - and
    // renders the same panning clip six times over three arms:
    //
    //   A  no lease, no guides   the spatial reference
    //   B  lease, the true field +8 px/frame, which is what this content does
    //   C  lease, a zero field   the same files and the same call order, one
    //                            float changed
    //
    // Each arm is rendered twice, with two grain seeds and nothing else
    // different. That pair is what makes a noise measurement possible at all, and
    // the first version of this run is the reason it is written this way: scored
    // against the grain-free clip, all three arms came out at about 8 while the
    // grain's own mean magnitude is 4, so the arm that scored best was the one
    // told the wrong motion and an arm that did nothing would have beaten all of
    // them. That number was measuring the network's reshaping of the sines. Two
    // renders of one arm differ in nothing but the noise they were fed, so their
    // difference cancels the content and the network's bias on it exactly and
    // leaves the noise that survived.
    //
    // B against C is the discrimination this run exists for, and it does not need
    // the estimator: one float of input separates them, so anything that
    // separates their output came through that float. That is what proves a
    // file's field reaches the network rather than merely being declared to it.
    // ---------------------------------------------------------------------
    Step("run 6  guides from files on a panning sequence");
    {
        constexpr uint64_t kPanRunFrames = 20;
        const std::filesystem::path trueMotionDirectory =
            outputDirectory / L"amd-lmxxf-pan-guides-mv";
        const std::filesystem::path zeroMotionDirectory =
            outputDirectory / L"amd-lmxxf-pan-guides-mv-zero";
        const std::filesystem::path depthDirectory = outputDirectory / L"amd-lmxxf-pan-guides-depth";

        // 160x90 is the analysis grid TemporalGuideGenerator produces for a
        // 1080p30 source, so these are the shape and the size a dump of the
        // renderer's own estimator would carry. The resample onto the evaluator's
        // full-frame texture is therefore exercised rather than bypassed.
        constexpr uint32_t kGridW = 160;
        constexpr uint32_t kGridH = 90;
        auto writeGuides = [&](const std::filesystem::path& directory, float motionX) {
            for (uint64_t frame = 0; frame < kPanRunFrames; ++frame) {
                guide_files::Plane motion{kGridW, kGridH, 3u,
                                          std::vector<float>(size_t(kGridW) * kGridH * 3u)};
                for (size_t cell = 0; cell < size_t(kGridW) * kGridH; ++cell) {
                    motion.values[cell * 3u + 0u] = motionX;
                    motion.values[cell * 3u + 1u] = 0.0f;
                    motion.values[cell * 3u + 2u] = 0.0f;
                }
                const guide_files::Plane depth{kGridW, kGridH, 1u,
                                               std::vector<float>(size_t(kGridW) * kGridH, 1.0f)};
                if (!guide_files::WritePfmFile(guide_files::FramePath(directory, frame), motion) ||
                    !guide_files::WritePfmFile(guide_files::FramePath(depthDirectory, frame), depth))
                    return false;
            }
            return true;
        };

        Line("  motion files: " + Narrow(trueMotionDirectory) + " (the true field, " +
             std::to_string(kPanPixelsPerFrame) + " px/frame) and " +
             Narrow(zeroMotionDirectory) + " (a zero field), " +
             std::to_string(kPanRunFrames) + " frames each");
        Line("  depth files: " + Narrow(depthDirectory) + " (one plane, all 1.0, shared)");
        Check(writeGuides(trueMotionDirectory, float(kPanPixelsPerFrame)) &&
                  writeGuides(zeroMotionDirectory, 0.0f),
              "both guide series were written");

        // The clip with no grain. Kept for one reported number and deliberately
        // not used as the reference a noise measurement is taken against - see the
        // run's header for what went wrong when it was.
        const std::vector<uint8_t> truth = PanningContent(kPanRunFrames - 1u, kWidth, kHeight);

        // Two grain seeds. Every arm is rendered once with each, which is what
        // turns a frame into a noise measurement: the content, the geometry and
        // the guide files are byte-identical between the pair, so their
        // difference cancels the content and the network's bias on it exactly,
        // whatever that bias is, and leaves the noise the model failed to remove.
        constexpr uint32_t kPanSeeds[2] = {0x9a13u, 0x4c7du};

        struct ArmOutcome {
            bool ok = false;
            uint64_t frames = 0;
            uint64_t nativeEvaluations = 0;
            uint64_t guidesFromFiles = 0;
            uint64_t leaseFrames = 0;
            std::string history;
        };

        // One arm's render. `temporal` picks the job model and `motionDirectory`
        // picks the field file; neither, together, is the plain spatial path.
        auto render = [&](bool temporal, const std::filesystem::path& motionDirectory, uint32_t seed,
                          const wchar_t* stem, uint64_t jobId,
                          std::vector<uint8_t>& produced) -> ArmOutcome {
            AmdLmxxfEvaluatorConfig armConfig = config;
            if (temporal) {
                armConfig.temporalHistory = true;
                armConfig.depthGuideDirectory = depthDirectory;
                armConfig.motionGuideDirectory = motionDirectory;
                // motionPixelsPerFrameX/Y are deliberately left at zero. With a
                // directory set the constants are only the texture's initial
                // contents and a file replaces them before the runtime sees the
                // first frame, so B and C differing from each other is also proof
                // that the FILE is what the runtime read.
            }
            AmdLmxxfEvaluator arm(armConfig);
            GeneratedSource source(PanningNoise, kWidth, kHeight, kPanRunFrames, kFps, seed);
            CountingEncoder encoder(arm, false);
            OfflineNeuralRenderer job(source, arm, encoder, [] { return std::string{}; });

            NeuralRenderRequest request;
            request.sourcePath = outputDirectory / L"synthetic-panning-1080p30.mkv";
            request.stagingVideoPath = outputDirectory / (std::wstring(stem) + L".partial.mkv");
            request.width = kWidth;
            request.height = kHeight;
            request.fps = kFps;
            request.durationSeconds = double(kPanRunFrames) / kFps;
            request.jobId = jobId;
            request.prerollFrames = 0;
            request.requireNeural = false;

            const NeuralRenderResult result = job.Run(request);
            ArmOutcome outcome;
            outcome.ok = result.ok;
            outcome.frames = result.frameCount;
            outcome.nativeEvaluations = result.nativeEvaluations;
            outcome.guidesFromFiles = arm.GuideFrames();
            outcome.leaseFrames = arm.TemporalFrames();
            outcome.history = arm.HistoryStatus();
            Line("    " + Narrow(std::wstring(stem)) + ": ok=" +
                 std::string(result.ok ? "yes" : "no") + " failure=" +
                 std::string(NeuralRenderFailureName(result.failure)) + " frames=" +
                 std::to_string(outcome.frames) + " guidesFromFiles=" +
                 std::to_string(outcome.guidesFromFiles) + " leaseFrames=" +
                 std::to_string(outcome.leaseFrames) + " history=" +
                 (outcome.history.empty() ? std::string("(none)") : outcome.history));
            if (!result.ok) Line("      detail: " + Narrow(result.detail));
            produced = std::move(encoder.lastFrame);
            return outcome;
        };

        std::vector<uint8_t> spatialFrame[2];
        std::vector<uint8_t> trueFrame[2];
        std::vector<uint8_t> zeroFrame[2];

        // A: the spatial reference, twice.
        for (int pass = 0; pass < 2; ++pass) {
            const ArmOutcome outcome = render(false, {}, kPanSeeds[pass],
                                              pass == 0 ? L"amd-lmxxf-pan-a-spatial-s1"
                                                        : L"amd-lmxxf-pan-a-spatial-s2",
                                              7, spatialFrame[pass]);
            Check(outcome.ok && outcome.frames == kPanRunFrames,
                  "the spatial reference rendered the panning clip");
            Check(outcome.nativeEvaluations == kPanRunFrames,
                  "the runtime evaluated once per frame in the reference");
            Check(outcome.guidesFromFiles == 0 && outcome.leaseFrames == 0,
                  "the spatial reference read no guide files and ran no lease frames");
        }

        // B: the lease path, told the field the content actually moves by.
        for (int pass = 0; pass < 2; ++pass) {
            const ArmOutcome outcome =
                render(true, trueMotionDirectory, kPanSeeds[pass],
                       pass == 0 ? L"amd-lmxxf-pan-b-true-motion-s1"
                                 : L"amd-lmxxf-pan-b-true-motion-s2",
                       8, trueFrame[pass]);
            Check(outcome.ok && outcome.frames == kPanRunFrames,
                  "the file-guided lease path rendered the panning clip");
            Check(outcome.guidesFromFiles == kPanRunFrames,
                  "every frame's guides came from the files rather than from the constant contents "
                  "the texture was created with (" + std::to_string(outcome.guidesFromFiles) + "/" +
                      std::to_string(kPanRunFrames) + ")");
            Check(outcome.leaseFrames == kPanRunFrames, "every frame went through the lease path");
            Check(!outcome.history.empty() && outcome.history != "off",
                  "the temporal model engaged on file guides (got '" + outcome.history + "')");
        }

        // C: the same files and the same call order, with a zero field.
        for (int pass = 0; pass < 2; ++pass) {
            const ArmOutcome outcome =
                render(true, zeroMotionDirectory, kPanSeeds[pass],
                       pass == 0 ? L"amd-lmxxf-pan-c-zero-motion-s1"
                                 : L"amd-lmxxf-pan-c-zero-motion-s2",
                       9, zeroFrame[pass]);
            Check(outcome.ok && outcome.frames == kPanRunFrames,
                  "the zero-field arm rendered the same clip");
            Check(outcome.guidesFromFiles == kPanRunFrames,
                  "the zero-field arm read its guides from files too, so B and C differ in the "
                  "field's VALUE and in nothing else");
        }

        bool measured = true;
        for (int pass = 0; pass < 2; ++pass) {
            if (spatialFrame[pass].size() != truth.size() ||
                trueFrame[pass].size() != truth.size() || zeroFrame[pass].size() != truth.size())
                measured = false;
        }
        if (!measured) {
            Check(false, "every arm produced a full frame to measure");
        } else {
            // The noise that survived, per arm. The two seeds of an arm differ in
            // nothing but the grain they were fed, so this cancels the content and
            // the network's bias on it and is the only number here that can be
            // compared between arms.
            const double noiseSpatial = MeanAbsoluteDifferenceRgb(spatialFrame[0], spatialFrame[1]);
            const double noiseTrue = MeanAbsoluteDifferenceRgb(trueFrame[0], trueFrame[1]);
            const double noiseZero = MeanAbsoluteDifferenceRgb(zeroFrame[0], zeroFrame[1]);
            // The same seed and the same source in two different arms, so one
            // float of input is the whole of what separates them.
            const double separation = MeanAbsoluteDifferenceRgb(trueFrame[0], zeroFrame[0]);
            const double leaseEffect = MeanAbsoluteDifferenceRgb(trueFrame[0], spatialFrame[0]);
            // Reported for the record. Not asserted on beyond being a number,
            // because it is the quantity the run's header explains is not a noise
            // measurement: it is dominated by the network's response to the sines.
            const double errorSpatial = MeanAbsoluteDifferenceRgb(spatialFrame[0], truth);
            const double errorTrue = MeanAbsoluteDifferenceRgb(trueFrame[0], truth);
            const double errorZero = MeanAbsoluteDifferenceRgb(zeroFrame[0], truth);
            // The same estimator applied to the INPUT, which is the score an arm
            // that removed no noise at all would get. Without it the three numbers
            // above have no scale: 5.31 is close to nothing or close to everything
            // depending on what the source itself reads as.
            const double noiseInput =
                MeanAbsoluteDifferenceRgb(PanningNoise(kPanRunFrames - 1u, kWidth, kHeight,
                                                       kPanSeeds[0]),
                                          PanningNoise(kPanRunFrames - 1u, kWidth, kHeight,
                                                       kPanSeeds[1]));

            Line("  noise surviving the last frame, |seed A - seed B| (lower is more denoised):");
            Line("    the source itself, no denoising  : " + Number3(noiseInput));
            Line("    A  spatial, no guides            : " + Number3(noiseSpatial));
            Line("    B  lease, true field +" + std::to_string(kPanPixelsPerFrame) +
                 " px/frame : " + Number3(noiseTrue));
            Line("    C  lease, zero field             : " + Number3(noiseZero));
            Line("    B/A " + Number3(noiseTrue / std::max(noiseSpatial, 1e-9)) + ", C/A " +
                 Number3(noiseZero / std::max(noiseSpatial, 1e-9)) + ", B/C " +
                 Number3(noiseTrue / std::max(noiseZero, 1e-9)));
            Line("  for the record, mean|produced - the grain-free clip|: A " + Number3(errorSpatial) +
                 ", B " + Number3(errorTrue) + ", C " + Number3(errorZero) +
                 " (a statement about the content, not about the noise - see the run's header)");
            Line("  what one float of motion input was worth, same seed and source, last frame:");
            Line("    B against C " + Number3(separation) + ", B against A " + Number3(leaseEffect));

            Check(noiseSpatial > 0.0 && noiseTrue > 0.0 && noiseZero > 0.0,
                  "all three arms produced a measurable noise level");
            // The estimator itself, checked against the one input whose answer is
            // known: a source compared with itself across the two seeds reads at
            // its own noise amplitude, so an arm anywhere near that number removed
            // little and an arm well below it removed a lot.
            Check(noiseInput > 0.0 && noiseSpatial <= noiseInput * 1.20,
                  "the noise estimator reads the source at " + Number3(noiseInput) +
                      ", which is the scale the three arms are on");
            // The discrimination this run exists for. One float separates B and C,
            // and the source, the geometry and the noise draw do not.
            Check(separation > 0.0,
                  "the motion VALUE reached the network: the same frame rendered with a zero field "
                  "instead of the true one came out different (" + Number3(separation) + ")");
            Check(leaseEffect > 0.0,
                  "the lease path and the spatial path published different frames on the same "
                  "source (" + Number3(leaseEffect) + ")");
            // Direction only, and loose on purpose: the size of any benefit is the
            // network's learned blend weight rather than a dial this host turns,
            // so what is asserted is that the field the content agrees with did
            // not leave MORE noise than either alternative.
            Check(noiseTrue <= noiseSpatial * 1.05,
                  "the file-guided lease path left no more noise than the spatial reference (" +
                      Number3(noiseTrue / std::max(noiseSpatial, 1e-9)) + " of it)");
            Check(noiseTrue <= noiseZero * 1.05,
                  "the true field left no more noise than a zero field on the same clip (" +
                      Number3(noiseTrue / std::max(noiseZero, 1e-9)) + " of it)");
        }
    }

    // ---------------------------------------------------------------------
    // Run 7: run 5's measurement taken on decoded video instead of on sines.
    //
    // Run 5 said the temporal path removes real noise from a synthetic still scene,
    // and that the size of what it removes is the network's learned blend weight
    // (0.020 there) rather than anything this host chooses. What that reading cannot
    // say is whether the weight belongs to the model or to the content the test
    // invented: a sine grating under +/-8 grain is nothing like natural video, and a
    // model trained on natural video is entitled to distrust it.
    //
    // So the same measurement runs again on frames decoded from a real clip, with
    // the same estimator and the same bounds. Two subjects, one clip:
    //
    //   still    every frame carries the clip's first frame, so the content is
    //            genuinely still, the correct motion is exactly zero and one depth
    //            plane is the correct depth. The guides are the truth, and the
    //            weight that comes out is comparable with run 5's.
    //   walking  the clip's own consecutive frames. The content moves, a zero field
    //            is now wrong, and what the run reports is whether the runtime's
    //            disocclusion guard keeps that from making the picture worse.
    //
    // Both subjects render the spatial path and the lease path twice each, with the
    // grain seed the only difference inside a pair, so the difference of a pair is
    // the noise that survived, on a scale set by the same estimator applied to the
    // input itself.
    //
    // What this deliberately still does not carry is a motion estimator. The guides
    // are constants, which is exactly what a host without one has, and the field the
    // content really moves by is not in this run. That is the order the plan asks
    // for: the weight first, and the cost of estimating for it only once the weight
    // on real content says it is worth paying.
    // ---------------------------------------------------------------------
    Step("run 7  real decoded video, still and walking");
    {
        constexpr uint64_t kRealFrames = 24;
        constexpr uint32_t kRealSeeds[2] = {0x3d91u, 0x81c7u};
        constexpr size_t kRealTailFrames = 8;

        std::error_code toolsError;
        if (!std::filesystem::is_regular_file(kFfmpegPath, toolsError) ||
            !std::filesystem::is_regular_file(kRealClipPath, toolsError)) {
            Line("  SKIPPED: this run needs both of these, and at least one is absent:");
            Line("    decoder: " + Narrow(kFfmpegPath) + " (staged by tools/fetch_ffmpeg_helpers.ps1)");
            Line("    clip   : " + Narrow(kRealClipPath) + " (tracked in git)");
        } else {
            Line("  clip: " + Narrow(kRealClipPath));
            Line("  decoder: " + Narrow(kFfmpegPath));

            // Read once and hold the frames. Every arm below then renders the same
            // bytes, so what separates the arms is the guides and never the decode.
            std::vector<std::vector<uint8_t>> clip;
            {
                FfmpegSource decoder(kFfmpegPath, kRealClipPath, kWidth, kHeight, kFps);
                if (decoder.Open(kRealClipPath, std::stop_token{}, 0.0)) {
                    for (uint64_t index = 0; index < kRealFrames; ++index) {
                        OfflineDecodedFrame decoded;
                        const OfflineFrameRead read = decoder.Read(decoded, std::stop_token{});
                        if (read != OfflineFrameRead::FrameReady) {
                            if (read == OfflineFrameRead::Error)
                                Line("    the decode stopped: " + decoder.Error());
                            break;
                        }
                        clip.push_back(std::move(decoded.bgra));
                    }
                } else {
                    Line("    the decoder did not start: " + decoder.Error());
                }
                decoder.Close();
            }
            Check(clip.size() == kRealFrames,
                  "the decoder produced " + std::to_string(clip.size()) + " of " +
                      std::to_string(kRealFrames) + " frames from the clip");

            if (clip.size() < 2) {
                Line("  SKIPPED: too few frames were decoded to measure anything");
            } else {
                const uint64_t realFrames = static_cast<uint64_t>(clip.size());
                Line("  measuring on " + std::to_string(realFrames) +
                     " decoded frames, tail of " + std::to_string(kRealTailFrames));
                Line("  the guides are constants: zero motion, one depth plane");

                struct ArmOutcome {
                    bool ok = false;
                    uint64_t frames = 0;
                    uint64_t leaseFrames = 0;
                    std::string history;
                };

                auto render = [&](bool temporal, bool freeze, uint32_t seed, const wchar_t* stem,
                                  uint64_t jobId, std::vector<uint8_t>& produced) -> ArmOutcome {
                    AmdLmxxfEvaluatorConfig armConfig = config;
                    if (temporal) {
                        armConfig.temporalHistory = true;
                        // Zero motion and one depth plane, as constants. On the
                        // frozen subject that IS the truth about the source; on the
                        // walking one it is what a host with no estimator has. The
                        // run reports both rather than picking one.
                        armConfig.motionPixelsPerFrameX = 0.0f;
                        armConfig.motionPixelsPerFrameY = 0.0f;
                        armConfig.depthValue = 1.0f;
                    }
                    AmdLmxxfEvaluator arm(armConfig);
                    RealFrameSource source(clip, kFps, seed, freeze);
                    CountingEncoder encoder(arm, false);
                    OfflineNeuralRenderer job(source, arm, encoder, [] { return std::string{}; });

                    NeuralRenderRequest request;
                    request.sourcePath = outputDirectory / L"real-demo-capture-1080p30.mkv";
                    request.stagingVideoPath =
                        outputDirectory / (std::wstring(stem) + L".partial.mkv");
                    request.width = kWidth;
                    request.height = kHeight;
                    request.fps = kFps;
                    request.durationSeconds = double(realFrames) / kFps;
                    request.jobId = jobId;
                    request.prerollFrames = 0;
                    request.requireNeural = false;

                    const NeuralRenderResult result = job.Run(request);
                    ArmOutcome outcome;
                    outcome.ok = result.ok;
                    outcome.frames = result.frameCount;
                    outcome.leaseFrames = arm.TemporalFrames();
                    outcome.history = arm.HistoryStatus();
                    Line("    " + Narrow(std::wstring(stem)) + ": ok=" +
                         std::string(result.ok ? "yes" : "no") + " failure=" +
                         std::string(NeuralRenderFailureName(result.failure)) + " frames=" +
                         std::to_string(outcome.frames) + " leaseFrames=" +
                         std::to_string(outcome.leaseFrames) + " history=" +
                         (outcome.history.empty() ? std::string("(none)") : outcome.history));
                    if (!result.ok) Line("      detail: " + Narrow(result.detail));
                    produced = std::move(encoder.lastFrame);
                    return outcome;
                };

                struct Subject {
                    bool freeze;
                    const wchar_t* name;
                    const char* what;
                    uint64_t jobBase;
                };
                const Subject subjects[2] = {
                    {true, L"still",
                     "every frame is the clip's first frame, so zero motion is the truth", 20},
                    {false, L"walking",
                     "the clip's own frames, so the content moves and a zero field is wrong", 40},
                };

                std::vector<std::string> weights;
                for (const Subject& subject : subjects) {
                    const std::string name = Narrow(std::wstring(subject.name));
                    Line("");
                    Line("  subject " + name + ": " + std::string(subject.what));

                    std::vector<uint8_t> spatial[2];
                    std::vector<uint8_t> lease[2];
                    for (int pass = 0; pass < 2; ++pass) {
                        const std::wstring stem = std::wstring(subject.name) + L"-a-spatial-s" +
                                                  std::to_wstring(pass + 1);
                        const ArmOutcome outcome =
                            render(false, subject.freeze, kRealSeeds[pass], stem.c_str(),
                                   subject.jobBase + static_cast<uint64_t>(pass), spatial[pass]);
                        Check(outcome.ok && outcome.frames == realFrames,
                              "the spatial reference rendered the " + name + " subject");
                        Check(outcome.leaseFrames == 0 && outcome.history.empty(),
                              "the spatial arm of the " + name +
                                  " subject ran no lease frame and reported no history");
                    }
                    for (int pass = 0; pass < 2; ++pass) {
                        const std::wstring stem =
                            std::wstring(subject.name) + L"-b-lease-s" + std::to_wstring(pass + 1);
                        const ArmOutcome outcome =
                            render(true, subject.freeze, kRealSeeds[pass], stem.c_str(),
                                   subject.jobBase + 2u + static_cast<uint64_t>(pass),
                                   lease[pass]);
                        Check(outcome.ok && outcome.frames == realFrames,
                              "the lease path rendered the " + name + " subject");
                        Check(outcome.leaseFrames == realFrames,
                              "every frame of the " + name + " subject went through the lease path (" +
                                  std::to_string(outcome.leaseFrames) + "/" +
                                  std::to_string(realFrames) + ")");
                        Check(!outcome.history.empty() && outcome.history != "off",
                              "the temporal model engaged on the " + name + " subject (got '" +
                                  outcome.history + "')");
                    }

                    if (spatial[0].size() != spatial[1].size() || lease[0].size() != lease[1].size()) {
                        Check(false, "both arms of the " + name +
                                         " subject produced a full frame to measure");
                        continue;
                    }

                    const double noiseSpatial = MeanAbsoluteDifferenceRgb(spatial[0], spatial[1]);
                    const double noiseLease = MeanAbsoluteDifferenceRgb(lease[0], lease[1]);
                    const double divergence = MeanAbsoluteDifferenceRgb(spatial[0], lease[0]);
                    // The same estimator on the input, which is what an arm that
                    // removed no noise at all would score. The frame measured is the
                    // one the pair's last pass encoded: index realFrames - 1, whose
                    // content is the clip's first frame on the frozen subject and the
                    // clip's last frame on the walking one.
                    const size_t contentFrame = subject.freeze ? 0u : static_cast<size_t>(realFrames - 1u);
                    std::vector<uint8_t> input[2];
                    for (int pass = 0; pass < 2; ++pass)
                        GrainInto(clip[contentFrame], realFrames - 1u, kRealSeeds[pass], input[pass]);
                    const double noiseInput = MeanAbsoluteDifferenceRgb(input[0], input[1]);

                    Line("    noise surviving the last frame, |seed A - seed B| "
                         "(lower is more denoised):");
                    Line("      the input itself           : " + Number3(noiseInput));
                    // The parenthesised figures are what the two paths do to the
                    // noise on their own. The spatial one is above 1, which matters:
                    // the pass this content goes through is not a no-op, so the
                    // temporal figure beside it is relative to a baseline that
                    // already amplifies noise rather than to the input.
                    Line("      the spatial path           : " + Number3(noiseSpatial) + " (" +
                         Number3(noiseSpatial / std::max(noiseInput, 1e-9)) + " of the input)");
                    Line("      the lease path, zero field : " + Number3(noiseLease) + " (" +
                         Number3(noiseLease / std::max(noiseInput, 1e-9)) + " of the input)");
                    const double ratio = noiseLease / std::max(noiseSpatial, 1e-9);
                    // A first-order blend v = (1-w)x + w*v_prev settles at a noise
                    // variance of (1-w)/(1+w), so the measured ratio inverts to the
                    // weight behind it. This runtime takes no host blend strength, so
                    // this number is the model's decision on this content - and it is
                    // the one figure that can be set beside run 5's 0.020.
                    const double impliedWeight = (1.0 - ratio * ratio) / (1.0 + ratio * ratio);
                    Line("      ratio temporal/spatial     : " + Number3(ratio));
                    Line("      implied history weight     : " + Number3(impliedWeight));
                    Line("      mean|spatial - lease|      : " + Number3(divergence));
                    weights.push_back("    " + name + ": implied weight " + Number3(impliedWeight) +
                                      ", ratio " + Number3(ratio));

                    Check(noiseSpatial > 0.0 && noiseLease > 0.0,
                          "both arms of the " + name + " subject produced a measurable noise level");
                    Check(noiseInput > 0.0 && noiseSpatial <= noiseInput * 1.20,
                          "the noise estimator reads the " + name + " input at " +
                              Number3(noiseInput) + ", which is the scale the two arms are on");
                    Check(divergence > 0.0,
                          "on the " + name +
                              " subject the lease path published a different frame than the spatial "
                              "one, so the history is part of the arithmetic");
                    // Direction only, and loose on purpose: the size of what
                    // accumulation does is the network's learned weight rather than a
                    // dial this host turns, so what is asserted is that it did not
                    // make the picture worse - on either subject, with the guides
                    // right on one and wrong on the other.
                    Check(ratio < 1.05,
                          "temporal accumulation did not leave more residual noise than the spatial "
                          "path on the " + name + " subject (" + Number3(ratio) + " of it)");
                }

                Line("");
                Line("  the same estimator on two subjects, next to run 5's synthetic still:");
                for (const std::string& weight : weights) Line(weight);
                Line("    run 5 (sine grating, still)  : implied weight 0.020");
            }
        }
    }

    Step("summary");
    Line("  runtime evaluations through this evaluator: " +
         std::to_string(evaluator.NeuralEvaluations()) + ", accepted submits " +
         std::to_string(evaluator.EvaluationCount()) + ", peak local VRAM " +
         std::to_string(evaluator.PeakLocalVideoMemoryMiB()) + " MiB");
    Line("  video memory: " + evaluator.LocalVideoMemoryNote());
    Line("  " + std::string(failures == 0 ? "RESULT: ok" : "RESULT: FAIL"));

    return failures == 0 ? kExitOk : kExitFailed;
}
