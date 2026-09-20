#include "NeuralCache.h"
#include "NeuralPreflight.h"
#include "LiveSessionPolicy.h"
#include "MediaPipeline.h"
#include "PlaybackTiming.h"
#include "NeuralSegmentIndex.h"
#include "OfflineNeuralRenderer.h"
#include "ResidentHelperPolicy.h"
#include "SynchronizedPlayback.h"
#include "TestSupport.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
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

    // Windows keeps a copied-and-launched image locked for a moment after the
    // child exits, so a single remove_all leaked one directory holding a 1 MB
    // fake ffmpeg.exe per run - 49 of them had accumulated. Retry briefly, and
    // sweep what earlier runs left behind so it cannot pile up again.
    ~TempDirectory()
    {
        std::error_code error;
        for (int attempt = 0; attempt < 40; ++attempt) {
            std::filesystem::remove_all(path_, error);
            if (!std::filesystem::exists(path_)) break;
            Sleep(25);
        }
        SweepAbandoned(path_.parent_path());
    }

    // Only directories from OTHER processes: several TempDirectory objects are
    // alive at once inside one test run, and deleting a live sibling here made
    // the suite flaky.
    static void SweepAbandoned(const std::filesystem::path& parent)
    {
        const std::wstring mine = L"DLSSVideoPlayer-NeuralCacheTests-" +
            std::to_wstring(GetCurrentProcessId()) + L"-";
        std::error_code error;
        for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end;
             it.increment(error)) {
            const std::wstring name = it->path().filename().wstring();
            if (name.rfind(L"DLSSVideoPlayer-NeuralCacheTests-", 0) != 0) continue;
            if (name.rfind(mine, 0) == 0) continue;
            std::error_code ignored;
            std::filesystem::remove_all(it->path(), ignored);
        }
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

int RunCacheRootProbe(std::wstring_view mode)
{
    const auto directory = CurrentExecutable().parent_path();
    const auto custom = directory / L"chosen-cache";
    NeuralCacheManager manager(mode == L"custom" || mode == L"invalid-custom"
        ? custom : std::filesystem::path{});
    if (mode == L"invalid-custom") {
        CHECK(!manager.Valid());
        CHECK(!std::filesystem::exists(directory / L"cache"));
        return test_support::failure_count ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    CHECK(manager.Valid());
    if (!manager.Valid()) return EXIT_FAILURE;
    std::filesystem::path expected = mode == L"custom" ? custom : directory / L"cache" / L"v1";
    if (mode == L"fallback") {
        PWSTR localAppData = nullptr;
        const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
        CHECK(SUCCEEDED(result) && localAppData);
        if (FAILED(result) || !localAppData) return EXIT_FAILURE;
        expected = std::filesystem::path(localAppData) / L"DLSSVideoPlayer" / L"NeuralCache" / L"v1";
        CoTaskMemFree(localAppData);
    }
    std::error_code error;
    CHECK(std::filesystem::equivalent(expected, manager.Root(), error));
    CHECK(!error);
    const auto staging = manager.BeginSourceStaging(std::string(64, '9'));
    CHECK(staging.has_value());
    if (staging) {
        WriteBytes(*staging / L"source.mkv", "portable-cache-probe");
        CHECK_EQ(std::string("portable-cache-probe"), ReadBytes(*staging / L"source.mkv"));
        // Remove only the unique staging directory created by this probe.
        std::filesystem::remove_all(*staging, error);
        CHECK(!error);
    }
    return test_support::failure_count ? EXIT_FAILURE : EXIT_SUCCESS;
}

void RunCacheRootChild(const TempDirectory& fixture, std::wstring_view mode)
{
    const auto executable = fixture.Path() / L"cache-probe.exe";
    std::filesystem::copy_file(CurrentExecutable(), executable);
    std::wstring arguments = L"\"" + executable.wstring() + L"\" --cache-root-probe " + std::wstring(mode);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    // A different working directory catches accidental CWD-relative defaults.
    const auto workingDirectory = CurrentExecutable().parent_path();
    CHECK(CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startup, &process));
    if (!process.hProcess) return;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    CHECK_EQ(DWORD{WAIT_OBJECT_0}, wait);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 99);
        WaitForSingleObject(process.hProcess, 10000);
    }
    DWORD exitCode = 99;
    CHECK(GetExitCodeProcess(process.hProcess, &exitCode));
    CloseHandle(process.hProcess);
    if (exitCode != EXIT_SUCCESS)
        std::wcerr << L"Cache root probe " << mode << L" exited with " << exitCode << L'\n';
    CHECK_EQ(DWORD{EXIT_SUCCESS}, exitCode);
}

void default_cache_is_writable_beside_executable_independent_of_working_directory_test()
{
    TempDirectory fixture;
    RunCacheRootChild(fixture, L"portable");
    CHECK(std::filesystem::is_directory(fixture.Path() / L"cache" / L"v1" / L"sources"));
}

void default_cache_falls_back_when_portable_directory_is_blocked_test()
{
    TempDirectory fixture;
    WriteBytes(fixture.Path() / L"cache", "keep");
    RunCacheRootChild(fixture, L"fallback");
    CHECK_EQ(std::string("keep"), ReadBytes(fixture.Path() / L"cache"));
}

void default_cache_falls_back_when_portable_layout_is_unusable_test()
{
    TempDirectory fixture;
    const auto portable = fixture.Path() / L"cache" / L"v1";
    std::filesystem::create_directories(portable);
    WriteBytes(portable / L"sources", "keep");
    RunCacheRootChild(fixture, L"fallback");
    CHECK_EQ(std::string("keep"), ReadBytes(portable / L"sources"));
}

void explicit_cache_root_remains_authoritative_test()
{
    TempDirectory fixture;
    RunCacheRootChild(fixture, L"custom");
    CHECK(!std::filesystem::exists(fixture.Path() / L"cache"));
}

void invalid_explicit_cache_root_does_not_silently_fall_back_test()
{
    TempDirectory fixture;
    WriteBytes(fixture.Path() / L"chosen-cache", "keep");
    RunCacheRootChild(fixture, L"invalid-custom");
    CHECK_EQ(std::string("keep"), ReadBytes(fixture.Path() / L"chosen-cache"));
}

// The player never promotes a render without its receipt: it builds the
// receipt JSON, writes receipt.json into staging and fails the render when it
// cannot, so a promotable fixture stages the same sidecar.
constexpr std::string_view kRenderReceipt = "{\"schema\":1,\"render\":\"fixture\"}\n";

void StageRenderReceipt(const std::filesystem::path& staging)
{
    WriteBytes(staging / L"receipt.json", kRenderReceipt);
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
    manifest.receiptDigest = Sha256Bytes(kRenderReceipt).value_or("");
    return manifest;
}

// The lookup is the whole reuse decision: a render is answered from the cache
// only under the key its identity builds, so an identity term that moves is a
// render that is re-made. The driver and model-store terms exist because
// neither moved before: gpuPath is a generation label, so a render made on one
// driver was served on every later one, and runtimeDigest covers the staged
// runtime directory only, never the weights the pass resolves out of the NGX
// core directory and the ProgramData model store.
// Every field of NeuralCacheIdentity has to reach BuildNeuralCacheKey, or a
// render made under one setting is served for another - the single failure the
// whole receipt design exists to prevent. The key is a hand-maintained field
// list, so nothing but this binding notices a field that was added and never
// keyed: it names all thirteen, and adding or removing one stops the suite
// compiling with an error that points here.
//
// Structured binding rather than a sizeof canary on purpose. sizeof moves with
// the standard library's std::string layout - MSVC's _ITERATOR_DEBUG_LEVEL
// changes it between Debug and Release - so a byte count fires for reasons
// that have nothing to do with the field list and gets bumped without thought.
//
// When this stops compiling: add the field to BuildNeuralCacheKey, add a case
// for it to the mutation loop below, then add its name here.
void cache_identity_field_list_is_pinned_test()
{
    const NeuralCacheIdentity identity{};
    const auto& [sourceDigest, width, height, applicationVersion, gpuPath,
                 runtimeDigest, quality, upscaling, settingsDigest, range,
                 guides, driverVersion, modelStoreDigest] = identity;
    // Referring to each name keeps /W4 quiet and makes the list a checklist a
    // reader can compare against BuildNeuralCacheKey line by line.
    CHECK(sourceDigest.empty() && applicationVersion.empty() && gpuPath.empty());
    CHECK(runtimeDigest.empty() && quality.empty() && settingsDigest.empty());
    CHECK(guides.empty() && driverVersion.empty() && modelStoreDigest.empty());
    CHECK_EQ(uint32_t{0}, width);
    CHECK_EQ(uint32_t{0}, height);
    CHECK(!upscaling);
    CHECK(range.Whole());
}

void published_render_is_not_reused_across_identity_changes_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    CHECK(manager.Valid());

    NeuralCacheIdentity identity;
    identity.sourceDigest = std::string(64, 'a');
    identity.width = 1920;
    identity.height = 1080;
    identity.applicationVersion = "0.21.2";
    identity.gpuPath = "rtx40";
    identity.runtimeDigest = std::string(64, 'b');
    identity.quality = "DLAA";
    identity.settingsDigest = std::string(64, 'c');
    identity.driverVersion = "32.0.16.1047";
    identity.modelStoreDigest = std::string(64, 'd');

    const std::string key = BuildNeuralCacheKey(identity);
    CHECK_EQ(size_t{64}, key.size());
    const auto staging = manager.BeginRenderStaging(key);
    CHECK(staging.has_value());
    if (!staging) return;
    WriteBytes(*staging / L"neural.mkv", "neural-frames");
    StageRenderReceipt(*staging);
    CHECK(manager.PromoteRender(key, *staging, CompleteRenderManifest()));
    CHECK(manager.LookupRender(key).has_value());

    const auto served = [&](const NeuralCacheIdentity& candidate) {
        const std::string other = BuildNeuralCacheKey(candidate);
        return other == key || manager.LookupRender(other).has_value();
    };
    auto changed = identity;
    changed.driverVersion = "32.0.16.2001";  // a driver update on the same card
    CHECK(!served(changed));
    changed = identity;
    changed.modelStoreDigest = std::string(64, 'e');  // refreshed weights, same driver
    CHECK(!served(changed));
    changed = identity;
    changed.runtimeDigest = std::string(64, 'f');  // a rebuilt worker or a swapped module
    CHECK(!served(changed));
    changed = identity;
    changed.sourceDigest = std::string(64, '9');
    CHECK(!served(changed));
    changed = identity;
    changed.width = 2560;
    CHECK(!served(changed));
    changed = identity;
    changed.height = 720;
    CHECK(!served(changed));
    changed = identity;
    changed.applicationVersion = "0.21.3";
    CHECK(!served(changed));
    changed = identity;
    changed.gpuPath = "rtx50";
    CHECK(!served(changed));
    changed = identity;
    changed.quality = "UltraPerformance";
    CHECK(!served(changed));
    changed = identity;
    changed.upscaling = true;
    CHECK(!served(changed));
    changed = identity;
    changed.settingsDigest = std::string(64, '8');
    CHECK(!served(changed));
    changed = identity;
    changed.range = NeuralRenderRange{100000000, 200000000};  // a sub-range of the same source
    CHECK(!served(changed));
    changed = identity;
    changed.guides = "depth-off";
    CHECK(!served(changed));
    // The terms discriminate rather than refuse: the identity that produced the
    // entry still answers from it.
    CHECK(served(identity));

    // Both terms are appended to the canonical form only when set, so a
    // downloaded source - which carries neither - keeps the key it was
    // published under; RenderSettingsTests pins that key's literal digest.
}

// A schema-4 entry was keyed under an identity that named neither the driver
// nor the model store, so it has to be retired - and retired for its schema,
// not because it is missing a field: schema 5 writes schema 4's field list, so
// the number is the only difference between a retired manifest and a current
// one, and it is the schema gate that refuses it.
void schema_four_entries_are_retired_by_the_schema_gate_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    CHECK(manager.Valid());
    const std::string key(64, '3');
    const auto staging = manager.BeginRenderStaging(key);
    CHECK(staging.has_value());
    if (!staging) return;
    WriteBytes(*staging / L"neural.mkv", "neural-frames");
    StageRenderReceipt(*staging);
    CHECK(manager.PromoteRender(key, *staging, CompleteRenderManifest()));
    const auto published = manager.LookupRender(key);
    CHECK(published.has_value());
    if (!published) return;
    CHECK_EQ(uint32_t{5}, published->manifest.schema);

    auto retired = published->manifest;
    retired.schema = 4;
    const std::string retiredBytes = SerializeNeuralCacheManifest(retired);
    std::string currentBytes = SerializeNeuralCacheManifest(published->manifest);
    CHECK(currentBytes.starts_with("{\"schema\":5,"));
    CHECK(retiredBytes.starts_with("{\"schema\":4,"));
    // Identical once the number is swapped: nothing but the schema refuses it.
    const size_t number = currentBytes.find("\"schema\":5");
    CHECK(number != std::string::npos);
    if (number != std::string::npos)
        CHECK_EQ(currentBytes.replace(number, 10, "\"schema\":4"), retiredBytes);
    CHECK(!ParseNeuralCacheManifest(retiredBytes).has_value());
    CHECK(!IsReusableNeuralCacheManifest(retired));

    // The same entry on disk, as the previous release left it: not served, and
    // the payload is untouched by the refusal.
    WriteBytes(published->directory / L"manifest.json", retiredBytes);
    CHECK(!manager.LookupRender(key).has_value());
    CHECK_EQ(std::string("neural-frames"), ReadBytes(published->payloadPath));
}

// The model-store term has to move when the weights do. Every file under a
// resolved root is listed by relative name, size and write time; the files
// small enough to afford it - the configs and selectors that decide which
// weights load - are hashed byte for byte as well.
void model_store_digest_tracks_root_contents_and_names_its_fallback_test()
{
    TempDirectory fixture;
    const auto models = fixture.Path() / L"models";
    const auto files = models / L"dlssd" / L"versions" / L"20318464" / L"files";
    std::error_code error;
    CHECK(std::filesystem::create_directories(files, error));
    const auto config = models / L"nvngx_config.txt";
    WriteBytes(config, "app_id=0\n");
    WriteBytes(files / L"160_E658700.bin", std::string((1u << 20) + 1u, 'w'));
    const std::array<NeuralModelRoot, 1> roots{NeuralModelRoot{models, true, {}}};

    const auto first = DigestNeuralModelStore(roots, L"32.0.16.1047");
    CHECK_EQ(size_t{64}, first.digest.size());
    CHECK(first.source == NeuralModelStoreSource::ModelContents);
    CHECK(first.fallbackDetail.empty());
    CHECK_EQ(uint32_t{2}, first.files);
    // The blob is over the content bound, so it is listed and not hashed.
    CHECK_EQ(uint32_t{1}, first.contentHashedFiles);
    CHECK_EQ(uint64_t{(1u << 20) + 1u + 9u}, first.bytes);
    // Unchanged contents digest the same, or every open would miss its own
    // cache; and the driver version is not folded in, so the receipt can say
    // which of the two terms moved.
    CHECK_EQ(first.digest, DigestNeuralModelStore(roots, L"32.0.16.1047").digest);
    CHECK_EQ(first.digest, DigestNeuralModelStore(roots, L"32.0.99.9999").digest);

    // An edited selector: same file, new bytes.
    WriteBytes(config, "app_id=1\n");
    const auto edited = DigestNeuralModelStore(roots, L"32.0.16.1047");
    CHECK(edited.digest != first.digest);

    // A grown blob: above the bound, so its size is what the listing carries.
    WriteBytes(files / L"160_E658700.bin", std::string((1u << 20) + 2u, 'w'));
    const auto grown = DigestNeuralModelStore(roots, L"32.0.16.1047");
    CHECK(grown.digest != edited.digest);

    // A refreshed model store: a new version directory beside the old one.
    const auto refreshedFiles = models / L"dlssd" / L"versions" / L"20318465" / L"files";
    CHECK(std::filesystem::create_directories(refreshedFiles, error));
    WriteBytes(refreshedFiles / L"160_E658701.bin", "refreshed-weights");
    const auto refreshed = DigestNeuralModelStore(roots, L"32.0.16.1047");
    CHECK(refreshed.digest != grown.digest);
    CHECK_EQ(uint32_t{3}, refreshed.files);

    // A driver-store root is not walked: only the NGX modules beside the
    // registered core belong in a render's identity, because the rest of that
    // directory is the whole display driver.
    const auto core = fixture.Path() / L"core";
    CHECK(std::filesystem::create_directories(core / L"nested", error));
    WriteBytes(core / L"nvngx.dll", "ngx-core");
    WriteBytes(core / L"nvcuda.dll", "unrelated");
    WriteBytes(core / L"nested" / L"nvngx_dlssd.dll", "nested");
    const std::array<NeuralModelRoot, 1> coreRoot{NeuralModelRoot{core, false, L"nvngx"}};
    const auto described = DigestNeuralModelStore(coreRoot, L"32.0.16.1047");
    CHECK_EQ(uint32_t{1}, described.files);
    CHECK(described.source == NeuralModelStoreSource::ModelContents);

    // A root that cannot be read is named rather than skipped, and with nothing
    // left to enumerate the driver version is the whole term - the cheap
    // fallback, on the receipt instead of silent.
    const std::array<NeuralModelRoot, 1> absent{
        NeuralModelRoot{fixture.Path() / L"no-such-root", true, {}}};
    const auto fallback = DigestNeuralModelStore(absent, L"32.0.16.1047");
    CHECK(fallback.source == NeuralModelStoreSource::DriverVersion);
    CHECK(!fallback.fallbackDetail.empty());
    CHECK(fallback.enumeratedRoots.empty());
    CHECK_EQ(size_t{64}, fallback.digest.size());
    CHECK(fallback.digest != DigestNeuralModelStore(absent, L"32.0.99.9999").digest);

    const std::string fallbackJson = NeuralModelStoreJson(fallback);
    CHECK(fallbackJson.find("\"source\":\"driverVersion\"") != std::string::npos);
    CHECK(fallbackJson.find(fallback.digest) != std::string::npos);
    CHECK(fallbackJson.find("\"fallback\":\"\"") == std::string::npos);
    const std::string contentsJson = NeuralModelStoreJson(refreshed);
    CHECK(contentsJson.find("\"source\":\"modelContents\"") != std::string::npos);
    CHECK(contentsJson.find("\"fallback\":\"\"") != std::string::npos);
}

// NeuralWorker.exe decides the guides and the cut classification, so a worker
// rebuilt with different logic is a different renderer and must not be handed
// the previous one's renders. It is hashed into the runtime digest for that
// reason, and deliberately not lock-pinned: the lock is the vendor stack, and
// every build of this repository changes the worker.
void hashed_runtime_set_covers_the_worker_and_the_lock_set_does_not_test()
{
    const auto hashed = LockedRuntimeFileNames();
    const auto pinned = LockPinnedRuntimeFileNames();
    CHECK_EQ(size_t{13}, hashed.size());
    CHECK_EQ(size_t{12}, pinned.size());
    CHECK(std::ranges::find(hashed, std::wstring_view(L"NeuralWorker.exe")) != hashed.end());
    CHECK(std::ranges::find(pinned, std::wstring_view(L"NeuralWorker.exe")) == pinned.end());
    for (const std::wstring_view name : pinned)
        CHECK(std::ranges::find(hashed, name) != hashed.end());

    TempDirectory fixture;
    for (const std::wstring_view name : hashed) WriteBytes(fixture.Path() / name, "staged-module");
    const auto staged = BuildRuntimeDigest(fixture.Path(), hashed);
    CHECK(staged.has_value());
    WriteBytes(fixture.Path() / L"NeuralWorker.exe", "rebuilt-worker");
    const auto rebuilt = BuildRuntimeDigest(fixture.Path(), hashed);
    CHECK(rebuilt.has_value());
    CHECK(staged != rebuilt);
    // A missing locked file still leaves no digest, which the render path
    // reports as an incomplete runtime rather than rendering against a stack
    // it cannot name.
    std::error_code error;
    CHECK(std::filesystem::remove(fixture.Path() / L"NeuralWorker.exe", error));
    CHECK(!BuildRuntimeDigest(fixture.Path(), hashed).has_value());
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
    manifest.feature18ArmedBeforeCapture = true;
    CHECK(IsReusableNeuralCacheManifest(manifest));
    // A render is served as verified neural output out of a user-writable
    // directory, and no release ever wrote a schema-4 render without a receipt
    // digest, so a manifest that simply omits one has nothing vouching for how
    // the payload was produced and is not reusable.
    manifest.receiptDigest.clear();
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
    StageRenderReceipt(*renderStaging);
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
    StageRenderReceipt(*restoredStaging);
    CHECK(manager.PromoteRender(renderKey, *restoredStaging, renderManifest));
    const auto restored = manager.LookupRender(renderKey);
    CHECK(restored.has_value());
    if (!restored) return;

    const auto replacement = manager.BeginRenderStaging(renderKey);
    CHECK(replacement.has_value());
    if (replacement) {
        WriteBytes(*replacement / L"neural.mkv", "must-not-replace-valid-cache");
        StageRenderReceipt(*replacement);
        CHECK(manager.PromoteRender(renderKey, *replacement, renderManifest));
    }
    CHECK_EQ(std::string("neural-frames"), ReadBytes(restored->payloadPath));

    WriteBytes(restored->payloadPath, "tampered");
    CHECK(!manager.LookupRender(renderKey).has_value());

    const auto repairStaging=manager.BeginRenderStaging(renderKey);
    CHECK(repairStaging.has_value());
    if(!repairStaging)return;
    WriteBytes(*repairStaging/L"neural.mkv","repaired-neural-frames");
    StageRenderReceipt(*repairStaging);
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

    // The published directory is user-writable, so its manifest can be
    // rewritten in place. Dropping the receipt digest, and the receipt with
    // it, leaves every other digest matching - and must still not produce a
    // served render.
    if (repaired) {
        auto stripped = repaired->manifest;
        stripped.receiptDigest.clear();
        WriteBytes(repaired->directory / L"manifest.json",
                   SerializeNeuralCacheManifest(stripped));
        std::error_code receiptRemoveError;
        CHECK(std::filesystem::remove(repaired->directory / L"receipt.json",
                                      receiptRemoveError));
        CHECK(!manager.LookupRender(renderKey).has_value());
    }
}

