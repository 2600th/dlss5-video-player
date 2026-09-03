#include "NeuralCache.h"
#include "MediaPipeline.h"
#include "OfflineNeuralRenderer.h"
#include "SynchronizedPlayback.h"
#include "TestSupport.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

class TempDirectory {
public:
    TempDirectory()
    {
        std::array<wchar_t, MAX_PATH> base{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(base.size()), base.data());
        CHECK(length > 0 && length < base.size());
        path_ = std::filesystem::path(base.data()) /
            (L"DLSSVideoPlayer-NeuralCacheTests-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(GetTickCount64()));
        std::error_code error;
        CHECK(std::filesystem::create_directories(path_, error));
        CHECK(!error);
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

void WriteBytes(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    CHECK(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::filesystem::path CurrentExecutable()
{
    std::wstring value(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    CHECK(length > 0 && length < value.size());
    value.resize(length);
    return value;
}

NeuralCacheManifest CompleteRenderManifest()
{
    NeuralCacheManifest manifest;
    manifest.kind = NeuralCacheEntryKind::Render;
    manifest.state = NeuralCacheState::Staging;
    manifest.sourceDigest = std::string(64, 'a');
    manifest.runtimeDigest = std::string(64, 'b');
    manifest.encoder = "hevc_nvenc";
    manifest.width = 1920;
    manifest.height = 1080;
    manifest.frameCount = 1800;
    manifest.duration100ns = 300300000;
    manifest.nativeEvaluations = 1800;
    manifest.verifiedNeuralFrames = 1800;
    manifest.observedFeature18Evaluations = 60;
    manifest.feature18Created = true;
    manifest.feature18ArmedBeforeCapture = true;
    manifest.upscaling = false;
    return manifest;
}

void cache_key_changes_for_every_material_input_test()
{
    NeuralCacheIdentity base;
    base.sourceDigest = std::string(64, 'a');
    base.width = 1920;
    base.height = 1080;
    base.applicationVersion = "0.12.0";
    base.gpuPath = "rtx50";
    base.runtimeDigest = std::string(64, 'b');
    base.quality = "DLAA";
    base.upscaling = false;

    const std::string key = BuildNeuralCacheKey(base);
    CHECK_EQ(size_t{64}, key.size());
    auto changed = base;
    changed.sourceDigest = std::string(64, 'c');
    CHECK(key != BuildNeuralCacheKey(changed));
    changed = base;
    changed.width = 2560;
    CHECK(key != BuildNeuralCacheKey(changed));
    changed = base;
    changed.applicationVersion = "0.12.1";
    CHECK(key != BuildNeuralCacheKey(changed));
    changed = base;
    changed.gpuPath = "rtx40";
    CHECK(key != BuildNeuralCacheKey(changed));
    changed = base;
    changed.runtimeDigest = std::string(64, 'd');
    CHECK(key != BuildNeuralCacheKey(changed));
    changed = base;
    changed.upscaling = true;
    CHECK(key != BuildNeuralCacheKey(changed));
}

void runtime_digest_is_order_independent_byte_sensitive_and_rejects_duplicates_test()
{
    TempDirectory fixture;
    WriteBytes(fixture.Path() / L"a.dll", "runtime-a");
    WriteBytes(fixture.Path() / L"b.dll", "runtime-b");
    constexpr std::array<std::wstring_view, 2> forward{L"a.dll", L"b.dll"};
    constexpr std::array<std::wstring_view, 2> reverse{L"b.dll", L"a.dll"};
    const auto first = BuildRuntimeDigest(fixture.Path(), forward);
    const auto reordered = BuildRuntimeDigest(fixture.Path(), reverse);
    CHECK(first.has_value());
    CHECK_EQ(first, reordered);

    WriteBytes(fixture.Path() / L"b.dll", "runtime-b-changed");
    const auto changed = BuildRuntimeDigest(fixture.Path(), forward);
    CHECK(changed.has_value());
    CHECK(first != changed);

    constexpr std::array<std::wstring_view, 2> duplicate{L"a.dll", L"A.DLL"};
    CHECK(!BuildRuntimeDigest(fixture.Path(), duplicate).has_value());
    constexpr std::array<std::wstring_view, 1> traversal{L"..\\outside.dll"};
    CHECK(!BuildRuntimeDigest(fixture.Path(), traversal).has_value());
}

void manifest_round_trip_rejects_partial_duplicate_and_unknown_state_test()
{
    auto manifest = CompleteRenderManifest();
    manifest.state = NeuralCacheState::Complete;
    manifest.neuralDigest = std::string(64, 'c');
    const std::string serialized = SerializeNeuralCacheManifest(manifest);
    const auto parsed = ParseNeuralCacheManifest(serialized);
    CHECK(parsed.has_value());
    if (parsed) CHECK_EQ(manifest, *parsed);

    std::string duplicate = serialized;
    const size_t end = duplicate.rfind('}');
    duplicate.insert(end, ",\"state\":\"complete\"");
    CHECK(!ParseNeuralCacheManifest(duplicate).has_value());

    std::string unknown = serialized;
    const size_t state = unknown.find("\"complete\"");
    CHECK(state != std::string::npos);
    if (state != std::string::npos) unknown.replace(state, 10, "\"paused\"");
    CHECK(!ParseNeuralCacheManifest(unknown).has_value());

    manifest.state = NeuralCacheState::Staging;
    CHECK(!IsReusableNeuralCacheManifest(manifest));
    manifest.state = NeuralCacheState::Complete;
    manifest.upscaling = true;
    CHECK(!IsReusableNeuralCacheManifest(manifest));
    manifest.upscaling = false;
    manifest.verifiedNeuralFrames = manifest.frameCount - 1;
    CHECK(!IsReusableNeuralCacheManifest(manifest));
    manifest.verifiedNeuralFrames = manifest.frameCount;
    manifest.feature18ArmedBeforeCapture = false;
    CHECK(!IsReusableNeuralCacheManifest(manifest));
}

void source_and_render_promotion_are_hash_validated_and_immutable_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    CHECK(manager.Valid());

    const std::string sourceKey(64, '1');
    const auto sourceStaging = manager.BeginSourceStaging(sourceKey);
    CHECK(sourceStaging.has_value());
    if (!sourceStaging) return;
    WriteBytes(*sourceStaging / L"source.mkv", "source-video-audio");

    NeuralCacheManifest sourceManifest;
    sourceManifest.kind = NeuralCacheEntryKind::Source;
    sourceManifest.state = NeuralCacheState::Staging;
    sourceManifest.encoder = "copy";
    sourceManifest.width = 1920;
    sourceManifest.height = 1080;
    sourceManifest.frameCount = 1800;
    sourceManifest.duration100ns = 300300000;
    CHECK(manager.PromoteSource(sourceKey, *sourceStaging, sourceManifest));
    const auto source = manager.LookupSource(sourceKey);
    CHECK(source.has_value());
    if (!source) return;
    CHECK_EQ(std::string("source-video-audio"), ReadBytes(source->payloadPath));

    const std::string renderKey(64, '2');
    const auto renderStaging = manager.BeginRenderStaging(renderKey);
    CHECK(renderStaging.has_value());
    if (!renderStaging) return;
    WriteBytes(*renderStaging / L"neural.mkv", "neural-frames");
    auto renderManifest = CompleteRenderManifest();
    renderManifest.sourceDigest = source->manifest.sourceDigest;
    CHECK(manager.PromoteRender(renderKey, *renderStaging, renderManifest));
    const auto render = manager.LookupRender(renderKey);
    CHECK(render.has_value());
    if (!render) return;
    CHECK_EQ(std::string("neural-frames"), ReadBytes(render->payloadPath));

    const std::wstring stagingName=renderStaging->filename().wstring();
    const size_t nonceSeparator=stagingName.rfind(L'-');
    CHECK(nonceSeparator!=std::wstring::npos);
    const uint64_t nextNonce=std::stoull(stagingName.substr(nonceSeparator+1))+1;
    const auto collision=fixture.Path()/L"cache"/L"staging"/
        (L"invalid-cache-"+std::to_wstring(GetCurrentProcessId())+L"-"+
         std::to_wstring(nextNonce));
    std::error_code collisionError;
    CHECK(std::filesystem::create_directories(collision,collisionError));
    CHECK(!collisionError);
    CHECK(manager.Quarantine(*render));
    CHECK(std::filesystem::is_directory(collision));
    CHECK(!manager.LookupRender(renderKey).has_value());

    const auto restoredStaging = manager.BeginRenderStaging(renderKey);
    CHECK(restoredStaging.has_value());
    if (!restoredStaging) return;
    WriteBytes(*restoredStaging / L"neural.mkv", "neural-frames");
    CHECK(manager.PromoteRender(renderKey, *restoredStaging, renderManifest));
    const auto restored = manager.LookupRender(renderKey);
    CHECK(restored.has_value());
    if (!restored) return;

    const auto replacement = manager.BeginRenderStaging(renderKey);
    CHECK(replacement.has_value());
    if (replacement) {
        WriteBytes(*replacement / L"neural.mkv", "must-not-replace-valid-cache");
        CHECK(manager.PromoteRender(renderKey, *replacement, renderManifest));
    }
    CHECK_EQ(std::string("neural-frames"), ReadBytes(restored->payloadPath));

    WriteBytes(restored->payloadPath, "tampered");
    CHECK(!manager.LookupRender(renderKey).has_value());

    const auto repairStaging=manager.BeginRenderStaging(renderKey);
    CHECK(repairStaging.has_value());
    if(!repairStaging)return;
    WriteBytes(*repairStaging/L"neural.mkv","repaired-neural-frames");
    const std::wstring repairName=repairStaging->filename().wstring();
    const size_t repairSeparator=repairName.rfind(L'-');
    CHECK(repairSeparator!=std::wstring::npos);
    const uint64_t repairMoveNonce=std::stoull(repairName.substr(repairSeparator+1))+1;
    const auto repairCollision=fixture.Path()/L"cache"/L"staging"/
        (L"invalid-existing-"+std::to_wstring(GetCurrentProcessId())+L"-"+
         std::to_wstring(repairMoveNonce));
    std::error_code repairCollisionError;
    CHECK(std::filesystem::create_directories(repairCollision,repairCollisionError));
    CHECK(!repairCollisionError);
    CHECK(manager.PromoteRender(renderKey,*repairStaging,renderManifest));
    const auto repaired=manager.LookupRender(renderKey);
    CHECK(repaired.has_value());
    if(repaired)CHECK_EQ(std::string("repaired-neural-frames"),ReadBytes(repaired->payloadPath));
    CHECK(std::filesystem::is_directory(repairCollision));
}

void interrupted_staging_is_never_reusable_and_clear_stays_inside_root_test()
{
    TempDirectory fixture;
    const auto cacheRoot = fixture.Path() / L"cache";
    const auto neighbor = fixture.Path() / L"keep.txt";
    WriteBytes(neighbor, "keep");
    NeuralCacheManager manager(cacheRoot);
    const std::string key(64, '3');
    const auto staging = manager.BeginRenderStaging(key);
    CHECK(staging.has_value());
    if (staging) WriteBytes(*staging / L"neural.mkv", "partial");
    CHECK(!manager.LookupRender(key).has_value());
    CHECK(manager.SizeBytes() >= 7);
    CHECK(manager.Clear());
    CHECK(std::filesystem::is_regular_file(neighbor));
    CHECK_EQ(std::string("keep"), ReadBytes(neighbor));
    CHECK_EQ(uintmax_t{0}, manager.SizeBytes());

    NeuralCacheManager unsafe(std::filesystem::path(fixture.Path().root_path()));
    CHECK(!unsafe.Valid());
    CHECK(!unsafe.Clear());
}

void media_pipeline_arguments_are_exact_and_never_use_a_shell_test()
{
    const MaterializeRequest materialize{
        .videoUrl=L"https://r1.googlevideo.com/video?id=abc&token=one two",
        .audioUrl=L"https://r1.googlevideo.com/audio?id=abc&token=three",
        .output=LR"(C:\Cache Root\source.partial.mkv)"};
    const std::vector<std::wstring> expectedMaterialize{
        L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y", L"-xerror",
        L"-rw_timeout", L"10000000", L"-reconnect", L"1", L"-reconnect_on_network_error", L"1",
        L"-reconnect_on_http_error", L"429,5xx", L"-reconnect_delay_max", L"2",
        L"-reconnect_max_retries", L"3", L"-reconnect_delay_total_max", L"8", L"-respect_retry_after", L"0",
        L"-i", materialize.videoUrl,
        L"-rw_timeout", L"10000000", L"-reconnect", L"1", L"-reconnect_on_network_error", L"1",
        L"-reconnect_on_http_error", L"429,5xx", L"-reconnect_delay_max", L"2",
        L"-reconnect_max_retries", L"3", L"-reconnect_delay_total_max", L"8", L"-respect_retry_after", L"0",
        L"-i", materialize.audioUrl,
        L"-map", L"0:v:0", L"-map", L"1:a:0?", L"-c", L"copy",
        L"-f", L"matroska", materialize.output.wstring()};
    CHECK_EQ(expectedMaterialize, BuildMaterializeArguments(materialize));

    const EncoderSpec encoder{1920, 1080, 60000.0 / 1001.0, EncoderKind::HevcNvenc};
    const std::vector<std::wstring> arguments = BuildEncoderArguments(
        encoder, LR"(C:\Cache Root\neural.partial.mkv)");
    CHECK(std::find(arguments.begin(), arguments.end(), L"hevc_nvenc") != arguments.end());
    CHECK(std::find(arguments.begin(), arguments.end(), L"pipe:0") != arguments.end());
    CHECK(std::find(arguments.begin(), arguments.end(), L"cmd.exe") == arguments.end());
    CHECK(std::find(arguments.begin(), arguments.end(), L"powershell.exe") == arguments.end());
}

void encoder_frame_contract_and_fallback_policy_are_fail_closed_test()
{
    const EncoderSpec valid{2, 2, 30.0, EncoderKind::HevcNvenc};
    CHECK_EQ(size_t{16}, ExpectedBgraFrameBytes(valid));
    CHECK_EQ(size_t{0}, ExpectedBgraFrameBytes(EncoderSpec{0, 2, 30.0, EncoderKind::HevcNvenc}));
    CHECK_EQ(size_t{0}, ExpectedBgraFrameBytes(EncoderSpec{2, 2, 0.0, EncoderKind::HevcNvenc}));
    CHECK(ShouldRetryWithSoftware(EncoderKind::HevcNvenc, EncodeError::StartFailed));
    CHECK(ShouldRetryWithSoftware(EncoderKind::HevcNvenc, EncodeError::WriteFailed));
    CHECK(ShouldRetryWithSoftware(EncoderKind::HevcNvenc, EncodeError::FinishFailed));
    CHECK(!ShouldRetryWithSoftware(EncoderKind::HevcNvenc, EncodeError::Cancelled));
    CHECK(!ShouldRetryWithSoftware(EncoderKind::H264Software, EncodeError::StartFailed));
}

void materialization_failure_reports_diagnostics_without_signed_urls_test()
{
    TempDirectory fixture;
    std::filesystem::copy_file(CurrentExecutable(), fixture.Path() / L"ffmpeg.exe");
    const auto result = MediaMaterializer(fixture.Path()).Run({
        L"https://media.invalid/diagnostic-error?token=secret-value", {}, fixture.Path() / L"output.mkv"}, {});
    CHECK(!result.ok);
    CHECK_EQ(MaterializeError::ProcessFailed, result.error);
    CHECK(result.detail.find(L"Connection reset") != std::wstring::npos);
    CHECK(result.detail.find(L"https://") == std::wstring::npos);
    CHECK(result.detail.find(L"secret-value") == std::wstring::npos);
    CHECK(result.detail.size() < 2200);
}

void materialization_discards_oversized_diagnostic_url_fragments_test()
{
    TempDirectory fixture;
    std::filesystem::copy_file(CurrentExecutable(), fixture.Path() / L"ffmpeg.exe");
    const auto result = MediaMaterializer(fixture.Path()).Run({
        L"https://media.invalid/diagnostic-overflow", {}, fixture.Path() / L"output.mkv"}, {});
    CHECK(!result.ok);
    CHECK_EQ(MaterializeError::ProcessFailed, result.error);
    CHECK(result.detail.find(L"signed-secret") == std::wstring::npos);
    CHECK(result.detail.find(L"https://") == std::wstring::npos);
}

void owned_media_pipeline_materializes_encodes_probes_and_cancels_test()
{
    TempDirectory fixture;
    const auto executable = CurrentExecutable();
    std::filesystem::copy_file(executable, fixture.Path() / L"ffmpeg.exe");
    std::filesystem::copy_file(executable, fixture.Path() / L"ffprobe.exe");

    MediaMaterializer materializer(fixture.Path());
    const auto source = fixture.Path() / L"source.partial.mkv";
    const MaterializeResult materialized = materializer.Run(MaterializeRequest{
        L"https://media.invalid/video", L"https://media.invalid/audio", source}, {});
    CHECK(materialized.ok);
    CHECK_EQ(MaterializeError::None, materialized.error);
    CHECK_EQ(std::string("materialized"), ReadBytes(source));

    RawVideoEncoder encoder(fixture.Path());
    const EncoderSpec spec{2, 2, 30.0, EncoderKind::HevcNvenc};
    const auto encoded = fixture.Path() / L"neural.partial.mkv";
    CHECK_EQ(EncodeError::None, encoder.Start(spec, encoded));
    const std::array<uint8_t, 15> shortFrame{};
    CHECK_EQ(EncodeError::InvalidFrame, encoder.WriteFrame(shortFrame));
    const std::array<uint8_t, 16> frame{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    CHECK_EQ(EncodeError::None, encoder.WriteFrame(frame));
    CHECK_EQ(EncodeError::None, encoder.Finish());
    CHECK_EQ(std::string(reinterpret_cast<const char*>(frame.data()), frame.size()),
             ReadBytes(encoded));

    const ProbeResult probe = ProbeMedia(fixture.Path(), encoded, {});
    CHECK(probe.ok);
    CHECK_EQ(uint32_t{2}, probe.width);
    CHECK_EQ(uint32_t{2}, probe.height);
    CHECK_EQ(uint64_t{1}, probe.frameCount);
    CHECK_EQ(int64_t{333333}, probe.duration100ns);
    CHECK(probe.decodedFinalFrame);

    std::stop_source stop;
    auto future = std::async(std::launch::async, [&] {
        return materializer.Run(MaterializeRequest{
            L"https://media.invalid/hang", L"", fixture.Path() / L"hang.mkv"},
            stop.get_token());
    });
    std::this_thread::sleep_for(50ms);
    stop.request_stop();
    CHECK(future.wait_for(2s) == std::future_status::ready);
    if (future.wait_for(0s) == std::future_status::ready) {
        const MaterializeResult cancelled = future.get();
        CHECK(!cancelled.ok);
        CHECK_EQ(MaterializeError::Cancelled, cancelled.error);
        CHECK(cancelled.detail.find(L"https://") == std::wstring::npos);
    }
}

void encoder_child_inherits_only_its_stdin_pipe_test()
{
    TempDirectory fixture;
    std::filesystem::copy_file(CurrentExecutable(), fixture.Path() / L"ffmpeg.exe");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE unrelated = CreateEventW(&security, TRUE, FALSE, nullptr);
    CHECK(unrelated != nullptr);
    const std::wstring value = std::to_wstring(reinterpret_cast<uintptr_t>(unrelated));
    CHECK(SetEnvironmentVariableW(L"DLSS_MEDIA_TEST_INHERIT_HANDLE", value.c_str()) != FALSE);

    RawVideoEncoder encoder(fixture.Path());
    const auto output = fixture.Path() / L"isolated.mkv";
    const EncoderSpec spec{2, 2, 30.0, EncoderKind::H264Software};
    CHECK_EQ(EncodeError::None, encoder.Start(spec, output));
    const std::array<uint8_t, 16> frame{};
    CHECK_EQ(EncodeError::None, encoder.WriteFrame(frame));
    CHECK_EQ(EncodeError::None, encoder.Finish());
    CHECK_EQ(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(unrelated, 0));

    SetEnvironmentVariableW(L"DLSS_MEDIA_TEST_INHERIT_HANDLE", nullptr);
    if (unrelated) CloseHandle(unrelated);
}

void cached_media_probe_reads_headers_without_redecoding_validated_video_test()
{
    TempDirectory fixture;
    std::filesystem::copy_file(CurrentExecutable(),fixture.Path()/L"ffprobe.exe");
    // No FFmpeg installed: opening an already hash-validated cache entry must
    // only inspect headers, not count/decode all frames or start an encoder.
    const auto probe=ProbeMedia(fixture.Path(),fixture.Path()/L"already-validated.mkv",{},
                                MediaProbeMode::CachedMetadata);
    CHECK(probe.ok);CHECK_EQ(uint32_t{2},probe.width);CHECK_EQ(uint32_t{2},probe.height);
    CHECK_EQ(int64_t{333333},probe.duration100ns);
    CHECK_EQ(uint64_t{0},probe.frameCount);CHECK(!probe.decodedFinalFrame);
    std::stop_source stop;stop.request_stop();
    CHECK(!ProbeMedia(fixture.Path(),fixture.Path()/L"already-validated.mkv",stop.get_token(),
                      MediaProbeMode::CachedMetadata).ok);
}

void probe_child_inherits_only_its_output_pipe_test()
{
    TempDirectory fixture;
    std::filesystem::copy_file(CurrentExecutable(), fixture.Path() / L"ffmpeg.exe");
    std::filesystem::copy_file(CurrentExecutable(), fixture.Path() / L"ffprobe.exe");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE unrelated = CreateEventW(&security, TRUE, FALSE, nullptr);
    CHECK(unrelated != nullptr);
    const std::wstring value = std::to_wstring(reinterpret_cast<uintptr_t>(unrelated));
    CHECK(SetEnvironmentVariableW(L"DLSS_MEDIA_TEST_INHERIT_HANDLE", value.c_str()) != FALSE);

    const ProbeResult probe = ProbeMedia(fixture.Path(), fixture.Path() / L"isolated.mkv", {});
    CHECK(probe.ok);
    CHECK_EQ(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(unrelated, 0));

    SetEnvironmentVariableW(L"DLSS_MEDIA_TEST_INHERIT_HANDLE", nullptr);
    if (unrelated) CloseHandle(unrelated);
}

void encoder_blocked_write_is_interrupted_by_stop_test()
{
    TempDirectory fixture;const auto helper=fixture.Path();
    std::filesystem::copy_file(CurrentExecutable(),helper/L"ffmpeg.exe");
    RawVideoEncoder encoder(helper);EncoderSpec spec{1024,1024,30.0,EncoderKind::H264Software};
    CHECK_EQ(EncodeError::None,encoder.Start(spec,fixture.Path()/L"hang-output.mkv"));
    std::vector<uint8_t> frame(ExpectedBgraFrameBytes(spec));std::stop_source stop;
    auto write=std::async(std::launch::async,[&]{return encoder.WriteFrame(frame,stop.get_token());});
    std::this_thread::sleep_for(100ms);stop.request_stop();
    CHECK_EQ(std::future_status::ready,write.wait_for(3s));
    if(write.wait_for(0s)==std::future_status::ready)CHECK_EQ(EncodeError::Cancelled,write.get());
}

std::vector<OfflineDecodedFrame> FiveOfflineFrames()
{
    constexpr std::array<int64_t,5> timestamps{0,333333,666666,999999,1333332};
    std::vector<OfflineDecodedFrame> frames;
    for(size_t index=0;index<timestamps.size();++index)
        frames.push_back(OfflineDecodedFrame{{uint8_t(index),0,0,255},timestamps[index],index==0});
    return frames;
}

class FakeOfflineSource final : public IFrameSource {
public:
    explicit FakeOfflineSource(std::vector<OfflineDecodedFrame> frames=FiveOfflineFrames())
        : frames_(std::move(frames)) {}
    bool Open(const std::filesystem::path&,std::stop_token stop) override
    { ++opens;index_=0;return !stop.stop_requested(); }
    void Close() override { ++closes; }
    OfflineFrameRead Read(OfflineDecodedFrame& frame,std::stop_token stop) override
    {
        if(stop.stop_requested())return OfflineFrameRead::Cancelled;
        if(index_>=frames_.size())return OfflineFrameRead::EndOfStream;
        frame=frames_[index_++];return OfflineFrameRead::FrameReady;
    }
    int opens{};int closes{};
private:
    std::vector<OfflineDecodedFrame> frames_;size_t index_{};
};

class FakeNeuralEvaluator final : public INeuralFrameEvaluator {
public:
    bool Initialize(HWND,uint32_t,uint32_t,double) override { initialized=true;return true; }
    bool Submit(const OfflineDecodedFrame& frame,bool temporalReset,bool capture,
                std::vector<uint8_t>& bgra) override
    {
        submitted.push_back(frame.timestamp100ns);resets.push_back(temporalReset);
        if(!capture){if(++primeSubmissions>=2)featureCreated=true;if(featureCreated)++evaluations;return true;}
        ++captureSubmissions;
        if(failCaptureAt&&captureSubmissions==*failCaptureAt)return false;
        if(!featureCreated)return false;
        ++evaluations;bgra=frame.bgra;captured.push_back(frame.timestamp100ns);return true;
    }
    bool FeatureCreated() const override { return featureCreated; }
    uint64_t EvaluationCount() const override { return evaluations; }
    void ResetTemporal() override { ++temporalResets; }
    bool initialized{};bool featureCreated{};uint64_t evaluations{};int primeSubmissions{};
    int captureSubmissions{};int temporalResets{};std::optional<int> failCaptureAt;
    std::vector<int64_t> submitted,captured;std::vector<bool> resets;
};

class FakeFrameEncoder final : public IFrameEncoder {
public:
    EncodeError Start(const EncoderSpec& spec,const std::filesystem::path&) override
    {
        starts.push_back(spec.kind);current=spec.kind;attempts.emplace_back();
        if(spec.kind==EncoderKind::HevcNvenc&&failNvencStart)return EncodeError::StartFailed;
        active=true;return EncodeError::None;
    }
    EncodeError WriteFrame(std::span<const uint8_t> frame,std::stop_token stop) override
    {
        if(stop.stop_requested())return EncodeError::Cancelled;
        if(!active)return EncodeError::WriteFailed;
        if(current==EncoderKind::HevcNvenc&&failNvencWriteAt&&
           attempts.back().size()==*failNvencWriteAt){active=false;return EncodeError::WriteFailed;}
        attempts.back().emplace_back(frame.begin(),frame.end());return EncodeError::None;
    }
    EncodeError Finish(std::stop_token stop) override
    { ++finishes;active=false;return stop.stop_requested()?EncodeError::Cancelled:EncodeError::None; }
    void Cancel() override { ++cancels;active=false; }
    bool failNvencStart{};std::optional<size_t> failNvencWriteAt;bool active{};
    EncoderKind current{EncoderKind::HevcNvenc};int finishes{};int cancels{};
    std::vector<EncoderKind> starts;std::vector<std::vector<std::vector<uint8_t>>> attempts;
};

std::string ValidNeuralEvidence()
{
    return "EnableHooks=2: NGX hooks only\nprivate feature-18 GPU ordering active\n"
           "active settings: upscaling=OFF\nfeature 18 created\n"
           "inline feature 18 evaluation succeeded evaluation count=5\n";
}

std::string NeuralEvidenceWithCount(uint64_t count)
{
    return "EnableHooks=2: NGX hooks only\nprivate feature-18 GPU ordering active\n"
           "active settings: upscaling=OFF\nfeature 18 created\n"
           "inline feature 18 evaluation succeeded evaluation count="+
           std::to_string(count)+"\n";
}

std::function<std::string()> AdvancingNeuralEvidence()
{
    return [calls=0]()mutable{
        ++calls;
        return NeuralEvidenceWithCount(1u+uint64_t(calls-1)*4u);
    };
}

NeuralRenderRequest OfflineRequest(const std::filesystem::path& directory)
{
    NeuralRenderRequest request;request.sourcePath=directory/L"source.mkv";
    request.stagingVideoPath=directory/L"neural.partial.mkv";request.width=1;request.height=1;
    request.fps=30.0;request.durationSeconds=5.0/30.0;return request;
}

void offline_job_primes_feature_then_restarts_source_and_captures_every_frame_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    int evidenceCalls=0;
    OfflineNeuralRenderer job(source,evaluator,encoder,[&]{
        ++evidenceCalls;
        return evidenceCalls==1
            ? std::string("EnableHooks=2: NGX hooks only\nprivate feature-18 GPU ordering active\n"
                          "active settings: upscaling=OFF\nfeature 18 created\n"
                          "inline feature 18 evaluation succeeded evaluation count=1\n")
            : ValidNeuralEvidence();
    });
    const NeuralRenderResult result=job.Run(OfflineRequest(fixture.Path()),{},{});
    CHECK(result.ok);CHECK(!result.cancelled);CHECK_EQ(uint64_t{5},result.frameCount);
    CHECK_EQ(uint64_t{5},result.nativeEvaluations);CHECK_EQ(2,source.opens);
    CHECK_EQ(uint64_t{5},result.verifiedNeuralFrames);
    CHECK(result.feature18ArmedBeforeCapture);
    CHECK_EQ(2,evaluator.primeSubmissions);CHECK_EQ(size_t{5},evaluator.captured.size());
    CHECK_EQ(int64_t{0},evaluator.captured.front());CHECK_EQ(int64_t{1333332},evaluator.captured.back());
    CHECK_EQ(size_t{1},encoder.attempts.size());CHECK_EQ(size_t{5},encoder.attempts.back().size());
    CHECK_EQ(2,evidenceCalls);
}

void offline_job_rejects_when_feature18_receipt_does_not_advance_after_capture_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source,evaluator,encoder,[]{return ValidNeuralEvidence();});
    const auto result=job.Run(OfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(1,encoder.finishes);
}

void offline_job_rejects_any_frame_without_a_neural_evaluation_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    evaluator.failCaptureAt=3;
    OfflineNeuralRenderer job(source,evaluator,encoder,[]{return ValidNeuralEvidence();});
    const auto result=job.Run(OfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(size_t{1},encoder.starts.size());
    CHECK_EQ(2,source.opens);CHECK(encoder.cancels>0);
}

void offline_job_rejects_when_inline_interception_was_not_armed_before_capture_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    int calls=0;
    OfflineNeuralRenderer job(source,evaluator,encoder,[&]{
        ++calls;
        if(calls==1)return std::string(
            "active settings: upscaling=OFF\nfeature 18 created\n"
            "inline feature 18 evaluation succeeded evaluation count=5\n");
        return ValidNeuralEvidence();
    });
    const auto result=job.Run(OfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(1,calls);
    CHECK_EQ(size_t{0},encoder.starts.size());
}

void offline_job_rejects_non_monotonic_source_timestamps_test()
{
    TempDirectory fixture;auto frames=FiveOfflineFrames();frames[3].timestamp100ns=frames[2].timestamp100ns;
    FakeOfflineSource source(std::move(frames));FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source,evaluator,encoder,[]{return ValidNeuralEvidence();});
    const auto result=job.Run(OfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK(encoder.cancels>0);
}

void offline_job_reports_monotonic_progress_and_smoothed_eta_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    auto now=std::chrono::steady_clock::time_point{};
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence(),
        [&]{now+=100ms;return now;});
    std::vector<NeuralRenderProgress> progress;
    const auto result=job.Run(OfflineRequest(fixture.Path()),[&](const auto& value){progress.push_back(value);},{});
    CHECK(result.ok);CHECK(!progress.empty());
    for(size_t index=1;index<progress.size();++index){
        CHECK(progress[index].completedFrames>=progress[index-1].completedFrames);
        CHECK(progress[index].elapsed>=progress[index-1].elapsed);
    }
    const auto rendered=std::ranges::find_if(progress,[](const auto& value){
        return value.phase==NeuralRenderPhase::NeuralRendering&&value.completedFrames==5;});
    CHECK(rendered!=progress.end());if(rendered!=progress.end())CHECK_EQ(0ms,rendered->estimatedRemaining);
}

void offline_job_cancel_stops_before_promotion_and_marks_result_cancelled_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    std::stop_source stop;OfflineNeuralRenderer job(source,evaluator,encoder,[]{return ValidNeuralEvidence();});
    const auto result=job.Run(OfflineRequest(fixture.Path()),[&](const auto& progress){
        if(progress.completedFrames==2)stop.request_stop();},stop.get_token());
    CHECK(!result.ok);CHECK(result.cancelled);CHECK_EQ(0,encoder.finishes);CHECK(encoder.cancels>0);
}

void offline_job_nvenc_start_failure_restarts_from_frame_zero_with_h264_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    encoder.failNvencStart=true;OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(OfflineRequest(fixture.Path()),{},{});
    CHECK(result.ok);CHECK_EQ(EncoderKind::H264Software,result.encoder);
    CHECK_EQ(std::vector<EncoderKind>({EncoderKind::HevcNvenc,EncoderKind::H264Software}),encoder.starts);
    CHECK_EQ(3,source.opens);CHECK_EQ(size_t{5},encoder.attempts.back().size());
    CHECK_EQ(uint8_t{0},encoder.attempts.back().front().front());
}

