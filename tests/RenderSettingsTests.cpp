#include "GuideControls.h"
#include "ReShadeConfig.h"
#include "NeuralCache.h"
#include "NeuralSettings.h"
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
    manifest.schema = 3;
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

void default_identity_key_is_stable_and_range_or_guides_change_it()
{
    // Literal captured from the schema-2 canonical form (schema 2 retired every
    // key written while the dead bias-mask guide was still part of the render).
    NeuralCacheIdentity identity{std::string(64, 'a'), 1920, 1080, "test", "rtx50",
                                 std::string(64, 'b'), "DLAA", false};
    const std::string legacyKey = "cb7ee88ed3bd79823a095f4330f9616221533e9c26746b6703001c98d088f2d2";
    CHECK_EQ(legacyKey, BuildNeuralCacheKey(identity));
    identity.settingsDigest = std::string(64, 'c');
    const std::string settingsKey = "50c16936c3de29b5fbaecec66964424ea1d3dd106654b202099cf7ebd8ed1c47";
    CHECK_EQ(settingsKey, BuildNeuralCacheKey(identity));

    identity.range = NeuralRenderRange{10000000, 30000000};
    const auto rangeKey = BuildNeuralCacheKey(identity);
    CHECK(rangeKey != settingsKey);
    identity.range = NeuralRenderRange{0, 30000000};
    const auto openStartKey = BuildNeuralCacheKey(identity);
    CHECK(openStartKey != settingsKey);
    CHECK(openStartKey != rangeKey);
    identity.range = {};
    CHECK_EQ(settingsKey, BuildNeuralCacheKey(identity));

    identity.guides = CanonicalGuideControls(GuideControls{true, false});
    const auto guidesKey = BuildNeuralCacheKey(identity);
    CHECK(guidesKey != settingsKey);
    identity.range = NeuralRenderRange{10000000, 30000000};
    CHECK(BuildNeuralCacheKey(identity) != guidesKey);
    CHECK(BuildNeuralCacheKey(identity) != rangeKey);
}

void schema_three_manifests_parse_with_defaults_and_stay_reusable()
{
    // Byte-exact schema-3 manifest as written by the previous release.
    const std::string legacy =
        "{\"schema\":3,\"kind\":\"render\",\"state\":\"complete\",\"sourceDigest\":\"" +
        std::string(64, 'a') + "\",\"neuralDigest\":\"" + std::string(64, 'd') +
        "\",\"runtimeDigest\":\"" + std::string(64, 'b') +
        "\",\"encoder\":\"hevc_nvenc\",\"width\":1920,\"height\":1080,\"frameCount\":10,"
        "\"duration100ns\":3333333,\"nativeEvaluations\":10,\"verifiedNeuralFrames\":10,"
        "\"observedFeature18Evaluations\":11,\"feature18Created\":true,"
        "\"feature18ArmedBeforeCapture\":true,\"upscaling\":false}\n";
    const auto parsed = ParseNeuralCacheManifest(legacy);
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK_EQ(uint32_t{3}, parsed->schema);
    CHECK(IsReusableNeuralCacheManifest(*parsed));
    CHECK_EQ(int64_t{0}, parsed->rangeStart100ns);
    CHECK_EQ(int64_t{0}, parsed->rangeEnd100ns);
    CHECK(parsed->guides.empty());
    CHECK_EQ(uint64_t{0}, parsed->jobId);
    CHECK_EQ(uint32_t{0}, parsed->historyResets);
    CHECK(parsed->receiptDigest.empty());
    // Re-serializing a schema-3 manifest stays byte-identical.
    CHECK_EQ(legacy, SerializeNeuralCacheManifest(*parsed));
    // Schema 3 never carried the schema-4 fields.
    auto extended = legacy;
    extended.insert(extended.rfind('}'), ",\"rangeStart100ns\":0");
    CHECK(!ParseNeuralCacheManifest(extended));
}

