#include "GpuTestGate.h"
#include "TestEnvironment.h"
// Opt-in real GPU verification; registered under the `gpu` CTest label, which
// the portable suite excludes (`ctest -LE gpu`).
// Usage: MediaGpuSmoke <ffmpeg-directory> <NeuralWorker.exe> <output-directory>
// Every run writes its evidence into a new, time-stamped directory under the
// output directory, so repeated runs never mix and a ctest registration can
// name one fixed path.
#include "MediaPipeline.h"
#include "NeuralWorker.h"
#include "NvencDirect.h"
#include "VideoDecoder.h"

#include <windows.h>
#include <d3d12.h>
#include <mfapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;

// Cooperative cancellation normally kills the isolated helper immediately.
// Terminate this opt-in test if a regression ignores cancellation entirely.
class Deadline {
public:
    Deadline() : watchdog_([this] {
        std::unique_lock lock(mutex_);
        if (cv_.wait_for(lock, 120s, [this] { return finished_; })) return;
        stop_.request_stop();
        if (cv_.wait_for(lock, 10s, [this] { return finished_; })) return;
        std::wcerr << L"FAIL: cancellation did not finish within the watchdog grace period.\n";
        TerminateProcess(GetCurrentProcess(), 124);
    }) {}
    ~Deadline() {
        { std::lock_guard lock(mutex_); finished_ = true; }
        cv_.notify_one();
    }
    std::stop_token Token() const { return stop_.get_token(); }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::stop_source stop_;
    bool finished_{};
    std::jthread watchdog_; // Destroyed first, while synchronization fields live.
};

std::wstring Quote(std::wstring_view value)
{
    std::wstring result(1, L'"');
    size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result.push_back(character); slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

bool Generate(const fs::path& helper, const std::vector<std::wstring>& args, const fs::path& log)
{
    std::wstring command = Quote(helper.wstring());
    for (const auto& arg : args) command += L" " + Quote(arg);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output; startup.hStdError = output;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    CloseHandle(output);
    if (!started) return false;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 2000);
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}

bool Check(bool condition, std::wstring_view detail, std::wofstream& report)
{
    if (!condition) {
        std::wcerr << L"FAIL: " << detail << L'\n';
        report << L"FAIL: " << detail << std::endl;
    }
    return condition;
}

bool Decode(const fs::path& media, uint32_t width, uint32_t height,
            uint64_t expectedFrames, bool requireMotion, std::stop_token stop, std::wofstream& report)
{
    VideoDecoder decoder;
    if (!Check(decoder.OpenSequential(media.wstring(), MediaSourceKind::LocalFile, stop),
        L"Decoder could not open " + media.wstring(), report)) return false;
    bool ok = Check(decoder.Width() == width && decoder.Height() == height,
        L"Export dimensions differ from the source", report);
    uint64_t frames = 0;
    std::vector<uint8_t> firstPixels;
    bool changedPixels = false;
    VideoFrame frame;
    while (!stop.stop_requested()) {
        const auto read = decoder.ReadNextAvailable(frame, stop);
        if (read == VideoReadResult::NotReady) { std::this_thread::sleep_for(1ms); continue; }
        if (read == VideoReadResult::EndOfStream) break;
        if (!Check(read == VideoReadResult::FrameReady, L"Export decode failed", report)) return false;
        // OpenSequential decodes to NV12 for even geometry (0.19.0's transport),
        // so the complete size is the layout's, not four bytes per pixel.
        if (!Check(frame.bgra.size() == FrameBytes(decoder.PixelLayout(), width, height),
            L"Export contains an incomplete decoded frame", report)) return false;
        if (frames == 0) firstPixels = frame.bgra;
        else if (frame.bgra != firstPixels) changedPixels = true;
        if (++frames > expectedFrames) break;
    }
    report << L"decoded=" << media.filename().wstring() << L" dimensions=" << width << L'x' << height
           << L" frames=" << frames << L" fps=" << decoder.FrameRate()
           << L" duration=" << decoder.DurationSeconds() << L" changed_pixels=" << changedPixels << std::endl;
    return Check(!stop.stop_requested() && frames == expectedFrames,
        L"Unexpected decoded export frame count or timeout", report) &&
        Check(!requireMotion || changedPixels, L"Moving source became a frozen export", report) && ok;
}