// A finished render used to be discarded because publishing it is a directory
// rename, and a directory cannot be renamed while any file inside it is open -
// which is exactly what an antivirus scanner does to a freshly written 186 MB
// entry. The rename now waits the scan out, and a promotion that still fails
// says which step failed rather than one shared verdict.
void promotion_waits_out_a_transient_lock_and_names_the_failing_step_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    CHECK(manager.Valid());

    const std::string renderKey(64, '7');
    const auto staging = manager.BeginRenderStaging(renderKey);
    CHECK(staging.has_value());
    if (!staging) return;
    const auto payload = *staging / L"neural.mkv";
    WriteBytes(payload, "neural-frames");
    StageRenderReceipt(*staging);
    const auto manifest = CompleteRenderManifest();

    // FILE_SHARE_READ|WRITE without DELETE is what a scanner holds, and it is
    // what blocks the rename of the directory the file sits in.
    const HANDLE scanner = CreateFileW(payload.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(scanner != INVALID_HANDLE_VALUE);
    if (scanner == INVALID_HANDLE_VALUE) return;
    // Released from the promotion's own retry hook rather than on a timer:
    // the first attempt is then known to have met the lock, and the retry
    // behind it is the one that publishes.
    unsigned releasedAfter = 0;
    manager.ObservePublishRetries([&](unsigned attempt) {
        if (releasedAfter) return;
        releasedAfter = attempt;
        CloseHandle(scanner);
    });
    NeuralCachePromotion promotion{};
    const bool published = manager.PromoteRender(renderKey, *staging, manifest, &promotion);
    CHECK(published);
    CHECK(manager.LookupRender(renderKey).has_value());
    CHECK(promotion.stage == NeuralCachePromotion::Stage::Published);
    CHECK_EQ(1u, releasedAfter);
    // The first attempt met the lock; anything past it (a real scanner on the
    // machine can add its own) is a retry that waited it out.
    CHECK(promotion.attempts > 1);
    // What a caller gets back is the entry it just published, without a
    // second pass over the payload.
    CHECK(promotion.entry.has_value());
    if (promotion.entry) {
        CHECK_EQ(std::string("neural-frames"), ReadBytes(promotion.entry->payloadPath));
        CHECK(promotion.entry->manifest.neuralDigest == Sha256File(promotion.entry->payloadPath));
    }
    manager.ObservePublishRetries({});
    CHECK_EQ(std::string("rename"),
             std::string(NeuralCachePromotionStageName(NeuralCachePromotion::Stage::Move)));

    const auto second = manager.BeginRenderStaging(std::string(64, '8'));
    CHECK(second.has_value());
    if (!second) return;
    WriteBytes(*second / L"neural.mkv", "neural-frames");
    StageRenderReceipt(*second);
    // The receipt is staged; it is neural-settings.ini that this manifest
    // promises and staging does not have.
    auto missingSettingsSidecar = manifest;
    missingSettingsSidecar.settingsDigest = std::string(64, 'a');
    NeuralCachePromotion rejected{};
    CHECK(!manager.PromoteRender(std::string(64, '8'), *second,
                                 missingSettingsSidecar, &rejected));
    CHECK(rejected.stage == NeuralCachePromotion::Stage::SidecarDigest);
    CHECK_EQ(0u, rejected.attempts);
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

// A pid the kernel does not know, found rather than guessed: the sweep must
// treat it as a process that is gone.
DWORD DeadProcessId()
{
    for (DWORD pid = 0xFFFFFF00u; pid > 0xFFFF0000u; pid -= 4) {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process) { CloseHandle(process); continue; }
        if (GetLastError() == ERROR_INVALID_PARAMETER) return pid;
    }
    return 0;
}

// MarkInvalid and Quarantine set a directory aside under staging/ and nothing
// ever removed it: a session's failed renders parked their partial payloads
// until the user cleared the whole cache. The sweep reaps what nothing will
// ever finish and leaves what a live process is still writing.
void staging_sweep_reaps_invalid_and_orphaned_entries_but_not_live_ones_test()
{
    TempDirectory fixture;
    const auto cacheRoot = fixture.Path() / L"cache";
    const auto staging = cacheRoot / L"staging";
    const std::wstring key(64, L'4');
    const std::wstring ownPid = std::to_wstring(GetCurrentProcessId());
    const DWORD deadPid = DeadProcessId();
    CHECK(deadPid != 0);
    if (!deadPid) return;
    const auto invalidOwn = staging / (L"invalid-" + ownPid + L"-1");
    const auto invalidExistingOwn = staging / (L"invalid-existing-" + ownPid + L"-2");
    const auto invalidCacheDead = staging / (L"invalid-cache-" + std::to_wstring(deadPid) + L"-3");
    const auto orphanRender = staging / (L"render-" + key + L"-" + std::to_wstring(deadPid) + L"-4");
    const auto orphanSource = staging / (L"source-" + key + L"-" + std::to_wstring(deadPid) + L"-5");
    const auto liveRender = staging / (L"render-" + key + L"-" + ownPid + L"-6");
    const auto liveSource = staging / (L"source-" + key + L"-" + ownPid + L"-7");
    const auto foreign = staging / L"notes";
    for (const auto& directory : {invalidOwn, invalidExistingOwn, invalidCacheDead, orphanRender,
                                  orphanSource, liveRender, liveSource, foreign}) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        CHECK(!error);
        WriteBytes(directory / L"neural.mkv", "partial");
    }

    NeuralCacheManager manager(cacheRoot);
    CHECK(manager.Valid());
    for (const auto& gone : {invalidOwn, invalidExistingOwn, invalidCacheDead, orphanRender, orphanSource})
        CHECK(!std::filesystem::exists(gone));
    for (const auto& kept : {liveRender, liveSource, foreign})
        CHECK(std::filesystem::is_directory(kept));
    // Nothing left to reap: a second sweep is a no-op, not a second pass over
    // the live entries.
    CHECK_EQ(size_t{0}, manager.SweepStaging());
    for (const auto& kept : {liveRender, liveSource, foreign})
        CHECK(std::filesystem::is_directory(kept));

    // An entry this process sets aside is reaped by the next sweep, whichever
    // process runs it.
    const auto parked = manager.BeginRenderStaging(std::string(64, '5'));
    CHECK(parked.has_value());
    if (!parked) return;
    WriteBytes(*parked / L"neural.mkv", "partial");
    CHECK(manager.MarkInvalid(*parked));
    CHECK(!std::filesystem::exists(*parked));
    CHECK_EQ(size_t{1}, manager.SweepStaging());
    size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(staging)) { (void)entry; ++entries; }
    CHECK_EQ(size_t{3}, entries);
}