void offline_job_nvenc_write_failure_restarts_the_whole_sequence_with_h264_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    encoder.failNvencWriteAt=2;OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    std::vector<NeuralRenderProgress> progress;
    const auto result=job.Run(OfflineRequest(fixture.Path()),[&](const auto& value){progress.push_back(value);},{});
    CHECK(result.ok);CHECK_EQ(EncoderKind::H264Software,result.encoder);CHECK_EQ(3,source.opens);
    CHECK_EQ(size_t{2},encoder.attempts.front().size());CHECK_EQ(size_t{5},encoder.attempts.back().size());
    CHECK_EQ(uint8_t{0},encoder.attempts.back().front().front());
    for(size_t index=1;index<progress.size();++index){
        CHECK(progress[index].completedFrames>=progress[index-1].completedFrames);
        CHECK(progress[index].bytes>=progress[index-1].bytes);
    }
}

void offline_job_rejects_retry_when_only_abandoned_attempt_advanced_feature18_receipt_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    encoder.failNvencWriteAt=2;int calls=0;
    OfflineNeuralRenderer job(source,evaluator,encoder,[&]{
        ++calls;return NeuralEvidenceWithCount(calls==1?1:5);
    });
    const auto result=job.Run(OfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(3,calls);
    CHECK_EQ(std::vector<EncoderKind>({EncoderKind::HevcNvenc,EncoderKind::H264Software}),encoder.starts);
}