bool VerifyInput(const fs::path& helpers, const fs::path& worker, const fs::path& source,
                 const fs::path& outputDirectory, std::wofstream& report)
{
    Deadline deadline;
    const auto stop = deadline.Token();
    VideoDecoder decoder;
    if (!Check(decoder.OpenSequential(source.wstring(), MediaSourceKind::LocalFile, stop),
        L"Source decoder could not open " + source.wstring(), report)) return false;
    const auto width = decoder.Width(), height = decoder.Height();
    const auto fps = decoder.FrameRate(), duration = decoder.DurationSeconds();
    const bool still = decoder.IsStillImage();
    decoder.Close();
    const uint64_t expectedFrames = static_cast<uint64_t>(std::llround(fps * duration));
    report << L"input=" << source.wstring() << L" dimensions=" << width << L'x' << height
           << L" fps=" << fps << L" duration=" << duration << L" expected_frames=" << expectedFrames << std::endl;
    const auto cache = outputDirectory / (source.stem().wstring() + L"-neural.mkv");
    const NeuralRenderRequest request{nullptr, source, cache, width, height, fps, duration};
    const auto start = std::chrono::steady_clock::now();
    const auto result = RunNeuralWorker(worker, request, {}, stop);
    // The next worker replaces its logs. Preserve the evidence for every input,
    // including rejected jobs, before starting another process.
    for (const auto* name : {L"ReShade.log", L"DLSSVideoPlayer.log"}) {
        std::error_code error;
        const auto log = worker.parent_path() / name;
        if (fs::is_regular_file(log, error)) {
            fs::copy_file(log, outputDirectory / (source.stem().wstring() + L"-" + name),
                fs::copy_options::none, error);
            if (error) report << L"log_copy_error=" << log.wstring() << L" " << error.value() << std::endl;
        }
    }
    report << L"neural_ok=" << result.ok << L" cancelled=" << result.cancelled
           << L" frames=" << result.frameCount << L" duration_100ns=" << result.duration100ns
           << L" native_evaluations=" << result.nativeEvaluations
           << L" verified_frames=" << result.verifiedNeuralFrames
           << L" feature18_armed=" << result.feature18ArmedBeforeCapture
           << L" evidence_valid=" << result.evidence.Valid()
           << L" highest_evaluation=" << result.evidence.highestObservedEvaluation
           << L" elapsed_seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
           << L" detail=" << result.detail << std::endl;
    std::wcout << source.filename().wstring() << L": neural=" << result.ok << L" frames=" << result.frameCount
               << L" verified=" << result.verifiedNeuralFrames << L" detail=" << result.detail << std::endl;
    if (!Check(result.ok && !result.cancelled && result.frameCount == expectedFrames &&
        result.nativeEvaluations > 0 && result.verifiedNeuralFrames == result.frameCount &&
        result.feature18ArmedBeforeCapture && result.evidence.Valid(), L"Strict neural evidence failed", report)) return false;
    const auto probe = ProbeMedia(helpers, cache, stop);
    if (!Check(probe.ok && probe.decodedFinalFrame && probe.width == width && probe.height == height &&
        probe.frameCount == expectedFrames && std::abs(double(probe.videoDuration100ns) / 1e7 - duration) < 0.03,
        L"Neural cache frame/duration validation failed: " + probe.detail, report)) return false;
    bool ok = true;
    CachedVideoExporter exporter(helpers);
    for (const auto* extension : {L".png", L".jpg", L".gif", L".mp4", L".mkv"}) {
        const auto output = outputDirectory / (source.stem().wstring() + L"-export" + extension);
        const bool image = std::wstring_view(extension) == L".png" || std::wstring_view(extension) == L".jpg";
        const bool gif = std::wstring_view(extension) == L".gif";
        const auto exported = exporter.Run({cache, source, output}, stop);
        if (!Check(exported.ok, L"Export failed: " + output.wstring() + L" " + exported.detail, report)) { ok = false; continue; }
        // GIF decoder expands centisecond timing to 100 fps; image exports are one frame.
        const uint64_t decodedFrames = image ? 1 : gif ? static_cast<uint64_t>(std::llround(duration * 100)) : expectedFrames;
        ok = Decode(output, width, height, decodedFrames, !image && !still, stop, report) && ok;
        if (!image) {
            const auto exportedProbe = ProbeMedia(helpers, output, stop);
            const uint64_t encodedFrames = gif ? static_cast<uint64_t>(std::llround(duration * 50)) : expectedFrames;
            report << L"probe=" << output.filename().wstring() << L" ok=" << exportedProbe.ok
                   << L" frames=" << exportedProbe.frameCount << L" duration_100ns=" << exportedProbe.duration100ns
                   << L" video_duration_100ns=" << exportedProbe.videoDuration100ns << std::endl;
            ok = Check(exportedProbe.ok && exportedProbe.decodedFinalFrame && exportedProbe.frameCount == encodedFrames &&
                std::abs(double(exportedProbe.videoDuration100ns) / 1e7 - duration) < 0.03,
                L"Export frame/duration validation failed: " + exportedProbe.detail, report) && ok;
        }
    }
    return Check(!still || expectedFrames == 1, L"Photo source did not remain a single frame", report) && ok;
}
// ---- --nvenc-direct-identity -------------------------------------------------
// The direct NVENC path (NvencDirect.h) against the encoder child it replaces, on
// the same frames: a testsrc2 clip as raw NV12 and as raw P010 is fed once
// through RawVideoEncoder - the exact child and arguments a render uses - and
// once uploaded into the direct path's surfaces and encoded by NVENC's D3D12
// interface. The two cache files must carry the same packets, byte for byte,
// with the same timestamps and the same codec private data, which framemd5 of a
// stream copy states in one line per packet. Both rungs NVENC writes are
// covered: Standard from an NV12 capture and High (Main10) from a P010 one.

