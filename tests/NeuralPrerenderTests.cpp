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
#include "TestEnvironment.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <atomic>
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
    static constexpr std::wstring_view kPrefix = L"DLSSVideoPlayer-NeuralCacheTests-";

    TempDirectory()
    {
        path_ = test_support::FixtureTempRoot() /
            (std::wstring(kPrefix) + std::to_wstring(GetCurrentProcessId()) +
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

    // Never this process's own: several TempDirectory objects are alive at
    // once inside one test run, and deleting a live sibling here made the
    // suite flaky. Nor another run's that is still going - the same flake
    // between two concurrent runs (FixtureAbandoned).
    static void SweepAbandoned(const std::filesystem::path& parent)
    {
        std::error_code error;
        for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end;
             it.increment(error)) {
            if (!test_support::FixtureAbandoned(it->path(), kPrefix)) continue;
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

void RunCacheRootChildIn(const std::filesystem::path& directory, std::wstring_view mode)
{
    const auto executable = directory / L"cache-probe.exe";
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

void RunCacheRootChild(const TempDirectory& fixture, std::wstring_view mode)
{
    RunCacheRootChildIn(fixture.Path(), mode);
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

// A directory under `fixture` whose own path is `length` characters long.
std::filesystem::path DirectoryOfLength(const TempDirectory& fixture, size_t length)
{
    const size_t base = fixture.Path().native().size() + 1;
    CHECK(length > base);
    return fixture.Path() / std::wstring(length > base ? length - base : 1, L'd');
}

// The root was accepted whatever its length, and the staging directory under
// it - a 64-character key plus pid and nonce - failed with error=3 once the
// root passed about 140 characters, so every render on a deep portable
// install or a long custom root died at staging. A manager that is valid can
// now stage a render and write its sidecars; one that cannot is refused when
// it is made. The test executable is not long-path aware, so the long end of
// the sweep is refused rather than served.
void a_valid_cache_root_can_always_stage_a_render_test()
{
    TempDirectory fixture;
    size_t tried = 0;
    size_t refused = 0;
    for (size_t length = std::max<size_t>(100, fixture.Path().native().size() + 10);
         length <= 240; length += 10, ++tried) {
        const auto root = DirectoryOfLength(fixture, length);
        NeuralCacheManager manager(root);
        if (!manager.Valid()) {
            ++refused;
            CHECK(manager.LastFailure().cause == NeuralCacheFailure::Cause::NoWritableRoot);
            CHECK(manager.LastFailure().error.value() != 0);
            continue;
        }
        const auto staging = manager.BeginRenderStaging(std::string(64, '6'));
        CHECK(staging.has_value());
        if (!staging) continue;
        for (const auto sidecar : {L"neural-settings.ini", L"receipt.json", L"manifest.json", L"neural.mkv"}) {
            std::ofstream output(*staging / sidecar, std::ios::binary);
            CHECK(output.is_open());
        }
        // The probe leaves nothing behind in a root it accepted.
        size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(manager.Root() / L"staging")) {
            (void)entry;
            ++entries;
        }
        CHECK_EQ(size_t{1}, entries);
    }
    CHECK(refused > 0);
    CHECK(refused < tried);
}

// The same limit on the default root: a portable folder too deep to stage in
// is passed over for LocalAppData, as an unwritable one already was.
void default_cache_falls_back_when_the_portable_root_is_too_deep_to_stage_in_test()
{
    TempDirectory fixture;
    const auto deep = DirectoryOfLength(fixture, 150);
    std::error_code error;
    std::filesystem::create_directories(deep, error);
    CHECK(!error);
    if (error) return;
    RunCacheRootChildIn(deep, L"fallback");
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
// The cache never removed anything except by Clear(), which is all or
// nothing. Its key retires entries wholesale - one driver update changes every
// key - so 40 GB of renders could become unreachable at once with no way to
// reclaim it that did not also destroy the new renders.
//
// This is the manager half; CacheEvictionPolicy's rules are asserted in
// PolicyTests. Here: does it find the entries on disk, judge them by the same
// gate lookup uses, and actually remove the right directories.
void eviction_reclaims_unreachable_renders_and_keeps_the_rest_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());

    const std::string goodKey(64, 'a');
    const auto goodStaging = manager.BeginRenderStaging(goodKey);
    REQUIRE(goodStaging.has_value());
    WriteBytes(*goodStaging / L"neural.mkv", "neural-frames");
    StageRenderReceipt(*goodStaging);
    REQUIRE(manager.PromoteRender(goodKey, *goodStaging, CompleteRenderManifest()));

    // A retired entry, written straight to disk the way a previous build would
    // have left one: the schema gate refuses it, so it can never be served.
    const std::string deadKey(64, 'b');
    const auto deadDirectory = manager.Root() / L"renders" / std::wstring(deadKey.begin(), deadKey.end());
    std::filesystem::create_directories(deadDirectory);
    WriteBytes(deadDirectory / L"neural.mkv", "stale-frames");
    WriteBytes(deadDirectory / L"manifest.json", "{\"schema\":4}");

    CHECK(manager.LookupRender(goodKey).has_value());
    CHECK(!manager.LookupRender(deadKey).has_value());

    // A floor of zero: no disk pressure, so only the dead entry may go.
    const auto report = manager.Evict({}, 0);
    CHECK_EQ(size_t{1}, report.unreachableRemoved);
    CHECK_EQ(size_t{0}, report.leastRecentlyUsedRemoved);
    CHECK_EQ(size_t{0}, report.failures);
    CHECK(report.freedBytes > 0);

    CHECK(!std::filesystem::exists(deadDirectory));
    // And the reusable entry is untouched and still serves.
    CHECK(manager.LookupRender(goodKey).has_value());
}

// The guard that matters most: a render in progress owns its entry.
void eviction_leaves_an_active_entry_alone_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());

    const std::string activeKey(64, 'c');
    const auto directory = manager.Root() / L"renders" / std::wstring(activeKey.begin(), activeKey.end());
    std::filesystem::create_directories(directory);
    WriteBytes(directory / L"neural.mkv", "frames-being-written");
    WriteBytes(directory / L"manifest.json", "{\"schema\":4}");   // unreachable AND in use

    const std::string active[] = {activeKey};
    const auto report = manager.Evict(active, 0);
    CHECK_EQ(size_t{0}, report.unreachableRemoved);
    CHECK(std::filesystem::exists(directory));
}

DWORD DeadProcessId();

// Clear() offered to free bytes it then kept: SizeBytes recurses the whole
// root, live/ included, and Clear did not remove live/. The session is a dead
// process's: a running instance's is kept (see
// clear_keeps_what_another_running_instance_owns_test).
void clearing_the_cache_frees_everything_size_bytes_counted_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());

    const auto liveSegment = manager.Root() / L"live" /
        (L"pid" + std::to_wstring(DeadProcessId())) / L"segment-000.mkv";
    std::filesystem::create_directories(liveSegment.parent_path());
    WriteBytes(liveSegment, std::string(4096, 'x'));

    const uintmax_t before = manager.SizeBytes();
    CHECK(before >= 4096);

    CHECK(manager.Clear());
    CHECK_EQ(uintmax_t{0}, manager.SizeBytes());
    CHECK(!std::filesystem::exists(liveSegment));
}

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

// The model-store term has to move when the weights the pass evaluates do.
// The files small enough to afford it - the configs and selectors that decide
// which weights load - are listed by name and content; the blobs above the
// bound by name, size and write time.
void model_store_digest_tracks_root_contents_and_names_its_fallback_test()
{
    TempDirectory fixture;
    const auto models = fixture.Path() / L"models";
    const auto files = models / L"dlss" / L"versions" / L"20318464" / L"files";
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
    const auto refreshedFiles = models / L"dlss" / L"versions" / L"20318465" / L"files";
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
// The walk took every NGX feature, so the NVIDIA App refreshing frame
// generation or ray reconstruction weights - or merely rewriting a config file
// with the bytes it already had - changed every render key and orphaned the
// whole cache. Only what the neural pass evaluates may move the term now; a
// feature nobody has classified still does.
void model_store_digest_ignores_features_outside_the_neural_pass_test()
{
    TempDirectory fixture;
    const auto models = fixture.Path() / L"models";
    const auto dlss = models / L"dlss" / L"versions" / L"20318464" / L"files";
    const auto serverConfig = models / L"config" / L"versions" / L"2" / L"files";
    std::error_code error;
    CHECK(std::filesystem::create_directories(dlss, error));
    CHECK(std::filesystem::create_directories(serverConfig, error));
    const auto blob = dlss / L"160_E658700.bin";
    WriteBytes(blob, std::string((1u << 20) + 1u, 'w'));
    const auto config = models / L"nvngx_config.txt";
    const std::string configBytes =
        "[dlss]\r\napp_E658700 = 310.9.0\r\n\r\n[DLSSG]\r\napp_E658703 = 310.0.0\r\n"
        "[sl_dlss_g_0]\r\napp_E658703 = 2.14.0\r\n";
    WriteBytes(config, configBytes);
    const auto server = serverConfig / L"nvngx_server_config.txt";
    WriteBytes(server, "[dlssd]\napp_0000000 = 0.0.0\n[driver]\nversion = 1\n");
    const std::array<NeuralModelRoot, 1> roots{NeuralModelRoot{models, true, {}}};
    // No quiet period: every file here was written a moment ago, and what this
    // test pins is which files the term covers, not when a read may be trusted
    // (model_store_read_mid_rewrite_is_unsettled_and_waited_out_test).
    const auto digest = [&] {
        return DigestNeuralModelStore(roots, L"32.0.16.1047", {}, std::chrono::milliseconds{0});
    };

    const auto first = digest();
    CHECK(NeuralModelStoreSettled(first));
    CHECK_EQ(uint32_t{3}, first.files);

    // Weights of features the pass never evaluates, arriving or changing.
    for (const wchar_t* feature : {L"dlssd", L"dlssg", L"dlisp", L"sl_dlss_g_override_0",
                                   L"sl_reflex_0", L"sl_deepdvc_0", L"nvbcast_vfx_gs_v0_9"}) {
        const auto directory = models / feature / L"versions" / L"1" / L"files";
        CHECK(std::filesystem::create_directories(directory, error));
        WriteBytes(directory / L"160_B9D3EF0.bin", std::string((1u << 20) + 7u, 'r'));
        WriteBytes(directory / L"nvngx_package_config.txt", "selector");
    }
    const auto unrelated = digest();
    CHECK_EQ(first.digest, unrelated.digest);
    CHECK_EQ(first.files, unrelated.files);

    // Their sections in the two selector files, edited.
    WriteBytes(config, "[dlss]\r\napp_E658700 = 310.9.0\r\n\r\n[DLSSG]\r\napp_E658703 = 310.5.0\r\n"
                       "[sl_dlss_g_0]\r\napp_E658703 = 2.15.0\r\n");
    WriteBytes(server, "[dlssd]\napp_0000000 = 9.9.9\n[driver]\nversion = 1\n");
    CHECK_EQ(first.digest, digest().digest);
    // ...while a section the pass does read still moves it, in either file.
    WriteBytes(config, "[dlss]\r\napp_E658700 = 310.9.1\r\n\r\n[DLSSG]\r\napp_E658703 = 310.5.0\r\n"
                       "[sl_dlss_g_0]\r\napp_E658703 = 2.15.0\r\n");
    const auto dlssEdited = digest();
    CHECK(dlssEdited.digest != first.digest);
    WriteBytes(server, "[dlssd]\napp_0000000 = 9.9.9\n[driver]\nversion = 2\n");
    const auto driverSection = digest();
    CHECK(driverSection.digest != dlssEdited.digest);

    // A config rewritten in place with the bytes it had: only its write time
    // moved, and a hashed file is its content.
    const auto rewritten = std::filesystem::last_write_time(config) - std::chrono::hours(48);
    std::filesystem::last_write_time(config, rewritten);
    CHECK_EQ(driverSection.digest, digest().digest);
    // A blob too large to hash is still identified by its write time.
    std::filesystem::last_write_time(blob, std::filesystem::last_write_time(blob) - std::chrono::hours(48));
    const auto touched = digest();
    CHECK(touched.digest != driverSection.digest);

    // A feature this list has never heard of - an NR directory, the day NGX
    // ships one - is in the term.
    const auto nr = models / L"dlssnr" / L"versions" / L"1" / L"files";
    CHECK(std::filesystem::create_directories(nr, error));
    WriteBytes(nr / L"160_E658700.bin", "neural-rendering-weights");
    const auto grown = digest();
    CHECK(grown.digest != touched.digest);
    CHECK_EQ(touched.files + 1u, grown.files);
    CHECK(NeuralModelStoreSettled(grown));

    // Settledness is what eviction trusts: a registered root that is simply
    // not there reads the same every time, one that is there and cannot be
    // walked does not.
    const std::array<NeuralModelRoot, 1> absent{
        NeuralModelRoot{fixture.Path() / L"no-such-root", true, {}}};
    CHECK(NeuralModelStoreSettled(DigestNeuralModelStore(absent, L"32.0.16.1047")));
    WriteBytes(fixture.Path() / L"not-a-directory", "file");
    const std::array<NeuralModelRoot, 1> unreadable{
        NeuralModelRoot{fixture.Path() / L"not-a-directory", true, {}}};
    CHECK(!NeuralModelStoreSettled(DigestNeuralModelStore(unreadable, L"32.0.16.1047")));
}

// NGX rewrites config/versions/<n>/files/{nvngx_server_config.txt,
// nvngx_mapping.json,nvngx_deny_list.txt} in place on every initialisation:
// truncated to 0 bytes, written back 35 ms to 1.2 s later (measured by polling
// the directory while GPU jobs ran). A read inside that window hashed the store
// with one file empty - same file count, same hashed count, nothing unreadable,
// so it passed as settled. On the development machine that read was
// a69cdc79..., the store with nvngx_server_config.txt empty, against 59792fa7...
// for the same store whole; renders were keyed under it and then evicted as
// "retired by a changed model store". A file written inside the quiet period
// now makes the read unsettled, the resolve waits it out, and eviction wants
// two settled reads that agree.
void model_store_read_mid_rewrite_is_unsettled_and_waited_out_test()
{
    using namespace std::chrono_literals;
    TempDirectory fixture;
    const auto models = fixture.Path() / L"models";
    const auto configFiles = models / L"config" / L"versions" / L"2" / L"files";
    std::error_code error;
    CHECK(std::filesystem::create_directories(configFiles, error));
    const auto server = configFiles / L"nvngx_server_config.txt";
    const std::string serverBytes = "[dlss]\napp_E658700 = 310.9.0\n[driver]\nversion = 1\n";
    WriteBytes(server, serverBytes);
    WriteBytes(configFiles / L"nvngx_mapping.json", "{\"mapping\":[]}");
    WriteBytes(models / L"nvngx_config.txt", "[dlss]\napp_E658700 = 310.9.0\n");
    const auto age = [&](std::chrono::hours by) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(models))
            if (entry.is_regular_file())
                std::filesystem::last_write_time(entry.path(), std::filesystem::file_time_type::clock::now() - by);
    };
    age(1h);
    const std::array<NeuralModelRoot, 1> roots{NeuralModelRoot{models, true, {}}};
    const auto read = [&] { return DigestNeuralModelStore(roots, L"32.0.16.1047"); };

    const auto whole = read();
    CHECK(NeuralModelStoreSettled(whole));
    CHECK_EQ(uint32_t{0}, whole.recentlyWrittenFiles);
    CHECK(NeuralModelStoresAgree(whole, read()));

    // NGX's truncation, caught: a different digest from the same file count,
    // which is exactly what went into render keys.
    WriteBytes(server, "");
    const auto torn = read();
    CHECK(torn.digest != whole.digest);
    CHECK_EQ(whole.files, torn.files);
    CHECK_EQ(whole.contentHashedFiles, torn.contentHashedFiles);
    CHECK_EQ(uint32_t{0}, torn.unreadableFiles);
    CHECK_EQ(uint32_t{1}, torn.recentlyWrittenFiles);
    CHECK(!NeuralModelStoreSettled(torn));
    CHECK(torn.settlesIn > 0ms && torn.settlesIn <= kModelStoreQuietPeriod);
    CHECK(!NeuralModelStoresAgree(whole, torn));
    CHECK(!NeuralModelStoresAgree(torn, torn));
    CHECK(NeuralModelStoreJson(torn).find("\"settled\":false") != std::string::npos);
    CHECK(NeuralModelStoreJson(whole).find("\"settled\":true") != std::string::npos);

    // Written back with the bytes it had: the digest is the store's again, but
    // it is not trusted until the file has been quiet for the period.
    WriteBytes(server, serverBytes);
    const auto rewritten = read();
    CHECK_EQ(whole.digest, rewritten.digest);
    CHECK(!NeuralModelStoreSettled(rewritten));

    // The resolve waits a rewrite out: truncate, then write back 150 ms later
    // on another thread, as NGX does, while the settled read runs.
    WriteBytes(server, "");
    std::thread ngx([&] {
        std::this_thread::sleep_for(150ms);
        WriteBytes(server, serverBytes);
    });
    const auto started = std::chrono::steady_clock::now();
    const auto settled = DigestSettledNeuralModelStore(roots, L"32.0.16.1047", {}, 300ms, 5s);
    const auto waited = std::chrono::steady_clock::now() - started;
    ngx.join();
    CHECK(NeuralModelStoreSettled(settled));
    CHECK_EQ(whole.digest, settled.digest);
    // At least the quiet period after the write-back, and nowhere near the patience.
    CHECK(waited >= 300ms);
    CHECK(waited < 4s);

    // No patience: one read, returned as it is.
    WriteBytes(server, "");
    const auto impatient = DigestSettledNeuralModelStore(roots, L"32.0.16.1047", {}, 300ms, 0ms);
    CHECK(!NeuralModelStoreSettled(impatient));
    CHECK(impatient.digest != whole.digest);
    // A stop ends the wait at once instead of sitting out the patience.
    std::stop_source stop;
    stop.request_stop();
    const auto stopStarted = std::chrono::steady_clock::now();
    const auto stopped = DigestSettledNeuralModelStore(roots, L"32.0.16.1047", stop.get_token(), 10s, 60s);
    CHECK(std::chrono::steady_clock::now() - stopStarted < 2s);
    CHECK(!NeuralModelStoreSettled(stopped));

    // A stamp far in the future is a skewed clock, not a write in progress;
    // counting it as recent would hold the store unsettled for good.
    WriteBytes(server, serverBytes);
    age(1h);
    std::filesystem::last_write_time(server, std::filesystem::file_time_type::clock::now() + 24h);
    const auto skewed = read();
    CHECK(NeuralModelStoreSettled(skewed));
    CHECK_EQ(whole.digest, skewed.digest);
}

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