void offline_job_does_not_retry_a_temporal_render_from_an_arbitrary_frame_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    evaluator.failCaptureAt=4;OfflineNeuralRenderer job(source,evaluator,encoder,[]{return ValidNeuralEvidence();});
    const auto result=job.Run(OfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK_EQ(std::vector<EncoderKind>({EncoderKind::HevcNvenc}),encoder.starts);
    CHECK_EQ(2,source.opens);
}

void reshade_evidence_requires_upscaling_off_feature18_create_and_evaluate_test()
{
    const auto valid=ParseNeuralRuntimeEvidence(ValidNeuralEvidence());
    CHECK(valid.Valid());CHECK(valid.upscalingOff);CHECK(valid.feature18Created);
    CHECK(valid.inlineInterceptionContract);
    CHECK(valid.feature18Evaluated);CHECK_EQ(uint64_t{5},valid.highestObservedEvaluation);
    const auto productionLog=ParseNeuralRuntimeEvidence(
        "EnableHooks=2: NGX hooks only\nprivate feature-18 GPU ordering active\n"
        "DLSS5 active settings: upscaling=OFF\nfeature 18 created via the signed snippet\n"
        "inline feature 18 evaluation succeeded (count=17, NR input 1920x1080)\n");
    CHECK(productionLog.Valid());CHECK_EQ(uint64_t{17},productionLog.highestObservedEvaluation);
    CHECK(!ParseNeuralRuntimeEvidence("feature 18 created\ninline feature 18 evaluation succeeded\n").Valid());
    CHECK(!ParseNeuralRuntimeEvidence("active settings: upscaling=OFF\nfeature 18 created\n").Valid());
    CHECK(!ParseNeuralRuntimeEvidence(
        "active settings: upscaling=OFF\nfeature 18 created\n"
        "inline feature 18 evaluation succeeded evaluation count=5\n").Valid());
}