// Runs a helper and returns what it printed, or nothing when it failed.
std::string Capture(const fs::path& helper, const std::vector<std::wstring>& args, const fs::path& log)
{
    if (!Generate(helper, args, log)) return {};
    std::ifstream in(log, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// framemd5 of the first video stream as it is stored, minus the header line that
// names the muxing library, which differs by construction and is not the video.
std::string PacketDigest(const fs::path& ffmpeg, const fs::path& media, const fs::path& log)
{
    const std::string text = Capture(ffmpeg, {L"-v", L"error", L"-nostdin", L"-i", media.wstring(), L"-map",
                                              L"0:v:0", L"-c", L"copy", L"-f", L"framemd5", L"-"}, log);
    std::string kept;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start + 1);
        if (line.rfind("#software", 0) != 0) kept += line;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return kept;
}

bool EncodeDirect(const fs::path& raw, const EncoderSpec& spec, uint32_t frames, const fs::path& helpers,
                  const fs::path& output, std::wofstream& report)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
        return Check(false, L"no D3D12 device", report);
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))) ||
        FAILED(list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return Check(false, L"D3D12 objects could not be created", report);
    NvencSurfacePool pool;
    if (!pool.Configure(device.Get(), fence.Get(), spec.pixelFormat, spec.width, spec.height, 4))
        return Check(false, L"the surface pool refused the geometry", report);
    NvencDirectEncoder encoder;
    if (encoder.Start(pool, spec, output, helpers) != EncodeError::None)
        return Check(false, L"direct session did not start: " +
                     std::wstring(encoder.Detail().begin(), encoder.Detail().end()), report);
    const D3D12_RESOURCE_DESC desc = pool.Surface(0)->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT planes[2]{};
    UINT rows[2]{};
    UINT64 rowBytes[2]{}, total = 0;
    device->GetCopyableFootprints(&desc, 0, 2, 0, planes, rows, rowBytes, &total);
    // One upload buffer per surface: a surface only comes back once NVENC has read
    // it, which was after this buffer's copy into it had finished.
    std::vector<ComPtr<ID3D12Resource>> uploads(pool.Count());
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc = {1, 0};
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (auto& upload : uploads)
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
            return Check(false, L"upload buffer", report);
    const size_t frameBytes = size_t(EncoderFrameBytes(spec.pixelFormat, spec.width, spec.height));
    const size_t lumaRow = size_t(spec.width) * (spec.pixelFormat == EncoderPixelFormat::P010 ? 2u : 1u);
    std::ifstream in(raw, std::ios::binary);
    std::vector<uint8_t> frame(frameBytes);
    uint64_t fenceValue = 0;
    for (uint32_t index = 0; index < frames; ++index) {
        if (!in.read(reinterpret_cast<char*>(frame.data()), std::streamsize(frameBytes)))
            return Check(false, L"raw frames ran short", report);
        const auto surface = pool.Acquire(std::chrono::seconds(20));
        if (!surface) return Check(false, L"no surface came free", report);
        ID3D12Resource* upload = uploads[*surface].Get();
        uint8_t* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return Check(false, L"map", report);
        for (UINT y = 0; y < rows[0]; ++y)
            memcpy(mapped + planes[0].Offset + size_t(planes[0].Footprint.RowPitch) * y, frame.data() + lumaRow * y, lumaRow);
        const uint8_t* chroma = frame.data() + lumaRow * spec.height;
        for (UINT y = 0; y < rows[1]; ++y)
            memcpy(mapped + planes[1].Offset + size_t(planes[1].Footprint.RowPitch) * y, chroma + lumaRow * y, lumaRow);
        upload->Unmap(0, nullptr);
        // One recording at a time: wait for the previous copy before reusing the
        // allocator. The harness measures identity, not throughput.
        if (fence->GetCompletedValue() < fenceValue) {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(fenceValue, event);
            WaitForSingleObject(event, 20000);
            CloseHandle(event);
        }
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = pool.Surface(*surface);
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barrier);
        for (UINT plane = 0; plane < 2; ++plane) {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = pool.Surface(*surface);
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = plane;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload;
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = planes[plane];
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        std::vector<uint8_t> token;
        WriteNvencDirectToken(NvencDirectToken{NvencDirectToken::kMagic, *surface, fenceValue}, token);
        if (encoder.Encode(token, index + 1 < frames) != EncodeError::None)
            return Check(false, L"direct encode failed: " +
                         std::wstring(encoder.Detail().begin(), encoder.Detail().end()), report);
    }
    if (encoder.Finish({}) != EncodeError::None)
        return Check(false, L"direct finish failed: " +
                     std::wstring(encoder.Detail().begin(), encoder.Detail().end()), report);
    return true;
}

