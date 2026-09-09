#include "TestSupport.h"

#include "NeuralCache.h"
#include "NeuralPreflight.h"
#include "NeuralReceipt.h"
#include "RuntimeLock.h"

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
    const auto names = LockedRuntimeFileNames();
    CHECK_EQ(size_t{12}, names.size());
    CHECK_EQ(names.size(), lock.entries.size());
    for (const std::wstring_view name : names) {
        const bool listed = std::any_of(lock.entries.begin(), lock.entries.end(),
                                        [&](const RuntimeLockEntry& entry) { return entry.destination == name; });
        CHECK(listed);
    }
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

constexpr std::string_view kSamplePreflight =
    "{\"schema\":1,\"ok\":true,\"workerVersion\":\"1.2.3\",\"elapsedMilliseconds\":812,"
    "\"gpu\":{\"description\":\"NVIDIA GeForce RTX 4090\",\"vendorId\":4318,\"deviceId\":9988,"
    "\"dedicatedVideoMemoryMiB\":24564,\"driverVersion\":\"32.0.15.6164\"},"
    "\"runtime\":{\"reshade\":\"6.8.0.2155\",\"addon\":\"0.2026.828.517\",\"addonApi\":\"18\",\"renodx\":\"4.7\","
    "\"renodxBuild\":\"Sep  2 2026 01:15:39\",\"dlssnr\":\"310.8.0\",\"activeSettings\":\"upscaling=OFF\"},"
    "\"modules\":[{\"name\":\"dxgi.dll\",\"present\":true,\"size\":5592064,\"fileVersion\":\"6.8.0.2155\",\"sha256\":\"00\"}],"
    "\"feature18\":{\"created\":true,\"evaluated\":true,\"armed\":true,\"upscalingOff\":true,\"inlineInterception\":true,"
    "\"laterFailure\":false,\"highestEvaluation\":61,\"probeFrames\":61,\"ngxCreateResult\":1,\"observations\":[]}}";

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
    inputs.result.firstTimestamp100ns = 10000000;
    inputs.result.timing.samples = 48;
    inputs.result.timing.neuralGpuMsP50 = 4.25;
    inputs.result.timing.peakLocalVramMiB = 3072;
    inputs.result.detail = L"GPU stalled after 48 frames";
    inputs.renderKey = "key";
    inputs.settingsDigest = "settings";
    inputs.runtimeDigest = "runtime";
    inputs.started = std::chrono::system_clock::time_point{};
    inputs.finished = std::chrono::system_clock::time_point{} + std::chrono::milliseconds(90061001);
    return inputs;
}