void reshade_evidence_rejects_a_later_feature18_failure_in_the_same_job_segment_test()
{
    const auto evidence=ParseNeuralRuntimeEvidence(
        ValidNeuralEvidence()+"feature 18 evaluation failed hr=0x80004005\n");
    CHECK(evidence.laterFailure);CHECK(!evidence.Valid());
}

void reshade_evidence_rejects_any_failure_or_passthrough_in_the_job_segment_test()
{
    const auto recovered=ParseNeuralRuntimeEvidence(
        "feature 18 evaluation failed hr=0x80004005\n"+ValidNeuralEvidence());
    CHECK(recovered.laterFailure);CHECK(!recovered.Valid());
    const auto passthrough=ParseNeuralRuntimeEvidence(
        ValidNeuralEvidence()+"NR workset pool exhausted; preserving game output\n");
    CHECK(passthrough.laterFailure);CHECK(!passthrough.Valid());
}

class FakeSynchronizedSource final : public ISynchronizedFrameSource {
public:
    explicit FakeSynchronizedSource(std::vector<int64_t> timestamps)
    {
        for(const auto timestamp:timestamps)
            frames.push_back(VideoFrame{{uint8_t(timestamp/333333),0,0,255},timestamp,timestamp==0});
        if(!frames.empty())duration=double(frames.back().timestamp100ns+333333)/10000000.0;
    }
    bool Open(const std::filesystem::path&,std::stop_token stop) override
    { ++opens;index=0;return !failOpen&&!stop.stop_requested(); }
    void Close() override { ++closes; }
    VideoReadResult Read(VideoFrame& frame,std::stop_token stop) override
    {
        if(stop.stop_requested())return VideoReadResult::Cancelled;
        if(notReadyReads>0){--notReadyReads;return VideoReadResult::NotReady;}
        if(index>=frames.size())return VideoReadResult::EndOfStream;
        frame=frames[index++];return VideoReadResult::FrameReady;
    }
    bool SeekSeconds(double seconds) override
    {
        ++seeks;if(failNextSeek){failNextSeek=false;return false;}
        const int64_t target=static_cast<int64_t>(seconds*10000000.0);
        index=0;while(index<frames.size()&&frames[index].timestamp100ns<target)++index;return true;
    }
    uint32_t Width() const override { return width; }
    uint32_t Height() const override { return height; }
    double FrameRate() const override { return fps; }
    double DurationSeconds() const override { return duration; }
    std::vector<VideoFrame> frames;size_t index{};uint32_t width{1},height{1};
    double fps{30.0},duration{};bool failOpen{},failNextSeek{};int opens{},closes{},seeks{},notReadyReads{};
};