// P1.16: the manifest's JsonEscape wrote "" for a control character without a
// short form, so the whole field came back as a different, valid value - an
// environment term of the render identity among them. Every control character
// now escapes, and the reader takes back exactly what the writer produces.
void manifest_fields_with_control_characters_round_trip_test()
{
    std::string every;
    for (char character = 1; character < 0x20; ++character) every.push_back(character);
    CHECK_EQ(std::string("\\u0001\\b\\t\\n\\f\\r\\u001f\\\"\\\\"),
             JsonEscape(std::string("\x01\b\t\n\f\r\x1f\"\\")));
    CHECK_EQ(std::string("caf\xc3\xa9"), JsonEscape("caf\xc3\xa9"));

    auto manifest = CompleteRenderManifest();
    manifest.state = NeuralCacheState::Complete;
    manifest.neuralDigest = std::string(64, 'c');
    manifest.environment = {"app" + every, "installation\x01", "driver\x1b[0m", std::string(64, 'd')};
    const auto parsed = ParseNeuralCacheManifest(SerializeNeuralCacheManifest(manifest));
    CHECK(parsed.has_value());
    if (parsed) CHECK(manifest.environment == parsed->environment);
    // Strict still: only the escapes the writer produces.
    for (const std::string_view foreign : {"\\u0020", "\\u00e9", "\\u12ab", "\\u00G1", "\\u001"}) {
        std::string planted = SerializeNeuralCacheManifest(manifest);
        const size_t at = planted.find(R"(installation\u0001)");
        CHECK(at != std::string::npos);
        if (at == std::string::npos) continue;
        planted.insert(at + 12, foreign);
        CHECK(!ParseNeuralCacheManifest(planted).has_value());
    }
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
    // Quarantined entries are kept for inspection for a few days
    // (quarantined_entries_are_kept_for_inspection_then_reaped_test); these
    // two are past it.
    for (const auto& aged : {invalidExistingOwn, invalidCacheDead})
        std::filesystem::last_write_time(
            aged, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 4));

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

// A process that is certainly alive and certainly not this one: a suspended
// copy of the test executable, terminated by the destructor. The owner checks
// under test treat any pid the kernel still knows as alive, so this is the
// "another running player instance" case without a second player.
class LiveChildProcess {
public:
    LiveChildProcess()
    {
        const auto executable = CurrentExecutable();
        std::wstring arguments = L"\"" + executable.wstring() + L"\" --suspended-child";
        STARTUPINFOW startup{sizeof(startup)};
        if (CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, FALSE,
                           CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process_)) CloseHandle(process_.hThread);
    }
    ~LiveChildProcess()
    {
        if (!process_.hProcess) return;
        TerminateProcess(process_.hProcess, 0);
        WaitForSingleObject(process_.hProcess, 10000);
        CloseHandle(process_.hProcess);
    }
    LiveChildProcess(const LiveChildProcess&) = delete;
    LiveChildProcess& operator=(const LiveChildProcess&) = delete;
    DWORD Pid() const { return process_.dwProcessId; }

private:
    PROCESS_INFORMATION process_{};
};

// The lead case for ContainChildProcesses: a test process that dies without
// unwinding - a crash, a debugger kill - takes its children with it. The
// middle process contains itself, starts a suspended child, and terminates,
// reporting the child's pid as its exit code; an orphan would outlive it.
int RunContainedParent()
{
    test_support::ContainChildProcesses();
    const auto executable = CurrentExecutable();
    std::wstring arguments = L"\"" + executable.wstring() + L"\" --suspended-child";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child))
        return 0;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    TerminateProcess(GetCurrentProcess(), child.dwProcessId);
    return 0;
}

void a_test_process_that_dies_takes_its_children_with_it_test()
{
    const auto executable = CurrentExecutable();
    std::wstring arguments = L"\"" + executable.wstring() + L"\" --contained-parent";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION middle{};
    CHECK(CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup, &middle));
    if (!middle.hProcess) return;
    CloseHandle(middle.hThread);
    CHECK_EQ(DWORD{WAIT_OBJECT_0}, WaitForSingleObject(middle.hProcess, 10000));
    DWORD orphan = 0;
    CHECK(GetExitCodeProcess(middle.hProcess, &orphan));
    CloseHandle(middle.hProcess);
    CHECK(orphan != 0 && orphan != STILL_ACTIVE);
    if (orphan == 0 || orphan == STILL_ACTIVE) return;
    // The kernel kills the job's members when the last handle closes, which is
    // a moment after the middle process is gone; a pid it no longer knows is
    // as dead as a signalled handle.
    const HANDLE child = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, orphan);
    if (!child) {
        CHECK_EQ(DWORD{ERROR_INVALID_PARAMETER}, GetLastError());
        return;
    }
    const DWORD waited = WaitForSingleObject(child, 5000);
    CHECK_EQ(DWORD{WAIT_OBJECT_0}, waited);
    if (waited != WAIT_OBJECT_0) TerminateProcess(child, 0);
    CloseHandle(child);
}

// A concurrent run's fixture is not this run's to delete: the sweep only takes
// a directory whose owner has exited (or one no run could still be using).
void fixture_sweep_leaves_a_live_runs_directory_alone_test()
{
    TempDirectory fixture;
    const LiveChildProcess other;
    CHECK(other.Pid() != 0);
    const DWORD deadPid = DeadProcessId();
    CHECK(deadPid != 0);
    const auto parent = fixture.Path().parent_path();
    const std::wstring prefix(TempDirectory::kPrefix);
    const auto live = parent / (prefix + std::to_wstring(other.Pid()) + L"-1");
    const auto dead = parent / (prefix + std::to_wstring(deadPid) + L"-1");
    const auto unrelated = parent / (prefix + L"notapid-1");
    for (const auto& directory : {live, dead, unrelated}) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        CHECK(!error);
    }
    TempDirectory::SweepAbandoned(parent);
    CHECK(std::filesystem::is_directory(live));
    CHECK(!std::filesystem::exists(dead));
    CHECK(std::filesystem::is_directory(unrelated));
    CHECK(std::filesystem::is_directory(fixture.Path()));
    // Past the age no run lasts, a live pid is a reused one.
    std::filesystem::last_write_time(
        live, std::filesystem::file_time_type::clock::now() - std::chrono::hours(25));
    TempDirectory::SweepAbandoned(parent);
    CHECK(!std::filesystem::exists(live));
    std::error_code error;
    std::filesystem::remove_all(unrelated, error);
}

// Publishes a render under `key` with `environment` recorded (or none), and
// the runtime digest `runtime`.
bool PublishRender(NeuralCacheManager& manager, const std::string& key,
                   const NeuralCacheEnvironment& environment, char runtime = 'b')
{
    const auto staging = manager.BeginRenderStaging(key);
    if (!staging) return false;
    WriteBytes(*staging / L"neural.mkv", "neural-frames-" + key.substr(0, 4));
    StageRenderReceipt(*staging);
    auto manifest = CompleteRenderManifest();
    manifest.runtimeDigest = std::string(64, runtime);
    manifest.environment = environment;
    return manager.PromoteRender(key, *staging, manifest);
}

std::filesystem::path RenderDirectory(const NeuralCacheManager& manager, const std::string& key)
{
    return manager.Root() / L"renders" / std::wstring(key.begin(), key.end());
}

// The manifest recorded none of the key's environment, so eviction could not
// tell an entry a driver update had orphaned from one that still worked. The
// field is additive: a manifest without it is byte-for-byte what earlier
// writers produced and still parses, and one with it is exact and all or
// nothing.
void manifest_environment_is_optional_additive_and_all_or_nothing_test()
{
    auto manifest = CompleteRenderManifest();
    manifest.state = NeuralCacheState::Complete;
    manifest.neuralDigest = std::string(64, 'c');
    const std::string legacyBytes = SerializeNeuralCacheManifest(manifest);
    CHECK(legacyBytes.find("environment") == std::string::npos);
    CHECK(legacyBytes.ends_with("\"}\n"));
    const auto legacy = ParseNeuralCacheManifest(legacyBytes);
    CHECK(legacy.has_value());
    if (legacy) {
        CHECK(!legacy->environment.Recorded());
        CHECK(IsReusableNeuralCacheManifest(*legacy));
    }

    manifest.environment = {"0.21.2", "c:/players/dlss", "32.0.16.1047", std::string(64, 'd')};
    const std::string recordedBytes = SerializeNeuralCacheManifest(manifest);
    CHECK(recordedBytes.find(",\"environment\":{\"application\":\"0.21.2\"") != std::string::npos);
    // Everything before the extension is the legacy manifest unchanged.
    CHECK(recordedBytes.starts_with(legacyBytes.substr(0, legacyBytes.size() - 2)));
    const auto recorded = ParseNeuralCacheManifest(recordedBytes);
    CHECK(recorded.has_value());
    if (recorded) {
        CHECK_EQ(manifest, *recorded);
        CHECK(IsReusableNeuralCacheManifest(*recorded));
    }

    // All or nothing, and renders only.
    auto partial = manifest;
    partial.environment.application.clear();
    CHECK(!IsReusableNeuralCacheManifest(partial));
    CHECK(!ParseNeuralCacheManifest(SerializeNeuralCacheManifest(partial)).has_value());
    auto badModels = manifest;
    badModels.environment.modelStore = "not-a-digest";
    CHECK(!ParseNeuralCacheManifest(SerializeNeuralCacheManifest(badModels)).has_value());
    auto source = manifest;
    source.kind = NeuralCacheEntryKind::Source;
    CHECK(!ParseNeuralCacheManifest(SerializeNeuralCacheManifest(source)).has_value());
    // An empty object is not something any writer produces.
    std::string empty = legacyBytes;
    empty.insert(empty.size() - 2, ",\"environment\":{\"application\":\"\",\"installation\":\"\","
                                   "\"driver\":\"\",\"models\":\"\"}");
    CHECK(!ParseNeuralCacheManifest(empty).has_value());

    // The recorder records nothing rather than something the gate refuses: an
    // undetected driver must not make a finished render unpublishable.
    NeuralCacheIdentity identity;
    identity.applicationVersion = "0.21.2";
    identity.modelStoreDigest = std::string(64, 'd');
    CHECK(!NeuralCacheEnvironmentFor(identity).Recorded());
    identity.driverVersion = "32.0.16.1047";
    const auto environment = NeuralCacheEnvironmentFor(identity);
    CHECK(environment.Recorded());
    CHECK_EQ(NeuralCacheInstallation(), environment.installation);
    CHECK(!environment.installation.empty());
    CHECK(environment.installation.find('\\') == std::string::npos);
}

