#include "GuideControls.h"
#include "ReShadeConfig.h"
#include "NeuralCache.h"
#include "NeuralSettings.h"
#include "TemporalSettings.h"
#include "OpticalFlowNvof.h"
#include "TestSupport.h"
#include "TestEnvironment.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

struct TempDirectory {
    std::filesystem::path path = test_support::FixtureTempRoot() /
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

// The sweep in WriteUpdatedIni judges a temporary against this process's start in
// FILETIME units, so the test speaks the same units rather than converting through
// file_time_type; SetFileTime on an attribute-only handle is the write itself.
FILETIME ProcessStart()
{
    FILETIME start{}, unused{};
    CHECK(GetProcessTimes(GetCurrentProcess(), &start, &unused, &unused, &unused));
    return start;
}

FILETIME Shifted(FILETIME time, int64_t ticks)
{
    LARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = static_cast<LONG>(time.dwHighDateTime);
    value.QuadPart += ticks;
    return {value.LowPart, static_cast<DWORD>(value.HighPart)};
}

void SetLastWrite(const std::filesystem::path& path, const FILETIME& time)
{
    HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    if (handle == INVALID_HANDLE_VALUE) return;
    CHECK(SetFileTime(handle, nullptr, nullptr, &time));
    CloseHandle(handle);
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

// The player never promotes a render without its receipt: it builds the
// receipt JSON, writes receipt.json into staging and fails the render when it
// cannot. RenderManifest() stays receipt-free for the parse-level tests, so a
// promotable render is the same manifest plus the sidecar it has to carry.
constexpr std::string_view kRenderReceipt = "{\"schema\":1,\"render\":\"fixture\"}\n";

NeuralCacheManifest PromotableRenderManifest()
{
    auto result = RenderManifest();
    result.receiptDigest = Sha256Bytes(kRenderReceipt).value_or("");
    return result;
}

void StageRenderReceipt(const std::filesystem::path& staging)
{
    Write(staging / L"receipt.json", kRenderReceipt);
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
    auto manifest = PromotableRenderManifest();
    const auto snapshot = SnapshotNeuralAddonSettings(UpdateNeuralAddonIni("", true));
    manifest.settingsDigest = Sha256Bytes(snapshot).value_or("");
    CHECK_EQ(size_t{64}, manifest.settingsDigest.size());
    auto staging = cache.BeginRenderStaging(key);
    CHECK(staging.has_value());
    if (!staging) return;
    Write(*staging / L"neural.mkv", "encoded video");
    StageRenderReceipt(*staging);
    // neural-settings.ini is the sidecar this manifest promises and staging
    // does not yet have.
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

void source_conversion_changes_the_render_key_and_the_default_path_is_unchanged()
{
    // The capture-side encoder switches stay out of the key on purpose: they
    // change how the result is written. This one changes what the model is shown -
    // NV12 converted on the GPU instead of BGRA from ffmpeg - so two renders that
    // differ in it must not share an entry.
    const std::string shipped =
        "DLAA|strict-timeline-v3|armed-inline-interception-v3|bt709-export-v1";
    CHECK_EQ(shipped, NeuralRenderPipelineIdentity(false, kDefaultNvencPreset, kDefaultGpuColorConversion));
    CHECK(NeuralRenderPipelineIdentity(true, kDefaultNvencPreset, kDefaultGpuColorConversion) != shipped);

    // Byte-for-byte the term the player shipped before it was extracted into a
    // function. A typo here would retire every cached render in the field, so the
    // key built from the function must equal the key built from the literal.
    NeuralCacheIdentity shippedIdentity{std::string(64, 'a'), 1920, 1080, "test", "rtx50",
                                        std::string(64, 'b'), shipped, false};
    NeuralCacheIdentity identity = shippedIdentity;
    identity.quality = NeuralRenderPipelineIdentity(false, kDefaultNvencPreset, kDefaultGpuColorConversion);
    CHECK_EQ(BuildNeuralCacheKey(shippedIdentity), BuildNeuralCacheKey(identity));
    identity.quality = NeuralRenderPipelineIdentity(true, kDefaultNvencPreset, kDefaultGpuColorConversion);
    CHECK(BuildNeuralCacheKey(identity) != BuildNeuralCacheKey(shippedIdentity));
}

// The Scene cuts ladder (P2.12) changes which frames reset the neural history,
// so it is a key term - but the default rung must add nothing, or every render
// in the field would be retired by the setting's mere existence.
void temporal_settings_change_the_render_key_only_off_their_defaults()
{
    const std::string shipped =
        "DLAA|strict-timeline-v3|armed-inline-interception-v3|bt709-export-v1";
    const auto pipeline = [&](const TemporalSettings& temporal) {
        return NeuralRenderPipelineIdentity(false, kDefaultNvencPreset, kDefaultGpuColorConversion) +
               TemporalPipelineTerm(temporal);
    };
    CHECK(TemporalSettings{}.IsDefault());
    CHECK_EQ(std::string{}, TemporalPipelineTerm(TemporalSettings{}));
    CHECK_EQ(shipped, pipeline(TemporalSettings{}));
    std::vector<std::string> terms;
    for (const auto rung : {scene_cut::Sensitivity::More, scene_cut::Sensitivity::Less,
                            scene_cut::Sensitivity::Off}) {
        TemporalSettings temporal;
        temporal.sceneCuts = rung;
        temporal.stability = TemporalStability::Medium;
        const auto parsedWithStability = ParseTemporalSettings(CanonicalTemporalSettings(temporal));
        CHECK(parsedWithStability.has_value() && *parsedWithStability == temporal);
        temporal.stability = TemporalStability::Off;
        CHECK(!temporal.IsDefault());
        terms.push_back(pipeline(temporal));
        CHECK(terms.back() != shipped);
        // The canonical form is what the helper parses; it has to come back whole.
        const auto parsed = ParseTemporalSettings(CanonicalTemporalSettings(temporal));
        CHECK(parsed.has_value());
        if (parsed) CHECK(*parsed == temporal);
    }
    // Every stability rung too (P2.5): each blends a different amount of history.
    for (const auto level : {TemporalStability::Low, TemporalStability::Medium, TemporalStability::High}) {
        TemporalSettings temporal;
        temporal.stability = level;
        terms.push_back(pipeline(temporal));
        CHECK(terms.back() != shipped);
        const auto parsed = ParseTemporalSettings(CanonicalTemporalSettings(temporal));
        CHECK(parsed.has_value());
        if (parsed) CHECK(*parsed == temporal);
    }
    // Every rung its own entry: two rungs reset or blend on different frames.
    for (size_t a = 0; a < terms.size(); ++a)
        for (size_t b = a + 1; b < terms.size(); ++b) CHECK(terms[a] != terms[b]);
    CHECK_EQ(std::string("cuts=default,stability=off"), CanonicalTemporalSettings(TemporalSettings{}));
    for (const std::string_view bad : {"", "cuts=", "cuts=default", "cuts=Default,stability=off", "cuts=more,",
                                       "stability=off", "cuts=on,stability=off", "cuts=off,stability=",
                                       "cuts=off,stability=max", "cuts=off,stability=low,", "cuts=off;stability=low"})
        CHECK(!ParseTemporalSettings(bad).has_value());
}

void encoder_settings_that_change_the_written_pixels_change_the_render_key()
{
    // Both of these change the bytes a cache hit hands back. The NVENC preset
    // changes the encode - the player's own tooltip measures p7 against p5 at
    // 0.12 VMAF on ordinary content and 0.53 on noise-heavy - and the colour
    // conversion runs a different chroma downsample over the neural output.
    // Without them in the key, "Applies to the next render" is false for every
    // range already rendered: the setting is accepted and then ignored.
    const std::string shipped =
        "DLAA|strict-timeline-v3|armed-inline-interception-v3|bt709-export-v1";

    // The shipped defaults must canonicalize to the term every field render was
    // published under, or adding these terms retires the whole cache.
    CHECK_EQ(shipped, NeuralRenderPipelineIdentity(false, kDefaultNvencPreset,
                                                   kDefaultGpuColorConversion));

    // Each term moves the key on its own.
    const auto preset = NeuralRenderPipelineIdentity(false, 7, kDefaultGpuColorConversion);
    const auto colour = NeuralRenderPipelineIdentity(false, kDefaultNvencPreset, true);
    CHECK(preset != shipped);
    CHECK(colour != shipped);
    CHECK(preset != colour);

    // Every preset is distinct: p1 and p7 are not one "non-default" bucket.
    CHECK(NeuralRenderPipelineIdentity(false, 1, false) !=
          NeuralRenderPipelineIdentity(false, 7, false));

    // The three terms stay independent of one another.
    CHECK(NeuralRenderPipelineIdentity(true, 7, true) !=
          NeuralRenderPipelineIdentity(false, 7, true));
    CHECK(NeuralRenderPipelineIdentity(true, 7, true) !=
          NeuralRenderPipelineIdentity(true, 7, false));

    // And the whole key moves with them, not just the term.
    NeuralCacheIdentity identity{std::string(64, 'a'), 1920, 1080, "test", "rtx50",
                                 std::string(64, 'b'), shipped, false};
    const auto shippedKey = BuildNeuralCacheKey(identity);
    identity.quality = preset;
    CHECK(BuildNeuralCacheKey(identity) != shippedKey);
    identity.quality = colour;
    CHECK(BuildNeuralCacheKey(identity) != shippedKey);
}

void capture_dither_changes_the_render_key_and_names_its_map()
{
    // The dither leaves the picture where it was and changes the bytes of every
    // captured frame, so a dithered render must never be served for an
    // undithered request or the other way round. The term follows every other
    // pipeline term, ahead of the rung's.
    CaptureQualityTerms plain;
    plain.captureDither = false;
    CaptureQualityTerms dithered;
    dithered.captureDither = true;
    CHECK_EQ(std::string("|standard-cq16-uncapped-v1"), CaptureQualityIdentityTerm(plain));
    CHECK_EQ(std::string("|dither-bayer8-v1|standard-cq16-uncapped-v1"), CaptureQualityIdentityTerm(dithered));
    const std::string pipeline =
        NeuralRenderPipelineIdentity(false, kDefaultNvencPreset, kDefaultGpuColorConversion) +
        TemporalPipelineTerm(TemporalSettings{});
    NeuralCacheIdentity identity{std::string(64, 'a'), 1920, 1080, "test", "rtx50",
                                 std::string(64, 'b'), pipeline + CaptureQualityIdentityTerm(plain), false};
    const auto plainKey = BuildNeuralCacheKey(identity);
    identity.quality = pipeline + CaptureQualityIdentityTerm(dithered);
    CHECK(BuildNeuralCacheKey(identity) != plainKey);
}

void quality_rung_changes_the_render_key_and_drops_the_switches_it_makes_inert()
{
    const std::string shipped =
        "DLAA|strict-timeline-v3|armed-inline-interception-v3|bt709-export-v1";
    const auto keyed = [&](bool source, uint32_t preset, bool colour, bool dither, EncoderQuality rung) {
        return NeuralRenderPipelineIdentity(source, KeyedNvencPreset(preset, rung),
                                            KeyedGpuColorConversion(colour, rung)) +
               TemporalPipelineTerm(TemporalSettings{}) + CaptureQualityIdentityTerm({dither, rung});
    };
    // Standard is constant quality now, which changed its bytes: its key must not be
    // the one every capped render in the field was published under.
    const auto standard = keyed(false, kDefaultNvencPreset, kDefaultGpuColorConversion, false, EncoderQuality::Standard);
    CHECK_EQ(shipped + "|standard-cq16-uncapped-v1", standard);
    CHECK(standard != shipped);
    const auto high = keyed(false, kDefaultNvencPreset, kDefaultGpuColorConversion, false, EncoderQuality::High);
    const auto lossless = keyed(false, kDefaultNvencPreset, kDefaultGpuColorConversion, false, EncoderQuality::Lossless);
    // The High term carries its CQ, so retuning the rung retires its renders; the
    // rung's term comes after the dither's, and both after every other term.
    CHECK_EQ(shipped + "|high-main10-cq" + std::to_string(kHighRungCq) + "-v1", high);
    CHECK_EQ(shipped + "|lossless-ffv1-10bit-v1", lossless);
    CHECK(high != lossless && high != standard && lossless != standard);
    // A 10-bit rung captures P010 whatever the colour switch says, and has no 8-bit
    // store to dither: neither switch may split its entries.
    CHECK_EQ(high, keyed(false, kDefaultNvencPreset, true, true, EncoderQuality::High));
    CHECK_EQ(lossless, keyed(false, kDefaultNvencPreset, true, true, EncoderQuality::Lossless));
    // FFV1 has no NVENC preset, so neither does Lossless's key; High still does.
    CHECK_EQ(lossless, keyed(false, 7, false, false, EncoderQuality::Lossless));
    CHECK(high != keyed(false, 7, false, false, EncoderQuality::High));
    // Standard keeps both switches.
    CHECK(keyed(false, 7, true, false, EncoderQuality::Standard) != standard);
    CHECK(keyed(false, kDefaultNvencPreset, false, true, EncoderQuality::Standard) != standard);
    // What the model is shown is not the encoder's business: the source term stays.
    CHECK(lossless != keyed(true, kDefaultNvencPreset, false, false, EncoderQuality::Lossless));
    NeuralCacheIdentity identity{std::string(64, 'a'), 1920, 1080, "test", "rtx50",
                                 std::string(64, 'b'), shipped, false};
    const auto shippedKey = BuildNeuralCacheKey(identity);
    identity.quality = high;
    const auto highKey = BuildNeuralCacheKey(identity);
    identity.quality = lossless;
    CHECK(highKey != shippedKey);
    CHECK(BuildNeuralCacheKey(identity) != shippedKey);
    CHECK(BuildNeuralCacheKey(identity) != highKey);
}

void deband_changes_the_render_key_on_every_rung()
{
    CaptureQualityTerms debanded;
    debanded.captureDither = false;
    debanded.sourceDeband = true;
    CHECK_EQ(std::string("|standard-cq16-uncapped-v1|deband-i1t3r16g4-static-v1"), CaptureQualityIdentityTerm(debanded));
    // It changes what the model is shown, so no rung makes it inert, and it follows
    // the rung's term.
    for (const EncoderQuality rung : {EncoderQuality::Standard, EncoderQuality::High, EncoderQuality::Lossless}) {
        CaptureQualityTerms plain;
        plain.quality = rung;
        CaptureQualityTerms withDeband = plain;
        withDeband.sourceDeband = true;
        CHECK(CaptureQualityIdentityTerm(withDeband) != CaptureQualityIdentityTerm(plain));
        CHECK_EQ(CaptureQualityIdentityTerm(plain) + "|deband-i1t3r16g4-static-v1",
                 CaptureQualityIdentityTerm(withDeband));
    }
}

void supplied_exposure_changes_the_render_key()
{
    CaptureQualityTerms exposed;
    exposed.captureDither = false;
    exposed.suppliedExposure = true;
    CHECK_EQ(std::string("|standard-cq16-uncapped-v1|exposure-key018-p20-cut-v1"), CaptureQualityIdentityTerm(exposed));
    // Last of the four, whatever else is set, so the order stays stable.
    CaptureQualityTerms all{true, EncoderQuality::Lossless, true, true};
    CHECK_EQ(std::string("|lossless-ffv1-10bit-v1|deband-i1t3r16g4-static-v1|exposure-key018-p20-cut-v1"),
             CaptureQualityIdentityTerm(all));
    all.suppliedExposure = false;
    CHECK(CaptureQualityIdentityTerm(all) != CaptureQualityIdentityTerm({true, EncoderQuality::Lossless, true, true}));
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

void current_schema_manifest_round_trips_with_receipt_digest()
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
    CHECK(bytes.starts_with("{\"schema\":5,"));
    CHECK(bytes.ends_with(tail));
    const auto parsed = ParseNeuralCacheManifest(bytes);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(manifest, *parsed);
        CHECK(IsReusableNeuralCacheManifest(*parsed));
    }
    // Empty digests and a whole-source range are valid schema-5 defaults.
    auto plain = RenderManifest();
    const auto plainParsed = ParseNeuralCacheManifest(SerializeNeuralCacheManifest(plain));
    CHECK(plainParsed.has_value());
    if (plainParsed) CHECK_EQ(plain, *plainParsed);
    // Schema 5 is fixed and ordered: a missing trailing field is rejected.
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
    // Schema 4 wrote this exact field list and is retired all the same: its
    // entries were keyed under an identity that named neither the driver
    // version nor the model store, so nothing recorded what produced them.
    // The schema gate is what refuses them - not a field they are missing.
    manifest.schema = 4;
    CHECK(!ParseNeuralCacheManifest(SerializeNeuralCacheManifest(manifest)));
    manifest.schema = 6;
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
    // receipt.json is the sidecar this manifest promises and staging does not
    // yet have.
    CHECK(!cache.PromoteRender(key, *staging, manifest));
    Write(*staging / L"receipt.json", receipt);
    CHECK(cache.PromoteRender(key, *staging, manifest));
    const auto found = cache.LookupRender(key);
    CHECK(found.has_value());
    if (!found) return;
    CHECK_EQ(uint32_t{5}, found->manifest.schema);
    CHECK_EQ(manifest.receiptDigest, found->manifest.receiptDigest);
    CHECK_EQ(uint64_t{7}, found->manifest.jobId);
    CHECK_EQ(receipt, Read(found->directory / L"receipt.json"));
    // One byte appended to the published receipt: the digest no longer
    // authenticates it, so the entry is not served.
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
        "EnableHooks=2\nNeuralUplift=1\nNRFollowInputRes=0\nNRResolutionScale=1\nNRPreset=3\n"
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
            {"NRStyle", "1\nNRIntensity=2"}, {"NRResolutionScale", "2"},
            {"NRFollowInputRes", "1"},
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

void writing_the_ini_sweeps_only_temporaries_older_than_this_process()
{
    TempDirectory temp;
    const auto ini = temp.path / L"ReShade.ini";
    Write(ini, "[RenoDX.DLSS5]\nNRIntensity=1.25\n");
    // GetTempFileNameW names its files <prefix><hex>.tmp. One a process left behind
    // when it died mid-write is older than we are; one a live writer holds is not.
    // The boundary is inclusive: written at our own start counts as ours.
    const auto stale = temp.path / L"RDX1234.tmp";
    const auto fresh = temp.path / L"RDX5678.tmp";
    const auto folder = temp.path / L"RDX9ABC.tmp";
    const auto other = temp.path / L"NOT1234.tmp";
    Write(stale, "half-written");
    Write(fresh, "in flight");
    Write(other, "somebody else's");
    CHECK(std::filesystem::create_directory(folder));
    const FILETIME start = ProcessStart();
    const FILETIME earlier = Shifted(start, -10'000'000); // one second, in 100 ns
    SetLastWrite(stale, earlier);
    SetLastWrite(fresh, start);
    SetLastWrite(folder, earlier);
    SetLastWrite(other, earlier);

    const ConfigUpdate configured = ConfigureNeuralAddon(ini, true);
    CHECK(configured.ok);
    CHECK(configured.changed);
    CHECK(configured.addonEnabled);
    CHECK(!std::filesystem::exists(stale));
    CHECK_EQ(std::string("in flight"), Read(fresh));
    CHECK(std::filesystem::is_directory(folder));
    CHECK_EQ(std::string("somebody else's"), Read(other));
    const auto rewritten = Read(ini);
    CHECK(rewritten.find("NRIntensity=1.25") != std::string::npos);
    CHECK(ReadNeuralAddonSettingsSnapshot(ini).has_value());
    // The write's own temporary was consumed by the rename, not left for a later sweep.
    size_t temporaries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temp.path))
        if (entry.is_regular_file() && entry.path().extension() == L".tmp") ++temporaries;
    CHECK_EQ(size_t{2}, temporaries);
}

void neural_settings_round_trip_and_format_renodx_overrides()
{
    const auto defaults = NeuralAddonOverridesFor(NeuralSettings{});
    const std::vector<NeuralAddonOverride> expected{
        {"NRIntensity", "1.000000"}, {"NRLocalTone", "1.000000"},
        {"NRLocalStructure", "1.000000"}, {"NRSkinStructure", "-1.000000"},
        {"NRColorStrength", "1.000000"}, {"NRPreset", "0"}, {"NRStyle", "0"},
        {"NRAutoMask", "1"}, {"NRPasses", "1"}, {"NRChainedHistory", "1"}};
    CHECK(expected == defaults);
    // The canonical form is the neural cache key, so every field belongs in it:
    // a render at two stack passes is a different render, and one that keyed the
    // same as a single-pass render would be served from the wrong cache entry.
    CHECK_EQ(std::string("intensity=1.000000 localTone=1.000000 localStructure=1.000000 "
                         "skinStructure=-1.000000 colorStrength=1.000000 preset=0 style=0 "
                         "autoMask=1 passes=1 chainedHistory=1"),
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
    tuned.passes = 3;
    tuned.chainedHistory = false;
    const auto tunedOverrides = NeuralAddonOverridesFor(tuned);
    CHECK_EQ(std::string("1.050000"), tunedOverrides[0].second);
    CHECK_EQ(std::string("0.250000"), tunedOverrides[3].second);
    CHECK_EQ(std::string("2"), tunedOverrides[5].second);
    CHECK_EQ(std::string("0"), tunedOverrides[7].second);
    CHECK_EQ(std::string("3"), tunedOverrides[8].second);
    CHECK_EQ(std::string("0"), tunedOverrides[9].second);
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
    StageRenderReceipt(*render);
    StageRenderReceipt(*other);
    Write(temp.path / L"user.mkv", "user file");
    CHECK(cache.PromoteSource(first, *source, RenderManifest()));
    CHECK(cache.PromoteRender(first, *render, PromotableRenderManifest()));
    CHECK(cache.PromoteRender(second, *other, PromotableRenderManifest()));
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

void a_cache_root_that_cannot_become_a_directory_is_invalid_and_names_the_cause()
{
    TempDirectory temp;
    // Standing in for an install directory the user cannot write to: the
    // player answered nullopt for every one of these, so the status string
    // could not tell an unwritable location from a rejected key.
    const auto occupied = temp.path / L"cache";
    Write(occupied, "not a directory");
    NeuralCacheManager cache(occupied);
    CHECK(!cache.Valid());
    CHECK_EQ(NeuralCacheFailure::Cause::NoWritableRoot, cache.LastFailure().cause);
    CHECK_EQ(std::filesystem::canonical(occupied), cache.LastFailure().path);
    CHECK(cache.LastFailure().error.value() != 0);
    // An invalid manager keeps that verdict: no attempt can get past it.
    CHECK(!cache.BeginRenderStaging(std::string(64, 'e')).has_value());
    CHECK_EQ(NeuralCacheFailure::Cause::NoWritableRoot, cache.LastFailure().cause);
}

void staging_reports_the_latest_refusal_and_keeps_an_accepted_one_in_the_root()
{
    TempDirectory temp;
    NeuralCacheManager cache(temp.path / L"cache");
    CHECK(cache.Valid());
    const auto staging = cache.Root() / L"staging";
    CHECK(!cache.BeginRenderStaging("../../escape").has_value());
    CHECK_EQ(NeuralCacheFailure::Cause::InvalidKey, cache.LastFailure().cause);
    CHECK_EQ(staging, cache.LastFailure().path);
    CHECK(std::filesystem::is_empty(staging));
    const auto accepted = cache.BeginRenderStaging(std::string(64, 'f'));
    CHECK(accepted.has_value());
    if (accepted) {
        CHECK_EQ(NeuralCacheFailure::Cause::None, cache.LastFailure().cause);
        CHECK(cache.LastFailure().path.empty());
        std::error_code error;
        CHECK_EQ(staging, std::filesystem::canonical(*accepted, error).parent_path());
        CHECK(!error);
        // Only an owned staging directory can be set aside, so the ownership
        // check answers here for itself.
        CHECK(cache.MarkInvalid(*accepted));
    }
    // The record is the latest attempt, not everything that ever failed.
    CHECK(!cache.BeginSourceStaging("not-a-digest").has_value());
    CHECK_EQ(NeuralCacheFailure::Cause::InvalidKey, cache.LastFailure().cause);
}

// The motion estimator a session runs on is picked from sizes alone, before any device
// call, so the two halves of that decision are checkable here: what geometry hardware
// flow is asked for and how its vectors are converted (PlanHardwareFlow, called by
// D3D12Renderer::CreateVideoResources), and the engine bound that sends a session back
// to the CPU block matcher (FlowGeometrySupported, called by OpticalFlowNvof::Initialize).
void hardware_flow_runs_at_the_decoded_size_and_scales_only_for_super_resolution()
{
    // Neural size: the decoded frame is already the DLSS input. This is the path that
    // shipped, and the scale has to be exactly 1 rather than nearly 1, because every
    // vector the engine produces is multiplied by it.
    const HardwareFlowPlan neural = PlanHardwareFlow(1920, 1080, 1920, 1080);
    CHECK(neural.attempt);
    CHECK_EQ(uint32_t{1920}, neural.width);
    CHECK_EQ(uint32_t{1080}, neural.height);
    CHECK_EQ(1.0f, neural.motionScaleX);
    CHECK_EQ(1.0f, neural.motionScaleY);

    // Runtime Super Resolution, the case that used to fall back to the CPU estimator:
    // the engine still compares the decoded frame, because that is the texture the
    // capture copies, and the vectors are carried into DLSS input pixels afterwards.
    const HardwareFlowPlan upscaled = PlanHardwareFlow(1920, 1080, 1280, 720);
    CHECK(upscaled.attempt);
    CHECK_EQ(uint32_t{1920}, upscaled.width);
    CHECK_EQ(uint32_t{1080}, upscaled.height);
    CHECK_EQ(1280.0f / 1920.0f, upscaled.motionScaleX);
    CHECK_EQ(720.0f / 1080.0f, upscaled.motionScaleY);

    // The other direction is a real one too: a source below the runtime's minimum input
    // for the requested output is given a DLSS input larger than the decoded frame.
    const HardwareFlowPlan raised = PlanHardwareFlow(1280, 720, 1920, 1080);
    CHECK(raised.attempt);
    CHECK_EQ(uint32_t{1280}, raised.width);
    CHECK_EQ(1.5f, raised.motionScaleX);
    CHECK_EQ(1.5f, raised.motionScaleY);

    // Per axis, not one ratio: the conversion pass stretches each axis on its own, so a
    // DLSS input whose aspect differs from the source's scales differently in x and y.
    const HardwareFlowPlan stretched = PlanHardwareFlow(1920, 1080, 1280, 1080);
    CHECK_EQ(1280.0f / 1920.0f, stretched.motionScaleX);
    CHECK_EQ(1.0f, stretched.motionScaleY);

    // A degenerate size says why and leaves the scale at 1, so a caller that used the
    // plan without reading `attempt` would still not multiply a vector by zero.
    const HardwareFlowPlan degenerate = PlanHardwareFlow(1920, 1080, 1920, 0);
    CHECK(!degenerate.attempt);
    CHECK(std::string_view(degenerate.refusal).find("zero dimension") != std::string_view::npos);
    CHECK_EQ(1.0f, degenerate.motionScaleX);
}

void a_decoded_frame_outside_the_engine_range_keeps_the_cpu_block_matcher()
{
    // Asking for the decoded size rather than the DLSS input size is what makes this
    // bound matter: a 4K source upscaled from a small DLSS input is refused by an engine
    // that stops at 1080p, and that session keeps the CPU estimator.
    CHECK(!FlowGeometrySupported(3840, 2160, 32, 32, 1920, 1080));
    CHECK(FlowGeometrySupported(1920, 1080, 32, 32, 4096, 4096));
    // Inclusive at both ends.
    CHECK(FlowGeometrySupported(4096, 4096, 32, 32, 4096, 4096));
    CHECK(!FlowGeometrySupported(16, 16, 32, 32, 4096, 4096));
    // A maximum the driver did not answer cannot refuse anything; the engine's own
    // nvOFInit is then the thing that decides.
    CHECK(FlowGeometrySupported(3840, 2160, 0, 0, 0, 0));
}

} // namespace

int main()
{
    harmless_ini_rewrites_preserve_snapshot_but_every_neural_tuning_changes_key();
    ambiguous_settings_and_unreadable_files_fail_closed();
    authenticated_settings_survive_promotion_and_tampering_invalidates_cache();
    manifest_accepts_legacy_and_valid_settings_but_rejects_malformed_extension();
    default_identity_key_is_stable_and_range_or_guides_change_it();
    source_conversion_changes_the_render_key_and_the_default_path_is_unchanged();
    temporal_settings_change_the_render_key_only_off_their_defaults();
    encoder_settings_that_change_the_written_pixels_change_the_render_key();
    capture_dither_changes_the_render_key_and_names_its_map();
    quality_rung_changes_the_render_key_and_drops_the_switches_it_makes_inert();
    deband_changes_the_render_key_on_every_rung();
    supplied_exposure_changes_the_render_key();
    schema_three_manifests_parse_with_defaults_and_stay_reusable();
    current_schema_manifest_round_trips_with_receipt_digest();
    receipt_is_authenticated_on_promotion_and_lookup();
    overrides_follow_managed_keys_and_replace_existing_values();
    writing_the_ini_sweeps_only_temporaries_older_than_this_process();
    neural_settings_round_trip_and_format_renodx_overrides();
    removing_one_owned_entry_preserves_other_entries_and_outside_files();
    default_cache_root_owns_new_writes_under_windows_appdata_virtualization();
    a_cache_root_that_cannot_become_a_directory_is_invalid_and_names_the_cause();
    staging_reports_the_latest_refusal_and_keeps_an_accepted_one_in_the_root();
    hardware_flow_runs_at_the_decoded_size_and_scales_only_for_super_resolution();
    a_decoded_frame_outside_the_engine_range_keeps_the_cpu_block_matcher();
    return test_support::failure_count ? EXIT_FAILURE : EXIT_SUCCESS;
}