void synchronized_seek_waits_for_decoder_startup_and_preserves_comparison_test()
{
    FakeSynchronizedSource original({0,333333,666666,999999});
    FakeSynchronizedSource neural({0,333333,666666,999999});
    SynchronizedPlayback playback(original,neural);CHECK(playback.Open(L"o",L"n",{}));
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    CHECK(playback.SetView(ComparisonView::Neural));playback.SetPaused(true);
    original.notReadyReads=2;neural.notReadyReads=4;
    CHECK(playback.SeekSeconds(0.066,{}));
    CHECK(playback.NeuralAvailable());CHECK(playback.Paused());
    CHECK_EQ(ComparisonView::Neural,playback.View());
    CHECK(playback.CurrentPair()!=nullptr);
    if(const auto* pair=playback.CurrentPair()){
        CHECK_EQ(int64_t{666666},pair->original.timestamp100ns);
        CHECK_EQ(int64_t{666666},pair->neural.timestamp100ns);
    }
    playback.SetPaused(false);
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
}

void synchronized_seek_at_end_selects_last_frame_test()
{
    FakeSynchronizedSource original({0,333333,666666,999999});
    FakeSynchronizedSource neural({0,333333,666666,999999});
    SynchronizedPlayback playback(original,neural);CHECK(playback.Open(L"o",L"n",{}));
    CHECK(playback.SeekSeconds(original.duration,{}));
    CHECK(playback.VisibleFrame()!=nullptr);
    if(playback.VisibleFrame())CHECK_EQ(int64_t{999999},playback.VisibleFrame()->timestamp100ns);
}