void schema_four_manifest_round_trips_with_receipt_digest()
{
    auto manifest = RenderManifest();
    manifest.state = NeuralCacheState::Complete;
    manifest.neuralDigest = std::string(64, 'd');
    manifest.settingsDigest = std::string(64, 'e');
    manifest.rangeStart100ns = 10000000;
    manifest.rangeEnd100ns = 13333333;
    manifest.guides = "mv=1,depth=0";
    manifest.jobId = 42;
    manifest.historyResets = 3;
    manifest.receiptDigest = std::string(64, 'f');
    const auto bytes = SerializeNeuralCacheManifest(manifest);
    const std::string tail =
        ",\"upscaling\":false,\"settingsDigest\":\"" + std::string(64, 'e') +
        "\",\"rangeStart100ns\":10000000,\"rangeEnd100ns\":13333333,"
        "\"guides\":\"mv=1,depth=0\",\"jobId\":42,\"historyResets\":3,"
        "\"receiptDigest\":\"" + std::string(64, 'f') + "\"}\n";
    CHECK(bytes.starts_with("{\"schema\":4,"));
    CHECK(bytes.ends_with(tail));
    const auto parsed = ParseNeuralCacheManifest(bytes);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(manifest, *parsed);
        CHECK(IsReusableNeuralCacheManifest(*parsed));
    }
    // Empty digests and a whole-source range are valid schema-4 defaults.
    auto plain = RenderManifest();
    const auto plainParsed = ParseNeuralCacheManifest(SerializeNeuralCacheManifest(plain));
    CHECK(plainParsed.has_value());
    if (plainParsed) CHECK_EQ(plain, *plainParsed);
    // Schema 4 is fixed and ordered: a missing trailing field is rejected.
    auto truncated = bytes;
    truncated.erase(truncated.find(",\"receiptDigest\""));
    truncated += "}\n";
    CHECK(!ParseNeuralCacheManifest(truncated));
    manifest.receiptDigest = "nothex";
    CHECK(!ParseNeuralCacheManifest(SerializeNeuralCacheManifest(manifest)));
    manifest.receiptDigest.clear();
    manifest.guides = "mv=1";
    CHECK(!ParseNeuralCacheManifest(SerializeNeuralCacheManifest(manifest)));
    manifest.guides.clear();
    manifest.rangeEnd100ns = manifest.rangeStart100ns;
    CHECK(!ParseNeuralCacheManifest(SerializeNeuralCacheManifest(manifest)));
    manifest.rangeEnd100ns = 0;
    CHECK(ParseNeuralCacheManifest(SerializeNeuralCacheManifest(manifest)).has_value());
    manifest.schema = 5;
    CHECK(!ParseNeuralCacheManifest(SerializeNeuralCacheManifest(manifest)));
}

void receipt_is_authenticated_on_promotion_and_lookup()
{
    TempDirectory temp;
    NeuralCacheManager cache(temp.path / L"cache");
    const std::string key(64, 'c');
    auto manifest = RenderManifest();
    const std::string receipt = "{\"schema\":1,\"ok\":true}\n";
    manifest.receiptDigest = Sha256Bytes(receipt).value_or("");
    manifest.jobId = 7;
    manifest.historyResets = 1;
    auto staging = cache.BeginRenderStaging(key);
    CHECK(staging.has_value());
    if (!staging) return;
    Write(*staging / L"neural.mkv", "encoded video");
    CHECK(!cache.PromoteRender(key, *staging, manifest));
    Write(*staging / L"receipt.json", receipt);
    CHECK(cache.PromoteRender(key, *staging, manifest));
    const auto found = cache.LookupRender(key);
    CHECK(found.has_value());
    if (!found) return;
    CHECK_EQ(uint32_t{4}, found->manifest.schema);
    CHECK_EQ(manifest.receiptDigest, found->manifest.receiptDigest);
    CHECK_EQ(uint64_t{7}, found->manifest.jobId);
    CHECK_EQ(receipt, Read(found->directory / L"receipt.json"));
    Write(found->directory / L"receipt.json", receipt + " ");
    CHECK(!cache.LookupRender(key));
}