int NvencDirectIdentity(const fs::path& helpers, const fs::path& root, uint32_t width, uint32_t height,
                        uint32_t frames, double fps, uint32_t preset)
{
    std::wofstream report(root / L"results.txt");
    std::string why;
    if (!nvenc_direct::DriverAvailable(&why)) {
        std::wcerr << L"FAIL: the driver offers no direct NVENC: " << std::wstring(why.begin(), why.end()) << L'\n';
        return 1;
    }
    const auto ffmpeg = helpers / L"ffmpeg.exe";
    bool ok = true;
    for (const bool tenBit : {false, true}) {
        const std::wstring rung = tenBit ? L"high-p010" : L"standard-nv12";
        const auto raw = root / (rung + L".yuv");
        if (!Generate(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
                               std::format(L"testsrc2=s={}x{}:r={}", width, height, fps), L"-frames:v",
                               std::to_wstring(frames), L"-pix_fmt", tenBit ? L"p010le" : L"nv12", L"-f",
                               L"rawvideo", raw.wstring()}, root / (rung + L"-generation.log"))) {
            ok = Check(false, L"raw frames could not be generated", report);
            continue;
        }
        EncoderSpec spec{width, height, fps, EncoderKind::HevcNvenc,
                         tenBit ? EncoderPixelFormat::P010 : EncoderPixelFormat::Nv12};
        spec.nvencPreset = preset;
        spec.quality = tenBit ? EncoderQuality::High : EncoderQuality::Standard;
        const auto child = root / (rung + L"-child.mkv"), direct = root / (rung + L"-direct.mkv");
        RawVideoEncoder encoder(helpers);
        bool fed = encoder.Start(spec, child) == EncodeError::None;
        {
            std::ifstream in(raw, std::ios::binary);
            std::vector<uint8_t> frame(size_t(EncoderFrameBytes(spec.pixelFormat, width, height)));
            for (uint32_t index = 0; fed && index < frames; ++index)
                fed = in.read(reinterpret_cast<char*>(frame.data()), std::streamsize(frame.size())) &&
                      encoder.WriteFrame(frame) == EncodeError::None;
        }
        fed = fed && encoder.Finish() == EncodeError::None;
        ok = Check(fed, rung + L": the encoder child failed", report) && ok;
        const auto started = std::chrono::steady_clock::now();
        const bool encoded = EncodeDirect(raw, spec, frames, helpers, direct, report);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        ok = encoded && ok;
        if (!fed || !encoded) continue;
        const std::string a = PacketDigest(ffmpeg, child, root / (rung + L"-child.framemd5"));
        const std::string b = PacketDigest(ffmpeg, direct, root / (rung + L"-direct.framemd5"));
        const bool same = !a.empty() && a == b;
        report << rung << L": child and direct " << (same ? L"identical" : L"DIFFER") << L", "
               << std::count(a.begin(), a.end(), '\n') << L" digest lines, direct " << seconds << L" s" << std::endl;
        std::wcout << rung << L": child and direct " << (same ? L"identical" : L"DIFFER") << L'\n';
        ok = Check(same, rung + L": the direct path's packets differ from the encoder child's", report) && ok;
        std::error_code ignored;
        fs::remove(raw, ignored);
    }
    report << L"overall=" << (ok ? L"PASS" : L"FAIL") << std::endl;
    std::wcout << L"Evidence: " << root.wstring() << std::endl;
    return ok ? 0 : 1;
}