void synchronized_seek_handles_container_duration_padding_at_end_test()
{
    for(const double neuralDuration:{0.1333332,0.16}){
        FakeSynchronizedSource original({0,333333,666666,999999});
        FakeSynchronizedSource neural({0,333333,666666,999999});
        original.duration=0.16;neural.duration=neuralDuration;
        SynchronizedPlayback playback(original,neural);CHECK(playback.Open(L"o",L"n",{}));
        CHECK(playback.SeekSeconds(0.16,{}));
        CHECK(playback.VisibleFrame()!=nullptr);
        if(playback.VisibleFrame())CHECK_EQ(int64_t{999999},playback.VisibleFrame()->timestamp100ns);
    }
}

void synchronized_seek_wait_can_be_cancelled_test()
{
    FakeSynchronizedSource original({0,333333});FakeSynchronizedSource neural({0,333333});
    SynchronizedPlayback playback(original,neural);CHECK(playback.Open(L"o",L"n",{}));
    original.notReadyReads=100000;
    std::stop_source stop;
    auto seek=std::async(std::launch::async,[&]{return playback.SeekSeconds(0,stop.get_token());});
    std::this_thread::sleep_for(25ms);stop.request_stop();
    CHECK_EQ(std::future_status::ready,seek.wait_for(1s));CHECK(!seek.get());
    CHECK(playback.VisibleFrame()==nullptr);
}