void overrides_follow_managed_keys_and_replace_existing_values()
{
    constexpr std::string_view input =
        "[RenoDX.DLSS5]\nNRIntensity=1.25\nNRStyle=2\nFutureTuning=red,blue\n";
    const std::string base = UpdateNeuralAddonIni(input, true);
    const std::vector<NeuralAddonOverride> overrides{
        {"NRIntensity", "1.000000"}, {"NRPreset", "3"}};
    constexpr std::string_view expected =
        "[RenoDX.DLSS5]\nNRIntensity=1.000000\nNRStyle=2\nFutureTuning=red,blue\n"
        "EnableHooks=2\nNeuralUplift=1\nNREnableUpscaling=0\nNRPreset=3\n"
        "[ADDON]\nDisabledAddons=\n";
    const std::string updated = UpdateNeuralAddonIni(input, true, overrides);
    CHECK_EQ(std::string(expected), updated);
    CHECK_EQ(updated, UpdateNeuralAddonIni(updated, true, overrides));
    CHECK_EQ(base, UpdateNeuralAddonIni(input, true, {}));
    // Overrides never replace the managed contract and never apply while
    // disabling.
    CHECK_EQ(UpdateNeuralAddonIni(input, false), UpdateNeuralAddonIni(input, false, overrides));
    CHECK(UpdateNeuralAddonIni(input, false, overrides).find("NRPreset") == std::string::npos);
    for (const auto& bad : std::vector<NeuralAddonOverride>{
            {"", "1"}, {"NR=Style", "1"}, {" NRStyle", "1"}, {"[NRStyle", "1"},
            {"NRStyle", "1\nNRIntensity=2"}, {"NREnableUpscaling", "1"},
            {"EnableHooks", "3"}, {"NeuralUplift", "0"}}) {
        bool rejected = false;
        try { (void)UpdateNeuralAddonIni(input, true, std::span{&bad, 1}); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
    TempDirectory temp;
    const auto path = temp.path / L"ReShade.ini";
    Write(path, input);
    const ConfigUpdate configured = ConfigureNeuralAddon(path, true, overrides);
    CHECK(configured.ok);
    CHECK(configured.changed);
    CHECK_EQ(std::string(expected), Read(path));
    const ConfigUpdate again = ConfigureNeuralAddon(path, true, overrides);
    CHECK(again.ok);
    CHECK(!again.changed);
    const auto snapshot = ReadNeuralAddonSettingsSnapshot(path);
    CHECK(snapshot.has_value());
    if (snapshot) {
        CHECK(snapshot->find("NRIntensity=1.000000\n") != std::string::npos);
        CHECK(snapshot->find("NRPreset=3\n") != std::string::npos);
    }
}

void neural_settings_round_trip_and_format_renodx_overrides()
{
    const auto defaults = NeuralAddonOverridesFor(NeuralSettings{});
    const std::vector<NeuralAddonOverride> expected{
        {"NRIntensity", "1.000000"}, {"NRLocalTone", "1.000000"},
        {"NRLocalStructure", "1.000000"}, {"NRSkinStructure", "-1.000000"},
        {"NRColorStrength", "1.000000"}, {"NRPreset", "0"}, {"NRStyle", "0"},
        {"NRAutoMask", "1"}};
    CHECK(expected == defaults);
    CHECK_EQ(std::string("intensity=1.000000 localTone=1.000000 localStructure=1.000000 "
                         "skinStructure=-1.000000 colorStrength=1.000000 preset=0 style=0 "
                         "autoMask=1"),
             CanonicalNeuralSettings(NeuralSettings{}));

    NeuralSettings tuned;
    tuned.intensity = 1.05f;
    tuned.localTone = 0.5f;
    tuned.localStructure = 1.5f;
    tuned.skinStructure = 0.25f;
    tuned.colorStrength = 0.75f;
    tuned.preset = 2;
    tuned.style = 1;
    tuned.autoMask = false;
    const auto tunedOverrides = NeuralAddonOverridesFor(tuned);
    CHECK_EQ(std::string("1.050000"), tunedOverrides[0].second);
    CHECK_EQ(std::string("0.250000"), tunedOverrides[3].second);
    CHECK_EQ(std::string("2"), tunedOverrides[5].second);
    CHECK_EQ(std::string("0"), tunedOverrides[7].second);
    CHECK(CanonicalNeuralSettings(tuned) != CanonicalNeuralSettings(NeuralSettings{}));

    TempDirectory temp;
    const auto ini = temp.path / L"DLSSVideoPlayer.ini";
    NeuralSettings missing;
    CHECK(!LoadNeuralSettings(ini, missing));
    CHECK(missing == NeuralSettings{});
    Write(ini, "[Playback]\r\nVolume=0.500000\r\n");
    CHECK(!LoadNeuralSettings(ini, missing));
    CHECK(SaveNeuralSettings(ini, tuned));
    NeuralSettings loaded;
    CHECK(LoadNeuralSettings(ini, loaded));
    CHECK(loaded == tuned);
    // Unrelated sections survive and the settings live in [NeuralSettings].
    const auto text = Read(ini);
    CHECK(text.find("[Playback]") != std::string::npos);
    CHECK(text.find("[NeuralSettings]") != std::string::npos);
    CHECK(text.find("Intensity=1.050000") != std::string::npos);
    // Out-of-range and partial entries clamp and keep defaults elsewhere.
    Write(ini, "[NeuralSettings]\r\nIntensity=9\r\nSkinStructure=-4\r\nPreset=7\r\nAutoMask=5\r\n");
    NeuralSettings clamped;
    CHECK(LoadNeuralSettings(ini, clamped));
    CHECK_EQ(2.0f, clamped.intensity);
    CHECK_EQ(-1.0f, clamped.skinStructure);
    CHECK_EQ(3, clamped.preset);
    CHECK(clamped.autoMask);
    CHECK_EQ(1.0f, clamped.localTone);
    CHECK_EQ(0, clamped.style);
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
    default_identity_key_is_stable_and_range_or_guides_change_it();
    schema_three_manifests_parse_with_defaults_and_stay_reusable();
    schema_four_manifest_round_trips_with_receipt_digest();
    receipt_is_authenticated_on_promotion_and_lookup();
    overrides_follow_managed_keys_and_replace_existing_values();
    neural_settings_round_trip_and_format_renodx_overrides();
    removing_one_owned_entry_preserves_other_entries_and_outside_files();
    default_cache_root_owns_new_writes_under_windows_appdata_virtualization();
    return test_support::failure_count ? EXIT_FAILURE : EXIT_SUCCESS;
}