// Entries a driver update, a model refresh or an upgrade of this installation
// orphaned were kept until the disk ran short. Now they go at the next start -
// and only they: an entry that recorded nothing, one that still matches, and
// one another installation sharing the root can still serve all stay.
void eviction_retires_entries_whose_recorded_environment_nothing_can_rebuild_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());

    const NeuralCacheEnvironment here{"0.21.2", NeuralCacheInstallation(), "32.0.16.1047",
                                      std::string(64, 'd')};
    const cache_eviction::Identity current{here.application, here.installation,
                                           std::string(64, 'b'), here.driver, here.modelStore};
    auto elsewhere = here;
    elsewhere.installation = "d:/another/player";
    auto oldDriver = here;
    oldDriver.driver = "32.0.15.9999";
    auto oldModels = here;
    oldModels.modelStore = std::string(64, 'e');
    auto oldVersion = here;
    oldVersion.application = "0.21.1";
    auto otherInstallOldVersion = elsewhere;
    otherInstallOldVersion.application = "0.21.1";

    const std::string legacy(64, '1'), match(64, '2'), driver(64, '3'), models(64, '4'),
        version(64, '5'), runtime(64, '6'), otherInstall(64, '7');
    REQUIRE(PublishRender(manager, legacy, {}));
    REQUIRE(PublishRender(manager, match, here));
    REQUIRE(PublishRender(manager, driver, oldDriver));
    REQUIRE(PublishRender(manager, models, oldModels));
    REQUIRE(PublishRender(manager, version, oldVersion));
    REQUIRE(PublishRender(manager, runtime, here, 'f'));
    REQUIRE(PublishRender(manager, otherInstall, otherInstallOldVersion));

    // Without the current identity nothing is judged by it.
    const auto blind = manager.Evict({}, 0);
    CHECK_EQ(size_t{0}, blind.retiredRemoved + blind.unreachableRemoved);
    // An identity that is not fully known decides nothing either.
    auto unsettled = current;
    unsettled.models.clear();
    const auto unsure = manager.Evict({}, 0, &unsettled);
    CHECK_EQ(size_t{0}, unsure.retiredRemoved);

    // A floor of zero: this is the always-on pass, not the pressure pass.
    const std::string activeKeys[] = {std::string(64, '9')};
    const auto report = manager.Evict(activeKeys, 0, &current);
    CHECK_EQ(size_t{4}, report.retiredRemoved);
    CHECK_EQ(size_t{0}, report.unreachableRemoved);
    CHECK_EQ(size_t{0}, report.leastRecentlyUsedRemoved);
    CHECK_EQ(size_t{0}, report.failures);
    for (const auto& gone : {driver, models, version, runtime})
        CHECK(!std::filesystem::exists(RenderDirectory(manager, gone)));
    for (const auto& kept : {legacy, match, otherInstall})
        CHECK(manager.LookupRender(kept).has_value());

    // An active key is never touched, retired or not.
    REQUIRE(PublishRender(manager, driver, oldDriver));
    const std::string active[] = {driver};
    CHECK_EQ(size_t{0}, manager.Evict(active, 0, &current).retiredRemoved);
    CHECK(std::filesystem::exists(RenderDirectory(manager, driver)));
}

// remove_all deleted file by file, so an entry whose payload another process
// had open lost its manifest and receipt and kept a payload nothing could
// serve. A rename is all or nothing: the entry stays whole until it is free.
void eviction_leaves_an_open_entry_whole_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());
    const std::string key(64, 'c');
    const auto directory = RenderDirectory(manager, key);
    std::filesystem::create_directories(directory);
    WriteBytes(directory / L"neural.mkv", "frames-being-played");
    WriteBytes(directory / L"manifest.json", "{\"schema\":4}");   // unreachable

    const HANDLE player = CreateFileW((directory / L"neural.mkv").c_str(), GENERIC_READ,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(player != INVALID_HANDLE_VALUE);
    const auto held = manager.Evict({}, 0);
    CHECK_EQ(size_t{1}, held.failures);
    CHECK_EQ(size_t{0}, held.unreachableRemoved);
    CHECK(std::filesystem::is_regular_file(directory / L"manifest.json"));
    CHECK(std::filesystem::is_regular_file(directory / L"neural.mkv"));
    CloseHandle(player);

    const auto freed = manager.Evict({}, 0);
    CHECK_EQ(size_t{1}, freed.unreachableRemoved);
    CHECK(!std::filesystem::exists(directory));
}

// Nothing recorded a use, so eviction by least recent use was eviction by age.
// A lookup now stamps the entry, and never fails for want of stamping it.
void lookup_marks_the_entry_used_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());
    const std::string key(64, 'e');
    REQUIRE(PublishRender(manager, key, {}));
    const auto directory = RenderDirectory(manager, key);

    // Back-date the entry to 2020, the way an entry written long ago looks.
    const HANDLE handle = CreateFileW(directory.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    SYSTEMTIME old{2020, 1, 3, 1, 0, 0, 0, 0};
    FILETIME oldTime{};
    SystemTimeToFileTime(&old, &oldTime);
    CHECK(SetFileTime(handle, nullptr, nullptr, &oldTime));
    CloseHandle(handle);
    const auto before = std::filesystem::last_write_time(directory);

    CHECK(manager.LookupRender(key).has_value());
    const auto after = std::filesystem::last_write_time(directory);
    CHECK(after > before);
    CHECK(after > std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
}

// Every instance sharing a root takes one named mutex around the operations
// that restructure it. Clear runs on the UI thread, so it gives up quickly and
// says so; eviction defers the entry; neither removes anything meanwhile.
void cache_operations_yield_to_another_holder_of_the_root_lock_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());
    const std::string good(64, 'a');
    REQUIRE(PublishRender(manager, good, {}));
    const std::string dead(64, 'b');
    const auto deadDirectory = RenderDirectory(manager, dead);
    std::filesystem::create_directories(deadDirectory);
    WriteBytes(deadDirectory / L"manifest.json", "{\"schema\":4}");

    const std::wstring name = NeuralCacheRootLockName(manager.Root());
    CHECK(name.starts_with(L"Local\\DLSSVideoPlayer.Cache."));
    CHECK_EQ(size_t{28 + 16}, name.size());
    // The same root spelled with a trailing separator and upper case is the
    // same lock.
    std::wstring shouted = manager.Root().wstring() + L"\\";
    std::ranges::transform(shouted, shouted.begin(), towupper);
    CHECK(name == NeuralCacheRootLockName(shouted));

    std::promise<void> taken;
    std::promise<void> release;
    std::thread holder([&] {
        const HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
        WaitForSingleObject(mutex, INFINITE);
        taken.set_value();
        release.get_future().wait();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    });
    taken.get_future().wait();

    const auto started = std::chrono::steady_clock::now();
    CHECK(!manager.Clear());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(std::filesystem::exists(RenderDirectory(manager, good)));
    const auto report = manager.Evict({}, 0);
    CHECK_EQ(size_t{1}, report.deferred);
    CHECK_EQ(size_t{0}, report.unreachableRemoved);
    CHECK(std::filesystem::exists(deadDirectory));
    // A lookup does not wait long for it and still answers.
    CHECK(manager.LookupRender(good).has_value());

    release.set_value();
    holder.join();
    CHECK_EQ(size_t{1}, manager.Evict({}, 0).unreachableRemoved);
    CHECK(manager.Clear());
    CHECK(!manager.LookupRender(good).has_value());
}

// Clear() ran remove_all over live/ and staging/ and deleted what another
// running instance was writing: its session's segments, its unfinished
// renders. Those stay now; this process's and dead processes' go.
void clear_keeps_what_another_running_instance_owns_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());
    const LiveChildProcess other;
    REQUIRE(other.Pid() != 0);
    const DWORD deadPid = DeadProcessId();
    REQUIRE(deadPid != 0);
    const std::wstring key(64, L'4');
    const auto live = manager.Root() / L"live";
    const auto staging = manager.Root() / L"staging";
    const auto otherSession = live / (L"pid" + std::to_wstring(other.Pid()));
    const auto deadSession = live / (L"pid" + std::to_wstring(deadPid));
    const auto ownSession = live / (L"pid" + std::to_wstring(GetCurrentProcessId()));
    const auto otherRender = staging / (L"render-" + key + L"-" + std::to_wstring(other.Pid()) + L"-1");
    const auto deadRender = staging / (L"render-" + key + L"-" + std::to_wstring(deadPid) + L"-2");
    for (const auto& directory : {otherSession, deadSession, ownSession, otherRender, deadRender}) {
        std::filesystem::create_directories(directory);
        WriteBytes(directory / L"neural-00000.mkv", "segment");
    }
    REQUIRE(PublishRender(manager, std::string(64, 'a'), {}));

    CHECK(manager.Clear());
    for (const auto& kept : {otherSession, otherRender})
        CHECK(std::filesystem::is_regular_file(kept / L"neural-00000.mkv"));
    for (const auto& gone : {deadSession, ownSession, deadRender})
        CHECK(!std::filesystem::exists(gone));
    CHECK(!manager.LookupRender(std::string(64, 'a')).has_value());
    for (const auto bucket : {L"sources", L"renders", L"staging", L"frame-generation", L"live"})
        CHECK(std::filesystem::is_directory(manager.Root() / bucket));
}

// A crash during a live session left its live/pid<N> - gigabytes of segments -
// and only the current process's own directory was ever removed.
void startup_sweep_removes_live_sessions_whose_process_is_gone_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());
    const LiveChildProcess other;
    REQUIRE(other.Pid() != 0);
    const DWORD deadPid = DeadProcessId();
    REQUIRE(deadPid != 0);
    const auto live = manager.Root() / L"live";
    const auto dead = live / (L"pid" + std::to_wstring(deadPid));
    const auto alive = live / (L"pid" + std::to_wstring(other.Pid()));
    const auto own = live / (L"pid" + std::to_wstring(GetCurrentProcessId()));
    const auto padded = live / (L"pid0" + std::to_wstring(deadPid));
    const auto foreign = live / L"notes";
    for (const auto& directory : {dead, alive, own, padded, foreign}) {
        std::filesystem::create_directories(directory / L"job1");
        WriteBytes(directory / L"job1" / L"neural-00000.mkv", "segment");
    }
    CHECK_EQ(size_t{1}, manager.SweepLiveSessions());
    CHECK(!std::filesystem::exists(dead));
    for (const auto& kept : {alive, own, padded, foreign})
        CHECK(std::filesystem::is_directory(kept));
    CHECK_EQ(size_t{0}, manager.SweepLiveSessions());
}

// Lookup hashed the whole payload on every call, including the entry this
// process had published and hashed a moment before - a multi-GB read each
// time - and a cancelled job could not abandon it. The digest promotion took
// is reused only while the file is provably the one it was taken over.
void lookup_reuses_its_own_published_digest_only_for_the_same_file_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());
    const std::string key(64, 'a');
    REQUIRE(PublishRender(manager, key, {}));
    const auto payload = RenderDirectory(manager, key) / L"neural.mkv";

    // Held with no sharing at all, the payload cannot be read - so a lookup
    // that answers did not hash it. Any manager in this process may use it.
    {
        const HANDLE exclusive = CreateFileW(payload.c_str(), GENERIC_READ, 0, nullptr,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(exclusive != INVALID_HANDLE_VALUE);
        CHECK(!Sha256File(payload).has_value());
        CHECK(manager.LookupRender(key).has_value());
        NeuralCacheManager second(fixture.Path() / L"cache");
        CHECK(second.LookupRender(key).has_value());
        CloseHandle(exclusive);
    }

    // Rewritten in place with the same bytes: the file is no longer provably
    // the hashed one, so it is read again - and a stopped lookup gives up on
    // that read instead of finishing it.
    const std::string original = ReadBytes(payload);
    WriteBytes(payload, original);
    std::stop_source stopped;
    stopped.request_stop();
    CHECK(!manager.LookupRender(key, stopped.get_token()).has_value());
    CHECK(manager.LookupRender(key).has_value());

    // Tampered at the same size, then with its write time put back: the
    // change time still moved, so the stale digest is never used.
    const auto published = RenderDirectory(manager, key);
    REQUIRE(PublishRender(manager, std::string(64, 'b'), {}));
    const auto other = RenderDirectory(manager, std::string(64, 'b')) / L"neural.mkv";
    const auto written = std::filesystem::last_write_time(other);
    std::string tampered = ReadBytes(other);
    tampered.front() = tampered.front() == 'x' ? 'y' : 'x';
    WriteBytes(other, tampered);
    std::filesystem::last_write_time(other, written);
    CHECK(!manager.LookupRender(std::string(64, 'b')).has_value());
    CHECK(std::filesystem::exists(published));
}

// A quarantined entry is the one artifact that says why a render went bad,
// and the next sweep deleted it. It is kept, noted, for a few days.
void quarantined_entries_are_kept_for_inspection_then_reaped_test()
{
    TempDirectory fixture;
    NeuralCacheManager manager(fixture.Path() / L"cache");
    REQUIRE(manager.Valid());
    const std::string key(64, 'c');
    REQUIRE(PublishRender(manager, key, {}));
    const auto entry = manager.LookupRender(key);
    REQUIRE(entry.has_value());
    CHECK(manager.Quarantine(*entry));
    CHECK(!manager.LookupRender(key).has_value());

    std::filesystem::path quarantined;
    for (const auto& child : std::filesystem::directory_iterator(manager.Root() / L"staging"))
        if (child.path().filename().wstring().starts_with(L"invalid-cache-")) quarantined = child.path();
    REQUIRE(!quarantined.empty());
    CHECK(std::filesystem::is_regular_file(quarantined / L"neural.mkv"));
    CHECK(ReadBytes(quarantined / L"quarantine.txt").starts_with("reason="));

    CHECK_EQ(size_t{0}, manager.SweepStaging());
    CHECK(std::filesystem::is_directory(quarantined));
    std::filesystem::last_write_time(
        quarantined, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 4));
    CHECK_EQ(size_t{1}, manager.SweepStaging());
    CHECK(!std::filesystem::exists(quarantined));
}

