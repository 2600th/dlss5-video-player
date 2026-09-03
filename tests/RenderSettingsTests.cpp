#include "ReShadeConfig.h"
#include "NeuralCache.h"
#include "TestSupport.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (L"DLSS-RenderSettings-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    TempDirectory() { CHECK(std::filesystem::create_directories(path)); }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

void Write(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

NeuralCacheManifest RenderManifest()
{
    NeuralCacheManifest result;
    result.sourceDigest = std::string(64, 'a');
    result.runtimeDigest = std::string(64, 'b');
    result.encoder = "hevc_nvenc";
    result.width = 1920;
    result.height = 1080;
    result.frameCount = 10;
    result.duration100ns = 3333333;
    result.nativeEvaluations = 10;
    result.verifiedNeuralFrames = 10;
    result.observedFeature18Evaluations = 11;
    result.feature18Created = true;
    result.feature18ArmedBeforeCapture = true;
    return result;
}

void harmless_ini_rewrites_preserve_snapshot_but_every_neural_tuning_changes_key()
{
    const std::string first = UpdateNeuralAddonIni(
        "[RenoDX.DLSS5]\nNRIntensity=1.25\nNRStyle=2\nFutureTuning=red,blue\n"
        "[OVERLAY]\nWindow=old\n[GENERAL]\nPresetPath=old.ini\n", true);
    const std::string rewritten = UpdateNeuralAddonIni(
        "\xEF\xBB\xBF[GENERAL]\r\nPresetPath=new.ini\r\n[OVERLAY]\r\nWindow=new\r\n"
        "[RenoDX.DLSS5] ; tuning\r\nFutureTuning=red,blue\r\n"
        " NRStyle = 2 \r\n; comment\r\nNRIntensity=1.25\r\n", true);
    const auto snapshot = SnapshotNeuralAddonSettings(first);
    CHECK(!snapshot.empty());
    CHECK_EQ(snapshot, SnapshotNeuralAddonSettings(rewritten));
    CHECK(snapshot.find("NRIntensity=1.25") != std::string::npos);
    CHECK(snapshot.find("Window=") == std::string::npos);

    NeuralCacheIdentity identity{std::string(64, 'a'), 1920, 1080, "test", "rtx50",
                                 std::string(64, 'b'), "DLAA", false};
    const auto legacyKey = BuildNeuralCacheKey(identity);
    identity.settingsDigest = Sha256Bytes(snapshot).value_or("");
    const auto key = BuildNeuralCacheKey(identity);
    CHECK(key != legacyKey);
    for (const auto text : {"NRIntensity=1.25", "NRStyle=2", "FutureTuning=red,blue"}) {
        auto changed = first;
        const size_t at = changed.find(text);
        CHECK(at != std::string::npos);
        changed.insert(at + std::string_view(text).size(), "1");
        identity.settingsDigest = Sha256Bytes(SnapshotNeuralAddonSettings(changed)).value_or("");
        CHECK(key != BuildNeuralCacheKey(identity));
    }
    CHECK(snapshot != SnapshotNeuralAddonSettings(UpdateNeuralAddonIni(first, false)));
    // Wrong-case sections are unrelated to RenoDX's exact-case lookup.
    CHECK_EQ(snapshot, SnapshotNeuralAddonSettings(first + "[renodx.dlss5]\nNRStyle=3\n"));
}

void ambiguous_settings_and_unreadable_files_fail_closed()
{
    for (const std::string& ini : std::vector<std::string>{
            "[RenoDX.DLSS5]\nNRStyle=1\nNRStyle=2\n",
            "[RenoDX.DLSS5]\nNRStyle=1\n[RenoDX.DLSS5]\nNRIntensity=2\n",
            "[ADDON]\nDisabledAddons=\nDisabledAddons=x\n",
            "[RenoDX.DLSS5]\nNRStyle=1" + std::string(1, '\0') + "hidden"}) {
        bool rejected = false;
        try { (void)SnapshotNeuralAddonSettings(ini); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
    TempDirectory temp;
    std::wstring error;
    CHECK(!ReadNeuralAddonSettingsSnapshot(temp.path / L"missing.ini", &error));
    CHECK(!error.empty());
    const auto path = temp.path / L"ReShade.ini";
    Write(path, "[RenoDX.DLSS5]\nNRIntensity=1.25\n");
    CHECK(ConfigureNeuralAddon(path, true).ok);
    const auto original = Read(path);
    const auto snapshot = ReadNeuralAddonSettingsSnapshot(path, &error);
    CHECK(snapshot.has_value());
    CHECK(error.empty());
    CHECK_EQ(original, Read(path));
    Write(path, "[RenoDX.DLSS5]\nNRStyle=1\nNRStyle=2\n");
    CHECK(!ReadNeuralAddonSettingsSnapshot(path, &error));
    CHECK(!error.empty());
}

void authenticated_settings_survive_promotion_and_tampering_invalidates_cache()
{
    TempDirectory temp;
    NeuralCacheManager cache(temp.path / L"cache");
    const std::string key(64, 'c');
    auto manifest = RenderManifest();
    const auto snapshot = SnapshotNeuralAddonSettings(UpdateNeuralAddonIni("", true));
    manifest.settingsDigest = Sha256Bytes(snapshot).value_or("");
    CHECK_EQ(size_t{64}, manifest.settingsDigest.size());
    auto staging = cache.BeginRenderStaging(key);
    CHECK(staging.has_value());
    if (!staging) return;
    Write(*staging / L"neural.mkv", "encoded video");
    CHECK(!cache.PromoteRender(key, *staging, manifest));
    Write(*staging / L"neural-settings.ini", snapshot);
    CHECK(cache.PromoteRender(key, *staging, manifest));
    const auto found = cache.LookupRender(key);
    CHECK(found.has_value());
    if (!found) return;
    CHECK_EQ(manifest.settingsDigest, found->manifest.settingsDigest);
    CHECK_EQ(snapshot, Read(found->directory / L"neural-settings.ini"));
    Write(found->directory / L"neural-settings.ini", snapshot + "NRStyle=2\n");
    CHECK(!cache.LookupRender(key));
}

void manifest_accepts_legacy_and_valid_settings_but_rejects_malformed_extension()
{
    auto manifest = RenderManifest();
    const auto old = SerializeNeuralCacheManifest(manifest);
    CHECK(old.find("settingsDigest") == std::string::npos);
    CHECK(ParseNeuralCacheManifest(old).has_value());
    manifest.settingsDigest = std::string(64, 'a');
    const auto bytes = SerializeNeuralCacheManifest(manifest);
    const auto parsed = ParseNeuralCacheManifest(bytes);
    CHECK(parsed.has_value());
    if (parsed) CHECK_EQ(manifest, *parsed);
    auto duplicate = bytes;
    duplicate.insert(duplicate.rfind('}'), ",\"settingsDigest\":\"\"");
    CHECK(!ParseNeuralCacheManifest(duplicate));
    manifest.settingsDigest = "invalid";
    CHECK(!ParseNeuralCacheManifest(SerializeNeuralCacheManifest(manifest)));
}

void removing_one_owned_entry_preserves_other_entries_and_outside_files()
{
    TempDirectory temp;
    NeuralCacheManager cache(temp.path / L"cache");
    const std::string first(64, 'c'), second(64, 'd');
    auto source = cache.BeginSourceStaging(first);
    auto render = cache.BeginRenderStaging(first);
    auto other = cache.BeginRenderStaging(second);
    CHECK(source && render && other);
    if (!source || !render || !other) return;
    Write(*source / L"source.mkv", "original");
    Write(*render / L"neural.mkv", "first render");
    Write(*other / L"neural.mkv", "second render");
    Write(temp.path / L"user.mkv", "user file");
    CHECK(cache.PromoteSource(first, *source, RenderManifest()));
    CHECK(cache.PromoteRender(first, *render, RenderManifest()));
    CHECK(cache.PromoteRender(second, *other, RenderManifest()));
    CHECK(cache.RemoveRender(first));
    CHECK(!cache.LookupRender(first));
    CHECK(cache.LookupSource(first).has_value());
    CHECK(cache.LookupRender(second).has_value());
    CHECK(cache.RemoveSource(first));
    CHECK(!cache.LookupSource(first));
    CHECK(cache.RemoveSource(first));
    CHECK(!cache.RemoveRender("../../user.mkv"));
    CHECK_EQ(std::string("user file"), Read(temp.path / L"user.mkv"));
    // A junction/symlink outside the owned root must never be traversed.
    const auto link = cache.Root() / L"renders" / std::wstring(first.begin(), first.end());
    std::error_code error;
    std::filesystem::create_directory_symlink(temp.path, link, error);
    if (!error) {
        CHECK(!cache.RemoveRender(first));
        CHECK_EQ(std::string("user file"), Read(temp.path / L"user.mkv"));
        std::filesystem::remove(link, error);
    }
}

void default_cache_root_owns_new_writes_under_windows_appdata_virtualization()
{
    // Unlike temporary fixtures, an existing LocalAppData root can be merged
    // with a package-private writable directory by Windows virtualization.
    NeuralCacheManager cache;
    CHECK(cache.Valid());
    const auto key = Sha256Bytes("root-regression-" + std::to_string(GetCurrentProcessId()) +
                                "-" + std::to_string(GetTickCount64())).value_or("");
    const auto source = cache.BeginSourceStaging(key);
    const auto render = cache.BeginRenderStaging(key);
    CHECK(source.has_value());
    CHECK(render.has_value());
    for (const auto name : {L"sources", L"renders", L"staging"}) {
        std::error_code error;
        const auto physical = std::filesystem::canonical(cache.Root() / name, error);
        CHECK(!error);
        CHECK_EQ(cache.Root(), physical.parent_path());
    }
    for (const auto& staging : {source, render}) {
        if (!staging) continue;
        std::error_code error;
        const auto physical = std::filesystem::canonical(*staging, error);
        CHECK(!error);
        CHECK_EQ(cache.Root(), physical.parent_path().parent_path());
        // Remove only the fresh empty directory returned to this test, never
        // existing cache entries or the shared default cache root.
        CHECK(std::filesystem::remove(*staging, error));
        CHECK(!error);
    }
}

} // namespace

int main()
{
    harmless_ini_rewrites_preserve_snapshot_but_every_neural_tuning_changes_key();
    ambiguous_settings_and_unreadable_files_fail_closed();
    authenticated_settings_survive_promotion_and_tampering_invalidates_cache();
    manifest_accepts_legacy_and_valid_settings_but_rejects_malformed_extension();
    removing_one_owned_entry_preserves_other_entries_and_outside_files();
    default_cache_root_owns_new_writes_under_windows_appdata_virtualization();
    return test_support::failure_count ? EXIT_FAILURE : EXIT_SUCCESS;
}