void media_pipeline_arguments_are_exact_and_never_use_a_shell_test()
{
    const MaterializeRequest materialize{
        .videoUrl=L"https://r1.googlevideo.com/video?id=abc&token=one two",
        .audioUrl=L"https://r1.googlevideo.com/audio?id=abc&token=three",
        .output=LR"(C:\Cache Root\source.partial.mkv)"};
    const std::vector<std::wstring> expectedMaterialize{
        L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-progress", L"pipe:1", L"-y", L"-xerror",
        L"-tls_verify", L"1", L"-protocol_whitelist", L"https,tls,tcp",
        L"-rw_timeout", L"10000000", L"-reconnect", L"1", L"-reconnect_on_network_error", L"1",
        L"-reconnect_on_http_error", L"429,5xx", L"-reconnect_delay_max", L"2",
        L"-reconnect_max_retries", L"3", L"-reconnect_delay_total_max", L"8", L"-respect_retry_after", L"0",
        L"-i", materialize.videoUrl,
        L"-tls_verify", L"1", L"-protocol_whitelist", L"https,tls,tcp",
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
    // The CPU conversion inside ffmpeg is what the BGRA path pays for, so the pixel
    // format it is given stays BGRA in, yuv420p out. It is NOT untagged: this path used
    // to state no colorimetry at all, which shipped every default render as an untagged
    // file carrying ffmpeg's BT.601 default, and a BT.709 source decoded wrongly
    // downstream. It now states BT.709 *and* converts with it - a label without the
    // matrix would be worse than the original defect.
    const auto value = [](const std::vector<std::wstring>& list, const wchar_t* flag) {
        std::vector<std::wstring> found;
        for (size_t index = 0; index + 1 < list.size(); ++index)
            if (list[index] == flag) found.push_back(list[index + 1]);
        return found;
    };
    CHECK_EQ((std::vector<std::wstring>{L"bgra", L"yuv420p"}), value(arguments, L"-pix_fmt"));
    CHECK_EQ((std::vector<std::wstring>{L"bt709"}), value(arguments, L"-colorspace"));
    CHECK_EQ((std::vector<std::wstring>{L"bt709"}), value(arguments, L"-color_primaries"));
    CHECK_EQ((std::vector<std::wstring>{L"tv"}), value(arguments, L"-color_range"));
    // scale picks the coefficients; setparams is what makes primaries and transfer
    // survive this FFmpeg's muxers, and it is pixel-safe here (scale-only and
    // scale+setparams decode to identical planes).
    CHECK_EQ((std::vector<std::wstring>{L"scale=out_color_matrix=bt709:out_range=tv,"
                                        L"setparams=color_primaries=bt709:color_trc=bt709:"
                                        L"colorspace=bt709:range=tv"}),
             value(arguments, L"-vf"));

    // A GPU-converted capture arrives as NV12 and leaves as NV12: NVENC takes it as it
    // stands, so no frame is converted on the CPU. It states the same colorimetry the
    // capture shader produced, and it carries `setparams` to do it - metadata only, and
    // measured pixel-exact through a lossless round trip. What it must NOT carry is a
    // `scale` filter: converting those pixels again is the entire cost this path exists
    // to avoid.
    EncoderSpec gpuConverted = encoder;
    gpuConverted.pixelFormat = EncoderPixelFormat::Nv12;
    const std::vector<std::wstring> nv12 = BuildEncoderArguments(
        gpuConverted, LR"(C:\Cache Root\neural.partial.mkv)");
    CHECK_EQ((std::vector<std::wstring>{L"nv12", L"nv12"}), value(nv12, L"-pix_fmt"));
    CHECK_EQ((std::vector<std::wstring>{L"bt709"}), value(nv12, L"-colorspace"));
    CHECK_EQ((std::vector<std::wstring>{L"tv"}), value(nv12, L"-color_range"));
    CHECK_EQ((std::vector<std::wstring>{L"setparams=color_primaries=bt709:color_trc=bt709:"
                                        L"colorspace=bt709:range=tv"}), value(nv12, L"-vf"));
    for (const std::wstring& argument : nv12)
        CHECK(argument.find(L"scale=") == std::wstring::npos);
    // x264 has no NV12 input, so that pairing converts one plane instead of a frame.
    EncoderSpec software = gpuConverted;
    software.kind = EncoderKind::H264Software;
    CHECK_EQ((std::vector<std::wstring>{L"nv12", L"yuv420p"}),
             value(BuildEncoderArguments(software, LR"(C:\Cache Root\neural.partial.mkv)"), L"-pix_fmt"));

    // nvencPreset selects the NVENC "-preset" value, clamped to the p1..p7 range NVENC
    // accepts; the software encoder ignores it entirely and always encodes at "slow".
    // p5 is the default, on the measurement recorded beside EncoderSpec::nvencPreset.
    CHECK_EQ((std::vector<std::wstring>{L"p5"}), value(arguments, L"-preset"));
    // Asked for something OTHER than the default, so this still proves the field
    // reaches the argument rather than agreeing with it by accident.
    EncoderSpec preset7 = encoder;
    preset7.nvencPreset = 7;
    CHECK_EQ((std::vector<std::wstring>{L"p7"}),
             value(BuildEncoderArguments(preset7, LR"(C:\Cache Root\neural.partial.mkv)"), L"-preset"));
    EncoderSpec presetTooLow = encoder;
    presetTooLow.nvencPreset = 0;
    CHECK_EQ((std::vector<std::wstring>{L"p1"}),
             value(BuildEncoderArguments(presetTooLow, LR"(C:\Cache Root\neural.partial.mkv)"), L"-preset"));
    EncoderSpec presetTooHigh = encoder;
    presetTooHigh.nvencPreset = 9;
    CHECK_EQ((std::vector<std::wstring>{L"p7"}),
             value(BuildEncoderArguments(presetTooHigh, LR"(C:\Cache Root\neural.partial.mkv)"), L"-preset"));
    EncoderSpec softwarePreset = software;
    softwarePreset.nvencPreset = 3;
    CHECK_EQ((std::vector<std::wstring>{L"slow"}),
             value(BuildEncoderArguments(softwarePreset, LR"(C:\Cache Root\neural.partial.mkv)"), L"-preset"));

    // 1.5 bytes per pixel instead of 4, which is what the readback and the pipe carry.
    CHECK_EQ(uint64_t{1920 * 1080 * 3 / 2}, EncoderFrameBytes(EncoderPixelFormat::Nv12, 1920, 1080));
    CHECK_EQ(uint64_t{1920 * 1080 * 4}, EncoderFrameBytes(EncoderPixelFormat::Bgra, 1920, 1080));
}

void encoder_frame_contract_and_fallback_policy_are_fail_closed_test()
{
    const EncoderSpec valid{2, 2, 30.0, EncoderKind::HevcNvenc};
    CHECK_EQ(size_t{16}, ExpectedFrameBytes(valid));
    CHECK_EQ(size_t{0}, ExpectedFrameBytes(EncoderSpec{0, 2, 30.0, EncoderKind::HevcNvenc}));
    CHECK_EQ(size_t{0}, ExpectedFrameBytes(EncoderSpec{2, 2, 0.0, EncoderKind::HevcNvenc}));
    // The size the encoder demands has to follow the format the capture hands over, or
    // a GPU-converted frame is rejected as the wrong size on every single write.
    EncoderSpec converted = valid;
    converted.pixelFormat = EncoderPixelFormat::Nv12;
    CHECK_EQ(size_t{6}, ExpectedFrameBytes(converted));
    // NV12 has no half-pixel chroma sample, so an odd size is refused rather than
    // encoded at a size the capture does not produce.
    EncoderSpec oddConverted = converted;
    oddConverted.width = 3;
    CHECK_EQ(size_t{0}, ExpectedFrameBytes(oddConverted));
    CHECK_EQ(size_t{12}, ExpectedFrameBytes(EncoderSpec{3, 1, 30.0, EncoderKind::H264Software}));
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

void media_progress_reader_buffers_split_keys_and_limits_its_report_rate_test()
{
    using Reader = media_pipeline_detail::MediaProgressReader;
    std::vector<MediaDownloadProgress> reports;
    std::string diagnostic;
    Reader reader([&](const MediaDownloadProgress& progress) { reports.push_back(progress); },
                  [&](std::string_view line) { diagnostic.append(line); diagnostic.push_back('\n'); });
    const Reader::Clock::time_point start{};

    // FFmpeg reports "N/A" until the muxer has written something, and a block of
    // nothing but unknowns must not wake the caller with an empty report.
    reader.Consume("frame=12\nbitrate=N/A\ntotal_size=N/A\nout_time_us=N/A\nprogress=continue\r\n", start);
    CHECK(reports.empty());
    // The capture hands over whatever one ReadFile returned, so a key arrives
    // cut in half often enough that losing it would lose the whole download.
    reader.Consume("total_si", start);
    reader.Consume("ze=2048\nout_time_us=1500000\nprogress=continue\n", start);
    CHECK_EQ(size_t{1}, reports.size());
    if (!reports.empty()) {
        CHECK_EQ(uint64_t{2048}, reports.back().bytes);
        CHECK_EQ(1.5, reports.back().seconds);
    }
    // A block inside the interval is withheld, and the next block past it
    // carries the newest figures rather than replaying the withheld ones.
    reader.Consume("total_size=4096\nout_time_ms=2000000\nprogress=continue\n", start + 100ms);
    CHECK_EQ(size_t{1}, reports.size());
    reader.Consume("total_size=8192\nout_time_us=3000000\nprogress=continue\n", start + 260ms);
    CHECK_EQ(size_t{2}, reports.size());
    if (reports.size() > 1) {
        CHECK_EQ(uint64_t{8192}, reports[1].bytes);
        CHECK_EQ(3.0, reports[1].seconds);
    }
    // Error text shares the pipe with the progress stream; it must reach the
    // caller's diagnostic instead of being parsed away as an unknown key.
    reader.Consume("[matroska @ 0000] Non-monotonic DTS\n", start + 300ms);
    // The final block is reported even though the interval would withhold it.
    reader.Consume("total_size=9216\nout_time_us=3500000\nprogress=end\n", start + 310ms);
    CHECK_EQ(size_t{3}, reports.size());
    if (reports.size() > 2) CHECK_EQ(uint64_t{9216}, reports[2].bytes);
    reader.Finish(start + 320ms);
    CHECK_EQ(size_t{3}, reports.size());
    CHECK_EQ(std::string("[matroska @ 0000] Non-monotonic DTS\n"), diagnostic);

    // A child that dies mid-block leaves values behind with no progress= line
    // and a trailing fragment with no newline; both still reach the caller once.
    std::vector<MediaDownloadProgress> interrupted;
    Reader tail([&](const MediaDownloadProgress& progress) { interrupted.push_back(progress); }, {});
    tail.Consume("total_size=4\nout_time_us=1000000\n", start);
    CHECK(interrupted.empty());
    tail.Consume("total_size=64", start + 300ms);
    tail.Finish(start + 320ms);
    CHECK_EQ(size_t{1}, interrupted.size());
    if (!interrupted.empty()) {
        CHECK_EQ(uint64_t{64}, interrupted.back().bytes);
        CHECK_EQ(1.0, interrupted.back().seconds);
    }
}

void materialization_reports_download_progress_while_the_source_is_copied_test()
{
    TempDirectory fixture;
    std::filesystem::copy_file(CurrentExecutable(), fixture.Path() / L"ffmpeg.exe");
    std::vector<MediaDownloadProgress> reports;
    const auto output = fixture.Path() / L"output.mkv";
    const auto result = MediaMaterializer(fixture.Path()).Run(
        {L"https://media.invalid/progress-stream", {}, output}, {},
        [&](const MediaDownloadProgress& progress) { reports.push_back(progress); });
    CHECK(result.ok);
    // The first block is all unknowns, the second reports, the third ends the
    // stream and is therefore reported whatever the rate limit would have said.
    CHECK_EQ(size_t{2}, reports.size());
    if (reports.size() == 2) {
        CHECK_EQ(uint64_t{1048576}, reports[0].bytes);
        CHECK_EQ(2.5, reports[0].seconds);
        CHECK_EQ(uint64_t{4194304}, reports[1].bytes);
        CHECK_EQ(9.0, reports[1].seconds);
    }
    // The two-argument form stays a valid call and asks for no reports at all.
    CHECK(MediaMaterializer(fixture.Path()).Run({L"https://media.invalid/video", {}, output}, {}).ok);
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
    RawVideoEncoder encoder(helper);EncoderSpec spec{2048,3072,30.0,EncoderKind::H264Software};
    // The write has to actually block before a stop has anything to interrupt, so the
    // frame must be larger than the child's stdin pipe buffer. Deriving the requirement
    // from the constant keeps this test honest if that buffer is ever retuned: it used to
    // rely on the pipe defaulting to a few kilobytes.
    CHECK(ExpectedFrameBytes(spec) > kChildStdinPipeBytes);
    CHECK_EQ(EncodeError::None,encoder.Start(spec,fixture.Path()/L"hang-output.mkv"));
    std::vector<uint8_t> frame(ExpectedFrameBytes(spec));std::stop_source stop;
    auto write=std::async(std::launch::async,[&]{return encoder.WriteFrame(frame,stop.get_token());});
    std::this_thread::sleep_for(100ms);stop.request_stop();
    CHECK_EQ(std::future_status::ready,write.wait_for(3s));
    if(write.wait_for(0s)==std::future_status::ready){
        // CHECK_EQ prints the expressions, not the values, and the future can only be
        // read once - so name the outcome here or a failure says nothing about which
        // error the write actually returned.
        const EncodeError result=write.get();
        if(result!=EncodeError::Cancelled)
            std::cerr<<"blocked-write diagnostic: result="<<static_cast<int>(result)<<'\n';
        CHECK_EQ(EncodeError::Cancelled,result);
    }
}

std::vector<OfflineDecodedFrame> FiveOfflineFrames()
{
    constexpr std::array<int64_t,5> timestamps{0,333333,666666,999999,1333332};
    std::vector<OfflineDecodedFrame> frames;
    for(size_t index=0;index<timestamps.size();++index)
        frames.push_back(OfflineDecodedFrame{{uint8_t(index),0,0,255},timestamps[index],index==0,index,1});
    return frames;
}

// `count` frames on an exact 25 fps grid (400000 * index), so range edges and
// preroll arithmetic have no rounding.
std::vector<OfflineDecodedFrame> OfflineFramesAt25Fps(size_t count)
{
    std::vector<OfflineDecodedFrame> frames;
    for(size_t index=0;index<count;++index)
        frames.push_back(OfflineDecodedFrame{{uint8_t(index),0,0,255},int64_t(index)*400000,index==0,index,1});
    return frames;
}

class FakeOfflineSource final : public IFrameSource {
public:
    explicit FakeOfflineSource(std::vector<OfflineDecodedFrame> frames=FiveOfflineFrames())
        : frames_(std::move(frames)) {}
    bool Open(const std::filesystem::path&,std::stop_token stop,double seekSeconds) override
    {
        ++opens;seeks.push_back(seekSeconds);
        const auto seek100ns=int64_t(std::llround(seekSeconds*10000000.0));
        index_=0;while(index_<frames_.size()&&frames_[index_].timestamp100ns<seek100ns)++index_;
        return !stop.stop_requested();
    }
    void Close() override { ++closes; }
    OfflineFrameRead Read(OfflineDecodedFrame& frame,std::stop_token stop) override
    {
        if(stop.stop_requested())return OfflineFrameRead::Cancelled;
        if(index_>=frames_.size())return OfflineFrameRead::EndOfStream;
        frame=frames_[index_++];return OfflineFrameRead::FrameReady;
    }
    int opens{};int closes{};std::vector<double> seeks;
private:
    std::vector<OfflineDecodedFrame> frames_;size_t index_{};
};

class FakeNeuralEvaluator final : public INeuralFrameEvaluator {
public:
    bool Initialize(HWND,uint32_t width,uint32_t height,double,const GuideControls& guides) override
    { initialized=true;expectedBytes=size_t(width)*height*4u;controls=guides;return true; }
    bool Submit(const OfflineDecodedFrame& frame,const FrameIdentity& id,bool capture,
                OfflineEvaluation& out) override
    {
        submitted.push_back(frame.timestamp100ns);resets.push_back(id.reset!=HistoryReset::None);
        ids.push_back(id);
        out.id=id;
        if(id.reset!=HistoryReset::None)++historyGeneration;
        out.id.historyGeneration=historyGeneration;
        if(!capture){if(++primeSubmissions>=requiredPrimeSubmissions)featureCreated=true;if(featureCreated){++evaluations;++backendEvaluations;}return true;}
        ++captureSubmissions;
        if((failCaptureAt&&captureSubmissions==*failCaptureAt)||
           (failCaptureFrom&&captureSubmissions>=*failCaptureFrom)){lastFailure=captureFailure;return false;}
        if(!featureCreated){lastFailure=NeuralRenderFailure::Neural;return false;}
        if(cutAtCapture&&captureSubmissions==*cutAtCapture){out.id.reset=HistoryReset::Cut;out.id.historyGeneration=++historyGeneration;}
        if(mismatchAtCapture&&captureSubmissions==*mismatchAtCapture)++out.id.frameNumber;
        ++evaluations;
        // The backend's own tally: what an accepted submit is normally also
        // counted by, unless the test says the pass silently did not run.
        if(!(backendMissesCaptureAt&&captureSubmissions==*backendMissesCaptureAt))++backendEvaluations;
        out.bgra=frame.bgra;if(out.bgra.size()<expectedBytes)out.bgra.resize(expectedBytes);
        if(stampCaptureCount&&!out.bgra.empty())out.bgra[0]=static_cast<uint8_t>(captureSubmissions);
        captured.push_back(frame.timestamp100ns);return true;
    }
    bool FeatureCreated() const override { return featureCreated; }
    uint64_t EvaluationCount() const override { return evaluations; }
    uint64_t NeuralEvaluations() const override { return backendEvaluations; }
    void ResetTemporal() override { ++temporalResets; }
    NeuralRenderFailure LastFailure() const override { return lastFailure; }
    double LastNeuralGpuMs() const override { return neuralGpuMs; }
    uint64_t PeakLocalVideoMemoryMiB() const override { return peakVramMiB; }
    bool initialized{};bool featureCreated{};uint64_t evaluations{};int primeSubmissions{};
    int requiredPrimeSubmissions{2};
    bool stampCaptureCount{};
    int captureSubmissions{};int temporalResets{};size_t expectedBytes{};
    std::optional<int> failCaptureAt,failCaptureFrom,cutAtCapture,mismatchAtCapture,backendMissesCaptureAt;
    uint64_t backendEvaluations{};
    NeuralRenderFailure captureFailure{NeuralRenderFailure::Neural};
    NeuralRenderFailure lastFailure{NeuralRenderFailure::None};
    // A plausible healthy 1080p median (receipts on this machine read 3.68 to
    // 3.72 ms); the render refuses a job whose median falls under the
    // per-geometry floor, so the default has to look like real neural work.
    double neuralGpuMs{3.7};uint64_t peakVramMiB{};uint32_t historyGeneration{};
    GuideControls controls;
    std::vector<int64_t> submitted,captured;std::vector<bool> resets;std::vector<FrameIdentity> ids;
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

NeuralRenderRequest EvenOfflineRequest(const std::filesystem::path& directory)
{
    auto request=OfflineRequest(directory);request.width=2;request.height=2;return request;
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
    CHECK_EQ(3,evidenceCalls);
}

// nativeEvaluations used to be a copy of frameCount, so the cache's
// "every frame was evaluated" gate could not fail. It is the backend's own
// tally now, held to the frame count by the render and by the manifest gate.
void offline_job_refuses_a_backend_that_evaluated_fewer_frames_than_it_captured_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    // The third capture is accepted by the evaluator but the backend never
    // ran it: exactly the frame a stalled neural pass would hand back as
    // upscaler output.
    evaluator.backendMissesCaptureAt=3;
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
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(NeuralRenderFailure::Neural,result.failure);
    // Every frame was captured and encoded; only the backend's word is missing.
    CHECK_EQ(size_t{5},evaluator.captured.size());
    CHECK_EQ(size_t{1},encoder.attempts.size());

    // The manifest gate is the same claim on the published side: fewer
    // evaluations than frames is refused, more (resubmits) is not.
    auto manifest=CompleteRenderManifest();
    manifest.state=NeuralCacheState::Complete;manifest.neuralDigest=std::string(64,'c');
    manifest.nativeEvaluations=manifest.frameCount+3;
    CHECK(IsReusableNeuralCacheManifest(manifest));
    manifest.nativeEvaluations=manifest.frameCount-1;
    CHECK(!IsReusableNeuralCacheManifest(manifest));
}

void offline_sparse_receipt_gate_encodes_latest_capture_once_per_source_frame_test()
{
    for (const bool photo : {true, false}) {
        TempDirectory fixture;
        auto frames = FiveOfflineFrames();
        if (photo) frames.resize(1);
        FakeOfflineSource source(frames);
        FakeNeuralEvaluator evaluator;
        evaluator.stampCaptureCount = true;
        FakeFrameEncoder encoder;
        OfflineNeuralRenderer job(source, evaluator, encoder, [&] {
            return NeuralEvidenceWithCount(std::max<uint64_t>(1, (evaluator.EvaluationCount() / 60) * 60));
        });
        auto request = OfflineRequest(fixture.Path());
        if (photo) { request.fps = 1; request.durationSeconds = 1; }
        const auto result = job.Run(request);
        CHECK(result.ok);
        CHECK_EQ(static_cast<uint64_t>(frames.size()), result.frameCount);
        // The backend's tally counts the receipt gate's resubmits of the first
        // frame, which the encoded frame count does not.
        CHECK_EQ(uint64_t{60} + frames.size() - 1, result.nativeEvaluations);
        CHECK_EQ(uint64_t{60}, result.evidence.highestObservedEvaluation);
        CHECK_EQ(2, source.opens);
        CHECK_EQ(size_t{1}, encoder.attempts.size());
        if (!encoder.attempts.empty()) {
            CHECK_EQ(frames.size(), encoder.attempts.front().size());
            if (!encoder.attempts.front().empty()) CHECK_EQ(uint8_t{60}, encoder.attempts.front().front().front());
        }
        CHECK_EQ(size_t{60} + frames.size() - 1, evaluator.captured.size());
        if (evaluator.captured.size() >= 60) {
            CHECK(std::all_of(evaluator.captured.begin(), evaluator.captured.begin() + 60,
                             [](int64_t timestamp) { return timestamp == 0; }));
            for (size_t index = 1; index < frames.size(); ++index)
                CHECK_EQ(frames[index].timestamp100ns, evaluator.captured[59 + index]);
        }
        if (photo) CHECK_EQ(int64_t{10000000}, result.duration100ns);
    }
}

void offline_odd_dimensions_use_geometry_preserving_software_encoder_test()
{
    TempDirectory fixture;
    FakeOfflineSource source({OfflineDecodedFrame{{12, 34, 56, 255}, 0, true}});
    FakeNeuralEvaluator evaluator;
    FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source, evaluator, encoder, AdvancingNeuralEvidence());
    auto request = OfflineRequest(fixture.Path());
    request.fps = 1.0;
    request.durationSeconds = 1.0;
    const auto result = job.Run(request);
    CHECK(result.ok);
    CHECK_EQ(EncoderKind::H264Software, result.encoder);
    CHECK_EQ(std::vector<EncoderKind>({EncoderKind::H264Software}), encoder.starts);
    CHECK_EQ(2, source.opens);
}

void offline_sparse_receipt_gate_restarts_independently_for_software_retry_test()
{
    TempDirectory fixture;
    FakeOfflineSource source({OfflineDecodedFrame{{12, 34, 56, 255}, 0, true}});
    FakeNeuralEvaluator evaluator;
    evaluator.stampCaptureCount = true;
    FakeFrameEncoder encoder;
    encoder.failNvencWriteAt = 0;
    OfflineNeuralRenderer job(source, evaluator, encoder, [&] {
        return NeuralEvidenceWithCount(std::max<uint64_t>(1, (evaluator.EvaluationCount() / 60) * 60));
    });
    auto request = EvenOfflineRequest(fixture.Path());
    request.fps = 1; request.durationSeconds = 1;
    const auto result = job.Run(request);
    CHECK(result.ok);
    CHECK_EQ(EncoderKind::H264Software, result.encoder);
    CHECK_EQ(uint64_t{1}, result.frameCount);
    CHECK_EQ(uint64_t{120}, result.evidence.highestObservedEvaluation);
    CHECK_EQ(3, source.opens);
    CHECK_EQ(size_t{2}, encoder.attempts.size());
    if (encoder.attempts.size() == 2) {
        CHECK(encoder.attempts.front().empty());
        CHECK_EQ(size_t{1}, encoder.attempts.back().size());
        if (!encoder.attempts.back().empty()) CHECK_EQ(uint8_t{120}, encoder.attempts.back().front().front());
    }
    CHECK_EQ(120, evaluator.captureSubmissions);
}

void offline_receipt_gate_stops_before_encoding_on_failure_or_cancel_test()
{
    for (const bool cancel : {true, false}) {
        TempDirectory fixture;
        FakeOfflineSource source;
        FakeNeuralEvaluator evaluator;
        FakeFrameEncoder encoder;
        std::stop_source stop;
        OfflineNeuralRenderer job(source, evaluator, encoder, [&] {
            if (evaluator.captureSubmissions >= 10) {
                if (cancel) stop.request_stop();
                else return NeuralEvidenceWithCount(60) + "inline feature 18 evaluation failed\n";
            }
            return NeuralEvidenceWithCount(1);
        });
        const auto result = job.Run(OfflineRequest(fixture.Path()), {}, stop.get_token());
        CHECK(!result.ok);
        CHECK_EQ(cancel, result.cancelled);
        CHECK_EQ(10, evaluator.captureSubmissions);
        CHECK_EQ(0, encoder.finishes);
        CHECK_EQ(size_t{1}, encoder.attempts.size());
        if (!encoder.attempts.empty()) CHECK(encoder.attempts.front().empty());
    }
}

void offline_photo_reuses_warmup_frame_but_encodes_exactly_one_frame_test()
{
    for (const int warmupFrames : {2, 120}) {
        TempDirectory fixture;
        const std::vector<uint8_t> photo{12, 34, 56, 255};
        FakeOfflineSource source({OfflineDecodedFrame{photo, 0, true}});
        FakeNeuralEvaluator evaluator;
        evaluator.requiredPrimeSubmissions = warmupFrames;
        FakeFrameEncoder encoder;
        OfflineNeuralRenderer job(source, evaluator, encoder, [&] {
            return NeuralEvidenceWithCount(evaluator.EvaluationCount());
        });
        auto request = OfflineRequest(fixture.Path());
        request.fps = 1.0;
        request.durationSeconds = 1.0;
        const auto result = job.Run(request);
        CHECK(result.ok);
        CHECK_EQ(uint64_t{1}, result.frameCount);
        CHECK_EQ(uint64_t{1}, result.nativeEvaluations);
        CHECK_EQ(uint64_t{1}, result.verifiedNeuralFrames);
        CHECK_EQ(int64_t{10000000}, result.duration100ns);
        CHECK(result.feature18ArmedBeforeCapture);
        CHECK(result.evidence.Valid());
        CHECK_EQ(warmupFrames, evaluator.primeSubmissions);
        CHECK_EQ(2, source.opens);
        CHECK_EQ(1, evaluator.temporalResets);
        CHECK_EQ(std::vector<int64_t>{0}, evaluator.captured);
        CHECK_EQ(size_t{1}, encoder.attempts.size());
        if (!encoder.attempts.empty()) {
            CHECK_EQ(size_t{1}, encoder.attempts.front().size());
            if (!encoder.attempts.front().empty()) CHECK_EQ(photo, encoder.attempts.front().front());
        }
        CHECK_EQ(1, encoder.finishes);
        // A replayed still is continuous during warm-up; capture starts fresh.
        if (evaluator.resets.size() == static_cast<size_t>(warmupFrames + 1)) {
            CHECK(evaluator.resets.front());
            CHECK(!evaluator.resets[1]);
            CHECK(evaluator.resets.back());
        }
    }
}

void offline_photo_stops_after_bounded_warmup_without_encoding_test()
{
    TempDirectory fixture;
    FakeOfflineSource source({OfflineDecodedFrame{{12, 34, 56, 255}, 0, true}});
    FakeNeuralEvaluator evaluator;
    evaluator.requiredPrimeSubmissions = 121;
    FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source, evaluator, encoder, AdvancingNeuralEvidence());
    auto request = OfflineRequest(fixture.Path());
    request.fps = 1.0;
    request.durationSeconds = 1.0;
    const auto result = job.Run(request);
    CHECK(!result.ok);
    CHECK(!result.cancelled);
    CHECK_EQ(120, evaluator.primeSubmissions);
    CHECK_EQ(1, source.opens);
    CHECK(encoder.starts.empty());
    CHECK(evaluator.captured.empty());
}

void offline_job_rejects_when_feature18_receipt_does_not_advance_after_capture_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source,evaluator,encoder,[]{return ValidNeuralEvidence();});
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(0,encoder.finishes);
    CHECK_EQ(120,evaluator.captureSubmissions);
    CHECK(encoder.attempts.front().empty());
}

void offline_job_rejects_any_frame_without_a_neural_evaluation_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    evaluator.failCaptureAt=3;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    auto request=OfflineRequest(fixture.Path());request.frameRetryLimit=0;
    const auto result=job.Run(request,{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(size_t{1},encoder.starts.size());
    CHECK_EQ(NeuralRenderFailure::Neural,result.failure);CHECK_EQ(uint32_t{0},result.frameRetries);
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
    for(const bool byFrameNumber:{false,true}){
        TempDirectory fixture;auto frames=FiveOfflineFrames();
        if(byFrameNumber)frames[3].frameNumber=5;else frames[3].timestamp100ns=frames[2].timestamp100ns;
        FakeOfflineSource source(std::move(frames));FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        const auto result=job.Run(OfflineRequest(fixture.Path()),{},{});
        CHECK(!result.ok);CHECK(!result.cancelled);CHECK(encoder.cancels>0);
        CHECK_EQ(NeuralRenderFailure::Source,result.failure);
        CHECK_EQ(size_t{3},evaluator.captured.size());
    }
}

void offline_job_reports_monotonic_progress_and_smoothed_eta_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    auto now=std::chrono::steady_clock::time_point{};
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence(),
        [&]{now+=100ms;return now;});
    std::vector<NeuralRenderProgress> progress;
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),[&](const auto& value){progress.push_back(value);},{});
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
    std::stop_source stop;OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(OfflineRequest(fixture.Path()),[&](const auto& progress){
        if(progress.completedFrames==2)stop.request_stop();},stop.get_token());
    CHECK(!result.ok);CHECK(result.cancelled);CHECK_EQ(0,encoder.finishes);CHECK(encoder.cancels>0);
    CHECK_EQ(NeuralRenderFailure::Cancelled,result.failure);
}

void offline_job_nvenc_start_failure_restarts_from_frame_zero_with_h264_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    encoder.failNvencStart=true;OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
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
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),[&](const auto& value){progress.push_back(value);},{});
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
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK(calls>3);
    CHECK_EQ(std::vector<EncoderKind>({EncoderKind::HevcNvenc,EncoderKind::H264Software}),encoder.starts);
    CHECK(encoder.attempts.back().empty());
}

void offline_job_does_not_retry_a_temporal_render_from_an_arbitrary_frame_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    evaluator.failCaptureFrom=4;OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK_EQ(std::vector<EncoderKind>({EncoderKind::HevcNvenc}),encoder.starts);
    CHECK_EQ(2,source.opens);
}

NeuralRenderRequest RangeOfflineRequest(const std::filesystem::path& directory,size_t sourceFrames,
                                        int64_t start100ns,int64_t end100ns,uint32_t preroll)
{
    auto request=EvenOfflineRequest(directory);request.fps=25.0;
    request.durationSeconds=double(sourceFrames)/25.0;request.jobId=77;
    request.range={start100ns,end100ns};request.prerollFrames=preroll;return request;
}

void offline_range_render_prerolls_without_capture_and_encodes_only_the_range_test()
{
    TempDirectory fixture;FakeOfflineSource source(OfflineFramesAt25Fps(30));
    FakeNeuralEvaluator evaluator;evaluator.neuralGpuMs=2.5;evaluator.peakVramMiB=512;FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    std::vector<NeuralRenderProgress> progress;
    const auto result=job.Run(RangeOfflineRequest(fixture.Path(),30,10*400000,20*400000,4),
        [&](const auto& value){progress.push_back(value);},{});
    CHECK(result.ok);CHECK_EQ(NeuralRenderFailure::None,result.failure);
    CHECK_EQ(uint64_t{77},result.jobId);
    CHECK_EQ(uint64_t{10},result.frameCount);CHECK_EQ(uint64_t{10},result.nativeEvaluations);
    CHECK_EQ(int64_t{10*400000},result.firstTimestamp100ns);
    CHECK_EQ(int64_t{10*400000},result.duration100ns);
    // Priming reads from the range start; capture restarts at start - preroll.
    CHECK_EQ(std::vector<double>({0.4,0.24}),source.seeks);
    CHECK_EQ(size_t{1},encoder.attempts.size());
    if(!encoder.attempts.empty())CHECK_EQ(size_t{10},encoder.attempts.front().size());
    std::vector<int64_t> expectedCaptured;
    for(int64_t index=10;index<20;++index)expectedCaptured.push_back(index*400000);
    CHECK_EQ(expectedCaptured,evaluator.captured);
    // Four preroll frames were evaluated (not captured) after priming; the
    // first carries the only reset of the capture pass and the range start
    // continues warm history.
    CHECK_EQ(size_t{2+4+10},evaluator.submitted.size());
    if(evaluator.submitted.size()==16){
        CHECK_EQ(int64_t{6*400000},evaluator.submitted[2]);
        CHECK(evaluator.resets[2]);CHECK_EQ(HistoryReset::Preroll,evaluator.ids[2].reset);
        CHECK(std::none_of(evaluator.resets.begin()+3,evaluator.resets.end(),[](bool reset){return reset;}));
        CHECK_EQ(uint64_t{10},evaluator.ids[6].frameNumber);CHECK_EQ(uint64_t{77},evaluator.ids[6].jobId);
    }
    CHECK_EQ(uint32_t{1},result.historyResets);CHECK_EQ(uint32_t{0},result.frameRetries);
    CHECK(std::ranges::all_of(progress,[](const auto& value){return value.totalFrames==10;}));
    CHECK_EQ(uint64_t{10},result.timing.samples);CHECK_EQ(2.5,result.timing.neuralGpuMsP50);
    CHECK_EQ(2.5,result.timing.neuralGpuMsP95);CHECK_EQ(2.5,result.timing.neuralGpuMsMax);
    CHECK_EQ(uint64_t{512},result.timing.peakLocalVramMiB);
}