void receipt_json_records_failure_lock_status_and_preflight_verbatim_test()
{
    const NeuralRenderReceiptInputs inputs = SampleInputs();
    const std::string json = BuildNeuralRenderReceiptJson(inputs);
    CHECK(json.starts_with("{\"schema\":1,\"jobId\":7,"));
    CHECK(json.ends_with("}}"));
    CHECK(Contains(json, "\"started\":\"1970-01-01T00:00:00.000Z\""));
    CHECK(Contains(json, "\"finished\":\"1970-01-02T01:01:01.001Z\""));
    CHECK(Contains(json, "\"source\":\"C:\\\\media\\\\clip \\\"one\\\".mkv\""));
    CHECK(Contains(json, "\"range\":{\"start\":10000000,\"end\":30000000}"));
    CHECK(Contains(json, "\"guides\":\"mv=1,depth=0\""));
    CHECK(Contains(json, "\"prerollFrames\":24"));
    CHECK(Contains(json, "\"preflight\":" + std::string(kSamplePreflight) + ",\"lock\":{\"satisfied\":false,\"checks\":["));
    CHECK(Contains(json, "{\"name\":\"nvngx_dlss.dll\",\"present\":true,\"sizeMatches\":true,\"hashMatches\":true,\"versionMatches\":true,\"actualSize\":58956400,"));
    CHECK(Contains(json, "{\"name\":\"dxgi.dll\",\"present\":true,\"sizeMatches\":true,\"hashMatches\":false,\"versionMatches\":false,\"actualSize\":5592064,\"actualSha256\":\"" + Hex("other") + "\",\"actualFileVersion\":\"\"}"));
    CHECK(Contains(json, "\"result\":{\"ok\":false,\"cancelled\":false,\"failure\":\"gpu-stall\",\"encoder\":\"h264_software\",\"frameCount\":48,"));
    CHECK(Contains(json, "\"historyResets\":2,\"frameRetries\":1,\"firstTimestamp100ns\":10000000,\"timing\":{\"samples\":48,\"neuralGpuMsP50\":4.25,"));
    CHECK(Contains(json, "\"peakLocalVramMiB\":3072}"));
    CHECK(Contains(json, "\"detail\":\"GPU stalled after 48 frames\""));
    CHECK(Contains(json, "\"evidence\":{\"upscalingOff\":false,"));
    CHECK(Contains(json, "\"valid\":false}"));

    NeuralRenderReceiptInputs noProbe = inputs;
    noProbe.preflightJson = "   \n";
    noProbe.lockChecks = {inputs.lockChecks[0]};
    noProbe.result.failure = NeuralRenderFailure::None;
    noProbe.result.ok = true;
    const std::string satisfied = BuildNeuralRenderReceiptJson(noProbe);
    CHECK(Contains(satisfied, "\"preflight\":null,\"lock\":{\"satisfied\":true,"));
    CHECK(Contains(satisfied, "\"failure\":\"none\""));

    NeuralRenderReceiptInputs partial = inputs;
    partial.preflightJson = "{\"schema\":1,\"ok\":false";
    CHECK(Contains(BuildNeuralRenderReceiptJson(partial), "\"preflight\":null,"));
}

void receipt_log_summary_extracts_runtime_identity_and_lock_state_test()
{
    const NeuralRenderReceiptInputs inputs = SampleInputs();
    CHECK_EQ(std::string("gpu=\"NVIDIA GeForce RTX 4090\" driver=32.0.15.6164 reshade=6.8.0.2155 renodx=4.7 nr=310.8.0 "
                         "feature18=armed lock=drift(dxgi.dll) failure=gpu-stall frames=48/48 verified=48 resets=2 retries=1"),
             SummarizeNeuralReceiptForLog(inputs));

    NeuralRenderReceiptInputs bare = inputs;
    bare.preflightJson.clear();
    bare.lockChecks.clear();
    bare.result.failure = NeuralRenderFailure::Preflight;
    CHECK_EQ(std::string("gpu=\"\" driver=- reshade=- renodx=- nr=- feature18=unknown lock=unverified failure=preflight "
                         "frames=48/48 verified=48 resets=2 retries=1"),
             SummarizeNeuralReceiptForLog(bare));

    NeuralRenderReceiptInputs notArmed = inputs;
    notArmed.preflightJson = "{\"schema\":1,\"ok\":false,\"gpu\":{\"description\":\"Escaped \\\"GPU\\\"\",\"driverVersion\":\"\"},"
                             "\"feature18\":{\"created\":true,\"evaluated\":false,\"armed\":false}}";
    notArmed.lockChecks = {inputs.lockChecks[0]};
    CHECK_EQ(std::string("gpu=\"Escaped \\\"GPU\\\"\" driver=- reshade=- renodx=- nr=- feature18=not-armed lock=ok "
                         "failure=gpu-stall frames=48/48 verified=48 resets=2 retries=1"),
             SummarizeNeuralReceiptForLog(notArmed));
}

} // namespace

int wmain()
{
    parse_accepts_the_lock_shape_and_normalizes_hashes_test();
    parse_rejects_malformed_documents_test();
    parse_rejects_missing_fields_and_wrong_schema_test();
    embedded_lock_parses_and_names_the_locked_runtime_files_test();
    verify_reports_each_drift_kind_and_names_only_failing_files_test();
    receipt_json_records_failure_lock_status_and_preflight_verbatim_test();
    receipt_log_summary_extracts_runtime_identity_and_lock_state_test();
    return test_support::failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