// remove_all could not be interrupted, so the sweep's budget meant nothing
// against one big abandoned directory. It deletes file by file now, and a
// directory it did not finish is finished by a later sweep.
void staging_sweep_resumes_a_directory_it_could_not_finish_test()
{
    TempDirectory fixture;
    const auto cacheRoot = fixture.Path() / L"cache";
    const DWORD deadPid = DeadProcessId();
    REQUIRE(deadPid != 0);
    const auto orphan = cacheRoot / L"staging" /
        (L"render-" + std::wstring(64, L'6') + L"-" + std::to_wstring(deadPid) + L"-1");
    std::filesystem::create_directories(orphan / L"job1");
    for (int index = 0; index < 400; ++index)
        WriteBytes(orphan / L"job1" / (L"neural-" + std::to_wstring(index) + L".mkv"), "segment");
    NeuralCacheManager manager(cacheRoot);
    REQUIRE(manager.Valid());
    for (int pass = 0; pass < 200 && std::filesystem::exists(orphan); ++pass) manager.SweepStaging();
    CHECK(!std::filesystem::exists(orphan));
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
    // P010 is NV12's layout at two bytes a sample.
    CHECK_EQ(uint64_t{1920 * 1080 * 3}, EncoderFrameBytes(EncoderPixelFormat::P010, 1920, 1080));
}