void synchronized_playback_starts_original_and_switches_same_timestamp_test()
{
    FakeSynchronizedSource original({0,333333});FakeSynchronizedSource neural({0,333333});
    SynchronizedPlayback playback(original,neural);
    CHECK(playback.Open(L"original.mkv",L"neural.mkv",{}));
    CHECK_EQ(ComparisonView::Original,playback.View());
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    CHECK(playback.VisibleFrame()!=nullptr);if(playback.VisibleFrame())CHECK_EQ(int64_t{0},playback.VisibleFrame()->timestamp100ns);
    CHECK(playback.SetView(ComparisonView::Neural));
    CHECK(playback.VisibleFrame()!=nullptr);if(playback.VisibleFrame())CHECK_EQ(int64_t{0},playback.VisibleFrame()->timestamp100ns);
}

void synchronized_playback_advances_both_streams_under_one_clock_test()
{
    FakeSynchronizedSource original({0,333333,666666});FakeSynchronizedSource neural({0,333333,666666});
    SynchronizedPlayback playback(original,neural);CHECK(playback.Open(L"o",L"n",{}));
    for(const int64_t timestamp:{int64_t{0},int64_t{333333},int64_t{666666}}){
        CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        const auto* pair=playback.CurrentPair();CHECK(pair!=nullptr);
        if(pair){CHECK_EQ(timestamp,pair->timestamp100ns);CHECK_EQ(pair->original.timestamp100ns,pair->neural.timestamp100ns);}
    }
}

void synchronized_playback_seek_commits_only_after_both_streams_reach_target_test()
{
    FakeSynchronizedSource original({0,333333,666666,999999});
    FakeSynchronizedSource neural({0,333333,666666,999999});
    SynchronizedPlayback playback(original,neural);CHECK(playback.Open(L"o",L"n",{}));
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    neural.failNextSeek=true;CHECK(!playback.SeekSeconds(0.066,{}));
    CHECK(playback.VisibleFrame()==nullptr);
    CHECK_EQ(SynchronizedReadResult::Error,playback.ReadNextAvailable({}));
    CHECK(playback.Open(L"o",L"n",{}));
    CHECK(playback.SeekSeconds(0.066,{}));
    CHECK(playback.VisibleFrame()!=nullptr);if(playback.VisibleFrame())CHECK_EQ(int64_t{666666},playback.VisibleFrame()->timestamp100ns);
}

void synchronized_playback_refuses_a_mismatched_neural_frame_beyond_one_frame_test()
{
    FakeSynchronizedSource original({0});FakeSynchronizedSource neural({666666});
    neural.duration=original.duration;
    SynchronizedPlayback playback(original,neural);CHECK(playback.Open(L"o",L"n",{}));
    CHECK_EQ(SynchronizedReadResult::OutOfSync,playback.ReadNextAvailable({}));
    CHECK(playback.VisibleFrame()==nullptr);CHECK(!playback.SetView(ComparisonView::Neural));
}

void synchronized_playback_rejects_incompatible_cached_stream_metadata_test()
{
    FakeSynchronizedSource original({0,333333});FakeSynchronizedSource neural({0,333333});
    neural.fps=29.0;SynchronizedPlayback playback(original,neural);
    CHECK(!playback.Open(L"o",L"n",{}));CHECK(!playback.NeuralAvailable());
    neural.fps=30.0;neural.width=2;
    CHECK(!playback.Open(L"o",L"n",{}));CHECK(!playback.NeuralAvailable());
}

void synchronized_playback_pause_step_and_eos_apply_to_both_streams_test()
{
    FakeSynchronizedSource original({0,333333});FakeSynchronizedSource neural({0,333333});
    SynchronizedPlayback playback(original,neural);CHECK(playback.Open(L"o",L"n",{}));
    playback.SetPaused(true);CHECK_EQ(SynchronizedReadResult::NotReady,playback.ReadNextAvailable({}));
    CHECK(playback.Step());CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    CHECK_EQ(SynchronizedReadResult::NotReady,playback.ReadNextAvailable({}));
    CHECK(playback.Step());CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    CHECK(playback.Step());CHECK_EQ(SynchronizedReadResult::EndOfStream,playback.ReadNextAvailable({}));
}