// A worker that stops presenting after a feature recreate still reports full
// frame, evaluation and evidence counters, so only the GPU cost separates it
// from a real render: 0.46 ms per frame at 1920x1080 was the measured
// DLAA-only output, and 3.26 ms is the lowest healthy median this project has
// evidence for (docs/BENCHMARK.md reference run, same GPU and geometry).
void offline_render_refuses_a_dlaa_only_median_neural_gpu_time_test()
{
    const auto hdRequest=[](const std::filesystem::path& directory){
        auto request=OfflineRequest(directory);request.width=1920;request.height=1080;return request;
    };
    {
        TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        evaluator.neuralGpuMs=0.46;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        const auto result=job.Run(hdRequest(fixture.Path()),{},{});
        CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(NeuralRenderFailure::Neural,result.failure);
        // The refusal has to carry the numbers it judged, both to the user and
        // into the receipt.
        CHECK_EQ(uint64_t{5},result.timing.samples);CHECK_EQ(0.46,result.timing.neuralGpuMsP50);
        CHECK(result.detail.find(L"0.46")!=std::wstring::npos);
        CHECK(result.detail.find(L"1920x1080")!=std::wstring::npos);
    }
    {
        TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        evaluator.neuralGpuMs=3.26;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        const auto result=job.Run(hdRequest(fixture.Path()),{},{});
        CHECK(result.ok);CHECK_EQ(NeuralRenderFailure::None,result.failure);
        CHECK_EQ(uint64_t{5},result.frameCount);CHECK_EQ(3.26,result.timing.neuralGpuMsP50);
    }
    // A build without timing instrumentation reports no samples at all, which
    // is a missing measurement rather than a missing neural pass.
    CHECK(NeuralTimingClearsFloor(NeuralRenderTiming{},3840,2160));
}

void offline_range_start_without_preroll_resets_on_the_first_captured_frame_test()
{
    TempDirectory fixture;FakeOfflineSource source(OfflineFramesAt25Fps(30));
    FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(RangeOfflineRequest(fixture.Path(),30,10*400000,12*400000,0),{},{});
    CHECK(result.ok);CHECK_EQ(uint64_t{2},result.frameCount);
    CHECK_EQ(std::vector<double>({0.4,0.4}),source.seeks);
    CHECK_EQ(size_t{4},evaluator.submitted.size());
    if(evaluator.submitted.size()==4){
        CHECK(evaluator.resets[2]);CHECK_EQ(HistoryReset::FirstFrame,evaluator.ids[2].reset);
        CHECK(!evaluator.resets[3]);
    }
    CHECK_EQ(uint32_t{1},result.historyResets);
}

void offline_single_frame_preview_encodes_exactly_one_frame_test()
{
    TempDirectory fixture;FakeOfflineSource source(OfflineFramesAt25Fps(30));
    FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(RangeOfflineRequest(fixture.Path(),30,5*400000,6*400000,2),{},{});
    CHECK(result.ok);CHECK_EQ(uint64_t{1},result.frameCount);CHECK_EQ(uint64_t{1},result.verifiedNeuralFrames);
    CHECK_EQ(int64_t{5*400000},result.firstTimestamp100ns);CHECK_EQ(int64_t{400000},result.duration100ns);
    CHECK_EQ(std::vector<int64_t>{5*400000},evaluator.captured);
    CHECK_EQ(size_t{1},encoder.attempts.size());
    if(!encoder.attempts.empty())CHECK_EQ(size_t{1},encoder.attempts.front().size());
    CHECK_EQ(1,encoder.finishes);
    // Two preroll frames (3, 4) warmed history before the captured frame.
    CHECK_EQ(std::vector<double>({0.2,0.12}),source.seeks);
    CHECK_EQ(size_t{2+2+1},evaluator.submitted.size());
}

void offline_range_outside_the_source_fails_as_source_before_opening_test()
{
    for(const NeuralRenderRange range:{NeuralRenderRange{10*400000,32*400000},
                                       NeuralRenderRange{12*400000,12*400000},
                                       NeuralRenderRange{12*400000,10*400000}}){
        TempDirectory fixture;FakeOfflineSource source(OfflineFramesAt25Fps(30));
        FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        auto request=RangeOfflineRequest(fixture.Path(),30,range.start100ns,range.end100ns,4);
        const auto result=job.Run(request,{},{});
        CHECK(!result.ok);CHECK_EQ(NeuralRenderFailure::Source,result.failure);
        CHECK_EQ(0,source.opens);CHECK(encoder.starts.empty());CHECK(!result.detail.empty());
    }
    // One frame of container padding past the nominal duration is accepted.
    TempDirectory fixture;FakeOfflineSource source(OfflineFramesAt25Fps(30));
    FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(RangeOfflineRequest(fixture.Path(),30,28*400000,31*400000,4),{},{});
    CHECK(result.ok);CHECK_EQ(uint64_t{2},result.frameCount);
}

void offline_frame_retry_resubmits_the_same_frame_and_succeeds_without_reset_test()
{
    for(const NeuralRenderFailure failure:{NeuralRenderFailure::Neural,NeuralRenderFailure::GpuStall}){
        TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        evaluator.failCaptureAt=2;evaluator.captureFailure=failure;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        std::vector<NeuralRenderProgress> progress;
        const auto result=job.Run(EvenOfflineRequest(fixture.Path()),[&](const auto& value){progress.push_back(value);},{});
        CHECK(result.ok);CHECK_EQ(NeuralRenderFailure::None,result.failure);
        CHECK_EQ(uint64_t{5},result.frameCount);CHECK_EQ(uint32_t{1},result.frameRetries);
        CHECK_EQ(uint32_t{1},result.historyResets);
        CHECK_EQ(size_t{1},encoder.attempts.size());
        if(!encoder.attempts.empty())CHECK_EQ(size_t{5},encoder.attempts.front().size());
        // Priming (0, 333333), first capture (0), frame 2 twice, then the rest.
        CHECK_EQ(std::vector<int64_t>({0,333333,0,333333,333333,666666,999999,1333332}),evaluator.submitted);
        if(evaluator.resets.size()==8){CHECK(!evaluator.resets[3]);CHECK(!evaluator.resets[4]);}
        const auto recovering=std::ranges::find_if(progress,[](const auto& value){
            return value.phase==NeuralRenderPhase::Recovering;});
        CHECK(recovering!=progress.end());
        if(recovering!=progress.end()){
            CHECK_EQ(failure,recovering->recovering);CHECK_EQ(uint32_t{1},recovering->retries);
            CHECK_EQ(uint64_t{1},recovering->completedFrames);
        }
    }
}

void offline_frame_retry_exhaustion_fails_without_omitting_the_frame_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    evaluator.failCaptureFrom=3;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(NeuralRenderFailure::RetryExhausted,result.failure);
    CHECK_EQ(uint32_t{3},result.frameRetries);
    CHECK_EQ(0,encoder.finishes);CHECK_EQ(size_t{1},encoder.attempts.size());
    if(!encoder.attempts.empty())CHECK_EQ(size_t{2},encoder.attempts.front().size());
    // Frame 666666 was submitted once plus three retries, the last with a reset.
    CHECK_EQ(std::vector<int64_t>({0,333333,0,333333,666666,666666,666666,666666}),evaluator.submitted);
    if(evaluator.resets.size()==8){
        CHECK(!evaluator.resets[4]);CHECK(!evaluator.resets[5]);CHECK(!evaluator.resets[6]);
        CHECK(evaluator.resets[7]);CHECK_EQ(HistoryReset::Retry,evaluator.ids[7].reset);
        CHECK(evaluator.ids[7].SameSource(evaluator.ids[4]));
        CHECK(evaluator.ids[7].historyGeneration>evaluator.ids[4].historyGeneration);
    }
    CHECK_EQ(2,source.opens);
}

void offline_device_removal_is_not_retried_per_frame_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    evaluator.failCaptureAt=2;evaluator.captureFailure=NeuralRenderFailure::DeviceRemoved;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK_EQ(NeuralRenderFailure::DeviceRemoved,result.failure);
    CHECK_EQ(uint32_t{0},result.frameRetries);CHECK_EQ(2,evaluator.captureSubmissions);
    CHECK_EQ(0,encoder.finishes);CHECK(encoder.cancels>0);
}

void offline_cut_detected_inside_the_job_counts_as_a_history_reset_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    evaluator.cutAtCapture=3;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
    CHECK(result.ok);CHECK_EQ(uint64_t{5},result.frameCount);
    CHECK_EQ(uint32_t{2},result.historyResets);CHECK_EQ(uint32_t{0},result.frameRetries);
    // The job did not request that reset; the evaluator reported it.
    if(evaluator.resets.size()==7)CHECK(!evaluator.resets[4]);
}

void offline_pause_holds_between_frames_without_a_temporal_reset_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    bool pause=false;int polls=0;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence(),{},[&]{
        if(!pause)return false;
        if(++polls>=3)pause=false;
        return pause;
    });
    std::vector<NeuralRenderProgress> progress;
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),[&](const auto& value){
        progress.push_back(value);
        if(value.phase==NeuralRenderPhase::NeuralRendering&&value.completedFrames==2)pause=true;
    },{});
    CHECK(result.ok);CHECK_EQ(uint64_t{5},result.frameCount);
    CHECK_EQ(3,polls);
    CHECK_EQ(1,int(std::ranges::count_if(progress,[](const auto& value){return value.phase==NeuralRenderPhase::Paused;})));
    const auto paused=std::ranges::find_if(progress,[](const auto& value){return value.phase==NeuralRenderPhase::Paused;});
    if(paused!=progress.end())CHECK_EQ(uint64_t{2},paused->completedFrames);
    CHECK_EQ(uint32_t{1},result.historyResets);
    CHECK(std::none_of(evaluator.resets.begin()+3,evaluator.resets.end(),[](bool reset){return reset;}));
}

void offline_pause_still_honours_cancellation_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    std::stop_source stop;int polls=0;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence(),{},[&]{
        if(evaluator.captureSubmissions<2)return false;
        if(++polls==2)stop.request_stop();
        return true;
    });
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},stop.get_token());
    CHECK(!result.ok);CHECK(result.cancelled);CHECK_EQ(NeuralRenderFailure::Cancelled,result.failure);
    CHECK_EQ(2,evaluator.captureSubmissions);CHECK_EQ(0,encoder.finishes);
}

void offline_identity_mismatch_from_the_evaluator_fails_the_job_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    evaluator.mismatchAtCapture=2;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(NeuralRenderFailure::Identity,result.failure);
    CHECK_EQ(uint32_t{0},result.frameRetries);
    CHECK_EQ(size_t{1},encoder.attempts.size());
    if(!encoder.attempts.empty())CHECK_EQ(size_t{1},encoder.attempts.front().size());
    CHECK_EQ(0,encoder.finishes);CHECK(encoder.cancels>0);
}

void offline_job_passes_guide_controls_to_the_evaluator_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    auto request=EvenOfflineRequest(fixture.Path());request.guides.depth=false;
    const auto result=job.Run(request,{},{});
    CHECK(result.ok);CHECK(evaluator.controls.motionVectors);CHECK(!evaluator.controls.depth);
}

// One rotating segment encoder, with the on-disk behaviour of the real one:
// Start() creates the file, WriteFrame() appends a byte naming the encoder kind
// so a surviving file can be traced to the attempt that wrote it, Finish()
// keeps it, and Cancel() leaves removal to the writer. The writer thread, the
// finalize thread and the test all observe this farm, so it is mutex-guarded.
class SegmentEncoders {
public:
    size_t Start(const std::filesystem::path& output)
    {
        std::lock_guard lock(mutex_);
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        live_.push_back(output);
        starts_.push_back(output);
        return starts_.size() - 1;
    }
    EncodeError Write(size_t id, EncoderKind kind)
    {
        std::lock_guard lock(mutex_);
        if (kind == EncoderKind::HevcNvenc && failNvencWriteAt &&
            ++nvencWrites_ == *failNvencWriteAt) return EncodeError::WriteFailed;
        std::ofstream file(starts_[id], std::ios::binary | std::ios::app);
        file.put(kind == EncoderKind::HevcNvenc ? 'n' : 's');
        return EncodeError::None;
    }
    EncodeError Finish(size_t id)
    {
        std::lock_guard lock(mutex_);
        Retire(id);
        finished_.push_back(starts_[id]);
        return EncodeError::None;
    }
    void Cancel(size_t id)
    {
        std::lock_guard lock(mutex_);
        Retire(id);
    }
    size_t Started()
    {
        std::lock_guard lock(mutex_);
        return starts_.size();
    }
    // Encoders that were started and never finished or cancelled: in production
    // each one is a live ffmpeg process.
    size_t Live()
    {
        std::lock_guard lock(mutex_);
        return live_.size();
    }
    bool Finalized(const std::filesystem::path& path)
    {
        std::lock_guard lock(mutex_);
        return std::find(finished_.begin(), finished_.end(), path) != finished_.end();
    }
    std::optional<size_t> failNvencWriteAt;

private:
    void Retire(size_t id)
    {
        const auto live = std::find(live_.begin(), live_.end(), starts_[id]);
        if (live != live_.end()) live_.erase(live);
    }
    std::mutex mutex_;
    std::vector<std::filesystem::path> starts_, live_, finished_;
    size_t nvencWrites_{};
};

class FakeSegmentEncoder final : public IFrameEncoder {
public:
    explicit FakeSegmentEncoder(SegmentEncoders& farm) : farm_(farm) {}
    EncodeError Start(const EncoderSpec& spec, const std::filesystem::path& output) override
    {
        kind_ = spec.kind;id_ = farm_.Start(output);
        return EncodeError::None;
    }
    EncodeError WriteFrame(std::span<const uint8_t>, std::stop_token stop) override
    {
        if (stop.stop_requested()) return EncodeError::Cancelled;
        return farm_.Write(id_, kind_);
    }
    EncodeError Finish(std::stop_token stop) override
    {
        return stop.stop_requested() ? EncodeError::Cancelled : farm_.Finish(id_);
    }
    void Cancel() override { farm_.Cancel(id_); }

private:
    SegmentEncoders& farm_;
    EncoderKind kind_{EncoderKind::HevcNvenc};
    size_t id_{};
};

std::function<std::unique_ptr<IFrameEncoder>()> SegmentEncoderFactory(SegmentEncoders& farm)
{
    return [&farm] { return std::unique_ptr<IFrameEncoder>(std::make_unique<FakeSegmentEncoder>(farm)); };
}

// OfflineRequest's staging file is neural.partial.mkv, so its segments are
// neural.partial-00000.mkv and so on.
std::filesystem::path OfflineSegmentPath(const std::filesystem::path& directory, uint64_t index)
{
    std::wstring digits = std::to_wstring(index);
    if (digits.size() < 5) digits.insert(0, 5 - digits.size(), L'0');
    return directory / (L"neural.partial-" + digits + L".mkv");
}

// The sink runs on the job's finalize thread; Run() joins it before returning,
// so the test reads what it recorded without any further synchronization.
void segmented_offline_job_publishes_finalized_files_and_drops_the_armed_one_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    SegmentEncoders farm;
    std::vector<NeuralRenderSegment> announced;std::vector<bool> finalizedAtAnnouncement;
    NeuralSegmentSink sink;
    sink.onSegment=[&](const NeuralRenderSegment& segment){
        announced.push_back(segment);
        finalizedAtAnnouncement.push_back(farm.Finalized(fixture.Path()/segment.fileName));
    };
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence(),{},{},
                              SegmentEncoderFactory(farm));
    auto request=EvenOfflineRequest(fixture.Path());request.segmentFrames=2;
    const NeuralRenderResult result=job.Run(request,{},{},sink);
    CHECK(result.ok);CHECK_EQ(uint64_t{5},result.frameCount);
    CHECK_EQ(size_t{3},announced.size());
    for(size_t index=0;index<announced.size();++index){
        CHECK_EQ(uint64_t(index),announced[index].index);
        // A file is published only once its own encoder exited successfully.
        CHECK(finalizedAtAnnouncement[index]);
        CHECK_EQ(std::string(index+1==announced.size()?"n":"nn"),
                 ReadBytes(fixture.Path()/announced[index].fileName));
    }
    CHECK_EQ(uint64_t{2},announced[0].frameCount);CHECK_EQ(uint64_t{1},announced[2].frameCount);
    // Every rotation arms the following file ahead of time, so the job ends
    // holding one it never wrote to: no process and no file may survive it.
    CHECK_EQ(size_t{4},farm.Started());CHECK_EQ(size_t{0},farm.Live());
    CHECK(!std::filesystem::exists(OfflineSegmentPath(fixture.Path(),3)));
}

// The job reports the phases it reached and nothing else, exactly once, so a
// cold-start breakdown can never claim a stage the render never entered.
void offline_job_reports_the_cold_start_phases_it_reached_test()
{
    auto phases=[](const NeuralColdStartTimeline& timeline){
        return std::array{timeline.Phase(NeuralColdStartPhase::NeuralInit).has_value(),
                          timeline.Phase(NeuralColdStartPhase::FeatureArm).has_value(),
                          timeline.Phase(NeuralColdStartPhase::FirstOutput).has_value()};
    };
    // A segmented job reaches all three: the first finalized file is the last
    // boundary the helper owns.
    {
        TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        SegmentEncoders farm;std::vector<NeuralColdStartTimeline> reported;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence(),{},{},
                                  SegmentEncoderFactory(farm));
        auto request=EvenOfflineRequest(fixture.Path());request.segmentFrames=2;
        const auto result=job.Run(request,{},{},{},
            [&](const NeuralColdStartTimeline& timeline){reported.push_back(timeline);});
        CHECK(result.ok);CHECK_EQ(size_t{1},reported.size());
        if(reported.size()==1){
            CHECK((phases(reported[0])==std::array{true,true,true}));
            // The player's own phases and the request-to-picture total are not
            // the job's to measure.
            CHECK(!reported[0].Phase(NeuralColdStartPhase::Request).has_value());
            CHECK(!reported[0].Total().has_value());
        }
    }
    // A single-file job publishes nothing while it runs, so its last boundary
    // is the arming: firstOutput is absent rather than zero.
    {
        TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        std::vector<NeuralColdStartTimeline> reported;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{},{},
            [&](const NeuralColdStartTimeline& timeline){reported.push_back(timeline);});
        CHECK(result.ok);CHECK_EQ(size_t{1},reported.size());
        if(reported.size()==1)CHECK((phases(reported[0])==std::array{true,true,false}));
    }
    // A job whose feature is never created stops inside the arming, and still
    // reports the initialization it did pay for.
    {
        TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        evaluator.requiredPrimeSubmissions=1000;
        std::vector<NeuralColdStartTimeline> reported;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{},{},
            [&](const NeuralColdStartTimeline& timeline){reported.push_back(timeline);});
        CHECK(!result.ok);CHECK_EQ(size_t{1},reported.size());
        if(reported.size()==1)CHECK((phases(reported[0])==std::array{true,false,false}));
    }
    // A request the job refuses outright never entered a phase at all.
    {
        TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        std::vector<NeuralColdStartTimeline> reported;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        auto request=EvenOfflineRequest(fixture.Path());request.width=0;
        const auto result=job.Run(request,{},{},{},
            [&](const NeuralColdStartTimeline& timeline){reported.push_back(timeline);});
        CHECK(!result.ok);CHECK_EQ(size_t{1},reported.size());
        if(reported.size()==1)CHECK(reported[0]==NeuralColdStartTimeline{});
    }
}

// Nothing can be shown until the first file is muxed, so a live session asks
// for a short one. Only the first: a boundary costs an encoder start.
void segmented_offline_job_makes_only_the_first_file_short_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    SegmentEncoders farm;
    std::vector<NeuralRenderSegment> announced;
    NeuralSegmentSink sink;
    sink.onSegment=[&](const NeuralRenderSegment& segment){announced.push_back(segment);};
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence(),{},{},
                              SegmentEncoderFactory(farm));
    auto request=EvenOfflineRequest(fixture.Path());
    request.segmentFrames=2;request.firstSegmentFrames=1;
    const NeuralRenderResult result=job.Run(request,{},{},sink);
    CHECK(result.ok);CHECK_EQ(uint64_t{5},result.frameCount);
    CHECK_EQ(size_t{3},announced.size());
    const std::vector<uint64_t> frames{announced[0].frameCount,announced[1].frameCount,announced[2].frameCount};
    CHECK_EQ(std::vector<uint64_t>({1,2,2}),frames);
    for(size_t index=0;index<announced.size();++index)CHECK_EQ(uint64_t(index),announced[index].index);
}