// The quality ladder. Standard is every render before the ladder, so its arguments
// are the ones above, unchanged; the two 10-bit rungs are pinned here.
void encoder_quality_ladder_arguments_test()
{
    const auto value = [](const std::vector<std::wstring>& list, const wchar_t* flag) {
        std::vector<std::wstring> found;
        for (size_t index = 0; index + 1 < list.size(); ++index)
            if (list[index] == flag) found.push_back(list[index + 1]);
        return found;
    };
    const auto has = [](const std::vector<std::wstring>& list, const wchar_t* item) {
        return std::find(list.begin(), list.end(), std::wstring(item)) != list.end();
    };
    const std::filesystem::path output = LR"(C:\Cache Root\neural.partial.mkv)";
    EncoderSpec standard{1920, 1080, 30.0, EncoderKind::HevcNvenc};
    // The default rung is Standard, and asking for it explicitly changes nothing.
    CHECK(standard.quality == EncoderQuality::Standard);
    EncoderSpec explicitStandard = standard;
    explicitStandard.quality = EncoderQuality::Standard;
    CHECK(BuildEncoderArguments(standard, output) == BuildEncoderArguments(explicitStandard, output));
    // Standard is constant quality too: NVENC's bitrate ceiling is lifted on every rung,
    // and the software fallback has none to lift.
    CHECK_EQ((std::vector<std::wstring>{L"800M"}), value(BuildEncoderArguments(standard, output), L"-maxrate"));
    CHECK_EQ((std::vector<std::wstring>{L"800M"}), value(BuildEncoderArguments(standard, output), L"-bufsize"));
    CHECK_EQ((std::vector<std::wstring>{L"16"}), value(BuildEncoderArguments(standard, output), L"-cq"));
    EncoderSpec standardSoftware = standard;
    standardSoftware.kind = EncoderKind::H264Software;
    CHECK(!has(BuildEncoderArguments(standardSoftware, output), L"-maxrate"));

    // High through NVENC: P010 in and out, Main10, the rung's CQ, the ceiling lifted,
    // and only setparams - the capture shader already converted the colour.
    EncoderSpec high{1920, 1080, 30.0, EncoderKind::HevcNvenc, EncoderPixelFormat::P010};
    high.quality = EncoderQuality::High;
    const auto highArguments = BuildEncoderArguments(high, output);
    CHECK_EQ((std::vector<std::wstring>{L"p010le", L"p010le"}), value(highArguments, L"-pix_fmt"));
    CHECK_EQ((std::vector<std::wstring>{L"main10"}), value(highArguments, L"-profile:v"));
    CHECK_EQ((std::vector<std::wstring>{std::to_wstring(kHighRungCq)}), value(highArguments, L"-cq"));
    CHECK_EQ((std::vector<std::wstring>{L"800M"}), value(highArguments, L"-maxrate"));
    CHECK_EQ((std::vector<std::wstring>{L"setparams=color_primaries=bt709:color_trc=bt709:"
                                        L"colorspace=bt709:range=tv"}), value(highArguments, L"-vf"));
    // Its software fallback is x264 High 10 at the same number.
    EncoderSpec highSoftware = high;
    highSoftware.kind = EncoderKind::H264Software;
    const auto highSoftwareArguments = BuildEncoderArguments(highSoftware, output);
    CHECK_EQ((std::vector<std::wstring>{L"p010le", L"yuv420p10le"}), value(highSoftwareArguments, L"-pix_fmt"));
    CHECK_EQ((std::vector<std::wstring>{std::to_wstring(kHighRungCq)}), value(highSoftwareArguments, L"-crf"));
    // An odd size cannot be P010, so it arrives as BGRA, is converted with the matrix
    // stated, and keeps every row and column at 4:4:4.
    EncoderSpec highOdd{1919, 1079, 30.0, EncoderKind::H264Software};
    highOdd.quality = EncoderQuality::High;
    const auto highOddArguments = BuildEncoderArguments(highOdd, output);
    CHECK_EQ((std::vector<std::wstring>{L"bgra", L"yuv444p10le"}), value(highOddArguments, L"-pix_fmt"));
    CHECK(value(highOddArguments, L"-vf").front().starts_with(L"scale=out_color_matrix=bt709:out_range=tv,"));

    // Lossless: FFV1, every frame intra, 10-bit, no rate control at all.
    EncoderSpec lossless{1920, 1080, 30.0, EncoderKind::Ffv1, EncoderPixelFormat::P010};
    lossless.quality = EncoderQuality::Lossless;
    const auto losslessArguments = BuildEncoderArguments(lossless, output);
    CHECK_EQ((std::vector<std::wstring>{L"ffv1"}), value(losslessArguments, L"-c:v"));
    CHECK_EQ((std::vector<std::wstring>{L"p010le", L"yuv420p10le"}), value(losslessArguments, L"-pix_fmt"));
    CHECK_EQ((std::vector<std::wstring>{L"1"}), value(losslessArguments, L"-g"));
    CHECK(!has(losslessArguments, L"-cq") && !has(losslessArguments, L"-crf") && !has(losslessArguments, L"-preset"));
    CHECK(!has(losslessArguments, L"-init_hw_device"));
    EncoderSpec losslessOdd{1919, 1079, 30.0, EncoderKind::Ffv1};
    losslessOdd.quality = EncoderQuality::Lossless;
    CHECK_EQ((std::vector<std::wstring>{L"bgra", L"yuv444p10le"}),
             value(BuildEncoderArguments(losslessOdd, output), L"-pix_fmt"));
    // There is nothing to fall back to from FFV1.
    CHECK(!ShouldRetryWithSoftware(EncoderKind::Ffv1, EncodeError::StartFailed));
    CHECK(!ShouldRetryWithSoftware(EncoderKind::Ffv1, EncodeError::WriteFailed));
    // P010 has no half-pixel chroma sample either.
    EncoderSpec p010Odd = high;
    p010Odd.width = 1919;
    CHECK_EQ(size_t{0}, ExpectedFrameBytes(p010Odd));
    CHECK_EQ(size_t{1920 * 1080 * 3}, ExpectedFrameBytes(high));

    // The names are the wire and ini spelling, and nothing else parses.
    for (const EncoderQuality quality : {EncoderQuality::Standard, EncoderQuality::High, EncoderQuality::Lossless}) {
        EncoderQuality parsed{};
        CHECK(ParseEncoderQuality(EncoderQualityName(quality), parsed));
        CHECK(parsed == quality);
    }
    EncoderQuality untouched = EncoderQuality::High;
    CHECK(!ParseEncoderQuality("Standard", untouched));
    CHECK(!ParseEncoderQuality("best", untouched));
    CHECK(!ParseEncoderQuality("", untouched));
    CHECK(untouched == EncoderQuality::High);
    CHECK(!EncoderQualityIsTenBit(EncoderQuality::Standard));
    CHECK(EncoderQualityIsTenBit(EncoderQuality::High) && EncoderQualityIsTenBit(EncoderQuality::Lossless));
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

// P1.16: the diagnostic was decoded strictly, so one byte that was not UTF-8
// - a Latin-1 tag, which ffmpeg prints raw - dropped the whole message.
void materialization_diagnostic_survives_output_that_is_not_utf8_test()
{
    TempDirectory fixture;
    std::filesystem::copy_file(CurrentExecutable(), fixture.Path() / L"ffmpeg.exe");
    const auto result = MediaMaterializer(fixture.Path()).Run({
        L"https://media.invalid/diagnostic-latin1?token=secret-value", {}, fixture.Path() / L"output.mkv"}, {});
    CHECK(!result.ok);
    CHECK_EQ(MaterializeError::ProcessFailed, result.error);
    CHECK(result.detail.find(L"Connection reset") != std::wstring::npos);
    CHECK(result.detail.find(L"Caf\uFFFD del Mar") != std::wstring::npos);
    CHECK(result.detail.find(L"secret-value") == std::wstring::npos);
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
    // The stop used to follow a 100 ms sleep, so on a slow start it could land
    // before the write had blocked and test the cancel-before-block path
    // instead. The fake child now says when the write is blocked: it signals
    // this event once its stdin holds more than the pipe's whole buffer, which
    // only a WriteFile still waiting for the reader can account for.
    const std::wstring blockedName=L"Local\\DlssMediaTestWriteBlocked-"+std::to_wstring(GetCurrentProcessId());
    HANDLE blocked=CreateEventW(nullptr,TRUE,FALSE,blockedName.c_str());
    REQUIRE(blocked!=nullptr);
    CHECK(SetEnvironmentVariableW(L"DLSS_MEDIA_TEST_WRITE_BLOCKED_EVENT",blockedName.c_str())!=FALSE);
    CHECK_EQ(EncodeError::None,encoder.Start(spec,fixture.Path()/L"hang-output.mkv"));
    SetEnvironmentVariableW(L"DLSS_MEDIA_TEST_WRITE_BLOCKED_EVENT",nullptr);
    std::vector<uint8_t> frame(ExpectedFrameBytes(spec));std::stop_source stop;
    auto write=std::async(std::launch::async,[&]{return encoder.WriteFrame(frame,stop.get_token());});
    CHECK_EQ(static_cast<DWORD>(WAIT_OBJECT_0),WaitForSingleObject(blocked,10000));
    CloseHandle(blocked);
    stop.request_stop();
    // Generous: the failure this guards is a write that never returns.
    CHECK_EQ(std::future_status::ready,write.wait_for(10s));
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
    bool Initialize(HWND,uint32_t width,uint32_t height,uint32_t outputWidth,uint32_t outputHeight,double,
                    const GuideControls& guides) override
    {
        initialized=true;inputWidth=width;inputHeight=height;
        // A capture is the output size, which is the input size for every job
        // but a reduced processing scale.
        expectedBytes=size_t(outputWidth)*outputHeight*4u;controls=guides;return true;
    }
    bool Submit(const OfflineDecodedFrame& frame,const FrameIdentity& id,bool capture,
                OfflineEvaluation& out) override
    {
        submitted.push_back(frame.timestamp100ns);resets.push_back(id.reset!=HistoryReset::None);
        submittedFrames.push_back(frame.bgra);
        ids.push_back(id);
        // Submissions since the last reset, including this one: what a temporal
        // model's output for this frame depends on.
        history=id.reset!=HistoryReset::None?1:history+1;
        if(capture)historyAtCapture.push_back(history);
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
        // A readback ring one slot behind: every capture from here on hands back
        // the frame captured before it.
        const FrameIdentity stamped=out.id;
        if(staleIdentityFromCapture&&captureSubmissions>=*staleIdentityFromCapture){
            out.id.frameNumber=previousCapture.frameNumber;out.id.pts100ns=previousCapture.pts100ns;
        }
        previousCapture=stamped;
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
    void ResetTemporal() override { ++temporalResets;history=0; }
    NeuralRenderFailure LastFailure() const override { return lastFailure; }
    double LastNeuralGpuMs() const override { return neuralGpuMs; }
    uint64_t PeakLocalVideoMemoryMiB() const override { return peakVramMiB; }
    bool initialized{};bool featureCreated{};uint64_t evaluations{};int primeSubmissions{};
    uint32_t inputWidth{},inputHeight{};std::vector<std::vector<uint8_t>> submittedFrames;
    int requiredPrimeSubmissions{2};
    bool stampCaptureCount{};
    int captureSubmissions{};int temporalResets{};size_t expectedBytes{};
    std::optional<int> failCaptureAt,failCaptureFrom,cutAtCapture,mismatchAtCapture,backendMissesCaptureAt;
    std::optional<int> staleIdentityFromCapture;FrameIdentity previousCapture{};
    uint64_t backendEvaluations{};
    NeuralRenderFailure captureFailure{NeuralRenderFailure::Neural};
    NeuralRenderFailure lastFailure{NeuralRenderFailure::None};
    // A plausible healthy 1080p median (receipts on this machine read 3.68 to
    // 3.72 ms); the render refuses a job whose median falls under the
    // per-geometry floor, so the default has to look like real neural work.
    double neuralGpuMs{3.7};uint64_t peakVramMiB{};uint32_t historyGeneration{};
    GuideControls controls;
    std::vector<int64_t> submitted,captured;std::vector<bool> resets;std::vector<FrameIdentity> ids;
    uint64_t history{};std::vector<uint64_t> historyAtCapture;
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

// The four lines the contract is proved from, transcribed from a real RenoDX
// 6.5.3 render rather than paraphrased: this suite is the only thing standing
// between a runtime bump and a job that publishes unrendered frames as
// verified, so the strings it matches have to be the strings the add-on emits.
//
// 4.70 proved the same two facts with "active settings: upscaling=OFF" and the
// "private feature-18 GPU ordering active" startup banner. 6.x prints neither -
// the upscaling key is gone and the architecture banners were dropped - so the
// proof moved to what the add-on reports building and evaluating.
constexpr const char* kHooksLine =
    "EnableHooks=2: NGX hooks only, Streamline modules left unpatched\n";
constexpr const char* kResourcesLine =
    "created inline NR resources ws1 640x360 -> 640x360 (native 1:1) format=10 "
    "hdr=v6/linear relative (workset 1/4, 11 MiB)\n";
constexpr const char* kCreateLine =
    "feature 18 created via the signed snippet after DLSS/DLAA for NR input "
    "640x360 -> output 640x360 with guides 640x360\n";

std::string NeuralEvidenceWithCount(uint64_t count)
{
    return std::string(kHooksLine) + kResourcesLine + kCreateLine +
           "inline feature 18 evaluation succeeded (count=" + std::to_string(count) +
           ", NR input 640x360 (guides 640x360), output 640x360, 1 stack pass(es) [native])\n";
}

std::string ValidNeuralEvidence() { return NeuralEvidenceWithCount(5); }

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
            ? std::string(std::string(kHooksLine)+kResourcesLine+kCreateLine+
                          "inline feature 18 evaluation succeeded [native] evaluation count=1\n")
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
            ? std::string(std::string(kHooksLine)+kResourcesLine+kCreateLine+
                          "inline feature 18 evaluation succeeded [native] evaluation count=1\n")
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

// The gate resubmits frame 0 until the add-on logs a receipt past the armed
// baseline. Those resubmits are not captures: each used to be a synchronous
// capture and readback, and the frame that was finally encoded had been fed
// into history as many times as the log took to flush.
void offline_receipt_gate_resubmits_without_capturing_and_captures_each_frame_once_test()
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
        // The backend's tally covers the captured frames only; the gate's
        // resubmits happened before it was sampled.
        CHECK_EQ(static_cast<uint64_t>(frames.size()), result.nativeEvaluations);
        CHECK(evaluator.EvaluationCount() >= 60);
        CHECK_EQ(uint64_t{60}, result.evidence.highestObservedEvaluation);
        CHECK_EQ(2, source.opens);
        CHECK_EQ(size_t{1}, encoder.attempts.size());
        if (!encoder.attempts.empty()) {
            CHECK_EQ(frames.size(), encoder.attempts.front().size());
            if (!encoder.attempts.front().empty()) CHECK_EQ(uint8_t{1}, encoder.attempts.front().front().front());
        }
        CHECK_EQ(static_cast<int>(frames.size()), evaluator.captureSubmissions);
        CHECK_EQ(frames.size(), evaluator.captured.size());
        for (size_t index = 0; index < std::min(frames.size(), evaluator.captured.size()); ++index)
            CHECK_EQ(frames[index].timestamp100ns, evaluator.captured[index]);
        // Frame 0 is captured on fresh history, as if the gate had not run.
        if (!evaluator.historyAtCapture.empty()) CHECK_EQ(uint64_t{1}, evaluator.historyAtCapture.front());
        if (photo) CHECK_EQ(int64_t{10000000}, result.duration100ns);
    }
}

// The same cache key has to produce the same bytes whenever the add-on's log
// happens to flush. How many resubmits the gate takes varies run to run, so
// the history every captured frame is evaluated on must not depend on it.
void offline_receipt_gate_output_does_not_depend_on_when_the_log_flushed_test()
{
    const auto run = [](uint64_t flushedAt, bool preroll) {
        TempDirectory fixture;
        FakeOfflineSource source(OfflineFramesAt25Fps(20));
        FakeNeuralEvaluator evaluator;
        FakeFrameEncoder encoder;
        // The line past the baseline appears once `flushedAt` evaluations ran.
        OfflineNeuralRenderer job(source, evaluator, encoder, [&evaluator, flushedAt] {
            return NeuralEvidenceWithCount(evaluator.EvaluationCount() >= flushedAt ? 60 : 1);
        });
        auto request = OfflineRequest(fixture.Path());
        request.width = 2; request.height = 2; request.fps = 25.0; request.durationSeconds = 20.0 / 25.0;
        if (preroll) { request.range = {4000000, 0}; request.prerollFrames = 5; }
        const auto result = job.Run(request);
        CHECK(result.ok);
        return std::make_pair(evaluator.historyAtCapture, source.opens);
    };
    for (const bool preroll : {false, true}) {
        // 2 opens: the log flushed during the preroll, so the gate never resubmitted.
        const auto [early, earlyOpens] = run(preroll ? 6 : 2, preroll);
        const auto [late, lateOpens] = run(40, preroll);
        const auto [later, laterOpens] = run(90, preroll);
        CHECK(!early.empty());
        CHECK(early == late);
        CHECK(early == later);
        if (preroll) {
            // A preroll is replayed rather than thrown away: frame 0 is still
            // evaluated on the five frames before the range.
            if (!early.empty()) CHECK_EQ(uint64_t{6}, early.front());
            CHECK_EQ(2, earlyOpens);
            CHECK_EQ(3, lateOpens);
            CHECK_EQ(3, laterOpens);
        } else {
            if (!early.empty()) CHECK_EQ(uint64_t{1}, early.front());
            CHECK_EQ(2, lateOpens);
        }
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

// The add-on logs its evaluation counter at N=1 and N=60 in a process and
// then goes quiet, so a software retry can never watch it advance. The first
// pass's gate already proved the process; the retry is held to the backend's
// own per-frame count and the timing floor, as a reused evaluator is, and
// captures its first frame once instead of waiting 120 resubmits for a line.
std::function<std::string()> LogCounterThatStopsAt60(const FakeNeuralEvaluator& evaluator)
{
    return [&evaluator] {
        return NeuralEvidenceWithCount(evaluator.EvaluationCount() >= 60 ? 60 : 1);
    };
}

void offline_software_retry_after_the_log_counter_went_quiet_succeeds_test()
{
    // A photo, where the whole first pass is the receipt gate.
    {
        TempDirectory fixture;
        FakeOfflineSource source({OfflineDecodedFrame{{12, 34, 56, 255}, 0, true}});
        FakeNeuralEvaluator evaluator;
        evaluator.stampCaptureCount = true;
        FakeFrameEncoder encoder;
        encoder.failNvencWriteAt = 0;
        OfflineNeuralRenderer job(source, evaluator, encoder, LogCounterThatStopsAt60(evaluator));
        auto request = EvenOfflineRequest(fixture.Path());
        request.fps = 1; request.durationSeconds = 1;
        const auto result = job.Run(request);
        CHECK(result.ok);
        CHECK_EQ(EncoderKind::H264Software, result.encoder);
        CHECK_EQ(uint64_t{1}, result.frameCount);
        CHECK_EQ(uint64_t{1}, result.nativeEvaluations);
        CHECK_EQ(uint64_t{60}, result.evidence.highestObservedEvaluation);
        CHECK_EQ(3, source.opens);
        CHECK_EQ(size_t{2}, encoder.attempts.size());
        if (encoder.attempts.size() == 2) {
            CHECK(encoder.attempts.front().empty());
            CHECK_EQ(size_t{1}, encoder.attempts.back().size());
        }
        // One capture per pass: the first pass's 60 gate resubmits are not
        // captures, and the retry has no gate.
        CHECK_EQ(2, evaluator.captureSubmissions);
    }
    // A clip whose NVENC encoder fails after N frames were written.
    {
        TempDirectory fixture;
        FakeOfflineSource source;
        FakeNeuralEvaluator evaluator;
        FakeFrameEncoder encoder;
        encoder.failNvencWriteAt = 3;
        OfflineNeuralRenderer job(source, evaluator, encoder, LogCounterThatStopsAt60(evaluator));
        const auto result = job.Run(EvenOfflineRequest(fixture.Path()));
        CHECK(result.ok);
        CHECK_EQ(EncoderKind::H264Software, result.encoder);
        CHECK_EQ(uint64_t{5}, result.frameCount);
        CHECK_EQ(uint64_t{5}, result.nativeEvaluations);
        CHECK_EQ(uint64_t{60}, result.evidence.highestObservedEvaluation);
        CHECK_EQ(std::vector<EncoderKind>({EncoderKind::HevcNvenc, EncoderKind::H264Software}),
                 encoder.starts);
        if (encoder.attempts.size() == 2) {
            CHECK_EQ(size_t{3}, encoder.attempts.front().size());
            CHECK_EQ(size_t{5}, encoder.attempts.back().size());
        }
    }
    // The retry is still held to the timing floor: a pass whose neural GPU
    // time says DLAA alone ran is refused, receipt or no receipt.
    {
        TempDirectory fixture;
        FakeOfflineSource source;
        FakeNeuralEvaluator evaluator;
        evaluator.neuralGpuMs = 0.0;
        FakeFrameEncoder encoder;
        encoder.failNvencWriteAt = 3;
        OfflineNeuralRenderer job(source, evaluator, encoder, LogCounterThatStopsAt60(evaluator));
        const auto result = job.Run(EvenOfflineRequest(fixture.Path()));
        CHECK(!result.ok);
        CHECK(!result.cancelled);
        CHECK_EQ(NeuralRenderFailure::Neural, result.failure);
        CHECK_EQ(std::vector<EncoderKind>({EncoderKind::HevcNvenc, EncoderKind::H264Software}),
                 encoder.starts);
    }
    // And to the backend's count: a retry frame the backend never evaluated
    // is refused exactly as it is on a first pass.
    {
        TempDirectory fixture;
        FakeOfflineSource source;
        FakeNeuralEvaluator evaluator;
        FakeFrameEncoder encoder;
        encoder.failNvencWriteAt = 3;
        OfflineNeuralRenderer job(source, evaluator, encoder, LogCounterThatStopsAt60(evaluator));
        // First pass: frames 0-4 captured (the gate's resubmits are not
        // captures), so 7 is the retry's second frame.
        evaluator.backendMissesCaptureAt = 7;
        const auto result = job.Run(EvenOfflineRequest(fixture.Path()));
        CHECK(!result.ok);
        CHECK_EQ(NeuralRenderFailure::Neural, result.failure);
        CHECK_EQ(size_t{2}, encoder.starts.size());
    }
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
            if (evaluator.EvaluationCount() >= 10) {
                if (cancel) stop.request_stop();
                else return NeuralEvidenceWithCount(60) + "inline feature 18 evaluation failed\n";
            }
            return NeuralEvidenceWithCount(1);
        });
        const auto result = job.Run(OfflineRequest(fixture.Path()), {}, stop.get_token());
        CHECK(!result.ok);
        CHECK_EQ(cancel, result.cancelled);
        // Stopped inside the gate: nothing was captured, let alone encoded.
        CHECK_EQ(0, evaluator.captureSubmissions);
        CHECK_EQ(uint64_t{10}, evaluator.EvaluationCount());
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
        // The fake counts every uncaptured submit as priming, and the receipt
        // gate took one uncaptured resubmit of the photo before it opened,
        // then reset the guides so the capture starts from nothing.
        CHECK_EQ(warmupFrames + 1, evaluator.primeSubmissions);
        CHECK_EQ(2, source.opens);
        CHECK_EQ(2, evaluator.temporalResets);
        CHECK_EQ(std::vector<int64_t>{0}, evaluator.captured);
        CHECK_EQ(size_t{1}, encoder.attempts.size());
        if (!encoder.attempts.empty()) {
            CHECK_EQ(size_t{1}, encoder.attempts.front().size());
            if (!encoder.attempts.front().empty()) CHECK_EQ(photo, encoder.attempts.front().front());
        }
        CHECK_EQ(1, encoder.finishes);
        // A replayed still is continuous during warm-up; capture starts fresh.
        CHECK_EQ(static_cast<size_t>(warmupFrames + 2), evaluator.resets.size());
        if (evaluator.resets.size() == static_cast<size_t>(warmupFrames + 2)) {
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
    // 120 uncaptured resubmits of frame 0, and not one capture.
    CHECK_EQ(0,evaluator.captureSubmissions);CHECK_EQ(2+120,evaluator.primeSubmissions);
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
        // Evaluated and created, but no hook-mode line and no inline
        // resources: the pass could have come through Streamline or at a
        // scaled working resolution, and neither is what was asked for.
        if(calls==1)return std::string(kCreateLine)+
            "inline feature 18 evaluation succeeded (count=5) [native]\n";
        return ValidNeuralEvidence();
    });
    const auto result=job.Run(OfflineRequest(fixture.Path()),{},{});
    CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(1,calls);
    CHECK_EQ(size_t{0},encoder.starts.size());
}

// Super Resolution alone runs with the neural add-on disabled, so the session
// log never names feature 18. The job primes until the NGX carrier exists -
// the renderer creates it on the second present whatever is watching - then
// captures without waiting for any feature-18 evidence, and reports that
// evidence as absent rather than leaving it for a reader to guess.
void offline_super_resolution_only_job_waits_for_no_neural_evidence_and_says_so_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    // Upscaler speed: far under the neural floor, which this job is not held to.
    evaluator.neuralGpuMs=0.2;
    int evidenceCalls=0;
    OfflineNeuralRenderer job(source,evaluator,encoder,[&]{++evidenceCalls;return std::string{};});
    auto request=EvenOfflineRequest(fixture.Path());request.requireNeural=false;
    const NeuralRenderResult result=job.Run(request,{},{});
    CHECK(result.ok);CHECK_EQ(NeuralRenderFailure::None,result.failure);
    CHECK_EQ(uint64_t{5},result.frameCount);CHECK_EQ(uint64_t{5},result.nativeEvaluations);
    CHECK(!result.neural);CHECK_EQ(uint64_t{0},result.verifiedNeuralFrames);
    CHECK(!result.feature18ArmedBeforeCapture);CHECK(!result.evidence.Valid());
    // Primed exactly to the carrier's creation; no re-hook, no receipt gate.
    CHECK_EQ(2,evaluator.primeSubmissions);CHECK_EQ(size_t{5},evaluator.captured.size());
    CHECK_EQ(size_t{1},encoder.attempts.size());CHECK_EQ(size_t{5},encoder.attempts.back().size());
    // One read, after capture, to prove the add-on stayed out of it.
    CHECK_EQ(1,evidenceCalls);
}