std::string ReadBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A stored-landscape clip with a display matrix, decoded on the CUDA path: the
// matrix is applied by transpose_cuda before the frames leave the GPU. Before
// the fix that path ignored the matrix entirely and emitted the stored,
// sideways picture. Measured on the bundled 9.0.1, NVDEC plus transpose_cuda
// is byte for byte ffmpeg's CPU decode plus the CPU filter, so each layout is
// held to exact equality with a reference the CPU made: NV12 as the export's
// sequential open decodes it, and BGRA as playback does.
int TurnedDecode(const fs::path& helpers, const fs::path& root)
{
    std::wofstream report(root / L"results.txt");
    const auto ffmpeg = helpers / L"ffmpeg.exe";
    const auto stored = root / L"stored.mp4";
    constexpr uint32_t width = 320, height = 176, frames = 10;
    // Declared BT.709 limited, which is what lets the sequential open take NV12.
    if (!Generate(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
            L"testsrc2=s=320x176:r=10:d=1", L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p",
            L"-colorspace", L"bt709", L"-color_primaries", L"bt709", L"-color_trc", L"bt709",
            L"-color_range", L"tv", stored.wstring()}, root / L"stored-generation.log")) {
        std::wcerr << L"FAIL: fixture generation failed\n";
        return 2;
    }
    struct Case {
        const wchar_t* name;
        std::vector<std::wstring> tag;
        const wchar_t* filter;
        bool swaps;
    };
    const Case cases[] = {
        {L"rotate90", {L"-display_rotation", L"90"}, L"transpose=cclock", true},
        {L"rotate270", {L"-display_rotation", L"-90"}, L"transpose=clock", true},
        {L"rotate180", {L"-display_rotation", L"180"}, L"hflip,vflip", false},
        {L"rotate90-mirrored", {L"-display_rotation", L"90", L"-display_hflip"}, L"transpose=clock_flip", true},
    };
    bool ok = true;
    for (const Case& clip : cases) {
        const auto turned = root / (std::wstring(clip.name) + L".mp4");
        std::vector<std::wstring> tag{L"-v", L"error", L"-nostdin", L"-n"};
        tag.insert(tag.end(), clip.tag.begin(), clip.tag.end());
        tag.insert(tag.end(), {L"-i", stored.wstring(), L"-c", L"copy", turned.wstring()});
        if (!Check(Generate(ffmpeg, tag, root / (std::wstring(clip.name) + L"-generation.log")),
                   std::wstring(clip.name) + L": the display matrix could not be written", report)) { ok = false; continue; }
        const uint32_t shownWidth = clip.swaps ? height : width, shownHeight = clip.swaps ? width : height;
        for (const bool sequential : {true, false}) {
            const std::wstring layout = sequential ? L"nv12" : L"bgra";
            const auto reference = root / (std::wstring(clip.name) + L"." + layout);
            // BGRA goes through NV12 first, as the CUDA chain's does: the
            // conversion under test is the turn, not yuv420p against nv12.
            const std::wstring filter = sequential ? std::wstring(clip.filter) : std::wstring(clip.filter) + L",format=nv12,format=bgra";
            if (!Check(Generate(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-i", stored.wstring(), L"-vf", filter,
                    L"-f", L"rawvideo", L"-pix_fmt", layout, reference.wstring()},
                    root / (std::wstring(clip.name) + L"-" + layout + L"-reference.log")),
                    std::wstring(clip.name) + L": the reference could not be made", report)) { ok = false; continue; }
            const std::string expected = ReadBytes(reference);
            VideoDecoder decoder;
            const bool opened = sequential ? decoder.OpenSequential(turned.wstring()) : decoder.Open(turned.wstring());
            if (!Check(opened, std::wstring(clip.name) + L": the decoder could not open it", report)) { ok = false; continue; }
            bool fine = Check(decoder.Width() == shownWidth && decoder.Height() == shownHeight,
                std::format(L"{} {}: decoder reports {}x{}, upright is {}x{}", clip.name, layout,
                            decoder.Width(), decoder.Height(), shownWidth, shownHeight), report);
            fine = Check(decoder.PixelLayout() == (sequential ? PixelLayout::Nv12 : PixelLayout::Bgra),
                std::wstring(clip.name) + L" " + layout + L": unexpected layout", report) && fine;
            std::string decoded;
            VideoFrame frame;
            while (decoder.ReadNext(frame)) decoded.append(reinterpret_cast<const char*>(frame.bgra.data()), frame.bgra.size());
            fine = Check(decoder.DecodingOnCuda(), std::wstring(clip.name) + L" " + layout +
                L": the frames did not come from the CUDA path", report) && fine;
            const size_t frameBytes = FrameBytes(decoder.PixelLayout(), shownWidth, shownHeight);
            fine = Check(decoded.size() == frames * frameBytes && expected.size() == decoded.size(),
                std::format(L"{} {}: {} decoded bytes, {} expected", clip.name, layout, decoded.size(), expected.size()),
                report) && fine;
            size_t differing = 0;
            for (size_t index = 0; index < std::min(decoded.size(), expected.size()); ++index)
                differing += decoded[index] != expected[index];
            fine = Check(differing == 0 && decoded.size() == expected.size(),
                std::format(L"{} {}: {} bytes differ from {}", clip.name, layout, differing, clip.filter), report) && fine;
            report << clip.name << L" " << layout << L": " << decoder.Width() << L'x' << decoder.Height()
                   << L" cuda=" << decoder.DecodingOnCuda() << L" differing_bytes=" << differing
                   << L" " << (fine ? L"PASS" : L"FAIL") << std::endl;
            std::wcout << clip.name << L" " << layout << L": " << (fine ? L"PASS" : L"FAIL") << L'\n';
            ok = fine && ok;
        }
    }
    report << L"overall=" << (ok ? L"PASS" : L"FAIL") << std::endl;
    std::wcout << L"Evidence: " << root.wstring() << std::endl;
    return ok ? 0 : 1;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    test_support::ContainChildProcesses();
    if (const int skip = gpu_test_gate::SkipWithoutGpu()) return skip;
    // MediaGpuSmoke --nvenc-direct-identity <ffmpeg-directory> <output-directory>
    //               [WxH] [frames] [fps] [preset]
    if (argc >= 4 && argc <= 8 && std::wstring_view(argv[1]) == L"--nvenc-direct-identity") {
        const auto helpers = fs::absolute(argv[2]);
        const auto root = fs::absolute(argv[3]) /
            std::format(L"{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        unsigned width = 1280, height = 720, frames = 60;
        if (argc >= 5 && (swscanf_s(argv[4], L"%ux%u", &width, &height) != 2 || !width || !height || (width | height) & 1u))
            return 2;
        if (argc >= 6) frames = unsigned(_wtoi(argv[5]));
        const double fps = argc >= 7 ? _wtof(argv[6]) : 30.0;
        const unsigned preset = argc >= 8 ? unsigned(_wtoi(argv[7])) : 5u;
        if (!(fps > 0.0) || preset < 1 || preset > 7) return 2;
        if (!frames || !fs::is_regular_file(helpers / L"ffmpeg.exe") || !fs::create_directories(root)) {
            std::wcerr << L"ffmpeg.exe must exist and the run directory must be new.\n";
            return 2;
        }
        return NvencDirectIdentity(helpers, root, width, height, frames, fps, preset);
    }
    // MediaGpuSmoke --turned-decode <ffmpeg-directory> <output-directory>
    if (argc == 4 && std::wstring_view(argv[1]) == L"--turned-decode") {
        const auto helpers = fs::absolute(argv[2]);
        const auto root = fs::absolute(argv[3]) /
            std::format(L"{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        if (!fs::is_regular_file(helpers / L"ffmpeg.exe") || !fs::is_regular_file(helpers / L"ffprobe.exe") ||
            !fs::create_directories(root)) {
            std::wcerr << L"ffmpeg.exe and ffprobe.exe must exist and the run directory must be new.\n";
            return 2;
        }
        // Decoders look for FFmpeg beside this executable and then on PATH.
        std::wstring path(32768, L'\0');
        path.resize(GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size())));
        SetEnvironmentVariableW(L"PATH", (helpers.wstring() + L';' + path).c_str());
        return TurnedDecode(helpers, root);
    }
    if (argc != 4) {
        std::wcerr << L"Usage: MediaGpuSmoke <ffmpeg-directory> <NeuralWorker.exe> <output-directory>\n";
        return 2;
    }
    const auto helpers = fs::absolute(argv[1]), worker = fs::absolute(argv[2]);
    const auto root = fs::absolute(argv[3]) /
        std::format(L"{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    if (!fs::is_regular_file(helpers / L"ffmpeg.exe") || !fs::is_regular_file(helpers / L"ffprobe.exe") ||
        !fs::is_regular_file(worker) || !fs::create_directories(root)) {
        std::wcerr << L"Tools must exist and the run directory must be new.\n"; return 2;
    }
    std::wofstream report(root / L"results.txt");
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 2; }
    const auto photo = root / L"photo.png", gif = root / L"animation.gif", video = root / L"video.mp4";
    const auto ffmpeg = helpers / L"ffmpeg.exe";
    const bool fixtures =
        Generate(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
            L"testsrc=s=641x359:r=1:d=1", L"-frames:v", L"1", photo.wstring()}, root / L"photo-generation.log") &&
        Generate(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
            L"testsrc2=s=320x180:r=10:d=1", L"-vf", L"select='eq(n,0)+eq(n,1)+eq(n,4)'",
            L"-fps_mode", L"vfr", L"-final_delay", L"60", gif.wstring()}, root / L"gif-generation.log") &&
        Generate(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi", L"-i",
            L"testsrc2=s=640x360:r=24:d=1", L"-f", L"lavfi", L"-i", L"sine=frequency=440:duration=1",
            L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p", L"-c:a", L"aac", L"-shortest", video.wstring()}, root / L"video-generation.log");
    bool ok = Check(fixtures, L"Fixture generation failed; inspect generation logs", report);
    if (fixtures) for (const auto& source : {photo, gif, video})
        ok = VerifyInput(helpers, worker, source, root, report) && ok;
    report << L"overall=" << (ok ? L"PASS" : L"FAIL") << std::endl;
    MFShutdown(); CoUninitialize();
    std::wcout << L"Evidence: " << root.wstring() << std::endl;
    return ok ? 0 : 1;
}
