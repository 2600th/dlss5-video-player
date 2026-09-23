#include "TestSupport.h"

#include "NeuralCache.h"
#include "NeuralPreflight.h"
#include "NeuralReceipt.h"
#include "RuntimeLock.h"
#include "RuntimeModulePolicy.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string Hex(std::string_view bytes)
{
    const auto digest = Sha256Bytes(bytes);
    return digest ? *digest : std::string();
}

std::string Upper(std::string text)
{
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

std::string LockJson(std::string_view schema, std::vector<std::string> entries)
{
    std::string json = "{\"schemaVersion\":" + std::string(schema) + ",\"runtimeVersion\":\"310.8.0.0\",\"entries\":[";
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index) json += ',';
        json += entries[index];
    }
    return json + "]}";
}

std::string Entry(std::string_view destination, uint64_t size, std::string_view sha256, std::string_view version)
{
    return "{\"sourceName\":\"ignored\",\"destination\":\"" + std::string(destination) + "\",\"size\":" + std::to_string(size) +
           ",\"sha256\":\"" + std::string(sha256) + "\",\"fileVersion\":\"" + std::string(version) +
           "\",\"signatureStatus\":\"Valid\",\"provenance\":\"unit test\"}";
}

void WriteFile(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool Contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

bool Contains(std::wstring_view haystack, std::wstring_view needle)
{
    return haystack.find(needle) != std::wstring_view::npos;
}

void parse_accepts_the_lock_shape_and_normalizes_hashes_test()
{
    const std::string hash = Hex("alpha");
    const auto lock = ParseRuntimeLock(LockJson("1", {Entry("dxgi.dll", 5592064, Upper(hash), "6.8.0.2155"),
                                                      Entry("sl.\\u0064lss.dll", 421504, hash, "")}));
    CHECK(lock.has_value());
    if (!lock) return;
    CHECK_EQ(uint32_t{1}, lock->schemaVersion);
    CHECK_EQ(std::string("310.8.0.0"), lock->runtimeVersion);
    CHECK_EQ(size_t{2}, lock->entries.size());
    if (lock->entries.size() != 2) return;
    CHECK_EQ(std::wstring(L"dxgi.dll"), lock->entries[0].destination);
    CHECK_EQ(uint64_t{5592064}, lock->entries[0].size);
    CHECK_EQ(hash, lock->entries[0].sha256);
    CHECK_EQ(std::wstring(L"6.8.0.2155"), lock->entries[0].fileVersion);
    CHECK_EQ(std::wstring(L"sl.dlss.dll"), lock->entries[1].destination);
    CHECK_EQ(std::wstring(), lock->entries[1].fileVersion);

    const auto whitespace = ParseRuntimeLock("  \n{ \"schemaVersion\" : 1 , \"runtimeVersion\" : \"x\" , \"entries\" : [ ] }\r\n");
    CHECK(whitespace.has_value());
    CHECK(whitespace && whitespace->entries.empty());
}

void parse_rejects_malformed_documents_test()
{
    const std::string hash = Hex("alpha");
    const std::string valid = LockJson("1", {Entry("dxgi.dll", 1, hash, "1.0.0.0")});
    CHECK(ParseRuntimeLock(valid).has_value());
    CHECK(!ParseRuntimeLock(valid.substr(0, valid.size() - 1)).has_value());
    CHECK(!ParseRuntimeLock(valid + "}").has_value());
    CHECK(!ParseRuntimeLock("").has_value());
    CHECK(!ParseRuntimeLock("[]").has_value());
    CHECK(!ParseRuntimeLock("{\"schemaVersion\":1,\"runtimeVersion\":\"x\",\"entries\":[],}").has_value());
    CHECK(!ParseRuntimeLock("{\"schemaVersion\":1,\"runtimeVersion\":\"x\",\"entries\":[{}]}").has_value());
    CHECK(!ParseRuntimeLock("{\"schemaVersion\":1,\"runtimeVersion\":\"x\",\"entries\":[\"dxgi.dll\"]}").has_value());
    CHECK(!ParseRuntimeLock("{\"schemaVersion\":1,\"runtimeVersion\":\"x\",\"entries\":{}}").has_value());
    CHECK(!ParseRuntimeLock("{\"schemaVersion\":1,\"runtimeVersion\":\"a\tb\",\"entries\":[]}").has_value());
    CHECK(!ParseRuntimeLock("{\"schemaVersion\":1,\"runtimeVersion\":\"\\x\",\"entries\":[]}").has_value());
    CHECK(!ParseRuntimeLock("{\"schemaVersion\":01,\"runtimeVersion\":\"x\",\"entries\":[]}").has_value());
    CHECK(!ParseRuntimeLock("{\"schemaVersion\":1,\"runtimeVersion\":\"x\",\"entries\":[]} extra").has_value());
}

void parse_rejects_missing_fields_and_wrong_schema_test()
{
    const std::string hash = Hex("alpha");
    CHECK(!ParseRuntimeLock(LockJson("2", {Entry("dxgi.dll", 1, hash, "1")})).has_value());
    CHECK(!ParseRuntimeLock(LockJson("\"1\"", {Entry("dxgi.dll", 1, hash, "1")})).has_value());
    CHECK(!ParseRuntimeLock("{\"runtimeVersion\":\"x\",\"entries\":[]}").has_value());
    CHECK(!ParseRuntimeLock("{\"schemaVersion\":1,\"entries\":[]}").has_value());
    CHECK(!ParseRuntimeLock("{\"schemaVersion\":1,\"runtimeVersion\":\"x\"}").has_value());
    CHECK(!ParseRuntimeLock(LockJson("1", {"{\"destination\":\"dxgi.dll\",\"size\":1,\"sha256\":\"" + hash + "\"}"})).has_value());
    CHECK(!ParseRuntimeLock(LockJson("1", {"{\"destination\":\"dxgi.dll\",\"size\":1,\"fileVersion\":\"1\"}"})).has_value());
    CHECK(!ParseRuntimeLock(LockJson("1", {"{\"destination\":\"dxgi.dll\",\"sha256\":\"" + hash + "\",\"fileVersion\":\"1\"}"})).has_value());
    CHECK(!ParseRuntimeLock(LockJson("1", {"{\"size\":1,\"sha256\":\"" + hash + "\",\"fileVersion\":\"1\"}"})).has_value());
    CHECK(!ParseRuntimeLock(LockJson("1", {Entry("", 1, hash, "1")})).has_value());
    CHECK(!ParseRuntimeLock(LockJson("1", {Entry("dxgi.dll", 1, hash.substr(1), "1")})).has_value());
    CHECK(!ParseRuntimeLock(LockJson("1", {Entry("dxgi.dll", 1, "g" + hash.substr(1), "1")})).has_value());
    CHECK(!ParseRuntimeLock(LockJson("1", {"{\"destination\":\"dxgi.dll\",\"size\":\"1\",\"sha256\":\"" + hash + "\",\"fileVersion\":\"1\"}"})).has_value());
    CHECK(!ParseRuntimeLock(LockJson("1", {"{\"destination\":\"dxgi.dll\",\"size\":-1,\"sha256\":\"" + hash + "\",\"fileVersion\":\"1\"}"})).has_value());
    CHECK(!ParseRuntimeLock(LockJson("1", {"{\"destination\":\"dxgi.dll\",\"size\":1.5,\"sha256\":\"" + hash + "\",\"fileVersion\":\"1\"}"})).has_value());
}

void embedded_lock_parses_and_names_the_locked_runtime_files_test()
{
    const RuntimeLock& lock = EmbeddedRuntimeLock();
    CHECK_EQ(uint32_t{1}, lock.schemaVersion);
    CHECK(!lock.runtimeVersion.empty());
    // The lock pins the vendor stack; the render identity hashes that stack
    // plus NeuralWorker.exe, which is built here and changes with every build,
    // so the two sets are deliberately different sizes.
    const auto pinned = LockPinnedRuntimeFileNames();
    const auto hashed = LockedRuntimeFileNames();
    CHECK_EQ(size_t{12}, pinned.size());
    CHECK_EQ(size_t{13}, hashed.size());
    CHECK_EQ(pinned.size(), lock.entries.size());
    for (const std::wstring_view name : pinned) {
        const bool listed = std::any_of(lock.entries.begin(), lock.entries.end(),
                                        [&](const RuntimeLockEntry& entry) { return entry.destination == name; });
        CHECK(listed);
        CHECK(std::any_of(hashed.begin(), hashed.end(),
                          [&](const std::wstring_view hashedName) { return hashedName == name; }));
    }
    const bool workerPinned = std::any_of(lock.entries.begin(), lock.entries.end(),
                                          [](const RuntimeLockEntry& entry) {
                                              return entry.destination == L"NeuralWorker.exe";
                                          });
    CHECK(!workerPinned);
    for (const RuntimeLockEntry& entry : lock.entries) {
        CHECK(entry.size > 0);
        CHECK_EQ(size_t{64}, entry.sha256.size());
        CHECK_EQ(std::string::npos, entry.sha256.find_first_of("ABCDEF")); // stored lowercase
        const size_t duplicates = static_cast<size_t>(std::count_if(
            lock.entries.begin(), lock.entries.end(),
            [&](const RuntimeLockEntry& other) { return other.destination == entry.destination; }));
        CHECK_EQ(size_t{1}, duplicates);
    }
}

void verify_reports_each_drift_kind_and_names_only_failing_files_test()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (L"RuntimeLockTests-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const std::string good = "good module bytes";
    const std::string shortFile = "short";
    const std::string tampered = "tampered module bytes";
    WriteFile(directory / L"good.dll", good);
    WriteFile(directory / L"short.dll", shortFile);
    WriteFile(directory / L"tampered.dll", tampered);

    RuntimeLock lock;
    lock.schemaVersion = 1;
    lock.entries = {
        RuntimeLockEntry{L"good.dll", good.size(), Hex(good), L""},
        RuntimeLockEntry{L"short.dll", shortFile.size() + 3, Hex(shortFile), L"1.0.0.0"},
        RuntimeLockEntry{L"tampered.dll", tampered.size(), Hex("original module bytes"), L""},
        RuntimeLockEntry{L"missing.dll", 42, Hex("missing"), L""},
    };
    const auto checks = VerifyRuntimeLock(directory, lock);
    CHECK_EQ(size_t{4}, checks.size());
    if (checks.size() == 4) {
        CHECK_EQ(std::wstring(L"good.dll"), checks[0].name);
        CHECK(checks[0].present);
        CHECK(checks[0].sizeMatches);
        CHECK(checks[0].hashMatches);
        CHECK(checks[0].versionMatches); // both sides empty
        CHECK(checks[0].Ok());
        CHECK_EQ(good.size(), checks[0].actualSize);
        CHECK_EQ(Hex(good), checks[0].actualSha256);

        CHECK(checks[1].present);
        CHECK(!checks[1].sizeMatches);
        CHECK(checks[1].hashMatches);
        CHECK(!checks[1].versionMatches); // plain file has no version resource; informational
        CHECK(!checks[1].Ok());
        CHECK_EQ(shortFile.size(), checks[1].actualSize);
        CHECK_EQ(std::wstring(), checks[1].actualFileVersion);

        CHECK(checks[2].present);
        CHECK(checks[2].sizeMatches);
        CHECK(!checks[2].hashMatches);
        CHECK(!checks[2].Ok());
        CHECK_EQ(Hex(tampered), checks[2].actualSha256);

        CHECK(!checks[3].present);
        CHECK(!checks[3].Ok());
        CHECK_EQ(uint64_t{0}, checks[3].actualSize);
        CHECK(checks[3].actualSha256.empty());
    }
    CHECK(!RuntimeLockSatisfied(checks));
    const std::wstring drift = DescribeRuntimeLockDrift(checks);
    CHECK(!Contains(drift, L"good.dll"));
    CHECK(Contains(drift, L"short.dll: size"));
    CHECK(Contains(drift, L"tampered.dll: hash mismatch"));
    CHECK(Contains(drift, L"missing.dll: missing"));

    // Version drift alone never fails the lock: community runtime builds may
    // ship without a version resource.
    RuntimeLock versionOnly;
    versionOnly.schemaVersion = 1;
    versionOnly.entries = {RuntimeLockEntry{L"good.dll", good.size(), Upper(Hex(good)), L"9.9.9.9"}};
    const auto versionChecks = VerifyRuntimeLock(directory, versionOnly);
    CHECK_EQ(size_t{1}, versionChecks.size());
    CHECK(versionChecks.size() == 1 && versionChecks[0].hashMatches); // uppercase lock hash still matches
    CHECK(versionChecks.size() == 1 && !versionChecks[0].versionMatches);
    CHECK(RuntimeLockSatisfied(versionChecks));
    CHECK(DescribeRuntimeLockDrift(versionChecks).empty());

    // Cancellation leaves the hash unverified, which is not a satisfied lock.
    std::stop_source cancel;
    cancel.request_stop();
    const auto cancelled = VerifyRuntimeLock(directory, versionOnly, cancel.get_token());
    CHECK(cancelled.size() == 1 && cancelled[0].present && !cancelled[0].hashMatches);
    CHECK(!RuntimeLockSatisfied(cancelled));

    CHECK(!RuntimeLockSatisfied(std::vector<RuntimeLockCheck>{}));
    std::filesystem::remove_all(directory);
}

// P1.11: a stray add-on or DLL beside the helper is loaded by the proxy or
// the loader, so it is named and refused even though every locked file
// verifies. The files the build and the package legitimately put there are
// not modules and stay allowed.
void unlocked_runtime_modules_are_named_and_packaged_files_are_allowed_test()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (L"RuntimeLockModules-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory / L"ngx_logs");
    RuntimeLock lock;
    lock.schemaVersion = 1;
    lock.entries = {RuntimeLockEntry{L"dxgi.dll", 1, Hex("x"), L""},
                    RuntimeLockEntry{L"renodx-dlss5.addon64", 1, Hex("x"), L""}};
    for (const wchar_t* name : {L"DXGI.dll", L"renodx-dlss5.addon64", L"NeuralWorker.exe", L"NeuralWorker.log",
                                L"ReShade.ini", L"ReShadePreset.ini", L"ReShade.log", L"ReShade.log1",
                                L"ReShade.ini.renodx-dlss5-pre-v4-migration-20260922-120343.bak",
                                L"dxgi.dll.bak"}) {
        WriteFile(directory / name, "x");
    }
    WriteFile(directory / L"ngx_logs" / L"nested.dll", "x"); // not on any load path
    const auto clean = FindUnlockedRuntimeModules(directory, lock);
    CHECK(clean.has_value() && clean->empty());

    WriteFile(directory / L"zzz-extra.addon64", "x");
    WriteFile(directory / L"Stray.DLL", "x");
    WriteFile(directory / L"other.addon32", "x");
    WriteFile(directory / L"plain.addon", "x");
    std::filesystem::create_directories(directory / L"folder.dll"); // a directory is not a module
    const auto stray = FindUnlockedRuntimeModules(directory, lock);
    CHECK(stray.has_value());
    CHECK_EQ((std::vector<std::wstring>{L"Stray.DLL", L"other.addon32", L"plain.addon", L"zzz-extra.addon64"}),
             stray.value_or(std::vector<std::wstring>{}));

    // A directory that cannot be listed is a refusal, not a pass.
    CHECK(!FindUnlockedRuntimeModules(directory / L"does-not-exist", lock).has_value());

    // A preflight helper that refuses to start says why in a diagnosis, which
    // is where the parent reads the user's message from - not only in its log.
    CHECK(Contains(BuildPreflightFailureJson(L"holds modules the runtime lock does not name: Stray.DLL"),
                   "\"ok\":false,\"diagnosis\":{\"cause\":\"probeFailed\",\"detail\":\"holds modules the "
                   "runtime lock does not name: Stray.DLL\"}"));
    std::filesystem::remove_all(directory);
}

// F3: the player names a stray module itself, in the helper's words, before it
// launches the helper; and a refused preflight is remembered against the
// module listing, so removing the stray file ends the refusal without a
// restart. The listing must not move with the files every job rewrites.
void player_names_unlocked_modules_and_the_refusal_key_follows_the_module_listing_test()
{
    CHECK(runtime_modules::UnlockedModulesRefusal({}).empty());
    CHECK(runtime_modules::UnlockedModulesRefusal({L"Stray.DLL", L"plain.addon"}) ==
          L"The neural runtime directory holds modules the runtime lock does not name, which the helper "
          L"would load: Stray.DLL, plain.addon. Remove them and try again.");

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (L"RuntimeModuleListing-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    RuntimeLock lock;
    lock.schemaVersion = 1;
    lock.entries = {RuntimeLockEntry{L"dxgi.dll", 1, Hex("x"), L""}};
    for (const wchar_t* name : {L"dxgi.dll", L"ReShade.ini", L"ReShade.log", L"NeuralWorker.exe"})
        WriteFile(directory / name, "x");
    const std::wstring clean = runtime_modules::ModuleListingIdentity(directory);
    CHECK(!clean.empty());
    // Every job rewrites the INI and every run the log: neither is a module.
    WriteFile(directory / L"ReShade.ini", "rewritten for the next job");
    WriteFile(directory / L"ReShade.log", "a new run");
    CHECK(clean == runtime_modules::ModuleListingIdentity(directory));

    WriteFile(directory / L"Stray.addon64", "x");
    const auto unlocked = FindUnlockedRuntimeModules(directory, lock);
    CHECK(unlocked.has_value());
    CHECK(runtime_modules::UnlockedModulesRefusal(unlocked.value_or(std::vector<std::wstring>{}))
              .find(L"would load: Stray.addon64.") != std::wstring::npos);
    const std::wstring stray = runtime_modules::ModuleListingIdentity(directory);
    CHECK(stray != clean);
    // The user removes it: the key is the clean one again, so a refusal
    // latched while it was there no longer applies.
    std::filesystem::remove(directory / L"Stray.addon64");
    CHECK(clean == runtime_modules::ModuleListingIdentity(directory));
    // A replaced module is a different directory too, locked name or not.
    WriteFile(directory / L"dxgi.dll", "a different, larger build");
    CHECK(clean != runtime_modules::ModuleListingIdentity(directory));

    CHECK(runtime_modules::ModuleListingIdentity(directory / L"does-not-exist").empty());
    std::filesystem::remove_all(directory);
}

constexpr std::string_view kSamplePreflight =
    "{\"schema\":2,\"ok\":true,\"workerVersion\":\"1.2.3\",\"elapsedMilliseconds\":812,"
    "\"gpu\":{\"description\":\"NVIDIA GeForce RTX 4090\",\"vendorId\":4318,\"deviceId\":9988,"
    "\"dedicatedVideoMemoryMiB\":24564,\"driverVersion\":\"32.0.15.6164\"},"
    "\"runtime\":{\"reshade\":\"6.8.0.2155\",\"addon\":\"0.2026.828.517\",\"addonApi\":\"18\",\"renodx\":\"4.7\","
    "\"renodxBuild\":\"Sep  2 2026 01:15:39\",\"dlssnr\":\"310.8.0\",\"activeSettings\":\"upscaling=OFF\"},"
    "\"modules\":[{\"name\":\"dxgi.dll\",\"present\":true,\"size\":5592064,\"fileVersion\":\"6.8.0.2155\",\"sha256\":\"00\"}],"
    "\"feature18\":{\"created\":true,\"evaluated\":true,\"armed\":true,\"nativeResolution\":true,\"inlineInterception\":true,"
    "\"laterFailure\":false,\"highestEvaluation\":61,\"probeFrames\":61,\"carrierCreateResult\":\"0x00000001\","
    "\"createResult\":\"\",\"observations\":[]},\"diagnosis\":{\"cause\":\"none\",\"detail\":\"\"}}";

NeuralRenderReceiptInputs SampleInputs()
{
    NeuralRenderReceiptInputs inputs;
    inputs.preflightJson = std::string(kSamplePreflight);
    RuntimeLockCheck ok;
    ok.name = L"nvngx_dlss.dll";
    ok.present = ok.sizeMatches = ok.hashMatches = ok.versionMatches = true;
    ok.actualSize = 58956400;
    ok.actualSha256 = Hex("dlss");
    ok.actualFileVersion = L"310.8.0.0";
    RuntimeLockCheck drifted;
    drifted.name = L"dxgi.dll";
    drifted.present = drifted.sizeMatches = true;
    drifted.actualSize = 5592064;
    drifted.actualSha256 = Hex("other");
    inputs.lockChecks = {ok, drifted};
    inputs.request.sourcePath = L"C:\\media\\clip \"one\".mkv";
    inputs.request.width = 1920;
    inputs.request.height = 1080;
    inputs.request.fps = 23.976;
    inputs.request.durationSeconds = 12.5;
    inputs.request.jobId = 7;
    inputs.request.range = {10000000, 30000000};
    inputs.request.prerollFrames = 24;
    inputs.request.guides.depth = false;
    inputs.request.temporal.sceneCuts = scene_cut::Sensitivity::Less;
    inputs.request.frameRetryLimit = 3;
    inputs.result.ok = false;
    inputs.result.failure = NeuralRenderFailure::GpuStall;
    inputs.result.encoder = EncoderKind::H264Software;
    inputs.result.frameCount = 48;
    inputs.result.nativeEvaluations = 48;
    inputs.result.verifiedNeuralFrames = 48;
    inputs.result.duration100ns = 20000000;
    inputs.result.historyResets = 2;
    inputs.result.frameRetries = 1;
    inputs.result.sceneCuts = {.acceptedStrong = 1, .acceptedWeak = 2, .suppressed = 4};
    inputs.result.firstTimestamp100ns = 10000000;
    inputs.result.timing.samples = 48;
    inputs.result.timing.neuralGpuMsP50 = 4.25;
    inputs.result.timing.peakLocalVramMiB = 3072;
    // The idle-VRAM pair and the arm that produced it. Distinct values so a
    // receipt that transposed the two samples, or reported the default arm for
    // a run that used the other one, reads wrong here.
    inputs.result.timing.postJobLocalVramMiB = 1408;
    inputs.result.timing.idleLocalVramMiB = 406;
    inputs.result.timing.idleVramPolicy = resident_helper::IdleVramPolicy::FreeFeature;
    inputs.result.detail = L"GPU stalled after 48 frames";
    inputs.renderKey = "key";
    inputs.settingsDigest = "settings";
    inputs.runtimeDigest = "runtime";
    inputs.started = std::chrono::system_clock::time_point{};
    inputs.finished = std::chrono::system_clock::time_point{} + std::chrono::milliseconds(90061001);
    return inputs;
}

// The flat JSON object around `anchor`. A lock check nests nothing, so the
// nearest brace either side of one of its own values is the object's own.
std::string FlatObjectAround(std::string_view json, std::string_view anchor)
{
    const size_t at = json.find(anchor);
    if (at == std::string_view::npos) return {};
    const size_t open = json.rfind('{', at);
    const size_t close = json.find('}', at);
    if (open == std::string_view::npos || close == std::string_view::npos) return {};
    return std::string(json.substr(open, close - open + 1));
}

void receipt_json_records_failure_lock_status_and_preflight_verbatim_test()
{
    const NeuralRenderReceiptInputs inputs = SampleInputs();
    const std::string json = BuildNeuralRenderReceiptJson(inputs);
    CHECK(json.front() == '{' && json.back() == '}');
    CHECK(Contains(json, "\"schema\":1"));
    CHECK(Contains(json, "\"jobId\":7"));
    CHECK(Contains(json, "\"started\":\"1970-01-01T00:00:00.000Z\""));
    CHECK(Contains(json, "\"finished\":\"1970-01-02T01:01:01.001Z\""));
    CHECK(Contains(json, "\"source\":\"C:\\\\media\\\\clip \\\"one\\\".mkv\""));
    CHECK(Contains(json, "\"range\":{\"start\":10000000,\"end\":30000000}"));
    CHECK(Contains(json, "\"guides\":\"mv=1,depth=0\""));
    CHECK(Contains(json, "\"temporal\":\"cuts=less\""));
    CHECK(Contains(json, "\"prerollFrames\":24"));
    // The probe's own JSON is embedded byte for byte, not re-serialized.
    CHECK(Contains(json, "\"preflight\":" + std::string(kSamplePreflight)));
    CHECK(Contains(json, "\"satisfied\":false"));
    // Each lock check is read back through its own object: the preflight's
    // module list also names dxgi.dll, so the anchor is a value only the check
    // carries.
    const std::string dlss = FlatObjectAround(json, "\"actualSize\":58956400");
    CHECK(Contains(dlss, "\"name\":\"nvngx_dlss.dll\""));
    CHECK(Contains(dlss, "\"present\":true"));
    CHECK(Contains(dlss, "\"sizeMatches\":true"));
    CHECK(Contains(dlss, "\"hashMatches\":true"));
    CHECK(Contains(dlss, "\"versionMatches\":true"));
    const std::string dxgi = FlatObjectAround(json, "\"actualSize\":5592064");
    CHECK(Contains(dxgi, "\"name\":\"dxgi.dll\""));
    CHECK(Contains(dxgi, "\"present\":true"));
    CHECK(Contains(dxgi, "\"sizeMatches\":true"));
    CHECK(Contains(dxgi, "\"hashMatches\":false"));
    CHECK(Contains(dxgi, "\"versionMatches\":false"));
    CHECK(Contains(dxgi, "\"actualSha256\":\"" + Hex("other") + "\""));
    CHECK(Contains(dxgi, "\"actualFileVersion\":\"\""));
    CHECK(Contains(json, "\"ok\":false"));
    CHECK(Contains(json, "\"cancelled\":false"));
    CHECK(Contains(json, "\"failure\":\"gpu-stall\""));
    CHECK(Contains(json, "\"encoder\":\"h264_software\""));
    CHECK(Contains(json, "\"frameCount\":48"));
    CHECK(Contains(json, "\"historyResets\":2"));
    CHECK(Contains(json, "\"frameRetries\":1"));
    CHECK(Contains(json, "\"acceptedStrong\":1"));
    CHECK(Contains(json, "\"acceptedWeak\":2"));
    CHECK(Contains(json, "\"suppressed\":4"));
    CHECK(Contains(json, "\"firstTimestamp100ns\":10000000"));
    CHECK(Contains(json, "\"samples\":48"));
    CHECK(Contains(json, "\"neuralGpuMsP50\":4.25"));
    CHECK(Contains(json, "\"peakLocalVramMiB\":3072"));
    CHECK(Contains(json, "\"postJobLocalVramMiB\":1408"));
    CHECK(Contains(json, "\"idleLocalVramMiB\":406"));
    CHECK(Contains(json, "\"idleVramPolicy\":\"free\""));
    CHECK(Contains(json, "\"detail\":\"GPU stalled after 48 frames\""));
    CHECK(Contains(json, "\"nativeResolution\":false"));
    CHECK(Contains(json, "\"valid\":false"));

    NeuralRenderReceiptInputs noProbe = inputs;
    noProbe.preflightJson = "   \n";
    noProbe.lockChecks = {inputs.lockChecks[0]};
    noProbe.result.failure = NeuralRenderFailure::None;
    noProbe.result.ok = true;
    const std::string satisfied = BuildNeuralRenderReceiptJson(noProbe);
    CHECK(Contains(satisfied, "\"preflight\":null"));
    CHECK(Contains(satisfied, "\"satisfied\":true"));
    CHECK(Contains(satisfied, "\"failure\":\"none\""));

    NeuralRenderReceiptInputs partial = inputs;
    partial.preflightJson = "{\"schema\":2,\"ok\":false";
    CHECK(Contains(BuildNeuralRenderReceiptJson(partial), "\"preflight\":null"));
}

// The log line is read by a person or a grep, field by field: the identity
// of the runtime, the feature's arming (with its own NGX code and the
// diagnosed cause when the probe had one), the lock verdict naming the
// drifted file, and the failure kind. Their order and spacing are the
// line's own business.
void receipt_log_summary_extracts_runtime_identity_and_lock_state_test()
{
    const NeuralRenderReceiptInputs inputs = SampleInputs();
    const std::string drifted = SummarizeNeuralReceiptForLog(inputs);
    CHECK(Contains(drifted, "gpu=\"NVIDIA GeForce RTX 4090\""));
    CHECK(Contains(drifted, "driver=32.0.15.6164"));
    CHECK(Contains(drifted, "reshade=6.8.0.2155"));
    CHECK(Contains(drifted, "renodx=4.7"));
    CHECK(Contains(drifted, "nr=310.8.0"));
    CHECK(Contains(drifted, "feature18=armed"));
    CHECK(!Contains(drifted, "cause="));
    CHECK(Contains(drifted, "lock=drift(dxgi.dll)"));
    CHECK(Contains(drifted, "failure=gpu-stall"));
    CHECK(Contains(drifted, "frames=48/48"));
    CHECK(Contains(drifted, "verified=48"));
    CHECK(Contains(drifted, "cuts=3"));
    CHECK(Contains(drifted, "suppressed=4"));

    NeuralRenderReceiptInputs bare = inputs;
    bare.preflightJson.clear();
    bare.lockChecks.clear();
    bare.result.failure = NeuralRenderFailure::Preflight;
    const std::string unverified = SummarizeNeuralReceiptForLog(bare);
    CHECK(Contains(unverified, "gpu=\"\""));
    CHECK(Contains(unverified, "driver=-"));
    CHECK(Contains(unverified, "feature18=unknown"));
    CHECK(Contains(unverified, "lock=unverified"));
    CHECK(Contains(unverified, "failure=preflight"));

    // A schema-1 receipt from before the diagnosis existed still summarizes,
    // and a quote inside the GPU name stays escaped so the field parses.
    NeuralRenderReceiptInputs notArmed = inputs;
    notArmed.preflightJson = "{\"schema\":1,\"ok\":false,\"gpu\":{\"description\":\"Escaped \\\"GPU\\\"\",\"driverVersion\":\"\"},"
                             "\"feature18\":{\"created\":true,\"evaluated\":false,\"armed\":false}}";
    notArmed.lockChecks = {inputs.lockChecks[0]};
    const std::string escaped = SummarizeNeuralReceiptForLog(notArmed);
    CHECK(Contains(escaped, "gpu=\"Escaped \\\"GPU\\\"\""));
    CHECK(Contains(escaped, "driver=-"));
    CHECK(Contains(escaped, "feature18=not-armed"));
    CHECK(!Contains(escaped, "feature18=not-armed("));
    CHECK(Contains(escaped, "lock=ok"));

    // The field receipt: the carrier reported success while feature 18 was
    // refused, so the summary must carry the feature's own code and cause.
    NeuralRenderReceiptInputs refused = inputs;
    refused.preflightJson =
        "{\"schema\":2,\"ok\":false,\"gpu\":{\"description\":\"NVIDIA GeForce RTX 3060 Laptop GPU\","
        "\"driverVersion\":\"32.0.15.6614\"},\"feature18\":{\"created\":false,\"evaluated\":false,\"armed\":false,"
        "\"carrierCreateResult\":\"0x00000001\",\"createResult\":\"0xbad00002\"},"
        "\"diagnosis\":{\"cause\":\"driverBelowFloor\",\"detail\":\"NVIDIA driver 566.14 is below the 610.47 minimum.\"}}";
    refused.lockChecks = {inputs.lockChecks[0]};
    refused.result.failure = NeuralRenderFailure::Preflight;
    const std::string diagnosed = SummarizeNeuralReceiptForLog(refused);
    CHECK(Contains(diagnosed, "gpu=\"NVIDIA GeForce RTX 3060 Laptop GPU\""));
    CHECK(Contains(diagnosed, "driver=32.0.15.6614"));
    CHECK(Contains(diagnosed, "feature18=not-armed(0xbad00002)"));
    CHECK(!Contains(diagnosed, "0x00000001"));
    CHECK(Contains(diagnosed, "cause=driverBelowFloor"));
    CHECK(Contains(diagnosed, "lock=ok"));
    CHECK(Contains(diagnosed, "failure=preflight"));
}

void feature18_create_result_is_read_out_of_the_runtime_log_test()
{
    // The shape the failing machine actually logged, prefix and suffix and all.
    const auto refused = CollectFeature18Observations(
        "13:50:19:704 [23096] | INFO  | NR: Feature 18 create failed with 0xBAD00002, retrying next frame\r\n"
        "13:50:19:705 [23096] | INFO  | NR skipped: feature unavailable\r\n");
    CHECK_EQ(size_t{2}, refused.size());
    const auto code = ParseFeature18CreateResult(refused);
    CHECK(code.has_value());
    CHECK(code && *code == 0xbad00002u);

    // A clean session carries no code at all.
    CHECK(!ParseFeature18CreateResult(CollectFeature18Observations(
              "13:50:19:704 [23096] | INFO  | NR: feature 18 created (1920x1080)\r\n"
              "13:50:19:812 [23096] | INFO  | NR: feature 18 evaluation succeeded\r\n")).has_value());
    CHECK(!ParseFeature18CreateResult({}).has_value());

    // Malformed or over-long hex is not a 32-bit NGX result.
    CHECK(!ParseFeature18CreateResult(CollectFeature18Observations(
              "| INFO | NR: feature 18 create failed with 0xnope\r\n")).has_value());
    CHECK(!ParseFeature18CreateResult(CollectFeature18Observations(
              "| INFO | NR: feature 18 create failed with 0x\r\n")).has_value());
    CHECK(!ParseFeature18CreateResult(CollectFeature18Observations(
              "| INFO | NR: feature 18 create failed with 0xbad000021\r\n")).has_value());

    // The last create decides: an early refusal the runtime recovered from is
    // not the verdict.
    const auto sequence = ParseFeature18CreateResult(CollectFeature18Observations(
        "| INFO | NR: feature 18 create failed with 0xbad0000d\r\n"
        "| INFO | NR: feature 18 create failed with 0xbad00001\r\n"));
    CHECK(sequence.has_value());
    CHECK(sequence && *sequence == 0xbad00001u);
}

void neural_preflight_diagnosis_blames_the_actionable_cause_test()
{
    DetectedGpu ampere;
    ampere.generation = GpuGeneration::Rtx30Ampere;
    ampere.description = L"NVIDIA GeForce RTX 3060 Laptop GPU";
    ampere.driverVersion = L"32.0.15.6614";  // 566.14
    DetectedGpu blackwell;
    blackwell.generation = GpuGeneration::Rtx50Blackwell;
    blackwell.description = L"NVIDIA GeForce RTX 5090";
    blackwell.driverVersion = L"32.0.16.1664";  // 616.64

    const auto platformError = CollectFeature18Observations(
        "| INFO | NR: feature 18 create failed with 0xbad00002\r\n");

    // The field failure: the platform refusal is real, but the driver below
    // the floor is the thing the user can fix, and the message says so.
    const NeuralPreflightDiagnosis field = DiagnoseNeuralPreflight(ampere, platformError, true, false, {});
    CHECK_EQ(NeuralPreflightCause::DriverBelowFloor, field.cause);
    CHECK(field.ngxResult.has_value());
    CHECK(field.ngxResult && *field.ngxResult == 0xbad00002u);
    CHECK(Contains(field.detail, L"566.14"));
    CHECK(Contains(field.detail, L"610.47"));
    CHECK(Contains(field.detail, L"616.64"));
    CHECK_EQ(std::string_view("driverBelowFloor"), std::string_view(NeuralPreflightCauseName(field.cause)));

    // The same code on a driver above the floor is the runtime's own refusal.
    const NeuralPreflightDiagnosis platform =
        DiagnoseNeuralPreflight(blackwell, platformError, true, false, {});
    CHECK_EQ(NeuralPreflightCause::PlatformRefusal, platform.cause);
    CHECK(Contains(platform.detail, L"0xbad00002"));
    CHECK(Contains(platform.detail, L"616.64"));
    CHECK_EQ(std::string_view("platformRefusal"), std::string_view(NeuralPreflightCauseName(platform.cause)));

    // Architecture and memory refusals are the runtime's answer whatever the
    // driver is, so the driver floor never masks them.
    const auto architecture = CollectFeature18Observations(
        "| INFO | NR: feature 18 create failed with 0xbad00001\r\n");
    for (const DetectedGpu& gpu : {ampere, blackwell}) {
        const NeuralPreflightDiagnosis refusal = DiagnoseNeuralPreflight(gpu, architecture, true, false, {});
        CHECK_EQ(NeuralPreflightCause::ArchitectureUnsupported, refusal.cause);
        CHECK(Contains(refusal.detail, L"0xbad00001"));
    }
    const auto memory = CollectFeature18Observations(
        "| INFO | NR: feature 18 create failed with 0xbad0000d\r\n");
    const NeuralPreflightDiagnosis outOfMemory = DiagnoseNeuralPreflight(ampere, memory, true, false, {});
    CHECK_EQ(NeuralPreflightCause::OutOfVideoMemory, outOfMemory.cause);
    CHECK(Contains(outOfMemory.detail, L"0xbad0000d"));

    // Without a code the probe's own failure is the message - unless the
    // driver is below the floor, which explains it.
    const NeuralPreflightDiagnosis probe =
        DiagnoseNeuralPreflight(blackwell, {}, false, false, L"The probe renderer could not be initialized.");
    CHECK_EQ(NeuralPreflightCause::ProbeFailed, probe.cause);
    CHECK_EQ(std::wstring(L"The probe renderer could not be initialized."), probe.detail);
    CHECK(!probe.ngxResult.has_value());
    const NeuralPreflightDiagnosis staleDriver =
        DiagnoseNeuralPreflight(ampere, {}, false, false, L"The probe renderer could not be initialized.");
    CHECK_EQ(NeuralPreflightCause::DriverBelowFloor, staleDriver.cause);

    // Created but unproven evidence is its own cause.
    const NeuralPreflightDiagnosis incomplete = DiagnoseNeuralPreflight(blackwell, {}, true, false, {});
    CHECK_EQ(NeuralPreflightCause::EvidenceIncomplete, incomplete.cause);
    CHECK(!incomplete.detail.empty());

    // A probe that ended up armed is never blamed, not even for a code the
    // runtime recovered from: that receipt would contradict itself.
    const NeuralPreflightDiagnosis armed = DiagnoseNeuralPreflight(ampere, platformError, true, true, {});
    CHECK_EQ(NeuralPreflightCause::None, armed.cause);
    CHECK(armed.detail.empty());
    CHECK(armed.ngxResult.has_value());
    CHECK_EQ(std::string_view("none"), std::string_view(NeuralPreflightCauseName(armed.cause)));
}

// P1.12: a probe that fails every frame logs an observation per frame, and the
// metadata pipe refuses a frame over 64 KiB. The receipt keeps the two ends of
// the list and counts the middle; a short list is written exactly as before.
void preflight_observations_are_bounded_to_fit_one_pipe_frame_test()
{
    const std::vector<Feature18Observation> few{{"feature 18 created", false},
                                                {"say \"hi\"", true}};
    CHECK_EQ(std::string("\"observations\":[{\"failure\":false,\"line\":\"feature 18 created\"},"
                         "{\"failure\":true,\"line\":\"say \\\"hi\\\"\"}]"),
             ReportedFeature18ObservationsJson(few));
    CHECK_EQ(std::string("\"observations\":[]"), ReportedFeature18ObservationsJson({}));

    std::vector<Feature18Observation> flood;
    for (int index = 0; index < 5000; ++index) {
        flood.push_back({"line " + std::to_string(index) + " " + std::string(4000, 'x'), true});
    }
    const std::string bounded = ReportedFeature18ObservationsJson(flood);
    CHECK(bounded.size() < 8 * 1024);
    CHECK(Contains(bounded, "\"line 0 x"));
    CHECK(Contains(bounded, "\"line 7 x"));
    CHECK(!Contains(bounded, "\"line 8 x"));
    CHECK(!Contains(bounded, "\"line 4991 x"));
    CHECK(Contains(bounded, "\"line 4992 x"));
    CHECK(Contains(bounded, "\"line 4999 x"));
    CHECK(Contains(bounded, "x...\"}"));
    CHECK(Contains(bounded, "],\"observationsOmitted\":4984"));

    // Cut on a UTF-8 boundary: the two-byte character straddling the limit
    // goes whole rather than leaving half of it in the receipt.
    const std::string straddle = std::string(kReportedObservationLineBytes - 1, 'a') + "\xC3\xA9" + "tail";
    const std::string cut = ReportedFeature18ObservationsJson(std::vector<Feature18Observation>{{straddle, true}});
    CHECK(Contains(cut, std::string(kReportedObservationLineBytes - 1, 'a') + "...\""));
    CHECK(!Contains(cut, "\xC3"));
}

void receipt_carries_the_cold_start_and_keeps_absent_phases_absent_test()
{
    NeuralRenderReceiptInputs inputs = SampleInputs();
    NeuralColdStartTimeline timeline;
    // A latched preflight verdict and a whole-file render: two phases that did
    // not happen, around seven that did.
    timeline.Record(NeuralColdStartPhase::Request, std::chrono::microseconds(81000));
    timeline.Record(NeuralColdStartPhase::Launch, std::chrono::microseconds(12000));
    timeline.Record(NeuralColdStartPhase::HelperStart, std::chrono::microseconds(712000));
    timeline.Record(NeuralColdStartPhase::RuntimeReady, std::chrono::microseconds(94000));
    timeline.Record(NeuralColdStartPhase::NeuralInit, std::chrono::microseconds(1511000));
    timeline.Record(NeuralColdStartPhase::FeatureArm, std::chrono::microseconds(631000));
    timeline.Record(NeuralColdStartPhase::Attach, std::chrono::microseconds(0));
    timeline.RecordTotal(std::chrono::microseconds(3041000));
    inputs.result.coldStart = timeline;

    const std::string json = BuildNeuralRenderReceiptJson(inputs);
    CHECK(Contains(json, "\"idleVramPolicy\":\"free\"},\"coldStartMicroseconds\":{\"total\":3041000,"
                         "\"request\":81000,\"preflight\":null,\"launch\":12000,\"helperStart\":712000,"
                         "\"runtimeReady\":94000,\"neuralInit\":1511000,\"featureArm\":631000,"
                         "\"firstOutput\":null,\"attach\":0}"));
    CHECK_EQ(std::string("total=3.041s request=0.081s preflight=- launch=0.012s helperStart=0.712s "
                         "runtimeReady=0.094s neuralInit=1.511s featureArm=0.631s firstOutput=- attach=0.000s"),
             SummarizeNeuralColdStartForLog(timeline));

    // A render that measured nothing - a refusal before the helper, or a build
    // whose helper never reported - must not read as a render that cost nothing.
    const std::string unmeasured = BuildNeuralRenderReceiptJson(SampleInputs());
    CHECK(Contains(unmeasured, "\"coldStartMicroseconds\":{\"total\":null,\"request\":null,\"preflight\":null,"
                               "\"launch\":null,\"helperStart\":null,\"runtimeReady\":null,\"neuralInit\":null,"
                               "\"featureArm\":null,\"firstOutput\":null,\"attach\":null}"));
    CHECK_EQ(std::string("total=- request=- preflight=- launch=- helperStart=- runtimeReady=- "
                         "neuralInit=- featureArm=- firstOutput=- attach=-"),
             SummarizeNeuralColdStartForLog({}));
}

} // namespace

int wmain()
{
    parse_accepts_the_lock_shape_and_normalizes_hashes_test();
    parse_rejects_malformed_documents_test();
    parse_rejects_missing_fields_and_wrong_schema_test();
    embedded_lock_parses_and_names_the_locked_runtime_files_test();
    verify_reports_each_drift_kind_and_names_only_failing_files_test();
    unlocked_runtime_modules_are_named_and_packaged_files_are_allowed_test();
    player_names_unlocked_modules_and_the_refusal_key_follows_the_module_listing_test();
    receipt_json_records_failure_lock_status_and_preflight_verbatim_test();
    receipt_log_summary_extracts_runtime_identity_and_lock_state_test();
    receipt_carries_the_cold_start_and_keeps_absent_phases_absent_test();
    feature18_create_result_is_read_out_of_the_runtime_log_test();
    neural_preflight_diagnosis_blames_the_actionable_cause_test();
    preflight_observations_are_bounded_to_fit_one_pipe_frame_test();
    return test_support::failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