void synchronized_playback_original_only_mode_remains_available_after_cancel_test()
{
    FakeSynchronizedSource original({0});FakeSynchronizedSource neural({0});neural.failOpen=true;
    SynchronizedPlayback playback(original,neural);CHECK(!playback.Open(L"o",L"n",{}));
    CHECK(playback.Open(L"o",{},{}));
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    CHECK(!playback.SetView(ComparisonView::Neural));CHECK_EQ(ComparisonView::Original,playback.View());
}

int RunFakeMediaPipelineChild(int argc, wchar_t* argv[])
{
    const std::wstring name = CurrentExecutable().filename().wstring();
    std::vector<std::wstring_view> arguments;
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    std::array<wchar_t, 64> inheritedValue{};
    if (GetEnvironmentVariableW(L"DLSS_MEDIA_TEST_INHERIT_HANDLE", inheritedValue.data(),
                                static_cast<DWORD>(inheritedValue.size())) > 0) {
        const uintptr_t raw = static_cast<uintptr_t>(_wcstoui64(inheritedValue.data(), nullptr, 10));
        SetEvent(reinterpret_cast<HANDLE>(raw));
    }
    if (_wcsicmp(name.c_str(), L"ffprobe.exe") == 0) {
        const bool cached=std::ranges::any_of(arguments,[](std::wstring_view value){return value.ends_with(L"already-validated.mkv");});
        if(cached){
            if(std::find(arguments.begin(),arguments.end(),L"-count_frames")!=arguments.end())return 92;
            std::cout << "width=2\nheight=2\nduration=0.0333333\n";return 0;
        }
        std::cout << "width=2\nheight=2\nnb_read_frames=1\nduration=0.0333333\n";
        return 0;
    }
    if (_wcsicmp(name.c_str(), L"ffmpeg.exe") != 0) return 90;
    if (std::ranges::any_of(arguments, [](std::wstring_view value) {
            return value.find(L"/diagnostic-overflow") != std::wstring_view::npos;
        })) {
        std::string diagnostic = "https://redirect.invalid/video?token=";
        for (size_t index = 0; index < 32768; ++index) diagnostic += "signed-secret";
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), diagnostic.data(),
                  static_cast<DWORD>(diagnostic.size()), &written, nullptr);
        return 7;
    }
    if (std::ranges::any_of(arguments, [](std::wstring_view value) {
            return value.find(L"/diagnostic-error") != std::wstring_view::npos;
        })) {
        std::cerr << "Connection reset while reading https://media.invalid/diagnostic-error?token=secret-value\n";
        return 7;
    }
    const bool raw = std::find(arguments.begin(), arguments.end(), L"rawvideo") != arguments.end();
    const bool finalProbe = std::find(arguments.begin(), arguments.end(), L"-sseof") != arguments.end();
    const bool hang = std::ranges::any_of(arguments, [](std::wstring_view value) {
        return value.find(L"/hang") != std::wstring_view::npos ||
               value.find(L"hang-output") != std::wstring_view::npos;
    });
    if (hang) {
        Sleep(INFINITE);
        return 0;
    }
    if (finalProbe) return 0;
    if (arguments.empty()) return 91;
    const std::filesystem::path output(arguments.back());
    if (raw) {
        const std::string bytes{std::istreambuf_iterator<char>(std::cin),
                                std::istreambuf_iterator<char>()};
        WriteBytes(output, bytes);
    } else {
        WriteBytes(output, "materialized");
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    const std::wstring executableName = CurrentExecutable().filename().wstring();
    if (_wcsicmp(executableName.c_str(), L"ffmpeg.exe") == 0 ||
        _wcsicmp(executableName.c_str(), L"ffprobe.exe") == 0)
        return RunFakeMediaPipelineChild(argc, argv);
    cache_key_changes_for_every_material_input_test();
    runtime_digest_is_order_independent_byte_sensitive_and_rejects_duplicates_test();
    manifest_round_trip_rejects_partial_duplicate_and_unknown_state_test();
    source_and_render_promotion_are_hash_validated_and_immutable_test();
    interrupted_staging_is_never_reusable_and_clear_stays_inside_root_test();
    media_pipeline_arguments_are_exact_and_never_use_a_shell_test();
    materialization_failure_reports_diagnostics_without_signed_urls_test();
    materialization_discards_oversized_diagnostic_url_fragments_test();
    encoder_frame_contract_and_fallback_policy_are_fail_closed_test();
    owned_media_pipeline_materializes_encodes_probes_and_cancels_test();
    encoder_child_inherits_only_its_stdin_pipe_test();
    cached_media_probe_reads_headers_without_redecoding_validated_video_test();
    probe_child_inherits_only_its_output_pipe_test();
    encoder_blocked_write_is_interrupted_by_stop_test();
    offline_job_primes_feature_then_restarts_source_and_captures_every_frame_test();
    offline_job_rejects_when_feature18_receipt_does_not_advance_after_capture_test();
    offline_job_rejects_any_frame_without_a_neural_evaluation_test();
    offline_job_rejects_when_inline_interception_was_not_armed_before_capture_test();
    offline_job_rejects_non_monotonic_source_timestamps_test();
    offline_job_reports_monotonic_progress_and_smoothed_eta_test();
    offline_job_cancel_stops_before_promotion_and_marks_result_cancelled_test();
    offline_job_nvenc_start_failure_restarts_from_frame_zero_with_h264_test();
    offline_job_nvenc_write_failure_restarts_the_whole_sequence_with_h264_test();
    offline_job_rejects_retry_when_only_abandoned_attempt_advanced_feature18_receipt_test();
    offline_job_does_not_retry_a_temporal_render_from_an_arbitrary_frame_test();
    reshade_evidence_requires_upscaling_off_feature18_create_and_evaluate_test();
    reshade_evidence_rejects_a_later_feature18_failure_in_the_same_job_segment_test();
    reshade_evidence_rejects_any_failure_or_passthrough_in_the_job_segment_test();
    synchronized_playback_starts_original_and_switches_same_timestamp_test();
    synchronized_playback_advances_both_streams_under_one_clock_test();
    synchronized_playback_seek_commits_only_after_both_streams_reach_target_test();
    synchronized_seek_waits_for_decoder_startup_and_preserves_comparison_test();
    synchronized_seek_at_end_selects_last_frame_test();
    synchronized_seek_handles_container_duration_padding_at_end_test();
    synchronized_seek_wait_can_be_cancelled_test();
    synchronized_playback_refuses_a_mismatched_neural_frame_beyond_one_frame_test();
    synchronized_playback_rejects_incompatible_cached_stream_metadata_test();
    synchronized_playback_pause_step_and_eos_apply_to_both_streams_test();
    synchronized_playback_original_only_mode_remains_available_after_cancel_test();

    if (test_support::failure_count != 0) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