// Below 100% the model is shown the frame reduced to ProcessingSize and the
// carrier restores the source size: the evaluator is created for the reduced
// input and the source size as its output, every frame it is handed is the
// area average of the decoded one, and what is encoded is the output size.
void offline_reduced_processing_scale_shows_the_model_the_area_reduced_frame_test()
{
    // Four 4x4 frames whose 2x2 blocks each average to a known value.
    std::vector<OfflineDecodedFrame> frames;
    for(size_t index=0;index<4;++index){
        std::vector<uint8_t> bgra(4*4*4);
        for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<4;++x){
            uint8_t* pixel=bgra.data()+(size_t(y)*4+x)*4;
            // Top-left block {0,40,80,120} -> 60; the others a flat 10*index.
            pixel[0]=(x<2&&y<2)?uint8_t((y*2+x)*40):uint8_t(10*index);
            pixel[1]=uint8_t(200);pixel[2]=uint8_t(x*20+y*20);pixel[3]=255;
        }
        frames.push_back(OfflineDecodedFrame{std::move(bgra),int64_t(index)*333333,index==0,index,1});
    }
    TempDirectory fixture;FakeOfflineSource source(frames);FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    auto request=OfflineRequest(fixture.Path());
    request.width=4;request.height=4;request.durationSeconds=4.0/30.0;request.processingScale=50;
    const NeuralRenderResult result=job.Run(request,{},{});
    CHECK(result.ok);CHECK_EQ(uint64_t{4},result.frameCount);
    CHECK_EQ(uint32_t{2},evaluator.inputWidth);CHECK_EQ(uint32_t{2},evaluator.inputHeight);
    CHECK(!evaluator.submittedFrames.empty());
    for(const auto& submitted:evaluator.submittedFrames)CHECK_EQ(size_t{2*2*4},submitted.size());
    if(!evaluator.submittedFrames.empty()){
        const auto& first=evaluator.submittedFrames.front();
        CHECK_EQ(uint8_t{60},first[0]);                 // mean of 0, 40, 80, 120
        CHECK_EQ(uint8_t{200},first[1]);CHECK_EQ(uint8_t{255},first[3]);
        CHECK_EQ(uint8_t{20},first[2]);                  // mean of 0, 20, 20, 40
    }
    // Encoded at the source size: the model's input is not the file's size.
    CHECK(!encoder.attempts.empty());
    if(!encoder.attempts.empty())
        for(const auto& written:encoder.attempts.back())CHECK_EQ(size_t{4*4*4},written.size());

    // A reduced scale beside an upscaling output is two jobs for one carrier.
    FakeOfflineSource upscaleSource(frames);FakeNeuralEvaluator upscaleEvaluator;FakeFrameEncoder upscaleEncoder;
    OfflineNeuralRenderer refused(upscaleSource,upscaleEvaluator,upscaleEncoder,AdvancingNeuralEvidence());
    auto both=request;both.outputWidth=8;both.outputHeight=8;
    const NeuralRenderResult bothResult=refused.Run(both,{},{});
    CHECK(!bothResult.ok);CHECK_EQ(NeuralRenderFailure::Source,bothResult.failure);
    CHECK(!upscaleEvaluator.initialized);
}

// The claim a Super Resolution-only job makes is that the model did NOT run.
// A session log that shows feature 18 evaluating means the add-on loaded
// anyway, which is how the old upscale-only export came out byte-identical
// to the neural one; that render is refused, not published under the label.
void offline_super_resolution_only_job_refuses_a_session_where_the_add_on_ran_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
    auto request=EvenOfflineRequest(fixture.Path());request.requireNeural=false;
    const NeuralRenderResult result=job.Run(request,{},{});
    CHECK(!result.ok);CHECK_EQ(NeuralRenderFailure::Neural,result.failure);
    CHECK(!result.neural);CHECK_EQ(uint64_t{0},result.verifiedNeuralFrames);

    // And a carrier that skipped a captured frame is not an upscale either.
    FakeOfflineSource secondSource;FakeNeuralEvaluator skipping;FakeFrameEncoder secondEncoder;
    skipping.backendMissesCaptureAt=3;
    OfflineNeuralRenderer skipped(secondSource,skipping,secondEncoder,[]{return std::string{};});
    auto secondRequest=EvenOfflineRequest(fixture.Path());secondRequest.requireNeural=false;
    const NeuralRenderResult refused=skipped.Run(secondRequest,{},{});
    CHECK(!refused.ok);CHECK_EQ(NeuralRenderFailure::Neural,refused.failure);
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

// A source that fails its Nth open, or opens and has nothing to read from its
// Nth open on: the two ways the job's reopen at the preroll can go wrong.
class ScriptedOpenSource final : public IFrameSource {
public:
    bool Open(const std::filesystem::path& path,std::stop_token stop,double seekSeconds) override
    {
        ++opens;
        if(failOpen&&opens==*failOpen)return false;
        if(emptyFromOpen&&opens>=*emptyFromOpen)empty_=true;
        return inner.Open(path,stop,seekSeconds);
    }
    void Close() override { ++closes;inner.Close(); }
    OfflineFrameRead Read(OfflineDecodedFrame& frame,std::stop_token stop) override
    {
        if(empty_)return OfflineFrameRead::EndOfStream;
        return inner.Read(frame,stop);
    }
    FakeOfflineSource inner;
    std::optional<int> failOpen,emptyFromOpen;
    int opens{},closes{};
private:
    bool empty_{};
};