void segmented_offline_job_software_retry_deletes_the_failed_attempts_files_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    SegmentEncoders farm;farm.failNvencWriteAt=3;   // first frame of segment 1
    size_t restarts=0;std::vector<NeuralRenderSegment> announced;
    NeuralSegmentSink sink;
    sink.onSegment=[&](const NeuralRenderSegment& segment){announced.push_back(segment);};
    sink.onRestart=[&]{++restarts;};
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence(),{},{},
                              SegmentEncoderFactory(farm));
    auto request=EvenOfflineRequest(fixture.Path());request.segmentFrames=2;
    const NeuralRenderResult result=job.Run(request,{},{},sink);
    CHECK(result.ok);CHECK_EQ(EncoderKind::H264Software,result.encoder);
    CHECK_EQ(uint64_t{5},result.frameCount);CHECK_EQ(size_t{1},restarts);
    CHECK(announced.size()>=size_t{3});
    for(size_t index=0;index<3;++index){
        const auto& segment=announced[announced.size()-3+index];
        CHECK_EQ(uint64_t(index),segment.index);
    }
    // Renumbering from zero only means anything if nothing of the abandoned
    // attempt is left: no NVENC-written file, no armed file, no live encoder.
    CHECK_EQ(size_t{0},farm.Live());
    for(uint64_t index=0;index<3;++index){
        CHECK_EQ(std::string(index==2?"s":"ss"),
                 ReadBytes(OfflineSegmentPath(fixture.Path(),index)));
    }
    CHECK(!std::filesystem::exists(OfflineSegmentPath(fixture.Path(),3)));
}

void segmented_offline_job_cancel_leaves_no_unpublished_file_or_live_encoder_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    SegmentEncoders farm;std::stop_source stop;
    std::vector<NeuralRenderSegment> announced;
    NeuralSegmentSink sink;
    sink.onSegment=[&](const NeuralRenderSegment& segment){announced.push_back(segment);};
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence(),{},{},
                              SegmentEncoderFactory(farm));
    auto request=EvenOfflineRequest(fixture.Path());request.segmentFrames=2;
    const NeuralRenderResult result=job.Run(request,[&](const NeuralRenderProgress& progress){
        if(progress.completedFrames==3)stop.request_stop();
    },stop.get_token(),sink);
    CHECK(!result.ok);CHECK(result.cancelled);
    // No orphan ffmpeg, and no file on disk that no consumer was ever told
    // about: the half-written segment and the armed next one are both gone.
    CHECK_EQ(size_t{0},farm.Live());
    for(uint64_t index=0;index<5;++index){
        const auto path=OfflineSegmentPath(fixture.Path(),index);
        const bool published=std::any_of(announced.begin(),announced.end(),
            [&](const NeuralRenderSegment& segment){return segment.fileName==path.filename();});
        CHECK_EQ(published,std::filesystem::exists(path));
    }
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

constexpr int64_t kLiveFrame100ns=333333;

// One stream per path: 30 fps frames rebased to the file's own zero with
// authoritative frame numbers, exactly like a finalized segment file. Segment
// files are opened on a worker thread now, so the counters are guarded.
struct LiveFrameLibrary {
    struct Stream {
        std::vector<VideoFrame> frames;
        int opens{},closes{},seeks{},notReadyReads{};
        bool failOpen{};
        // How the last open arrived: with the parameters a sibling probed, and
        // on which thread. The boundary must pay for neither a probe nor a
        // process start on the thread that presents frames.
        bool known{};
        std::thread::id thread{};
    };
    Stream& Add(const std::filesystem::path& path,uint64_t frameCount)
    {
        Stream& stream=streams[path];
        stream.frames.clear();
        for(uint64_t index=0;index<frameCount;++index){
            VideoFrame frame;
            frame.bgra={uint8_t(index),0,0,255};
            frame.timestamp100ns=int64_t(index)*kLiveFrame100ns;
            frame.frameNumber=index;
            stream.frames.push_back(std::move(frame));
        }
        return stream;
    }
    int Opens(const std::filesystem::path& path){std::lock_guard lock(mutex);return streams[path].opens;}
    int Closes(const std::filesystem::path& path){std::lock_guard lock(mutex);return streams[path].closes;}
    int Seeks(const std::filesystem::path& path){std::lock_guard lock(mutex);return streams[path].seeks;}
    // The background open lands within milliseconds; the deadline only keeps a
    // broken prefetch from hanging the suite.
    bool WaitForOpen(const std::filesystem::path& path,int expected)
    {
        for(int attempt=0;attempt<400;++attempt){
            if(Opens(path)>=expected)return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }
    bool OpenedKnown(const std::filesystem::path& path){std::lock_guard lock(mutex);return streams[path].known;}
    std::thread::id OpenThread(const std::filesystem::path& path)
    {
        std::lock_guard lock(mutex);return streams[path].thread;
    }
    std::map<std::filesystem::path,Stream> streams;
    std::mutex mutex;
};

class LiveLibrarySource final : public ISynchronizedFrameSource {
public:
    explicit LiveLibrarySource(LiveFrameLibrary& library):library_(library){}
    bool Open(const std::filesystem::path& path,std::stop_token stop) override
    {
        return OpenRecording(path,stop,false);
    }
    bool OpenKnown(const std::filesystem::path& path,const VideoDecoder::KnownMedia& media,
                   std::stop_token stop) override
    {
        // Real segments after the first arrive here, with the geometry and frame
        // rate the first one probed rather than a probe of their own.
        if(!media.Valid())return false;
        return OpenRecording(path,stop,true);
    }
    void Close() override
    {
        std::lock_guard lock(library_.mutex);
        if(stream_)++stream_->closes;stream_=nullptr;
    }
    VideoReadResult Read(VideoFrame& frame,std::stop_token stop) override
    {
        std::lock_guard lock(library_.mutex);
        if(stop.stop_requested())return VideoReadResult::Cancelled;
        if(!stream_)return VideoReadResult::Error;
        if(stream_->notReadyReads>0){--stream_->notReadyReads;return VideoReadResult::NotReady;}
        if(index_>=stream_->frames.size())return VideoReadResult::EndOfStream;
        frame=stream_->frames[index_++];return VideoReadResult::FrameReady;
    }
    bool SeekSeconds(double seconds) override
    {
        std::lock_guard lock(library_.mutex);
        if(!stream_)return false;
        ++stream_->seeks;
        const int64_t target=static_cast<int64_t>(seconds*10000000.0);
        index_=0;
        while(index_<stream_->frames.size()&&stream_->frames[index_].timestamp100ns<target)++index_;
        return true;
    }
    uint32_t Width() const override { return 4; }
    uint32_t Height() const override { return 4; }
    double FrameRate() const override { return 30.0; }
    double DurationSeconds() const override
    {
        if(!stream_||stream_->frames.empty())return 0.0;
        return double(stream_->frames.back().timestamp100ns+kLiveFrame100ns)*1e-7;
    }
private:
    bool OpenRecording(const std::filesystem::path& path,std::stop_token stop,bool known)
    {
        std::lock_guard lock(library_.mutex);
        stream_=nullptr;
        const auto found=library_.streams.find(path);
        if(found==library_.streams.end()||found->second.failOpen||stop.stop_requested())return false;
        stream_=&found->second;++stream_->opens;index_=0;
        stream_->known=known;stream_->thread=std::this_thread::get_id();
        return true;
    }
    LiveFrameLibrary& library_;
    LiveFrameLibrary::Stream* stream_{};
    size_t index_{};
};

NeuralSegment LiveSegmentRecord(std::filesystem::path path,uint64_t index,uint64_t firstFrame,
                                uint64_t frameCount,uint64_t runId=0)
{
    NeuralSegment segment;
    segment.path=std::move(path);segment.runId=runId;segment.index=index;
    segment.firstFrameNumber=firstFrame;
    segment.firstTimestamp100ns=int64_t(firstFrame)*kLiveFrame100ns;
    segment.end100ns=int64_t(firstFrame+frameCount)*kLiveFrame100ns;
    segment.frameCount=frameCount;
    return segment;
}

// Mirrors DecodeSegment: the start is the file's own first pts on the exact CFR
// grid, while the exclusive end is rebuilt from the integer frame duration, so
// a fractional frame rate leaves a sub-frame hole before the next segment.
NeuralSegment RoundedSegmentRecord(std::filesystem::path path,uint64_t index,uint64_t firstFrame,
                                   uint64_t frameCount,uint64_t runId=0)
{
    NeuralSegment segment;
    segment.path=std::move(path);segment.runId=runId;segment.index=index;
    segment.firstFrameNumber=firstFrame;
    segment.firstTimestamp100ns=std::llround(double(firstFrame)*10000000.0/30.0);
    segment.end100ns=segment.firstTimestamp100ns+int64_t(frameCount)*kLiveFrame100ns;
    segment.frameCount=frameCount;
    return segment;
}

SynchronizedPlayback::SegmentSourceFactory LiveSegmentFactory(LiveFrameLibrary& library)
{
    return [&library]{return std::make_unique<LiveLibrarySource>(library);};
}

// The library numbers every file's frames from its own zero, so a frame served
// from the wrong rendered region carries the right number once the segment
// record rebases it. Tagging a file's pixels names the region it really is.
constexpr size_t kRegionTagChannel=1;
void TagLiveStream(LiveFrameLibrary::Stream& stream,uint8_t tag)
{
    for(VideoFrame& frame:stream.frames)frame.bgra[kRegionTagChannel]=tag;
}

int LiveStreamTag(const VideoFrame& frame)
{
    return frame.bgra.size()>kRegionTagChannel?int(frame.bgra[kRegionTagChannel]):-1;
}

// The coverage a backward seek leaves behind: run 1 rendered frames 20-29, the
// user seeked back to the start, and run 2 rendered frames 0-4 there. The two
// regions are disjoint and both are rendered work.
void AppendTwoDisjointRegions(NeuralSegmentIndex& index)
{
    index.Append(LiveSegmentRecord(L"run1/neural-00000.mkv",0,20,5,1));
    index.Append(LiveSegmentRecord(L"run1/neural-00001.mkv",1,25,5,1));
    index.Append(LiveSegmentRecord(L"run2/neural-00000.mkv",0,0,5,2));
}

void neural_segment_index_orders_appends_and_locates_by_timestamp_test()
{
    NeuralSegmentIndex index;
    CHECK(index.Empty());CHECK_EQ(size_t{0},index.Count());
    CHECK_EQ(int64_t{0},index.Start100ns());CHECK_EQ(int64_t{0},index.Head100ns());
    CHECK_EQ(uint64_t{0},index.TotalFrames());
    CHECK(!index.At(0).has_value());CHECK(!index.Containing(0).has_value());

    index.Append(LiveSegmentRecord(L"run1/neural-00000.mkv",0,10,5,1));
    index.Append(LiveSegmentRecord(L"run1/neural-00001.mkv",1,15,3,1));
    // Ground another file already owns is never published over: two owners for
    // one timestamp would move the timeline under a decoder reading it.
    index.Append(LiveSegmentRecord(L"overlap.mkv",2,16,3,1));
    CHECK(!index.Empty());CHECK_EQ(size_t{2},index.Count());
    CHECK_EQ(uint64_t{8},index.TotalFrames());

    // A backward seek makes the next run publish behind the first one. That
    // earlier region is kept and sorted into place, not dropped as stale.
    index.Append(LiveSegmentRecord(L"run2/neural-00000.mkv",0,0,3,2));
    CHECK_EQ(size_t{3},index.Count());
    CHECK_EQ(uint64_t{11},index.TotalFrames());
    CHECK_EQ(int64_t{0},index.Start100ns());
    CHECK_EQ(int64_t{18*kLiveFrame100ns},index.Head100ns());
    if(const auto earliest=index.At(0))
        CHECK_EQ(std::filesystem::path(L"run2/neural-00000.mkv"),earliest->path);
    if(const auto behind=index.Containing(1*kLiveFrame100ns)){
        CHECK_EQ(std::filesystem::path(L"run2/neural-00000.mkv"),behind->path);
        CHECK_EQ(uint64_t{2},behind->runId);
    }
    CHECK(!index.At(3).has_value());
    if(const auto second=index.At(2))
        CHECK_EQ(std::filesystem::path(L"run1/neural-00001.mkv"),second->path);
    // Start100ns() and Head100ns() only bound the set; the gap between the two
    // regions is still unrendered.
    CHECK(!index.Covered(5*kLiveFrame100ns));
    CHECK(!index.Containing(9*kLiveFrame100ns).has_value());

    if(const auto first=index.Containing(10*kLiveFrame100ns))CHECK_EQ(uint64_t{0},first->index);
    if(const auto beforeSeam=index.Containing(15*kLiveFrame100ns-1))CHECK_EQ(uint64_t{0},beforeSeam->index);
    if(const auto afterSeam=index.Containing(15*kLiveFrame100ns))CHECK_EQ(uint64_t{1},afterSeam->index);
    if(const auto tail=index.Containing(18*kLiveFrame100ns-1))CHECK_EQ(uint64_t{1},tail->index);
    CHECK(!index.Containing(18*kLiveFrame100ns).has_value());

    // A run filling the hole ends on its own segment boundary, so its last file
    // reaches into the region beyond. The declared end is clamped there: those
    // frames are already served from the older file.
    index.Append(LiveSegmentRecord(L"run3/neural-00000.mkv",0,6,6,3));
    if(const auto filler=index.Containing(6*kLiveFrame100ns)){
        CHECK_EQ(uint64_t{3},filler->runId);
        CHECK_EQ(int64_t{10*kLiveFrame100ns},filler->end100ns);
    }
    if(const auto kept=index.Containing(10*kLiveFrame100ns))
        CHECK_EQ(std::filesystem::path(L"run1/neural-00000.mkv"),kept->path);
    CHECK(!index.Covered(4*kLiveFrame100ns));
    CHECK_EQ(size_t{2},index.CoveredRanges().size());

    index.Restart();
    CHECK(index.Empty());CHECK_EQ(size_t{0},index.Count());
    CHECK_EQ(int64_t{0},index.Start100ns());CHECK_EQ(int64_t{0},index.Head100ns());
    CHECK_EQ(uint64_t{0},index.TotalFrames());
    CHECK(index.CoveredRanges().empty());
    CHECK(!index.Containing(10*kLiveFrame100ns).has_value());
    // A relaunched job numbers its segments from zero again.
    index.Append(LiveSegmentRecord(L"neural-00000.mkv",0,20,4));
    CHECK_EQ(size_t{1},index.Count());
    CHECK_EQ(int64_t{20*kLiveFrame100ns},index.Start100ns());
    CHECK_EQ(int64_t{24*kLiveFrame100ns},index.Head100ns());
}

// Turning the toggle off keeps rendered coverage so the next session resumes at
// the head. The index therefore has to accept a second job's segments after the
// first job's, and undo only that job's own segments when its worker relaunches.
void neural_segment_index_resumes_after_retained_coverage_test()
{
    NeuralSegmentIndex index;
    index.Append(LiveSegmentRecord(L"job1/neural-00000.mkv",0,10,5,1));
    index.Append(LiveSegmentRecord(L"job1/neural-00001.mkv",1,15,5,1));
    CHECK_EQ(size_t{2},index.Count());

    // The resumed job numbers from its own zero and continues at the seam.
    index.Append(LiveSegmentRecord(L"job2/neural-00000.mkv",0,20,5,2));
    index.Append(LiveSegmentRecord(L"job2/neural-00001.mkv",1,25,5,2));
    CHECK_EQ(size_t{4},index.Count());
    CHECK_EQ(uint64_t{20},index.TotalFrames());
    CHECK_EQ(int64_t{10*kLiveFrame100ns},index.Start100ns());
    CHECK_EQ(int64_t{30*kLiveFrame100ns},index.Head100ns());
    // Two jobs that meet leave one region, not a boundary to stall on.
    CHECK_EQ(size_t{1},index.CoveredRanges().size());
    if(const auto beforeSeam=index.Containing(20*kLiveFrame100ns-1))
        CHECK_EQ(uint64_t{1},beforeSeam->runId);
    if(const auto afterSeam=index.Containing(20*kLiveFrame100ns))
        CHECK_EQ(uint64_t{2},afterSeam->runId);

    // That job's worker crashed and republishes from its own index zero, so its
    // files are stale. Only they go; the first job's rendered seconds survive.
    index.DropRun(2);
    CHECK_EQ(size_t{2},index.Count());
    CHECK_EQ(uint64_t{10},index.TotalFrames());
    CHECK_EQ(int64_t{10*kLiveFrame100ns},index.Start100ns());
    CHECK_EQ(int64_t{20*kLiveFrame100ns},index.Head100ns());
    CHECK(index.Covered(15*kLiveFrame100ns));
    CHECK(!index.Covered(20*kLiveFrame100ns));

    // A run that published nothing is not a clear, and does not repaint.
    const uint64_t settled=index.Revision();
    index.DropRun(7);
    CHECK_EQ(size_t{2},index.Count());
    CHECK_EQ(settled,index.Revision());

    // A run that rendered behind the retained coverage is dropped the same way,
    // wherever in the timeline its segments sit.
    index.Append(LiveSegmentRecord(L"job3/neural-00000.mkv",0,0,5,3));
    CHECK_EQ(int64_t{0},index.Start100ns());
    CHECK_EQ(size_t{2},index.CoveredRanges().size());
    index.DropRun(3);
    CHECK_EQ(size_t{2},index.Count());
    CHECK_EQ(uint64_t{10},index.TotalFrames());
    CHECK_EQ(int64_t{10*kLiveFrame100ns},index.Start100ns());
    CHECK(index.Covered(12*kLiveFrame100ns));

    index.DropRun(1);
    CHECK(index.Empty());CHECK_EQ(uint64_t{0},index.TotalFrames());CHECK_EQ(int64_t{0},index.Head100ns());
}

// The pace a session reports is steady-state: it starts at the first segment
// this job published (which absorbs helper startup) and counts only the frames
// that arrived after it. Dropping a run's segments restarts that measurement.
void neural_segment_index_pace_counts_frames_after_the_first_segment_of_a_run_test()
{
    NeuralSegmentIndex index;
    CHECK_EQ(uint64_t{0},index.Pace().frames);
    CHECK_EQ(0.0,index.Pace().MsPerFrame());
    index.Append(LiveSegmentRecord(L"job1/neural-00000.mkv",0,10,60,1));
    CHECK_EQ(uint64_t{0},index.Pace().frames);
    index.Append(LiveSegmentRecord(L"job1/neural-00001.mkv",1,70,60,1));
    index.Append(LiveSegmentRecord(L"job1/neural-00002.mkv",2,130,18,1));
    CHECK_EQ(uint64_t{78},index.Pace().frames);
    CHECK(index.Pace().wallMs>=0.0);

    // A relaunch drops that run's segments and the pace measured from them; the
    // relaunched worker's own first segment pays for startup again.
    index.DropRun(1);
    CHECK_EQ(uint64_t{0},index.Pace().frames);
    index.Append(LiveSegmentRecord(L"job2/neural-00000.mkv",0,10,60,2));
    CHECK_EQ(uint64_t{0},index.Pace().frames);
    index.Append(LiveSegmentRecord(L"job2/neural-00001.mkv",1,70,60,2));
    CHECK_EQ(uint64_t{60},index.Pace().frames);
    index.Restart();
    CHECK_EQ(uint64_t{0},index.Pace().frames);
}

void live_playback_waits_at_the_render_head_and_resumes_on_a_new_segment_test()
{
    LiveFrameLibrary library;library.Add(L"original.mkv",40);
    library.Add(L"neural-00000.mkv",5);library.Add(L"neural-00001.mkv",5);
    LiveLibrarySource original(library);
    SynchronizedPlayback playback(original,LiveSegmentFactory(library));
    const auto segments=std::make_shared<NeuralSegmentIndex>();
    CHECK(playback.OpenLive(L"original.mkv",segments,SynchronizedRange{10*kLiveFrame100ns,0},{}));
    CHECK(playback.Live());CHECK(playback.NeuralAvailable());
    CHECK_EQ(int64_t{0},playback.LiveHead100ns());
    // Nothing is rendered yet: playback stalls instead of ending.
    CHECK_EQ(SynchronizedReadResult::WaitingForRender,playback.ReadNextAvailable({}));
    CHECK_EQ(SynchronizedReadResult::WaitingForRender,playback.ReadNextAvailable({}));
    CHECK(playback.CurrentPair()==nullptr);

    segments->Append(LiveSegmentRecord(L"neural-00000.mkv",0,10,5));
    CHECK_EQ(int64_t{15*kLiveFrame100ns},playback.LiveHead100ns());
    for(uint64_t expected=10;expected<15;++expected){
        CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        const auto* pair=playback.CurrentPair();CHECK(pair!=nullptr);
        if(!pair)return;
        CHECK_EQ(expected,pair->frameNumber);
        CHECK_EQ(int64_t(expected)*kLiveFrame100ns,pair->timestamp100ns);
        CHECK_EQ(pair->original.frameNumber,pair->neural.frameNumber);
        CHECK_EQ(pair->original.timestamp100ns,pair->neural.timestamp100ns);
        CHECK(!pair->neural.bgra.empty());
    }
    // The playhead caught the head again; the job still owes frames.
    CHECK_EQ(SynchronizedReadResult::WaitingForRender,playback.ReadNextAvailable({}));
    CHECK(playback.SetView(ComparisonView::Neural));
    CHECK_EQ(1,library.Opens(L"neural-00000.mkv"));
    // Waiting on the render is not a fault, and it is not an end: a live read
    // ends on the playable range's end, never on where coverage stops.
    CHECK(playback.LastFault().empty());
    segments->Append(LiveSegmentRecord(L"neural-00001.mkv",1,15,5));
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    if(const auto* resumed=playback.CurrentPair()){
        CHECK_EQ(uint64_t{15},resumed->frameNumber);
        CHECK_EQ(resumed->original.frameNumber,resumed->neural.frameNumber);
    }
}

void live_playback_crosses_a_segment_boundary_without_a_gap_or_stall_test()
{
    LiveFrameLibrary library;library.Add(L"original.mkv",40);
    library.Add(L"neural-00000.mkv",5);library.Add(L"neural-00001.mkv",5);
    LiveLibrarySource original(library);
    SynchronizedPlayback playback(original,LiveSegmentFactory(library));
    const auto segments=std::make_shared<NeuralSegmentIndex>();
    segments->Append(LiveSegmentRecord(L"neural-00000.mkv",0,10,5));
    segments->Append(LiveSegmentRecord(L"neural-00001.mkv",1,15,5));
    CHECK(playback.OpenLive(L"original.mkv",segments,
                            SynchronizedRange{10*kLiveFrame100ns,20*kLiveFrame100ns},{}));
    std::vector<uint64_t> played;
    for(int index=0;index<10;++index){
        CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        const auto* pair=playback.CurrentPair();CHECK(pair!=nullptr);
        if(!pair)return;
        played.push_back(pair->frameNumber);
        CHECK_EQ(pair->original.frameNumber,pair->neural.frameNumber);
        CHECK_EQ(pair->original.timestamp100ns,pair->neural.timestamp100ns);
        // The next file is opened on a worker thread while the current one still
        // serves frames, so the boundary itself spawns nothing.
        if(index==0)CHECK(library.WaitForOpen(L"neural-00001.mkv",1));
    }
    std::vector<uint64_t> expected;
    for(uint64_t number=10;number<20;++number)expected.push_back(number);
    CHECK_EQ(expected,played);
    // The seam cost no reopen and no decode stall.
    CHECK_EQ(1,library.Opens(L"neural-00000.mkv"));
    CHECK_EQ(1,library.Opens(L"neural-00001.mkv"));
    CHECK_EQ(0,library.Seeks(L"neural-00001.mkv"));
    CHECK_EQ(1,library.Closes(L"neural-00000.mkv"));
    // It also cost the presenting thread nothing: the second segment was opened
    // on another thread, and with the first segment's parameters rather than a
    // probe of its own. Doing either on this thread dropped 44% of the frames of
    // a 1080p30 session (docs/VERIFICATION-2026-09-12-RTX5090.md).
    CHECK(library.OpenThread(L"neural-00001.mkv")!=std::this_thread::get_id());
    CHECK(library.OpenedKnown(L"neural-00001.mkv"));
    CHECK(!library.OpenedKnown(L"neural-00000.mkv"));
    // The stream ends where the playable range ends - frame 20 - and not where
    // the coverage happens to stop.
    CHECK_EQ(SynchronizedReadResult::EndOfStream,playback.ReadNextAvailable({}));
    CHECK(playback.LastFault().empty());
}

// The seam a 30000/1001-style frame duration leaves behind: segment 0 declares
// an end a couple of ticks below segment 1's first pts, and the playhead of a
// seeked original lands inside that hole. It cost a live 1080p session on an
// RTX 5090 its playback with "out of sync" at the first boundary.
void live_playback_crosses_a_seam_whose_end_rounds_below_the_next_start_test()
{
    LiveFrameLibrary library;library.Add(L"original.mkv",20);
    library.Add(L"neural-00000.mkv",5);library.Add(L"neural-00001.mkv",5);
    LiveLibrarySource original(library);
    SynchronizedPlayback playback(original,LiveSegmentFactory(library));
    const auto segments=std::make_shared<NeuralSegmentIndex>();
    segments->Append(RoundedSegmentRecord(L"neural-00000.mkv",0,0,5));
    segments->Append(RoundedSegmentRecord(L"neural-00001.mkv",1,5,5));
    CHECK(playback.OpenLive(L"original.mkv",segments,SynchronizedRange{},{}));
    for(uint64_t expected=0;expected<10;++expected){
        CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        const auto* pair=playback.CurrentPair();CHECK(pair!=nullptr);
        if(!pair)return;
        CHECK_EQ(expected,pair->frameNumber);
        CHECK_EQ(pair->original.frameNumber,pair->neural.frameNumber);
    }
    CHECK(playback.LastFault().empty());
}

void neural_segment_index_covers_the_rounding_hole_but_not_a_real_gap_test()
{
    NeuralSegmentIndex index;
    index.Append(RoundedSegmentRecord(L"neural-00000.mkv",0,0,5));
    index.Append(RoundedSegmentRecord(L"neural-00001.mkv",1,5,5));
    const auto second=index.At(1);
    CHECK(second.has_value());
    if(!second)return;
    // Every timestamp up to the next segment's first pts belongs to the first.
    for(int64_t back=1;back<=3;++back)
        if(const auto before=index.Containing(second->firstTimestamp100ns-back))
            CHECK_EQ(uint64_t{0},before->index);
    CHECK(index.Containing(second->firstTimestamp100ns-1).has_value());
    if(const auto at=index.Containing(second->firstTimestamp100ns))CHECK_EQ(uint64_t{1},at->index);

    // A rebased relaunch leaves a real gap, which stays uncovered.
    NeuralSegmentIndex gapped;
    gapped.Append(RoundedSegmentRecord(L"neural-00000.mkv",0,0,5));
    gapped.Append(RoundedSegmentRecord(L"job2/neural-00000.mkv",1,8,5));
    CHECK(!gapped.Containing(6*kLiveFrame100ns).has_value());
}

void live_seek_enters_a_rendered_segment_and_refuses_an_unrendered_target_test()
{
    LiveFrameLibrary library;library.Add(L"original.mkv",40);
    library.Add(L"neural-00000.mkv",5);library.Add(L"neural-00001.mkv",5);
    LiveLibrarySource original(library);
    SynchronizedPlayback playback(original,LiveSegmentFactory(library));
    const auto segments=std::make_shared<NeuralSegmentIndex>();
    segments->Append(LiveSegmentRecord(L"neural-00000.mkv",0,10,5));
    segments->Append(LiveSegmentRecord(L"neural-00001.mkv",1,15,5));
    CHECK(playback.OpenLive(L"original.mkv",segments,SynchronizedRange{10*kLiveFrame100ns,0},{}));
    CHECK(playback.SeekSeconds(double(17*kLiveFrame100ns)*1e-7,{}));
    if(const auto* pair=playback.CurrentPair()){
        CHECK_EQ(uint64_t{17},pair->frameNumber);
        CHECK_EQ(uint64_t{17},pair->neural.frameNumber);
        CHECK_EQ(int64_t{17*kLiveFrame100ns},pair->neural.timestamp100ns);
    }
    // Past the render head: refused without unloading the session.
    CHECK(!playback.SeekSeconds(double(25*kLiveFrame100ns)*1e-7,{}));
    CHECK(playback.Live());CHECK(playback.NeuralAvailable());
    if(const auto* held=playback.CurrentPair())CHECK_EQ(uint64_t{17},held->frameNumber);
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    if(const auto* next=playback.CurrentPair())CHECK_EQ(uint64_t{18},next->frameNumber);
    // Seeking back reopens an earlier segment and seeks inside the file.
    CHECK(playback.SeekSeconds(double(11*kLiveFrame100ns)*1e-7,{}));
    if(const auto* back=playback.CurrentPair()){
        CHECK_EQ(uint64_t{11},back->frameNumber);
        CHECK_EQ(uint64_t{11},back->neural.frameNumber);
    }
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    if(const auto* forward=playback.CurrentPair())CHECK_EQ(uint64_t{12},forward->frameNumber);
}

// A session renders in runs, and a backward seek makes the next run start
// behind an earlier one. Both rendered regions have to survive that, and each
// one has to stay watchable from inside itself: the player used to hold a
// single render head, so the earlier region was either unreachable or the
// reason the later one was thrown away.
void neural_segment_index_keeps_two_disjoint_rendered_regions_test()
{
    NeuralSegmentIndex index;
    AppendTwoDisjointRegions(index);
    CHECK_EQ(size_t{3},index.Count());
    CHECK_EQ(uint64_t{15},index.TotalFrames());
    // Rendered inside either region, unrendered in the hole between them.
    CHECK(index.Covered(0));
    CHECK(index.Covered(4*kLiveFrame100ns));
    CHECK(index.Covered(20*kLiveFrame100ns));
    CHECK(index.Covered(27*kLiveFrame100ns));
    CHECK(!index.Covered(5*kLiveFrame100ns));
    CHECK(!index.Covered(12*kLiveFrame100ns));
    CHECK(!index.Covered(19*kLiveFrame100ns));
    CHECK(!index.Covered(30*kLiveFrame100ns));

    // Exactly two regions, the two segments of one run joined where they touch.
    const auto ranges=index.CoveredRanges();
    CHECK_EQ(size_t{2},ranges.size());
    if(ranges.size()!=2)return;
    CHECK_EQ(int64_t{0},ranges[0].start100ns);
    CHECK_EQ(int64_t{5*kLiveFrame100ns},ranges[0].end100ns);
    CHECK_EQ(int64_t{20*kLiveFrame100ns},ranges[1].start100ns);
    CHECK_EQ(int64_t{30*kLiveFrame100ns},ranges[1].end100ns);

    // The buffer a playhead has is its own region. Answering with the far one
    // would attach a session that has nothing to show at the playhead.
    const auto early=index.PlayableSpan(1*kLiveFrame100ns);
    CHECK(early.has_value());
    if(early){
        CHECK_EQ(int64_t{0},early->start100ns);
        CHECK_EQ(int64_t{5*kLiveFrame100ns},early->end100ns);
    }
    const auto late=index.PlayableSpan(21*kLiveFrame100ns);
    CHECK(late.has_value());
    if(late){
        CHECK_EQ(int64_t{20*kLiveFrame100ns},late->start100ns);
        CHECK_EQ(int64_t{30*kLiveFrame100ns},late->end100ns);
    }
    // Inside the hole there is nothing to play, however much is rendered ahead.
    CHECK(!index.PlayableSpan(12*kLiveFrame100ns).has_value());
}

// Where a read inside a hole has to continue, and where the timeline really
// ends. Both used to be the same question because coverage was one run.
void neural_segment_index_after_finds_the_next_region_from_a_hole_test()
{
    NeuralSegmentIndex index;
    AppendTwoDisjointRegions(index);
    // From inside the hole: the first segment of the region that follows it.
    if(const auto next=index.After(12*kLiveFrame100ns)){
        CHECK_EQ(int64_t{20*kLiveFrame100ns},next->firstTimestamp100ns);
        CHECK_EQ(uint64_t{1},next->runId);
        CHECK_EQ(uint64_t{0},next->index);
    }else CHECK(index.After(12*kLiveFrame100ns).has_value());
    // From inside a region: the file that continues it, whatever run wrote it.
    if(const auto following=index.After(21*kLiveFrame100ns)){
        CHECK_EQ(int64_t{25*kLiveFrame100ns},following->firstTimestamp100ns);
        CHECK_EQ(uint64_t{1},following->index);
    }else CHECK(index.After(21*kLiveFrame100ns).has_value());
    // Before everything: the earliest region, which a later run published.
    if(const auto first=index.After(-1)){
        CHECK_EQ(int64_t{0},first->firstTimestamp100ns);
        CHECK_EQ(uint64_t{2},first->runId);
    }else CHECK(index.After(-1).has_value());
    // Past the last segment there is nothing to leave for: waiting, not a gap.
    CHECK(!index.After(29*kLiveFrame100ns).has_value());
    CHECK(!index.After(40*kLiveFrame100ns).has_value());
}

// A run that fills a hole behind the newest rendered timestamp changes what the
// seek bar must show while leaving Head100ns() exactly where it was, so a
// repaint driven off the head never happens and the rendered region stays
// invisible until something else moves.
void neural_segment_index_revision_moves_when_a_run_fills_a_hole_behind_the_head_test()
{
    NeuralSegmentIndex index;
    index.Append(LiveSegmentRecord(L"run1/neural-00000.mkv",0,20,5,1));
    index.Append(LiveSegmentRecord(L"run1/neural-00001.mkv",1,25,5,1));
    const int64_t head=index.Head100ns();
    const uint64_t rendered=index.Revision();

    index.Append(LiveSegmentRecord(L"run2/neural-00000.mkv",0,0,5,2));
    CHECK_EQ(head,index.Head100ns());
    CHECK(index.Revision()!=rendered);
    CHECK_EQ(size_t{2},index.CoveredRanges().size());

    // A refused append changed no coverage, so it must not ask for a repaint.
    const uint64_t behind=index.Revision();
    index.Append(LiveSegmentRecord(L"overlap.mkv",1,22,3,2));
    CHECK_EQ(behind,index.Revision());

    // Closing the hole joins the two regions: still no new head, still a change.
    index.Append(LiveSegmentRecord(L"run2/neural-00001.mkv",1,5,15,2));
    CHECK(index.Revision()!=behind);
    CHECK_EQ(head,index.Head100ns());
    CHECK_EQ(size_t{1},index.CoveredRanges().size());
    CHECK(index.Covered(12*kLiveFrame100ns));
}

// The arithmetic that made 60 fps unrenderable. A segment's exclusive end is
// synthesized from ONE rounded frame duration - llround(1e7/fps) - while its
// first timestamp is the real pts of its first frame, so at any rate whose
// duration rounds UP the synthesized end lands past the next file's own start:
// 60 fps rounds 166666.67 to 166667, and 120 frames of it end 40 ticks beyond
// the file that follows. Read as a republish, that dropped every other segment
// of a 60 fps render - 14 of 28 measured on a 2560x1440 clip - and the publish
// gate then refused the joined result for carrying 1590 of the 3267 frames the
// render had just proven, so the session ended with nothing. 30 fps never saw
// it: 333333.33 rounds down, into the sub-frame hole the index already closed.
void neural_segment_index_keeps_a_segment_whose_predecessor_overshot_by_a_tick_test()
{
    // The producer's numbers, exactly: 60 fps, first pts from the decoder,
    // exclusive end from the rounded duration.
    constexpr int64_t kRounded=166667;          // llround(1e7/60)
    const auto sixtyFps=[](uint64_t firstFrame,uint64_t frames,uint64_t index){
        NeuralSegment segment;
        segment.path=std::filesystem::path(L"neural-0000"+std::to_wstring(index)+L".mkv");
        segment.runId=1;segment.index=index;segment.firstFrameNumber=firstFrame;
        segment.firstTimestamp100ns=int64_t(std::llround(double(firstFrame)*1e7/60.0));
        segment.end100ns=segment.firstTimestamp100ns+int64_t(frames)*kRounded;
        segment.frameCount=frames;
        return segment;
    };
    NeuralSegmentIndex index;
    index.Append(sixtyFps(2999,30,0));
    index.Append(sixtyFps(3029,120,1));
    index.Append(sixtyFps(3149,120,2));
    // Every file the render published is in the index, and the frames it claims
    // are the frames it can join.
    CHECK_EQ(size_t{3},index.Count());
    CHECK_EQ(uint64_t{270},index.TotalFrames());
    // One region, not three: the seam is closed at the arriving file's own pts,
    // so nothing between them reads as a hole either.
    CHECK_EQ(size_t{1},index.CoveredRanges().size());
    if(const auto first=index.At(0);first&&index.At(1))
        CHECK_EQ(index.At(1)->firstTimestamp100ns,first->end100ns);
    CHECK(index.Covered(index.At(1)->firstTimestamp100ns-1));
    CHECK(index.Containing(index.At(1)->firstTimestamp100ns)->index==uint64_t{1});

    // A whole frame of overlap is still a republish over playable ground, which
    // is the case the refusal exists for: a retargeted run must not move the
    // timeline under a decoder that is already reading it.
    NeuralSegment republished=sixtyFps(3269,120,3);
    republished.firstTimestamp100ns=index.At(2)->end100ns-kRounded;
    republished.end100ns=republished.firstTimestamp100ns+120*kRounded;
    const uint64_t revision=index.Revision();
    index.Append(republished);
    CHECK_EQ(size_t{3},index.Count());
    CHECK_EQ(revision,index.Revision());

    // And a file republished at the position one already holds is refused for
    // that same reason, whatever it contains: something may be reading it.
    if(const auto existing=index.At(1)){
        NeuralSegment same=*existing;
        same.path=L"republished.mkv";same.index=4;
        index.Append(same);
        CHECK_EQ(size_t{3},index.Count());
        CHECK_EQ(revision,index.Revision());
    }
}

// The read that used to report "out of sync" and take the session down with it:
// the playhead runs off the end of one rendered region into a hole the run has
// not filled yet. A hole is work still to do, so the read waits, and it resumes
// on real pairs the moment the covering file is published.
void live_playback_waits_inside_a_hole_and_resumes_when_it_is_filled_test()
{
    LiveFrameLibrary library;library.Add(L"original.mkv",40);
    library.Add(L"early.mkv",5);library.Add(L"filler.mkv",5);library.Add(L"late.mkv",5);
    LiveLibrarySource original(library);
    SynchronizedPlayback playback(original,LiveSegmentFactory(library));
    const auto segments=std::make_shared<NeuralSegmentIndex>();
    segments->Append(LiveSegmentRecord(L"early.mkv",0,0,5,1));
    segments->Append(LiveSegmentRecord(L"late.mkv",0,10,5,2));
    CHECK(playback.OpenLive(L"original.mkv",segments,SynchronizedRange{},{}));
    for(uint64_t expected=0;expected<5;++expected){
        CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        const auto* pair=playback.CurrentPair();CHECK(pair!=nullptr);
        if(!pair)return;
        CHECK_EQ(expected,pair->frameNumber);
    }
    // Frame 5 sits in the hole between the two regions, with ten rendered
    // frames waiting beyond it. Waiting is the answer: nothing is out of sync,
    // nothing has ended, and the session stays usable.
    CHECK(!segments->Covered(5*kLiveFrame100ns));
    CHECK_EQ(SynchronizedReadResult::WaitingForRender,playback.ReadNextAvailable({}));
    CHECK_EQ(SynchronizedReadResult::WaitingForRender,playback.ReadNextAvailable({}));
    CHECK(playback.LastFault().empty());
    CHECK(playback.Live());CHECK(playback.NeuralAvailable());
    CHECK_EQ(size_t{2},segments->CoveredRanges().size());

    // The run retargeted at the hole and published it. No frame was skipped
    // while waiting, and playback crosses into the region that was already
    // rendered beyond it.
    segments->Append(LiveSegmentRecord(L"filler.mkv",0,5,5,3));
    CHECK_EQ(size_t{1},segments->CoveredRanges().size());
    for(uint64_t expected=5;expected<15;++expected){
        CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        const auto* pair=playback.CurrentPair();CHECK(pair!=nullptr);
        if(!pair)return;
        CHECK_EQ(expected,pair->frameNumber);
        CHECK_EQ(pair->original.frameNumber,pair->neural.frameNumber);
        CHECK_EQ(pair->original.timestamp100ns,pair->neural.timestamp100ns);
        CHECK(!pair->neural.bgra.empty());
    }
    CHECK(playback.LastFault().empty());
}

// The bug this rebuild exists for. A session that had rendered part of the clip
// was seeked backwards, the earlier rendered region was refused as "not
// rendered", and the player then discarded the render. Seeking back into a
// region that was rendered must succeed and must serve that region's own
// frames: every segment file numbers its frames from its own zero, so handing
// back the wrong region's frame 0 looks right on the numbers and wrong on
// screen.
void live_seek_backward_into_an_earlier_region_serves_that_regions_frames_test()
{
    constexpr uint8_t kEarlyTag=0x11;
    constexpr uint8_t kLateTag=0x22;
    LiveFrameLibrary library;library.Add(L"original.mkv",40);
    TagLiveStream(library.Add(L"early.mkv",5),kEarlyTag);
    TagLiveStream(library.Add(L"late.mkv",5),kLateTag);
    LiveLibrarySource original(library);
    SynchronizedPlayback playback(original,LiveSegmentFactory(library));
    const auto segments=std::make_shared<NeuralSegmentIndex>();
    segments->Append(LiveSegmentRecord(L"early.mkv",0,0,5,1));
    segments->Append(LiveSegmentRecord(L"late.mkv",0,20,5,2));
    CHECK(playback.OpenLive(L"original.mkv",segments,SynchronizedRange{},{}));

    // Watching the later region, which is where the run is working.
    CHECK(playback.SeekSeconds(double(21*kLiveFrame100ns)*1e-7,{}));
    if(const auto* late=playback.CurrentPair()){
        CHECK_EQ(uint64_t{21},late->frameNumber);
        CHECK_EQ(uint64_t{21},late->neural.frameNumber);
        CHECK_EQ(int(kLateTag),LiveStreamTag(late->neural));
    }

    // Back into the earlier region: it is rendered, so the seek lands.
    CHECK(playback.SeekSeconds(double(2*kLiveFrame100ns)*1e-7,{}));
    CHECK(playback.LastFault().empty());
    const auto* back=playback.CurrentPair();CHECK(back!=nullptr);
    if(!back)return;
    CHECK_EQ(uint64_t{2},back->frameNumber);
    CHECK_EQ(uint64_t{2},back->neural.frameNumber);
    CHECK_EQ(int64_t{2*kLiveFrame100ns},back->neural.timestamp100ns);
    CHECK_EQ(int(kEarlyTag),LiveStreamTag(back->neural));

    // And playback continues inside that region rather than in the other one.
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    if(const auto* next=playback.CurrentPair()){
        CHECK_EQ(uint64_t{3},next->frameNumber);
        CHECK_EQ(int(kEarlyTag),LiveStreamTag(next->neural));
    }
    // Neither seek cost any rendered work.
    CHECK_EQ(size_t{2},segments->Count());
    CHECK_EQ(size_t{2},segments->CoveredRanges().size());
}

// A seek into a hole is the one target that cannot be served. It is refused
// with a reason, and refusing it is all that happens: the session stays open
// and every rendered region stays rendered, because deleting the render was
// what made the original defect unrecoverable.
void live_seek_into_a_hole_is_refused_without_discarding_coverage_test()
{
    LiveFrameLibrary library;library.Add(L"original.mkv",40);
    library.Add(L"early.mkv",5);library.Add(L"late.mkv",5);
    LiveLibrarySource original(library);
    SynchronizedPlayback playback(original,LiveSegmentFactory(library));
    const auto segments=std::make_shared<NeuralSegmentIndex>();
    segments->Append(LiveSegmentRecord(L"early.mkv",0,0,5,1));
    segments->Append(LiveSegmentRecord(L"late.mkv",0,20,5,2));
    CHECK(playback.OpenLive(L"original.mkv",segments,SynchronizedRange{},{}));
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));

    CHECK(!playback.SeekSeconds(double(12*kLiveFrame100ns)*1e-7,{}));
    CHECK(playback.LastFault().find("live-seek-uncovered")!=std::string::npos);
    CHECK(playback.Live());CHECK(playback.NeuralAvailable());
    // The playhead did not move, and nothing was unloaded or truncated.
    if(const auto* held=playback.CurrentPair())CHECK_EQ(uint64_t{0},held->frameNumber);
    CHECK_EQ(size_t{2},segments->Count());
    CHECK_EQ(size_t{2},segments->CoveredRanges().size());
    CHECK(segments->Covered(1*kLiveFrame100ns));
    CHECK(segments->Covered(21*kLiveFrame100ns));
    // Both regions are still reachable after the refusal.
    CHECK(playback.SeekSeconds(double(21*kLiveFrame100ns)*1e-7,{}));
    if(const auto* late=playback.CurrentPair())CHECK_EQ(uint64_t{21},late->frameNumber);
    CHECK(playback.SeekSeconds(double(1*kLiveFrame100ns)*1e-7,{}));
    if(const auto* early=playback.CurrentPair())CHECK_EQ(uint64_t{1},early->frameNumber);
}