// P3.4: RunJob is NeuralRenderJob's named steps now - check the request,
// bring the source and evaluator up, prime and arm, restart at the preroll,
// capture, judge. Each step that can end the job ends it with its own
// failure, closes the source it opened, and reaches nothing after it.
void offline_job_steps_end_where_they_fail_and_close_the_source_test()
{
    // The first open: nothing is initialized, nothing is encoded.
    {
        TempDirectory fixture;ScriptedOpenSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        source.failOpen=1;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
        CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(NeuralRenderFailure::Source,result.failure);
        CHECK(result.detail==L"The source video could not be opened.");
        CHECK(!evaluator.initialized);CHECK(encoder.starts.empty());
    }
    // The reopen at the capture start, after priming and arming.
    {
        TempDirectory fixture;ScriptedOpenSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        source.failOpen=2;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
        CHECK(!result.ok);CHECK_EQ(NeuralRenderFailure::Source,result.failure);
        CHECK(result.detail==L"The source could not be restarted at the capture start.");
        CHECK(evaluator.featureCreated);CHECK(result.feature18ArmedBeforeCapture);
        CHECK(encoder.starts.empty());CHECK(evaluator.captured.empty());
    }
    // A source that has nothing at the capture start is the source's failure.
    // Falling through to the encoder's finish used to report the error that
    // cancelling the encoder produces instead.
    {
        TempDirectory fixture;ScriptedOpenSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        source.emptyFromOpen=2;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
        CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(NeuralRenderFailure::Source,result.failure);
        CHECK(result.detail==L"The source decoder failed during neural rendering.");
        CHECK_EQ(std::vector<EncoderKind>({EncoderKind::HevcNvenc}),encoder.starts);
        CHECK_EQ(0,encoder.finishes);CHECK(encoder.cancels>=1);
        CHECK(evaluator.captured.empty());CHECK(source.closes>=2);
    }
    // The software retry is held to fresh evidence: a runtime whose log went
    // bad between the passes is refused before the second pass starts.
    {
        TempDirectory fixture;ScriptedOpenSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        encoder.failNvencWriteAt=2;int calls=0;
        OfflineNeuralRenderer job(source,evaluator,encoder,[&]{
            ++calls;return calls<=2?NeuralEvidenceWithCount(uint64_t(calls)*4u):std::string("the log went away\n");
        });
        const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
        CHECK(!result.ok);CHECK_EQ(NeuralRenderFailure::Neural,result.failure);
        CHECK(result.detail==L"Feature 18 evidence was not valid before the software retry.");
        CHECK_EQ(std::vector<EncoderKind>({EncoderKind::HevcNvenc}),encoder.starts);
        CHECK_EQ(3,source.opens);
    }
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

// This used to be refused: the log counter advanced only during the abandoned
// NVENC pass. That is the real add-on's behaviour after N=60, so the retry is
// held to the backend count and the timing floor instead (P0.5).
void offline_job_accepts_retry_when_only_abandoned_attempt_advanced_feature18_receipt_test()
{
    TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
    encoder.failNvencWriteAt=2;int calls=0;
    OfflineNeuralRenderer job(source,evaluator,encoder,[&]{
        ++calls;return NeuralEvidenceWithCount(calls==1?1:5);
    });
    const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
    CHECK(result.ok);CHECK(!result.cancelled);
    CHECK_EQ(std::vector<EncoderKind>({EncoderKind::HevcNvenc,EncoderKind::H264Software}),encoder.starts);
    CHECK_EQ(size_t{5},encoder.attempts.back().size());
    CHECK_EQ(uint64_t{5},result.nativeEvaluations);
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
    // Frame 1 is pipelined, so which frame its capture holds is only known when
    // its readback resolves: after frame 2 has been queued behind it.
    CHECK_EQ(3,evaluator.captureSubmissions);
}

// The identity a capture resolves with is compared with the frame queued in
// that position, on the synchronous first frame and on every pipelined drain.
// It used to be an echo of the request stamped before rendering, so a ring
// that slid out of step published shuffled frames as verified.
void offline_capture_identity_is_checked_where_the_readback_resolves_test()
{
    // The synchronous first frame.
    {
        TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        evaluator.mismatchAtCapture=1;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
        CHECK(!result.ok);CHECK_EQ(NeuralRenderFailure::Identity,result.failure);
        CHECK_EQ(1,evaluator.captureSubmissions);
        if(!encoder.attempts.empty())CHECK(encoder.attempts.front().empty());
    }
    // A readback ring one slot behind from frame 2: frames 0 and 1 are the
    // frames they claim, frame 2's slot hands back frame 1, and nothing from
    // the shifted ring reaches the encoder.
    {
        TempDirectory fixture;FakeOfflineSource source;FakeNeuralEvaluator evaluator;FakeFrameEncoder encoder;
        evaluator.stampCaptureCount=true;evaluator.staleIdentityFromCapture=3;
        OfflineNeuralRenderer job(source,evaluator,encoder,AdvancingNeuralEvidence());
        const auto result=job.Run(EvenOfflineRequest(fixture.Path()),{},{});
        CHECK(!result.ok);CHECK(!result.cancelled);CHECK_EQ(NeuralRenderFailure::Identity,result.failure);
        CHECK_EQ(0,encoder.finishes);
        CHECK_EQ(size_t{1},encoder.attempts.size());
        if(!encoder.attempts.empty()){
            CHECK_EQ(size_t{2},encoder.attempts.front().size());
            if(encoder.attempts.front().size()==2){
                CHECK_EQ(uint8_t{1},encoder.attempts.front()[0].front());
                CHECK_EQ(uint8_t{2},encoder.attempts.front()[1].front());
            }
        }
    }
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
// neural.partial-00000.mkv and so on; a software retry's are
// neural.partial-r1-00000.mkv.
std::filesystem::path OfflineSegmentPath(const std::filesystem::path& directory, uint64_t index,
                                         uint32_t attempt = 0)
{
    std::wstring digits = std::to_wstring(index);
    if (digits.size() < 5) digits.insert(0, 5 - digits.size(), L'0');
    const std::wstring retry = attempt ? L"r" + std::to_wstring(attempt) + L"-" : L"";
    return directory / (L"neural.partial-" + retry + digits + L".mkv");
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
        // The retry never reuses a name the failed attempt published: the
        // player may still be decoding that file, and a delete it blocks would
        // have left the retry's encoder truncating it.
        CHECK(segment.fileName==OfflineSegmentPath(fixture.Path(),index,1).filename().wstring());
    }
    // Renumbering from zero only means anything if nothing of the abandoned
    // attempt is left: no NVENC-written file, no armed file, no live encoder.
    CHECK_EQ(size_t{0},farm.Live());
    for(uint64_t index=0;index<3;++index){
        CHECK_EQ(std::string(index==2?"s":"ss"),
                 ReadBytes(OfflineSegmentPath(fixture.Path(),index,1)));
        CHECK(!std::filesystem::exists(OfflineSegmentPath(fixture.Path(),index)));
    }
    CHECK(!std::filesystem::exists(OfflineSegmentPath(fixture.Path(),3,1)));
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

// Every proof the contract needs, and what happens when each one is missing.
//
// Dropping one line at a time rather than asserting one hand-built negative:
// the failure this guards against is a runtime bump silently satisfying fewer
// conditions than before, and that shows up as a line going missing.
void reshade_evidence_requires_native_resolution_inline_path_create_and_evaluate_test()
{
    const auto valid=ParseNeuralRuntimeEvidence(ValidNeuralEvidence());
    CHECK(valid.Valid());CHECK(valid.nativeResolution);CHECK(valid.feature18Created);
    CHECK(valid.inlineInterceptionContract);
    CHECK(valid.feature18Evaluated);CHECK_EQ(uint64_t{5},valid.highestObservedEvaluation);

    const std::string evaluate=
        "inline feature 18 evaluation succeeded (count=17, NR input 2560x1440 "
        "(guides 2560x1440), output 2560x1440, 1 stack pass(es) [native])\n";
    const auto productionLog=ParseNeuralRuntimeEvidence(
        std::string(kHooksLine)+kResourcesLine+kCreateLine+evaluate);
    CHECK(productionLog.Valid());CHECK_EQ(uint64_t{17},productionLog.highestObservedEvaluation);

    // No hook-mode line: the pass may have gone through Streamline.
    CHECK(!ParseNeuralRuntimeEvidence(
        std::string(kResourcesLine)+kCreateLine+evaluate).Valid());
    // No inline resources: nothing says this feature's surfaces were ours.
    CHECK(!ParseNeuralRuntimeEvidence(
        std::string(kHooksLine)+kCreateLine+evaluate).Valid());
    // Created but never evaluated - the frame was not denoised.
    CHECK(!ParseNeuralRuntimeEvidence(
        std::string(kHooksLine)+kResourcesLine+kCreateLine).Valid());
    // Evaluated at a scaled working resolution rather than 1:1.
    CHECK(!ParseNeuralRuntimeEvidence(
        std::string(kHooksLine)+kResourcesLine+kCreateLine+
        "inline feature 18 evaluation succeeded (count=17, NR input 1280x720 "
        "(guides 1280x720), output 2560x1440, 1 stack pass(es))\n").Valid());
    // The 6.x pre-SR evaluate is the same frame by another path, so it counts.
    CHECK(ParseNeuralRuntimeEvidence(
        std::string(kHooksLine)+kResourcesLine+kCreateLine+
        "pre-SR feature 18 evaluation succeeded (count=17, NR input 2560x1440 "
        "(guides 2560x1440), output 2560x1440, 1 stack pass(es) [native])\n").Valid());
    // A startup retry notice is not a failure: 6.5.3 logs both of these on a
    // healthy run and then verifies every frame.
    CHECK(ParseNeuralRuntimeEvidence(
        ValidNeuralEvidence()+
        "NR skipped (after-upscale): compute-state restore target incomplete "
        "(hooks=1 heaps=1 root_signature=0 root_arguments=0 pso=0)\n"
        "compute-state restore target complete (after-upscale: hooks=1 heaps=1 "
        "root_signature=1 root_arguments=1 pso=1); injection admitted after 2 "
        "incomplete-target decline(s)\n").Valid());
    // A terminal skip still is one.
    CHECK(!ParseNeuralRuntimeEvidence(
        ValidNeuralEvidence()+
        "NR skipped: unsupported D3D12 command-list type 2\n").Valid());
    // 6.x-only failures the 4.70 vocabulary had no word for.
    for(const char* failure:{"feature 18 create raised an exception\n",
                             "pre-SR NR declined: color geometry is unsupported (dim=2)\n",
                             "NR workset request rejected (device=0x1)\n",
                             "workset/codec setup failed\n"})
        CHECK(!ParseNeuralRuntimeEvidence(ValidNeuralEvidence()+failure).Valid());
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
    bool OpenKnown(const std::filesystem::path& path,const VideoDecoder::KnownMedia&,std::stop_token stop) override
    { ++knownOpens;return Open(path,stop); }
    void Close() override { ++closes; }
    VideoReadResult Read(VideoFrame& frame,std::stop_token stop) override
    {
        if(stop.stop_requested())return VideoReadResult::Cancelled;
        if(notReadyReads>0){--notReadyReads;++notReadyServed;return VideoReadResult::NotReady;}
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
    double fps{30.0},duration{};bool failOpen{},failNextSeek{};int opens{},knownOpens{},closes{},seeks{},notReadyReads{};
    // Read from another thread by a test waiting for a reader to be inside its
    // not-ready loop, so it is the one atomic here.
    std::atomic<int> notReadyServed{};
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
    // Stop only once the seek is waiting on a decoder that is not ready: a
    // fixed 25 ms sleep could fire before the seek started, which tests a stop
    // that was already there rather than one that interrupts the wait.
    for(const auto deadline=std::chrono::steady_clock::now()+10s;
        original.notReadyServed.load()==0&&std::chrono::steady_clock::now()<deadline;)
        std::this_thread::sleep_for(1ms);
    REQUIRE(original.notReadyServed.load()>0);
    stop.request_stop();
    CHECK_EQ(std::future_status::ready,seek.wait_for(10s));CHECK(!seek.get());
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

// Cached playback already probed the original through its own decoder; the
// pair used to probe it a second time. Handed that probe, the original opens
// known and only the neural file - whose duration proves it whole - is probed.
void synchronized_playback_opens_a_described_original_without_a_probe_test()
{
    FakeSynchronizedSource original({0,333333,666666});FakeSynchronizedSource neural({0,333333,666666});
    SynchronizedPlayback playback(original,neural);
    VideoDecoder::KnownMedia media{};media.width=1;media.height=1;media.fps=30.0;media.durationSec=original.duration;
    CHECK(playback.Open(L"o",L"n",{},{},false,media));
    CHECK_EQ(1,original.knownOpens);CHECK_EQ(0,neural.knownOpens);
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    // Without one it probes, as it always has.
    CHECK(playback.Open(L"o",L"n",{}));
    CHECK_EQ(1,original.knownOpens);CHECK_EQ(0,neural.knownOpens);
}

// Behaves the way VideoDecoder does with the buffer a frame arrives holding:
// one of the right size is written into, anything else means a new frame
// buffer, allocated and zero-filled. Every frame is stamped with its index.
class BufferCountingSource final : public ISynchronizedFrameSource {
public:
    static constexpr size_t kBytes=64;
    bool Open(const std::filesystem::path&,std::stop_token)override{index=0;return true;}
    void Close()override{}
    VideoReadResult Read(VideoFrame& frame,std::stop_token)override
    {
        if(index>=kFrames)return VideoReadResult::EndOfStream;
        if(frame.bgra.size()==kBytes)++reused;
        else{frame.bgra.assign(kBytes,0);++fills;}
        std::fill(frame.bgra.begin(),frame.bgra.end(),uint8_t(index&0xFFu));
        frame.timestamp100ns=int64_t(index)*333333;frame.frameNumber=index;frame.discontinuity=index==0;
        ++index;return VideoReadResult::FrameReady;
    }
    bool SeekSeconds(double)override{return true;}
    uint32_t Width()const override{return 4;}
    uint32_t Height()const override{return 4;}
    double FrameRate()const override{return 30.0;}
    double DurationSeconds()const override{return double(kFrames)/30.0;}
    static constexpr uint64_t kFrames=400;
    uint64_t index{},fills{},reused{};
};

// Every pair used to be read into a fresh VideoFrame, so a decoder never got a
// buffer back and each member of each pair cost a whole-frame allocation. A
// pair's buffers now go back to the sources when the last holder lets go -
// and never while anyone still holds the pair.
void synchronized_playback_returns_released_pair_buffers_to_its_sources_test()
{
    BufferCountingSource original,neural;
    {
        SynchronizedPlayback playback(original,neural);
        CHECK(playback.Open(L"o",L"n",{}));
        constexpr int kPairs=200;
        for(int index=0;index<kPairs;++index)
            CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        std::cout<<"  synchronized pairs: "<<kPairs<<" pairs, buffer fills original="<<original.fills
                 <<" neural="<<neural.fills<<"\n";
        // The pair being published and the one it replaces.
        CHECK(original.fills<=2);CHECK(neural.fills<=2);
        CHECK(original.reused>=uint64_t(kPairs-2));CHECK(neural.reused>=uint64_t(kPairs-2));

        // A retained pair is never recycled from under its holder.
        const auto held=playback.CurrentPairShared();
        CHECK(held!=nullptr);
        const uint8_t stamp=held?held->original.bgra.front():0;
        for(int index=0;index<20;++index)
            CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        CHECK(held&&held->original.bgra.size()==BufferCountingSource::kBytes);
        CHECK(held&&std::all_of(held->original.bgra.begin(),held->original.bgra.end(),[&](uint8_t b){return b==stamp;}));
        CHECK(held&&std::all_of(held->neural.bgra.begin(),held->neural.bgra.end(),[&](uint8_t b){return b==stamp;}));
    }
    // Outliving the playback is allowed: the pool a late release feeds is
    // shared, not owned by the playback that was destroyed above.
    std::shared_ptr<const SynchronizedFramePair> survivor;
    {
        SynchronizedPlayback playback(original,neural);
        CHECK(playback.Open(L"o",L"n",{}));
        CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        survivor=playback.CurrentPairShared();
    }
    CHECK(survivor!=nullptr);
    survivor.reset();
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
        // The duration the last known open declared, and where the last seek
        // landed: a known open clamps its seeks to what it was told, exactly
        // as VideoDecoder::SeekSeconds does.
        double knownDuration{};
        size_t landedIndex{};
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
    double KnownDuration(const std::filesystem::path& path){std::lock_guard lock(mutex);return streams[path].knownDuration;}
    size_t LandedIndex(const std::filesystem::path& path){std::lock_guard lock(mutex);return streams[path].landedIndex;}
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
        if(!OpenRecording(path,stop,true))return false;
        knownDuration_=media.durationSec;
        std::lock_guard lock(library_.mutex);
        stream_->knownDuration=media.durationSec;
        return true;
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
        if(knownDuration_>0.0)seconds=std::min(seconds,knownDuration_);
        const int64_t target=static_cast<int64_t>(seconds*10000000.0);
        index_=0;
        while(index_<stream_->frames.size()&&stream_->frames[index_].timestamp100ns<target)++index_;
        stream_->landedIndex=index_;
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
        stream_=nullptr;knownDuration_=0.0;
        const auto found=library_.streams.find(path);
        if(found==library_.streams.end()||found->second.failOpen||stop.stop_requested())return false;
        stream_=&found->second;++stream_->opens;index_=0;
        stream_->known=known;stream_->thread=std::this_thread::get_id();
        return true;
    }
    LiveFrameLibrary& library_;
    LiveFrameLibrary::Stream* stream_{};
    size_t index_{};
    double knownDuration_{};
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

// The same seam, with the next file published only after the current one was
// opened - the order a live render produces. The index closes the seam in its
// own record, but the copy playback holds kept the old end, so the next file
// looked like it began after a hole: no prefetch, and the boundary opened it
// on the presenting thread. With the render loading the GPU that open took
// ~250 ms, and a live 1080p30 session dropped 10-14 frames at that boundary.
void live_playback_prefetches_across_a_seam_published_after_the_open_test()
{
    LiveFrameLibrary library;library.Add(L"original.mkv",20);
    library.Add(L"neural-00000.mkv",5);library.Add(L"neural-00001.mkv",5);
    LiveLibrarySource original(library);
    SynchronizedPlayback playback(original,LiveSegmentFactory(library));
    const auto segments=std::make_shared<NeuralSegmentIndex>();
    segments->Append(RoundedSegmentRecord(L"neural-00000.mkv",0,0,5));
    CHECK(playback.OpenLive(L"original.mkv",segments,SynchronizedRange{},{}));
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    segments->Append(RoundedSegmentRecord(L"neural-00001.mkv",1,5,5));
    for(uint64_t expected=1;expected<10;++expected){
        CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        const auto* pair=playback.CurrentPair();CHECK(pair!=nullptr);
        if(!pair)return;
        CHECK_EQ(expected,pair->frameNumber);
        if(expected==1)CHECK(library.WaitForOpen(L"neural-00001.mkv",1));
    }
    CHECK_EQ(1,library.Opens(L"neural-00001.mkv"));
    CHECK(library.OpenThread(L"neural-00001.mkv")!=std::this_thread::get_id());
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

// A session's first segment is deliberately short (kLiveFirstSegmentSeconds)
// and the ones behind it are not, yet every later file used to be opened with
// the first one's probed duration. A known open clamps its seeks to that, so a
// seek deep into a later segment landed at the first segment's length - frame
// 2 of 10 here - and the pair was only rebuilt by decoding up to the playhead.
void live_seek_into_a_later_segment_lands_on_the_frame_not_the_first_segments_length_test()
{
    LiveFrameLibrary library;library.Add(L"original.mkv",40);
    library.Add(L"neural-00000.mkv",2);library.Add(L"neural-00001.mkv",10);
    LiveLibrarySource original(library);
    SynchronizedPlayback playback(original,LiveSegmentFactory(library));
    const auto segments=std::make_shared<NeuralSegmentIndex>();
    segments->Append(LiveSegmentRecord(L"neural-00000.mkv",0,10,2));
    segments->Append(LiveSegmentRecord(L"neural-00001.mkv",1,12,10));
    CHECK(playback.OpenLive(L"original.mkv",segments,SynchronizedRange{10*kLiveFrame100ns,0},{}));
    // The first segment is the one that pays for the probe.
    CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
    CHECK(!library.OpenedKnown(L"neural-00000.mkv"));
    CHECK(playback.SeekSeconds(double(19*kLiveFrame100ns)*1e-7,{}));
    if(const auto* pair=playback.CurrentPair()){
        CHECK_EQ(uint64_t{19},pair->frameNumber);
        CHECK_EQ(uint64_t{19},pair->neural.frameNumber);
    }
    CHECK(library.OpenedKnown(L"neural-00001.mkv"));
    // Its own ten frames, not the first file's two.
    const double declared=library.KnownDuration(L"neural-00001.mkv");
    CHECK(declared>10.0/30.0-1e-9&&declared<10.0/30.0+1e-9);
    CHECK_EQ(size_t{7},library.LandedIndex(L"neural-00001.mkv"));
    CHECK(playback.LastFault().empty());
}

// A finished run is joined into its cache entry, and the entry then serves the
// run's frames from one file (P1.14). Coverage must not move by a tick, and a
// run whose files do not continue each other is left alone: joining those
// would not be the same frames at the same timestamps.
void neural_segment_index_serves_a_published_run_from_its_joined_entry_test()
{
    NeuralSegmentIndex index;
    index.Append(LiveSegmentRecord(L"run1/neural-00000.mkv",0,20,5,1));
    index.Append(LiveSegmentRecord(L"run1/neural-00001.mkv",1,25,5,1));
    index.Append(RoundedSegmentRecord(L"run2/neural-00000.mkv",0,0,5,2));
    index.Append(RoundedSegmentRecord(L"run2/neural-00001.mkv",1,5,5,2));
    const auto coverage=index.CoveredRanges();
    const uint64_t frames=index.TotalFrames(),revision=index.Revision();
    const auto lastOfRun2=index.At(1);
    CHECK(lastOfRun2.has_value());
    if(!lastOfRun2)return;

    CHECK(index.ReplaceRun(2,L"cache/run2.mkv"));
    CHECK_EQ(size_t{3},index.Count());
    CHECK(index.Revision()!=revision);
    CHECK_EQ(frames,index.TotalFrames());
    const auto after=index.CoveredRanges();
    CHECK_EQ(coverage.size(),after.size());
    for(size_t span=0;span<std::min(coverage.size(),after.size());++span){
        CHECK_EQ(coverage[span].start100ns,after[span].start100ns);
        CHECK_EQ(coverage[span].end100ns,after[span].end100ns);
    }
    if(const auto joined=index.At(0)){
        CHECK(joined->path==std::filesystem::path(L"cache/run2.mkv"));
        CHECK_EQ(uint64_t{2},joined->runId);
        CHECK_EQ(uint64_t{0},joined->firstFrameNumber);
        CHECK_EQ(uint64_t{10},joined->frameCount);
        CHECK_EQ(int64_t{0},joined->firstTimestamp100ns);
        CHECK_EQ(lastOfRun2->end100ns,joined->end100ns);
    }
    // Both files of the run, anywhere inside it, now resolve to the entry.
    for(uint64_t frame:{0ull,4ull,5ull,9ull})
        if(const auto owner=index.ContainingFrame(frame))CHECK(owner->path==std::filesystem::path(L"cache/run2.mkv"));
    if(const auto other=index.ContainingFrame(22))CHECK_EQ(uint64_t{1},other->runId);
    const std::vector<std::filesystem::path> retired{L"run2/neural-00000.mkv",L"run2/neural-00001.mkv"};
    CHECK(retired==index.RetiredFiles());
    index.ForgetRetired(L"run2/neural-00000.mkv");
    CHECK_EQ(size_t{1},index.RetiredFiles().size());
    CHECK_EQ(size_t{3},index.Files().size());

    // Unknown run, and a run with a frame missing between its files.
    CHECK(!index.ReplaceRun(9,L"cache/run9.mkv"));
    NeuralSegmentIndex gapped;
    gapped.Append(LiveSegmentRecord(L"run3/neural-00000.mkv",0,0,5,3));
    gapped.Append(LiveSegmentRecord(L"run3/neural-00001.mkv",1,6,5,3));
    const uint64_t before=gapped.Revision();
    CHECK(!gapped.ReplaceRun(3,L"cache/run3.mkv"));
    CHECK_EQ(size_t{2},gapped.Count());
    CHECK_EQ(before,gapped.Revision());
    CHECK(gapped.RetiredFiles().empty());
}

// The switch from a run's segments to its joined entry happens under a playing
// session, and has to be invisible: every frame once, at its own timestamp, and
// no process started on the presenting thread at the boundary where playback
// crosses into the entry. The file warmed before the join is out of service
// and is swapped for the entry while there is still lead to open it in.
void live_playback_crosses_into_a_joined_run_without_a_gap_or_stall_test()
{
    constexpr uint8_t kJoinedTag=7;
    LiveFrameLibrary library;library.Add(L"original.mkv",40);
    library.Add(L"neural-00000.mkv",5);library.Add(L"neural-00001.mkv",5);library.Add(L"neural-00002.mkv",5);
    TagLiveStream(library.Add(L"joined.mkv",15),kJoinedTag);
    LiveLibrarySource original(library);
    SynchronizedPlayback playback(original,LiveSegmentFactory(library));
    const auto segments=std::make_shared<NeuralSegmentIndex>();
    segments->Append(LiveSegmentRecord(L"neural-00000.mkv",0,10,5));
    segments->Append(LiveSegmentRecord(L"neural-00001.mkv",1,15,5));
    segments->Append(LiveSegmentRecord(L"neural-00002.mkv",2,20,5));
    CHECK(playback.OpenLive(L"original.mkv",segments,
                            SynchronizedRange{10*kLiveFrame100ns,25*kLiveFrame100ns},{}));
    std::vector<uint64_t> played;
    const auto readPair=[&](int expectedTag){
        CHECK_EQ(SynchronizedReadResult::PairReady,playback.ReadNextAvailable({}));
        const auto* pair=playback.CurrentPair();CHECK(pair!=nullptr);
        if(!pair)return;
        played.push_back(pair->frameNumber);
        CHECK_EQ(pair->original.frameNumber,pair->neural.frameNumber);
        CHECK_EQ(pair->original.timestamp100ns,pair->neural.timestamp100ns);
        CHECK_EQ(expectedTag,LiveStreamTag(pair->neural));
    };
    readPair(0);
    // The next segment is warming when the run is published.
    CHECK(library.WaitForOpen(L"neural-00001.mkv",1));
    // The open returns just after it counts; let its future settle so the next
    // read harvests it rather than finding it still in flight.
    std::this_thread::sleep_for(20ms);
    CHECK(segments->ReplaceRun(0,L"joined.mkv"));
    CHECK(playback.HoldsFile(L"neural-00000.mkv"));
    readPair(0);
    // The stale prefetch is dropped and the entry opened, off this thread,
    // already positioned where the playing file ends.
    CHECK(library.WaitForOpen(L"joined.mkv",1));
    CHECK(!playback.HoldsFile(L"neural-00001.mkv"));
    for(int index=0;index<3;++index)readPair(0);
    for(int index=0;index<10;++index)readPair(kJoinedTag);
    std::vector<uint64_t> expected;
    for(uint64_t number=10;number<25;++number)expected.push_back(number);
    CHECK_EQ(expected,played);
    CHECK(library.OpenThread(L"joined.mkv")!=std::this_thread::get_id());
    CHECK(library.OpenedKnown(L"joined.mkv"));
    CHECK_EQ(1,library.Opens(L"joined.mkv"));
    CHECK_EQ(1,library.Seeks(L"joined.mkv"));
    CHECK_EQ(size_t{5},library.LandedIndex(L"joined.mkv"));
    CHECK_EQ(0,library.Opens(L"neural-00002.mkv"));
    // The retired files are free for the caller to delete; the entry is not.
    CHECK(!playback.HoldsFile(L"neural-00000.mkv"));
    CHECK(playback.HoldsFile(L"joined.mkv"));
    CHECK_EQ(SynchronizedReadResult::EndOfStream,playback.ReadNextAvailable({}));
    CHECK(playback.LastFault().empty());
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
            return value.find(L"/diagnostic-latin1") != std::wstring_view::npos;
        })) {
        // FFmpeg prints container tags byte for byte, and a Latin-1 tag is not UTF-8.
        std::cerr << "  title           : Caf\xe9 del Mar\n"
                     "Connection reset while reading https://media.invalid/diagnostic-latin1?token=secret-value\n";
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
        // encoder_blocked_write_is_interrupted_by_stop_test's latch. Bytes
        // beyond the pipe's buffer can only be data of a write still pending
        // on the parent side, so once they show up the parent is blocked.
        std::array<wchar_t, 128> blockedName{};
        if (GetEnvironmentVariableW(L"DLSS_MEDIA_TEST_WRITE_BLOCKED_EVENT", blockedName.data(),
                                    static_cast<DWORD>(blockedName.size())) > 0) {
            const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            for (;;) {
                DWORD available = 0;
                if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) break;
                if (available > kChildStdinPipeBytes) {
                    if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, blockedName.data())) {
                        SetEvent(event);
                        CloseHandle(event);
                    }
                    break;
                }
                Sleep(1);
            }
        }
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
    // Never resumed: LiveChildProcess only needs a pid that stays alive.
    if (argc == 2 && std::wstring_view(argv[1]) == L"--suspended-child") return EXIT_SUCCESS;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--contained-parent") return RunContainedParent();
    const std::wstring executableName = CurrentExecutable().filename().wstring();
    if (_wcsicmp(executableName.c_str(), L"ffmpeg.exe") == 0 ||
        _wcsicmp(executableName.c_str(), L"ffprobe.exe") == 0)
        return RunFakeMediaPipelineChild(argc, argv);
    test_support::ContainChildProcesses();
    default_cache_is_writable_beside_executable_independent_of_working_directory_test();
    default_cache_falls_back_when_portable_directory_is_blocked_test();
    default_cache_falls_back_when_portable_layout_is_unusable_test();
    explicit_cache_root_remains_authoritative_test();
    invalid_explicit_cache_root_does_not_silently_fall_back_test();
    a_valid_cache_root_can_always_stage_a_render_test();
    default_cache_falls_back_when_the_portable_root_is_too_deep_to_stage_in_test();
    eviction_reclaims_unreachable_renders_and_keeps_the_rest_test();
    eviction_leaves_an_active_entry_alone_test();
    clearing_the_cache_frees_everything_size_bytes_counted_test();
    cache_identity_field_list_is_pinned_test();
    published_render_is_not_reused_across_identity_changes_test();
    schema_four_entries_are_retired_by_the_schema_gate_test();
    model_store_digest_tracks_root_contents_and_names_its_fallback_test();
    model_store_digest_ignores_features_outside_the_neural_pass_test();
    model_store_read_mid_rewrite_is_unsettled_and_waited_out_test();
    hashed_runtime_set_covers_the_worker_and_the_lock_set_does_not_test();
    runtime_digest_is_order_independent_byte_sensitive_and_rejects_duplicates_test();
    manifest_round_trip_rejects_partial_duplicate_and_unknown_state_test();
    manifest_fields_with_control_characters_round_trip_test();
    source_and_render_promotion_are_hash_validated_and_immutable_test();
    promotion_waits_out_a_transient_lock_and_names_the_failing_step_test();
    interrupted_staging_is_never_reusable_and_clear_stays_inside_root_test();
    staging_sweep_reaps_invalid_and_orphaned_entries_but_not_live_ones_test();
    a_test_process_that_dies_takes_its_children_with_it_test();
    fixture_sweep_leaves_a_live_runs_directory_alone_test();
    manifest_environment_is_optional_additive_and_all_or_nothing_test();
    eviction_retires_entries_whose_recorded_environment_nothing_can_rebuild_test();
    eviction_leaves_an_open_entry_whole_test();
    lookup_marks_the_entry_used_test();
    cache_operations_yield_to_another_holder_of_the_root_lock_test();
    clear_keeps_what_another_running_instance_owns_test();
    startup_sweep_removes_live_sessions_whose_process_is_gone_test();
    lookup_reuses_its_own_published_digest_only_for_the_same_file_test();
    quarantined_entries_are_kept_for_inspection_then_reaped_test();
    staging_sweep_resumes_a_directory_it_could_not_finish_test();
    media_pipeline_arguments_are_exact_and_never_use_a_shell_test();
    materialization_failure_reports_diagnostics_without_signed_urls_test();
    materialization_diagnostic_survives_output_that_is_not_utf8_test();
    materialization_discards_oversized_diagnostic_url_fragments_test();
    media_progress_reader_buffers_split_keys_and_limits_its_report_rate_test();
    materialization_reports_download_progress_while_the_source_is_copied_test();
    encoder_frame_contract_and_fallback_policy_are_fail_closed_test();
    encoder_quality_ladder_arguments_test();
    owned_media_pipeline_materializes_encodes_probes_and_cancels_test();
    encoder_child_inherits_only_its_stdin_pipe_test();
    cached_media_probe_reads_headers_without_redecoding_validated_video_test();
    probe_child_inherits_only_its_output_pipe_test();
    encoder_blocked_write_is_interrupted_by_stop_test();
    offline_job_primes_feature_then_restarts_source_and_captures_every_frame_test();
    offline_job_refuses_a_backend_that_evaluated_fewer_frames_than_it_captured_test();
    offline_receipt_gate_resubmits_without_capturing_and_captures_each_frame_once_test();
    offline_receipt_gate_output_does_not_depend_on_when_the_log_flushed_test();
    offline_odd_dimensions_use_geometry_preserving_software_encoder_test();
    offline_software_retry_after_the_log_counter_went_quiet_succeeds_test();
    offline_receipt_gate_stops_before_encoding_on_failure_or_cancel_test();
    offline_photo_reuses_warmup_frame_but_encodes_exactly_one_frame_test();
    offline_photo_stops_after_bounded_warmup_without_encoding_test();
    offline_job_rejects_when_feature18_receipt_does_not_advance_after_capture_test();
    offline_job_rejects_any_frame_without_a_neural_evaluation_test();
    offline_job_rejects_when_inline_interception_was_not_armed_before_capture_test();
    offline_super_resolution_only_job_waits_for_no_neural_evidence_and_says_so_test();
    offline_super_resolution_only_job_refuses_a_session_where_the_add_on_ran_test();
    offline_reduced_processing_scale_shows_the_model_the_area_reduced_frame_test();
    offline_job_rejects_non_monotonic_source_timestamps_test();
    offline_job_reports_monotonic_progress_and_smoothed_eta_test();
    offline_job_cancel_stops_before_promotion_and_marks_result_cancelled_test();
    offline_job_nvenc_start_failure_restarts_from_frame_zero_with_h264_test();
    offline_job_nvenc_write_failure_restarts_the_whole_sequence_with_h264_test();
    offline_job_accepts_retry_when_only_abandoned_attempt_advanced_feature18_receipt_test();
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
    offline_capture_identity_is_checked_where_the_readback_resolves_test();
    offline_job_passes_guide_controls_to_the_evaluator_test();
    segmented_offline_job_publishes_finalized_files_and_drops_the_armed_one_test();
    offline_job_reports_the_cold_start_phases_it_reached_test();
    segmented_offline_job_makes_only_the_first_file_short_test();
    segmented_offline_job_software_retry_deletes_the_failed_attempts_files_test();
    segmented_offline_job_cancel_leaves_no_unpublished_file_or_live_encoder_test();
    offline_job_steps_end_where_they_fail_and_close_the_source_test();
    reshade_evidence_requires_native_resolution_inline_path_create_and_evaluate_test();
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
    synchronized_playback_returns_released_pair_buffers_to_its_sources_test();
    synchronized_playback_opens_a_described_original_without_a_probe_test();
    neural_segment_index_orders_appends_and_locates_by_timestamp_test();
    neural_segment_index_resumes_after_retained_coverage_test();
    live_playback_prefetches_across_a_seam_published_after_the_open_test();
    neural_segment_index_covers_the_rounding_hole_but_not_a_real_gap_test();
    live_playback_waits_at_the_render_head_and_resumes_on_a_new_segment_test();
    live_playback_crosses_a_segment_boundary_without_a_gap_or_stall_test();
    live_playback_crosses_a_seam_whose_end_rounds_below_the_next_start_test();
    live_seek_enters_a_rendered_segment_and_refuses_an_unrendered_target_test();
    live_seek_into_a_later_segment_lands_on_the_frame_not_the_first_segments_length_test();
    neural_segment_index_serves_a_published_run_from_its_joined_entry_test();
    live_playback_crosses_into_a_joined_run_without_a_gap_or_stall_test();
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