// The session's UI-thread decisions: when playback may start, when a rebuffer
// ends, and when chasing the render head is worse than restarting it.
void live_session_attaches_on_lead_resumes_earlier_and_finishes_on_any_coverage_test()
{
    live_session::SessionView view{};
    view.rangeStartSec=10.0;view.positionSec=10.0;
    CHECK(!live_session::ShouldAttach(view));                     // nothing rendered yet
    view.headSec=13.0;
    CHECK(!live_session::ShouldAttach(view));                     // 3 s is short of the 4 s lead
    view.headSec=14.0;
    CHECK(live_session::ShouldAttach(view));
    // A finished job never grows again, so waiting for a full lead would hang.
    live_session::SessionView tail{};
    tail.rangeStartSec=10.0;tail.positionSec=10.0;tail.headSec=10.5;tail.finished=true;
    CHECK(live_session::ShouldAttach(tail));
    CHECK(!live_session::ShouldAttach({.positionSec=10.0,.rangeStartSec=10.0,.headSec=10.0,.attached=false,.finished=true}));
    // A seek in flight refuses, however much lead there is: the lead was
    // measured against the position playback is leaving, and attaching there
    // made the player attach, refuse the pending seek for landing in a hole,
    // hand playback back and attach again, twenty-eight times in seventeen
    // seconds of a driven session.
    live_session::SessionView seekAway{};
    seekAway.rangeStartSec=49.0;seekAway.positionSec=49.0;seekAway.headSec=113.0;
    CHECK(live_session::ShouldAttach(seekAway));
    seekAway.seeking=true;
    CHECK(!live_session::ShouldAttach(seekAway));
    // Resuming after a rebuffer needs less than starting did.
    live_session::SessionView playing{};
    playing.attached=true;playing.rangeStartSec=10.0;playing.positionSec=20.0;playing.headSec=21.5;
    CHECK(!live_session::ShouldAttach(playing));                  // already attached
    CHECK(!live_session::ShouldResume(playing));
    playing.headSec=22.0;
    CHECK(live_session::ShouldResume(playing));
    playing.headSec=20.1;playing.finished=true;
    CHECK(std::abs(live_session::Lead(playing)-0.1)<1e-9);
    // The cushion is sized for a card that barely keeps up. On one that renders
    // several times faster it only makes the user wait for a buffer the render
    // refills faster than playback drains it.
    CHECK(std::abs(live_session::StartLead(0.0)-live_session::kStartLead)<1e-9);   // unmeasured
    CHECK(std::abs(live_session::StartLead(1.0)-live_session::kStartLead)<1e-9);
    CHECK(std::abs(live_session::StartLead(1.49)-live_session::kStartLead)<1e-9);
    CHECK(std::abs(live_session::StartLead(1.5)-2.0)<1e-9);
    CHECK(std::abs(live_session::StartLead(4.8)-1.0)<1e-9);                        // the 5090 at 1080p30
    // Never above the caller's own ceiling.
    CHECK(std::abs(live_session::StartLead(4.8,0.5)-0.5)<1e-9);
    live_session::SessionView fast{};
    fast.rangeStartSec=10.0;fast.positionSec=10.0;fast.headSec=11.2;
    CHECK(!live_session::ShouldAttach(fast));
    CHECK(live_session::ShouldAttach(fast,live_session::StartLead(4.8)));
    // A lead with no picture is the coverage being in the wrong place. Retrying
    // an attach that cannot succeed left one session behind the buffering panel
    // with its head at the end of the file, so a run of failures rebases.
    live_session::SessionView stalled{};
    stalled.rangeStartSec=10.0;stalled.positionSec=10.0;stalled.headSec=56.0;
    CHECK(!live_session::ShouldRebaseStalledAttach(stalled,0));
    CHECK(!live_session::ShouldRebaseStalledAttach(stalled,live_session::kAttachFailureLimit-1));
    CHECK(live_session::ShouldRebaseStalledAttach(stalled,live_session::kAttachFailureLimit));
    // Nothing rendered yet is a slow render, not a misplaced session.
    live_session::SessionView empty=stalled;empty.headSec=0.0;
    CHECK(!live_session::ShouldRebaseStalledAttach(empty,live_session::kAttachFailureLimit));
    // A session that is playing, or a seek in flight, has nothing to recover.
    live_session::SessionView running=stalled;running.attached=true;
    CHECK(!live_session::ShouldRebaseStalledAttach(running,live_session::kAttachFailureLimit));
    live_session::SessionView midSeek=stalled;midSeek.seeking=true;
    CHECK(!live_session::ShouldRebaseStalledAttach(midSeek,live_session::kAttachFailureLimit));
}

void live_session_retargets_the_render_to_the_hole_the_playhead_needs_test()
{
    constexpr int64_t kFrame=333333;                             // 30 fps
    // The job is filling [10 s,40 s) and has rendered as far as 20 s. That reach
    // is its own, not the region around the playhead: a job cannot have rendered
    // past the hole it was given without having finished it.
    const CoverageSpan target{100000000,400000000};
    const double jobHead=20.0;
    live_session::SessionView view{};
    view.rangeStartSec=10.0;view.headSec=20.0;
    // Inside that hole the render is coming this way: waiting beats restarting
    // until it falls more than the 15 s budget behind the playhead.
    view.positionSec=25.0;
    CHECK(!live_session::ShouldRetarget(view,target,CoverageSpan{250000000,400000000},kFrame,jobHead));
    view.positionSec=34.9;
    CHECK(!live_session::ShouldRetarget(view,target,CoverageSpan{349000000,400000000},kFrame,jobHead));
    view.positionSec=35.1;
    CHECK(live_session::ShouldRetarget(view,target,CoverageSpan{351000000,400000000},kFrame,jobHead));
    // Behind the hole being filled - the reported bug's seek - the job will never
    // reach the playhead, so it moves. Frame snapping a few milliseconds back is
    // not a seek and must not move it.
    view.positionSec=5.0;
    // The hole is [5,10) - five seconds, less than a job startup - so a PLAYING
    // viewer crosses it before any frame of it could exist and the running job
    // keeps its place; a paused one is looking at that frame and gets it.
    CHECK(!live_session::ShouldRetarget(view,target,CoverageSpan{50000000,100000000},kFrame,jobHead));
    live_session::SessionView held=view;held.paused=true;
    CHECK(live_session::ShouldRetarget(held,target,CoverageSpan{50000000,100000000},kFrame,jobHead));
    // Wide enough to pay for its own startup: the render moves either way.
    CHECK(live_session::ShouldRetarget(view,target,CoverageSpan{0,100000000},kFrame,jobHead));
    view.positionSec=9.9;
    CHECK(!live_session::ShouldRetarget(view,target,CoverageSpan{99000000,100000000},kFrame,jobHead));
    // Past the hole entirely: this job is behind the viewer. Attached playback is
    // no exception - a viewer who seeks back onto rendered frames still wants the
    // render working where they are, which the old rebase refused to do.
    view.positionSec=45.0;view.headSec=40.0;
    CHECK(live_session::ShouldRetarget(view,target,CoverageSpan{450000000,600000000},kFrame,jobHead));
    live_session::SessionView attached=view;attached.attached=true;
    CHECK(live_session::ShouldRetarget(attached,target,CoverageSpan{450000000,600000000},kFrame,jobHead));
    // A seek in flight has not committed to a position yet, and the hole the job
    // already has is never worth restarting for.
    live_session::SessionView seeking=view;seeking.seeking=true;
    CHECK(!live_session::ShouldRetarget(seeking,target,CoverageSpan{450000000,600000000},kFrame,jobHead));
    CHECK(!live_session::ShouldRetarget(view,target,target,kFrame,jobHead));
    // Nothing left to render: there is no hole to move to.
    CHECK(!live_session::ShouldRetarget(view,target,CoverageSpan{},kFrame,jobHead));
}

// Measured on a driven session, and the reason this is a test: the job's range
// is snapped to a frame boundary when it starts, while the hole it fills begins
// wherever the previous segment's sub-frame residual left off - 4999995 against
// a range starting at 5000000. Comparing the starts made those different work,
// and the session cancelled and relaunched its helper on every tick, four times
// in 110 ms, leaving a part-rendered half-second region behind.
void live_session_does_not_retarget_onto_the_hole_it_is_already_filling_test()
{
    constexpr int64_t kFrame=333333;
    const CoverageSpan target{5000000,183000000};                 // [0.5,18.3) s, frame-snapped
    const CoverageSpan wanted{4999995,183000000};                 // the hole, five ticks earlier
    // The viewer is at the end of the clip, inside a region rendered earlier, so
    // the only work left is the hole this job already has. The job itself has
    // rendered to 6 s of it - short of its end, so it is still running.
    const double jobHead=6.0;
    const live_session::SessionView view{.positionSec=22.5667,.rangeStartSec=0.5,.headSec=22.6};
    CHECK(!live_session::ShouldRetarget(view,target,wanted,kFrame,jobHead));
    // Same as the job publishes into it: the hole shrinks from the front, which
    // is still the same work and still no reason to restart the helper.
    CHECK(!live_session::ShouldRetarget(view,target,CoverageSpan{60000000,183000000},kFrame,jobHead));
    // A hole outside the target is different work, and this one is wide enough
    // to be worth a job of its own.
    CHECK(live_session::ShouldRetarget(view,target,CoverageSpan{183000000,266000000},kFrame,jobHead));
    // And the slack is one frame, not unlimited: a hole starting a second early
    // is a different hole.
    CHECK(live_session::ShouldRetarget(view,target,CoverageSpan{4000000,183000000},kFrame,jobHead));
}

// The budget is measured against what the JOB has rendered, not against the
// region around the playhead. Those differ exactly when the playhead sits in a
// hole, where the region around it is empty: a viewer who seeks to one second
// ahead of the render head then looked like a viewer the job would never reach,
// and a seven-second helper restart replaced a one-second wait.
void live_session_waits_for_a_job_whose_head_is_close_behind_the_playhead_test()
{
    constexpr int64_t kFrame=333333;
    const CoverageSpan target{5000000,1000000000};                // [0.5,100) s
    // Playhead at 41 s, inside the target and inside its hole, so the region
    // around the playhead is empty and `headSec` is 0. The job has rendered to
    // 40 s: one more second and it is there.
    const live_session::SessionView view{.positionSec=41.0,.rangeStartSec=0.5,.headSec=0.0};
    CHECK(!live_session::ShouldRetarget(view,target,CoverageSpan{410000000,1000000000},kFrame,40.0));
    // Without the job's own reach the same call restarts the helper, which is
    // the defect this pins: 41 > max(0.5, 0.0) + 15.
    CHECK(live_session::ShouldRetarget(view,target,CoverageSpan{410000000,1000000000},kFrame,0.0));
    // A job that really has fallen behind still moves: rendered to 20 s with the
    // viewer at 41 is past the fifteen-second budget.
    CHECK(live_session::ShouldRetarget(view,target,CoverageSpan{410000000,1000000000},kFrame,20.0));
    // A hole narrower than a job startup does not justify cancelling a job that
    // is rendering: the startup is paid twice, once for the sliver and once to
    // come back, while the sliver is a second of video the original covers in a
    // second. One driven session traded a job on [38.6,104.4) for a one-second
    // hole at the playhead, and then gave up.
    const live_session::SessionView elsewhere{.positionSec=37.6,.rangeStartSec=38.6,.headSec=0.0};
    const CoverageSpan big{386000000,1044000000};
    CHECK(!live_session::ShouldRetarget(elsewhere,big,CoverageSpan{376000000,386000000},kFrame,45.0));
    // Wide enough to pay for itself, and the job is behind the viewer: it moves.
    CHECK(live_session::ShouldRetarget(elsewhere,big,CoverageSpan{286000000,386000000},kFrame,45.0));
    // A job that has rendered its whole hole is left to publish: the entry and
    // receipt it is about to promote are for work already done, and the next hole
    // starts from the completion path a tick later. One driven session cancelled
    // such a job five seconds after it finished [100.1,113) s, because its
    // completion had not been processed and it still counted as running.
    const live_session::SessionView atEnd{.positionSec=39.6,.rangeStartSec=100.1,.headSec=0.0};
    const CoverageSpan tail{1001333330,1130000000};
    CHECK(!live_session::ShouldRetarget(atEnd,tail,CoverageSpan{0,296333330},kFrame,113.0));
    // Still short of its end, and the viewer is elsewhere: that job does move.
    CHECK(live_session::ShouldRetarget(atEnd,tail,CoverageSpan{0,296333330},kFrame,110.0));
}

// A session toggled on at 12.0329 s published its first segment from 12.0662 s,
// a single 30 fps frame later. Joining at the playhead found no segment holding
// it, so the player waited behind a filling buffer, the stalled-attach recovery
// restarted the same session at the same instant, and the picture sat on one
// frame until the clip ran out.
void live_session_joins_the_render_where_its_coverage_actually_starts_test()
{
    constexpr int64_t kSecond=10'000'000;
    // Coverage that starts a frame late: join there, not at the playhead.
    CHECK_EQ(int64_t(120'662'000),
             live_session::AttachPosition100ns(120'329'000,120'329'000,120'662'000));
    // Coverage that already covers the playhead: the playhead stands.
    CHECK_EQ(int64_t(12*kSecond),
             live_session::AttachPosition100ns(12*kSecond,10*kSecond,10*kSecond));
    // A playhead before the range - a seek that landed short - starts at the
    // range, and coverage still wins when it begins later than that.
    CHECK_EQ(int64_t(10*kSecond),
             live_session::AttachPosition100ns(4*kSecond,10*kSecond,10*kSecond));
    CHECK_EQ(int64_t(11*kSecond),
             live_session::AttachPosition100ns(4*kSecond,10*kSecond,11*kSecond));
    // No segments yet: nothing to clamp to, so the playhead is unchanged and
    // the caller's own coverage check refuses the attach.
    CHECK_EQ(int64_t(12*kSecond),live_session::AttachPosition100ns(12*kSecond,10*kSecond,0));
}

// The third form of the same hang. A session toggled on at a playhead whose
// render key was already published is answered by the cache in about 50 ms: the
// job succeeds, renders no frame and appends no segment, so the index is empty
// AND finished. Every other decision here then says "wait" - zero lead against
// a finished session, a playhead inside the range - and the player sat behind
// the buffering panel until the user gave up. Reproduced twice on hardware,
// where the log showed publish (save=0) 54 ms after the cache check, no
// segment lines at all, and `Neural cold start: total=-`.
void live_session_with_no_published_segment_plays_the_cache_entry_test()
{
    using live_session::CompletedSessionPlan;
        // The defect: a successful job, an empty index, and an entry that covers
    // the whole range. Playback belongs on the entry, not on the index.
    CHECK(CompletedSessionPlan::PublishedEntry==
          live_session::PlanForCompletedSession({.covered=false,.ok=true,.publishedEntry=true}));
    // No entry either: there is nothing to show, so the session must end and
    // hand the original stream back rather than wait.
    CHECK(CompletedSessionPlan::Stop==
          live_session::PlanForCompletedSession({.covered=false,.ok=true,.publishedEntry=false}));
    CHECK(CompletedSessionPlan::Stop==
          live_session::PlanForCompletedSession({.covered=false,.ok=false,.publishedEntry=true}));
    // Coverage outranks the verdict: a job that failed partway still left
    // seconds of picture on screen, and those keep playing.
    CHECK(CompletedSessionPlan::Segments==
          live_session::PlanForCompletedSession({.covered=true,.ok=false,.publishedEntry=false}));
    CHECK(CompletedSessionPlan::Segments==
          live_session::PlanForCompletedSession({.covered=true,.ok=true,.publishedEntry=true}));
    // What made the hang invisible to the rest of the policy: the session the
    // cache hit leaves behind asks for neither an attach nor a retarget - there
    // is no hole to move the render to, because the entry covered the range.
    const live_session::SessionView empty{.positionSec=2.56667,.rangeStartSec=2.56667,
                                          .headSec=0.0,.attached=false,.finished=true};
    CHECK(!live_session::ShouldAttach(empty));
    CHECK(!live_session::ShouldRetarget(empty,CoverageSpan{25666700,1040000000},CoverageSpan{},333333,0.0));
    CHECK(!live_session::ShouldRebaseStalledAttach(empty,live_session::kAttachFailureLimit));
}

// A session fills its range one hole at a time and every job publishes a cache
// entry of its own hole, while "Save converted video" writes the entry out
// under the session's whole range. A session that rendered [30,60) and then
// [0,30) exported its 30 s tail labelled as the film.
void live_session_export_entry_needs_one_job_over_the_whole_range_test()
{
    using live_session::ExportableEntry;
    const int64_t frame = 333333;                       // 30 fps
    const CoverageSpan range{0, 600000000};             // a 60 s session
    // The defect: two holes filled by two jobs, the last one's entry offered
    // as the film. Neither hole's entry is the range, whichever finished last.
    const std::vector<CoverageSpan> twoJobs{{0, 300000000}, {300000000, 600000000}};
    CHECK(!ExportableEntry(twoJobs, range, {300000000, 600000000}, frame));
    CHECK(!ExportableEntry(twoJobs, range, {0, 300000000}, frame));
    // One job that rendered every frame of the range publishes the film.
    const std::vector<CoverageSpan> oneJob{{0, 600000000}};
    CHECK(ExportableEntry(oneJob, range, {0, 600000000}, frame));
    // An integer-frame head lands a few ticks short of a fractional rate's
    // declared end; that residual is coverage, the same slack the hole
    // arithmetic applies.
    const std::vector<CoverageSpan> shortHead{{0, 600000000 - 5}};
    CHECK(ExportableEntry(shortHead, range, {0, 600000000 - 5}, frame));
    // A frame or more missing is a hole, and an entry that stops a frame short
    // of the range is not the range.
    const std::vector<CoverageSpan> frameShort{{0, 600000000 - frame - 1}};
    CHECK(!ExportableEntry(frameShort, range, {0, 600000000 - frame - 1}, frame));
    // Coverage adopted from an earlier session can be wider than the range;
    // the entry still has to be the whole of THIS range.
    const std::vector<CoverageSpan> wider{{0, 900000000}};
    CHECK(ExportableEntry(wider, {300000000, 600000000}, {300000000, 600000000}, frame));
    CHECK(!ExportableEntry(wider, {300000000, 600000000}, {300000000, 450000000}, frame));
    // A job that published no segment - a cache hit - has an entry but no
    // coverage: the completed-session plan owns that case, not the export.
    CHECK(!ExportableEntry({}, range, {0, 600000000}, frame));
    CHECK(!ExportableEntry(oneJob, {}, {0, 600000000}, frame));
    CHECK(!ExportableEntry(oneJob, range, {}, frame));
}

void live_session_pace_reports_nothing_until_startup_stops_dominating_test()
{
    CHECK_EQ(0.0,live_session::RealtimeRatio(3.0,4.0));           // 4 s in, still mostly startup
    CHECK_EQ(0.0,live_session::RealtimeRatio(0.0,30.0));          // no coverage yet
    CHECK_EQ(0.5,live_session::RealtimeRatio(10.0,20.0));
    CHECK_EQ(1.0,live_session::RealtimeRatio(20.0,20.0));
}

// Cost per frame is a fixed part plus a part proportional to pixel count, so
// whether a session can follow playback is decided before a frame is rendered.
// Anchored on measured rates: 12.50 ms per 1080p frame, 16.60 at 1440p,
// 28.07 at 2160p.
void live_render_forecast_matches_the_measured_rate_and_flags_sources_that_cannot_keep_up_test()
{
    const auto hd=playback_timing::ForecastLiveRender(1920,1080,30.0);
    CHECK(hd.msPerFrame>12.2&&hd.msPerFrame<12.9);
    CHECK(hd.renderFps>77.0&&hd.renderFps<82.0);
    CHECK(hd.keepsUp);
    const auto qhd=playback_timing::ForecastLiveRender(2560,1440,30.0);
    CHECK(qhd.msPerFrame>16.2&&qhd.msPerFrame<17.0);
    CHECK(qhd.keepsUp);
    const auto uhd=playback_timing::ForecastLiveRender(3840,2160,30.0);
    CHECK(uhd.msPerFrame>27.6&&uhd.msPerFrame<28.6);
    CHECK(uhd.keepsUp);                                            // 35 rendered against 30 wanted
    CHECK(uhd.realtimeRatio>1.15&&uhd.realtimeRatio<1.25);
    // 4K60 is where it runs out: half the budget for twice the frames.
    const auto uhd60=playback_timing::ForecastLiveRender(3840,2160,60.0);
    CHECK(!uhd60.keepsUp);
    CHECK(uhd60.realtimeRatio>0.55&&uhd60.realtimeRatio<0.62);
    // 8K30 is far out of reach, and 1440p60 lands on the line.
    CHECK(!playback_timing::ForecastLiveRender(7680,4320,30.0).keepsUp);
    CHECK(playback_timing::ForecastLiveRender(1280,720,60.0).keepsUp);
    // Unknown geometry or frame rate must never block the user on a guess.
    CHECK(playback_timing::ForecastLiveRender(0,0,30.0).keepsUp);
    CHECK(playback_timing::ForecastLiveRender(1920,1080,0.0).keepsUp);
}

// The reference numbers belong to one GPU, and one scalar does not carry them
// to another: an RTX 5090 measured 11.888 ms/frame at 1080p, 17.149 at 1440p
// and 42.870 at 4K (0.95x, 1.04x and 1.53x the reference), and a 1080p-only
// scalar let 4K30 start and drop 848 of 869 frames. The profile predicts from
// what was measured, extrapolates conservatively from one point, and an
// unknown pace stays silent rather than promising anything.
void live_render_forecast_predicts_from_this_gpu_measured_geometries_test()
{
    using namespace playback_timing;
    CHECK(RenderPaceScale(15.31,1920,1080)>1.21&&RenderPaceScale(15.31,1920,1080)<1.23);
    CHECK_EQ(0.0,RenderPaceScale(0.0,1920,1080));
    CHECK_EQ(0.0,RenderPaceScale(15.31,0,1080));

    // Nothing measured: the generation prior scales the reference. Ada's 1.22x
    // puts 4K30 just under the line; a GPU three times slower misses 1080p60.
    const auto adaPrior=ForecastLiveRender(3840,2160,30.0,{},1.22);
    CHECK(!adaPrior.keepsUp);
    CHECK(adaPrior.realtimeRatio>0.95&&adaPrior.realtimeRatio<0.99);
    CHECK(adaPrior.msPerFrame>34.0&&adaPrior.msPerFrame<34.6);
    CHECK(ForecastLiveRender(1920,1080,60.0,{},1.22).keepsUp);
    CHECK(!ForecastLiveRender(1920,1080,60.0,{},3.0).keepsUp);
    const auto unknown=ForecastLiveRender(3840,2160,60.0,{},0.0);
    CHECK(unknown.keepsUp);
    CHECK_EQ(0.0,unknown.msPerFrame);

    // One 1080p sample from the 5090. Scaling the reference shape would say
    // 26.6 ms at 4K (and it really took 42.9); the proportional bound says
    // 47.6, so 4K30 is warned about. 1440p30 still clears comfortably, and a
    // smaller frame keeps the reference's fixed cost rather than shrinking to
    // nothing.
    RenderPaceProfile one;
    one.Record({1920,1080,11.888});
    CHECK_EQ(11.888,PredictRenderMs(one,1920,1080,1.0));
    const auto uhdFromOne=ForecastLiveRender(3840,2160,30.0,one,1.0);
    CHECK(!uhdFromOne.keepsUp);
    CHECK(uhdFromOne.msPerFrame>47.0&&uhdFromOne.msPerFrame<48.0);
    CHECK(ForecastLiveRender(2560,1440,30.0,one,1.0).keepsUp);
    CHECK(PredictRenderMs(one,2560,1440,1.0)>21.0&&PredictRenderMs(one,2560,1440,1.0)<21.3);
    CHECK(PredictRenderMs(one,1280,720,1.0)>9.0&&PredictRenderMs(one,1280,720,1.0)<9.3);
    // The prior is irrelevant once anything was measured.
    CHECK_EQ(PredictRenderMs(one,3840,2160,1.0),PredictRenderMs(one,3840,2160,0.0));

    // All three geometries: exact matches are used as is, other geometries
    // come from this GPU's own fitted line (0.015 ms + 5.113 ms/MP).
    RenderPaceProfile three=one;
    three.Record({2560,1440,17.149});
    three.Record({3840,2160,42.870});
    CHECK_EQ(size_t{3},three.samples.size());
    const auto uhd=ForecastLiveRender(3840,2160,30.0,three,1.0);
    CHECK_EQ(42.870,uhd.msPerFrame);
    CHECK(!uhd.keepsUp);
    CHECK(uhd.realtimeRatio>0.77&&uhd.realtimeRatio<0.79);
    const double fitted=PredictRenderMs(three,3200,1800,1.0);
    CHECK(fitted>29.2&&fitted<29.7);
    CHECK(ForecastLiveRender(3200,1800,30.0,three,1.0).keepsUp);
    CHECK(!ForecastLiveRender(3200,1800,60.0,three,1.0).keepsUp);

    // Re-measuring a geometry replaces its sample; the profile is bounded.
    three.Record({1920,1080,12.0});
    CHECK_EQ(size_t{3},three.samples.size());
    CHECK_EQ(12.0,PredictRenderMs(three,1920,1080,1.0));
    three.Record({0,0,5.0});three.Record({640,360,0.0});
    CHECK_EQ(size_t{3},three.samples.size());
    for(uint32_t h=400;h<=1000;h+=100)three.Record({h*16/9,h,1.0+h*0.01});
    CHECK_EQ(RenderPaceProfile::kMaxSamples,three.samples.size());
}

// Three fixed digests and one fixed directory: the policy never touches the
// filesystem, so the key's parts only have to be distinguishable.
resident_helper::HelperKey StubHelperKey(std::string settingsDigest = "settings-aaa")
{
    return resident_helper::MakeHelperKey(L"C:\\Player\\neural-runtime", "runtime-111",
                                          std::move(settingsDigest));
}

resident_helper::ResidentState RunningHelper(const resident_helper::HelperKey& key)
{
    return resident_helper::ResidentState{true, key};
}

// The decision residency exists for. Reuse is the only answer that skips
// neuralInit and featureArm, which on this machine is 2.10 s of a 4.88-5.16 s
// warm toggle, so it must be reachable for the ordinary case: the same runtime,
// the same files, the same settings, a different clip or a different range.
// Spelling of the runtime directory is not part of the decision - a key folded
// from a differently cased or slash-separated path is the same helper, because
// paying 2.10 s over a backslash would be indefensible.
void resident_helper_reuses_the_running_process_for_an_identical_key_test()
{
    using namespace resident_helper;
    const HelperKey resident=StubHelperKey();
    CHECK(PlanForJob(RunningHelper(resident),StubHelperKey())==HelperPlan::Reuse);
    CHECK(PlanForJob(RunningHelper(resident),
                     MakeHelperKey(L"c:/Player/Neural-Runtime\\","runtime-111","settings-aaa"))==HelperPlan::Reuse);
    // The job's own particulars are not in the key at all: a resident helper is
    // reused for any job the same loaded stack can render.
    CHECK(HelperPlanName(HelperPlan::Reuse)=="reuse");
}

// A settings change cannot be delivered to a running helper at all: ReShade and
// RenoDX read their INI when the proxy loads, so a helper that started under the
// old settings would render the new job with the old ones and the cache entry
// would be keyed to settings it does not contain. This is the one relaunch that
// is a correctness requirement rather than a cleanliness one.
void resident_helper_relaunches_when_the_neural_settings_digest_changes_test()
{
    using namespace resident_helper;
    const HelperKey resident=StubHelperKey("settings-aaa");
    CHECK(PlanForJob(RunningHelper(resident),StubHelperKey("settings-bbb"))==HelperPlan::Relaunch);
    // And back again: the digest is compared, not remembered as "changed once".
    CHECK(PlanForJob(RunningHelper(StubHelperKey("settings-bbb")),
                     StubHelperKey("settings-aaa"))==HelperPlan::Relaunch);
}

// A runtime digest change means the hashed files under neural-runtime - the
// twelve vendor modules and the worker beside them - are not the ones the
// resident helper mapped. Its loaded proxy, add-on and NGX all
// came from the old bytes, so there is nothing to reuse even though the
// directory and the settings are unchanged.
void resident_helper_relaunches_when_the_runtime_digest_changes_test()
{
    using namespace resident_helper;
    const HelperKey resident=StubHelperKey();
    CHECK(PlanForJob(RunningHelper(resident),
                     MakeHelperKey(L"C:\\Player\\neural-runtime","runtime-222","settings-aaa"))==HelperPlan::Relaunch);
    // A different runtime directory entirely is the same answer for the same
    // reason, and must not be mistaken for the same helper.
    CHECK(PlanForJob(RunningHelper(resident),
                     MakeHelperKey(L"D:\\Other\\neural-runtime","runtime-111","settings-aaa"))==HelperPlan::Relaunch);
}

// Launch is not a degraded Reuse: it is what the first job of a session does,
// and what every job does after the helper's 30 s idle timeout or its exit
// after a job it could not finish. The player finds out by looking at the
// process, so "gone" arrives here as `running == false` and must produce an
// ordinary launch rather than an error - the absence of a helper is never a
// failure to report.
void resident_helper_launches_when_no_helper_is_running_test()
{
    using namespace resident_helper;
    CHECK(PlanForJob({},StubHelperKey())==HelperPlan::Launch);
    // The key of a helper that has gone is still remembered; it must not make
    // the job look reusable.
    CHECK(PlanForJob(ResidentState{false,StubHelperKey()},StubHelperKey())==HelperPlan::Launch);
    CHECK(PlanForJob(ResidentState{false,StubHelperKey("settings-bbb")},StubHelperKey())==HelperPlan::Launch);
}

// A job that cannot be identified may not be given a process that outlives it.
// Without all three parts of the key, two jobs whose settings differ compare
// equal, and the second would silently reuse the first one's loaded INI - so an
// incomplete key runs the way every job ran before residency: one process, one
// job, exit.
void resident_helper_refuses_residency_for_an_unidentified_job_test()
{
    using namespace resident_helper;
    const HelperKey resident=StubHelperKey();
    CHECK(PlanForJob(RunningHelper(resident),MakeHelperKey(L"","runtime-111","settings-aaa"))==HelperPlan::SingleShot);
    CHECK(PlanForJob(RunningHelper(resident),
                     MakeHelperKey(L"C:\\Player\\neural-runtime","","settings-aaa"))==HelperPlan::SingleShot);
    CHECK(PlanForJob(RunningHelper(resident),
                     MakeHelperKey(L"C:\\Player\\neural-runtime","runtime-111",""))==HelperPlan::SingleShot);
    // Including when nothing is running: single-shot outranks launch, because
    // the objection is to keeping this helper, not to starting one.
    CHECK(PlanForJob({},MakeHelperKey(L"C:\\Player\\neural-runtime","runtime-111",""))==HelperPlan::SingleShot);
    CHECK(StubHelperKey().Complete());
    CHECK(HelperPlanName(HelperPlan::SingleShot)=="single-shot");
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
            if(std::find(arguments.begin(),arguments.end(),L"-count_packets")!=arguments.end())return 92;
            std::cout << "width=2\nheight=2\nduration=0.0333333\n";return 0;
        }
        std::cout << "width=2\nheight=2\nnb_read_packets=1\nduration=0.0333333\n";
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
    if (std::ranges::any_of(arguments, [](std::wstring_view value) {
            return value.find(L"/progress-stream") != std::wstring_view::npos;
        })) {
        // FFmpeg's -progress stream: one key per line, each block closed by
        // progress=continue and the last by progress=end, with the values still
        // unknown in the first block and an error line sharing the same pipe.
        std::cout << "frame=1\nbitrate=N/A\ntotal_size=N/A\nout_time_us=N/A\nprogress=continue\n" << std::flush;
        std::cerr << "Non-monotonic DTS in output stream\n" << std::flush;
        std::cout << "frame=60\ntotal_size=1048576\nout_time_ms=2500000\nprogress=continue\n" << std::flush;
        std::cout << "frame=240\ntotal_size=4194304\nout_time_us=9000000\nprogress=end\n" << std::flush;
        WriteBytes(std::filesystem::path(arguments.back()), "materialized");
        return 0;
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
    if (argc == 3 && std::wstring_view(argv[1]) == L"--cache-root-probe")
        return RunCacheRootProbe(argv[2]);
    const std::wstring executableName = CurrentExecutable().filename().wstring();
    if (_wcsicmp(executableName.c_str(), L"ffmpeg.exe") == 0 ||
        _wcsicmp(executableName.c_str(), L"ffprobe.exe") == 0)
        return RunFakeMediaPipelineChild(argc, argv);
    default_cache_is_writable_beside_executable_independent_of_working_directory_test();
    default_cache_falls_back_when_portable_directory_is_blocked_test();
    default_cache_falls_back_when_portable_layout_is_unusable_test();
    explicit_cache_root_remains_authoritative_test();
    invalid_explicit_cache_root_does_not_silently_fall_back_test();
    cache_identity_field_list_is_pinned_test();
    published_render_is_not_reused_across_identity_changes_test();
    schema_four_entries_are_retired_by_the_schema_gate_test();
    model_store_digest_tracks_root_contents_and_names_its_fallback_test();
    hashed_runtime_set_covers_the_worker_and_the_lock_set_does_not_test();
    runtime_digest_is_order_independent_byte_sensitive_and_rejects_duplicates_test();
    manifest_round_trip_rejects_partial_duplicate_and_unknown_state_test();
    source_and_render_promotion_are_hash_validated_and_immutable_test();
    promotion_waits_out_a_transient_lock_and_names_the_failing_step_test();
    interrupted_staging_is_never_reusable_and_clear_stays_inside_root_test();
    staging_sweep_reaps_invalid_and_orphaned_entries_but_not_live_ones_test();
    media_pipeline_arguments_are_exact_and_never_use_a_shell_test();
    materialization_failure_reports_diagnostics_without_signed_urls_test();
    materialization_discards_oversized_diagnostic_url_fragments_test();
    media_progress_reader_buffers_split_keys_and_limits_its_report_rate_test();
    materialization_reports_download_progress_while_the_source_is_copied_test();
    encoder_frame_contract_and_fallback_policy_are_fail_closed_test();
    owned_media_pipeline_materializes_encodes_probes_and_cancels_test();
    encoder_child_inherits_only_its_stdin_pipe_test();
    cached_media_probe_reads_headers_without_redecoding_validated_video_test();
    probe_child_inherits_only_its_output_pipe_test();
    encoder_blocked_write_is_interrupted_by_stop_test();
    offline_job_primes_feature_then_restarts_source_and_captures_every_frame_test();
    offline_job_refuses_a_backend_that_evaluated_fewer_frames_than_it_captured_test();
    offline_sparse_receipt_gate_encodes_latest_capture_once_per_source_frame_test();
    offline_odd_dimensions_use_geometry_preserving_software_encoder_test();
    offline_sparse_receipt_gate_restarts_independently_for_software_retry_test();
    offline_receipt_gate_stops_before_encoding_on_failure_or_cancel_test();
    offline_photo_reuses_warmup_frame_but_encodes_exactly_one_frame_test();
    offline_photo_stops_after_bounded_warmup_without_encoding_test();
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
    offline_range_render_prerolls_without_capture_and_encodes_only_the_range_test();
    offline_render_refuses_a_dlaa_only_median_neural_gpu_time_test();
    offline_range_start_without_preroll_resets_on_the_first_captured_frame_test();
    offline_single_frame_preview_encodes_exactly_one_frame_test();
    offline_range_outside_the_source_fails_as_source_before_opening_test();
    offline_frame_retry_resubmits_the_same_frame_and_succeeds_without_reset_test();
    offline_frame_retry_exhaustion_fails_without_omitting_the_frame_test();
    offline_device_removal_is_not_retried_per_frame_test();
    offline_cut_detected_inside_the_job_counts_as_a_history_reset_test();
    offline_pause_holds_between_frames_without_a_temporal_reset_test();
    offline_pause_still_honours_cancellation_test();
    offline_identity_mismatch_from_the_evaluator_fails_the_job_test();
    offline_job_passes_guide_controls_to_the_evaluator_test();
    segmented_offline_job_publishes_finalized_files_and_drops_the_armed_one_test();
    offline_job_reports_the_cold_start_phases_it_reached_test();
    segmented_offline_job_makes_only_the_first_file_short_test();
    segmented_offline_job_software_retry_deletes_the_failed_attempts_files_test();
    segmented_offline_job_cancel_leaves_no_unpublished_file_or_live_encoder_test();
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
    neural_segment_index_orders_appends_and_locates_by_timestamp_test();
    neural_segment_index_resumes_after_retained_coverage_test();
    neural_segment_index_covers_the_rounding_hole_but_not_a_real_gap_test();
    live_playback_waits_at_the_render_head_and_resumes_on_a_new_segment_test();
    live_playback_crosses_a_segment_boundary_without_a_gap_or_stall_test();
    live_playback_crosses_a_seam_whose_end_rounds_below_the_next_start_test();
    live_seek_enters_a_rendered_segment_and_refuses_an_unrendered_target_test();
    neural_segment_index_keeps_two_disjoint_rendered_regions_test();
    neural_segment_index_after_finds_the_next_region_from_a_hole_test();
    neural_segment_index_revision_moves_when_a_run_fills_a_hole_behind_the_head_test();
    neural_segment_index_keeps_a_segment_whose_predecessor_overshot_by_a_tick_test();
    live_playback_waits_inside_a_hole_and_resumes_when_it_is_filled_test();
    live_seek_backward_into_an_earlier_region_serves_that_regions_frames_test();
    live_seek_into_a_hole_is_refused_without_discarding_coverage_test();
    live_session_attaches_on_lead_resumes_earlier_and_finishes_on_any_coverage_test();
    live_session_retargets_the_render_to_the_hole_the_playhead_needs_test();
    live_session_does_not_retarget_onto_the_hole_it_is_already_filling_test();
    live_session_waits_for_a_job_whose_head_is_close_behind_the_playhead_test();
    live_session_joins_the_render_where_its_coverage_actually_starts_test();
    live_session_with_no_published_segment_plays_the_cache_entry_test();
    live_session_export_entry_needs_one_job_over_the_whole_range_test();
    live_session_pace_reports_nothing_until_startup_stops_dominating_test();
    live_render_forecast_matches_the_measured_rate_and_flags_sources_that_cannot_keep_up_test();
    live_render_forecast_predicts_from_this_gpu_measured_geometries_test();
    neural_segment_index_pace_counts_frames_after_the_first_segment_of_a_run_test();
    resident_helper_reuses_the_running_process_for_an_identical_key_test();
    resident_helper_relaunches_when_the_neural_settings_digest_changes_test();
    resident_helper_relaunches_when_the_runtime_digest_changes_test();
    resident_helper_launches_when_no_helper_is_running_test();
    resident_helper_refuses_residency_for_an_unidentified_job_test();

    if (test_support::failure_count != 0) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
