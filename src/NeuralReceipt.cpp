#include "NeuralReceipt.h"

#include "NeuralPreflight.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace {

std::string Utf8(std::wstring_view text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
}

std::string_view Bool(bool value) noexcept
{
    return value ? "true" : "false";
}

std::string Number(double value)
{
    if (!std::isfinite(value)) return "null";
    char buffer[32];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) return "null";
    return std::string(buffer, end);
}

std::string Quoted(std::string_view text)
{
    return "\"" + JsonEscape(text) + "\"";
}

std::string QuotedWide(std::wstring_view text)
{
    return "\"" + JsonEscapeWide(text) + "\"";
}

std::string Iso8601Utc(std::chrono::system_clock::time_point at)
{
    const auto seconds = std::chrono::floor<std::chrono::seconds>(at);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(at - seconds).count();
    const std::time_t time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
    if (gmtime_s(&utc, &time) != 0) return {};
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(millis));
    return buffer;
}

std::string_view EncoderName(EncoderKind kind) noexcept
{
    return kind == EncoderKind::HevcNvenc ? "hevc_nvenc" : "h264_software";
}

std::string_view TrimmedPreflight(std::string_view json) noexcept
{
    constexpr std::string_view whitespace = " \t\r\n";
    const size_t first = json.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    const size_t last = json.find_last_not_of(whitespace);
    const std::string_view trimmed = json.substr(first, last - first + 1);
    return trimmed.front() == '{' && trimmed.back() == '}' ? trimmed : std::string_view{};
}

// Raw (still escaped) contents of the string member `field` inside the object
// member `section` of a flat preflight receipt. Empty when absent.
std::string_view ScanString(std::string_view json, std::string_view section, std::string_view field)
{
    std::string key = "\"";
    key += section;
    key += "\":{";
    const size_t sectionStart = json.find(key);
    if (sectionStart == std::string_view::npos) return {};
    const size_t sectionEnd = json.find('}', sectionStart);
    const std::string_view body = json.substr(sectionStart, sectionEnd == std::string_view::npos ? std::string_view::npos : sectionEnd - sectionStart);
    key = "\"";
    key += field;
    key += "\":\"";
    const size_t fieldStart = body.find(key);
    if (fieldStart == std::string_view::npos) return {};
    size_t pos = fieldStart + key.size();
    const size_t valueStart = pos;
    while (pos < body.size()) {
        if (body[pos] == '\\') {
            pos += 2;
            continue;
        }
        if (body[pos] == '"') break;
        ++pos;
    }
    return body.substr(valueStart, std::min(pos, body.size()) - valueStart);
}

std::string_view ScanLiteral(std::string_view json, std::string_view section, std::string_view field)
{
    std::string key = "\"";
    key += section;
    key += "\":{";
    const size_t sectionStart = json.find(key);
    if (sectionStart == std::string_view::npos) return {};
    key = "\"";
    key += field;
    key += "\":";
    const size_t fieldStart = json.find(key, sectionStart);
    if (fieldStart == std::string_view::npos) return {};
    const size_t valueStart = fieldStart + key.size();
    const size_t valueEnd = json.find_first_of(",}", valueStart);
    return json.substr(valueStart, valueEnd == std::string_view::npos ? std::string_view::npos : valueEnd - valueStart);
}

std::string_view OrDash(std::string_view value) noexcept
{
    return value.empty() ? std::string_view("-") : value;
}

// Microseconds as a JSON integer, or null for a phase that did not happen. The
// two are different claims and the receipt keeps them apart.
std::string Microseconds(std::optional<std::chrono::microseconds> elapsed)
{
    return elapsed ? std::to_string(elapsed->count()) : std::string("null");
}

// Seconds to millisecond resolution for the log line, which is the register the
// handoff's cold-start table is written in. A dash is a phase that did not run.
std::string Seconds(std::optional<std::chrono::microseconds> elapsed)
{
    if (!elapsed) return "-";
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.3fs", double(elapsed->count()) / 1000000.0);
    return buffer;
}

} // namespace

std::string BuildNeuralRenderReceiptJson(const NeuralRenderReceiptInputs& inputs)
{
    const NeuralRenderRequest& request = inputs.request;
    const NeuralRenderResult& result = inputs.result;
    std::string json;
    json.reserve(2048 + inputs.preflightJson.size());
    json += "{\"schema\":1";
    json += ",\"jobId\":" + std::to_string(request.jobId ? request.jobId : result.jobId);
    json += ",\"renderKey\":" + Quoted(inputs.renderKey);
    json += ",\"settingsDigest\":" + Quoted(inputs.settingsDigest);
    json += ",\"runtimeDigest\":" + Quoted(inputs.runtimeDigest);
    json += ",\"started\":" + Quoted(Iso8601Utc(inputs.started));
    json += ",\"finished\":" + Quoted(Iso8601Utc(inputs.finished));

    json += ",\"request\":{\"source\":" + QuotedWide(request.sourcePath.wstring());
    json += ",\"width\":" + std::to_string(request.width);
    json += ",\"height\":" + std::to_string(request.height);
    json += ",\"fps\":" + Number(request.fps);
    json += ",\"durationSeconds\":" + Number(request.durationSeconds);
    json += ",\"range\":{\"start\":" + std::to_string(request.range.start100ns) +
            ",\"end\":" + std::to_string(request.range.end100ns) + "}";
    json += ",\"prerollFrames\":" + std::to_string(request.prerollFrames);
    json += ",\"guides\":" + Quoted(CanonicalGuideControls(request.guides));
    json += ",\"frameRetryLimit\":" + std::to_string(request.frameRetryLimit);
    json += "}";

    const std::string_view preflight = TrimmedPreflight(inputs.preflightJson);
    json += ",\"preflight\":";
    json += preflight.empty() ? std::string_view("null") : preflight;

    json += ",\"lock\":{\"satisfied\":";
    json += Bool(RuntimeLockSatisfied(inputs.lockChecks));
    json += ",\"checks\":[";
    for (size_t index = 0; index < inputs.lockChecks.size(); ++index) {
        const RuntimeLockCheck& check = inputs.lockChecks[index];
        if (index) json += ',';
        json += "{\"name\":" + QuotedWide(check.name);
        json += ",\"present\":";
        json += Bool(check.present);
        json += ",\"sizeMatches\":";
        json += Bool(check.sizeMatches);
        json += ",\"hashMatches\":";
        json += Bool(check.hashMatches);
        json += ",\"versionMatches\":";
        json += Bool(check.versionMatches);
        json += ",\"actualSize\":" + std::to_string(check.actualSize);
        json += ",\"actualSha256\":" + Quoted(check.actualSha256);
        json += ",\"actualFileVersion\":" + QuotedWide(check.actualFileVersion) + "}";
    }
    json += "]}";

    json += ",\"result\":{\"ok\":";
    json += Bool(result.ok);
    json += ",\"cancelled\":";
    json += Bool(result.cancelled);
    json += ",\"failure\":" + Quoted(NeuralRenderFailureName(result.failure));
    json += ",\"encoder\":" + Quoted(EncoderName(result.encoder));
    json += ",\"frameCount\":" + std::to_string(result.frameCount);
    json += ",\"duration100ns\":" + std::to_string(result.duration100ns);
    json += ",\"nativeEvaluations\":" + std::to_string(result.nativeEvaluations);
    json += ",\"verifiedNeuralFrames\":" + std::to_string(result.verifiedNeuralFrames);
    json += ",\"feature18ArmedBeforeCapture\":";
    json += Bool(result.feature18ArmedBeforeCapture);
    const NeuralRuntimeEvidence& evidence = result.evidence;
    json += ",\"evidence\":{\"upscalingOff\":";
    json += Bool(evidence.upscalingOff);
    json += ",\"inlineInterceptionContract\":";
    json += Bool(evidence.inlineInterceptionContract);
    json += ",\"feature18Created\":";
    json += Bool(evidence.feature18Created);
    json += ",\"feature18Evaluated\":";
    json += Bool(evidence.feature18Evaluated);
    json += ",\"laterFailure\":";
    json += Bool(evidence.laterFailure);
    json += ",\"highestObservedEvaluation\":" + std::to_string(evidence.highestObservedEvaluation);
    json += ",\"valid\":";
    json += Bool(evidence.Valid());
    json += "}";
    json += ",\"historyResets\":" + std::to_string(result.historyResets);
    json += ",\"frameRetries\":" + std::to_string(result.frameRetries);
    const SceneCutAccounting& cuts = result.sceneCuts;
    json += ",\"sceneCuts\":{\"acceptedStrong\":" + std::to_string(cuts.acceptedStrong);
    json += ",\"acceptedWeak\":" + std::to_string(cuts.acceptedWeak);
    json += ",\"suppressed\":" + std::to_string(cuts.suppressed) + "}";
    json += ",\"firstTimestamp100ns\":" + std::to_string(result.firstTimestamp100ns);
    const NeuralRenderTiming& timing = result.timing;
    json += ",\"timing\":{\"samples\":" + std::to_string(timing.samples);
    json += ",\"neuralGpuMsP50\":" + Number(timing.neuralGpuMsP50);
    json += ",\"neuralGpuMsP95\":" + Number(timing.neuralGpuMsP95);
    json += ",\"neuralGpuMsMax\":" + Number(timing.neuralGpuMsMax);
    json += ",\"guideMsMean\":" + Number(timing.guideMsMean);
    json += ",\"captureMsMean\":" + Number(timing.captureMsMean);
    json += ",\"peakLocalVramMiB\":" + std::to_string(timing.peakLocalVramMiB) + "}";
    // Beside the per-frame distribution above, and in the same unit-in-the-key
    // convention: one group, one unit, a null for every phase that never ran.
    const NeuralColdStartTimeline& coldStart = result.coldStart;
    json += ",\"coldStartMicroseconds\":{\"total\":" + Microseconds(coldStart.Total());
    for (uint32_t index = 0; index < kNeuralColdStartPhaseCount; ++index) {
        const auto phase = static_cast<NeuralColdStartPhase>(index);
        json += ",\"" + std::string(NeuralColdStartPhaseName(phase)) + "\":" +
                Microseconds(coldStart.Phase(phase));
    }
    json += "}";
    json += ",\"detail\":" + QuotedWide(result.detail);
    json += "}}";
    return json;
}

std::string SummarizeNeuralReceiptForLog(const NeuralRenderReceiptInputs& inputs)
{
    const std::string_view preflight = inputs.preflightJson;
    const std::string_view armed = ScanLiteral(preflight, "feature18", "armed");
    std::string line = "gpu=\"";
    line += ScanString(preflight, "gpu", "description");
    line += "\" driver=";
    line += OrDash(ScanString(preflight, "gpu", "driverVersion"));
    line += " reshade=";
    line += OrDash(ScanString(preflight, "runtime", "reshade"));
    line += " renodx=";
    line += OrDash(ScanString(preflight, "runtime", "renodx"));
    line += " nr=";
    line += OrDash(ScanString(preflight, "runtime", "dlssnr"));
    line += " feature18=";
    line += armed.empty() ? "unknown" : (armed == "true" ? "armed" : "not-armed");
    // The feature's own NGX result, not the carrier's: a receipt that showed
    // only the carrier's success once hid a 0xbad00002 refusal here.
    const std::string_view createResult = ScanString(preflight, "feature18", "createResult");
    if (!createResult.empty()) {
        line += '(';
        line += createResult;
        line += ')';
    }
    const std::string_view cause = ScanString(preflight, "diagnosis", "cause");
    if (!cause.empty() && cause != "none") {
        line += " cause=";
        line += cause;
    }
    line += " lock=";
    if (RuntimeLockSatisfied(inputs.lockChecks)) {
        line += "ok";
    } else if (inputs.lockChecks.empty()) {
        line += "unverified";
    } else {
        line += "drift(";
        bool first = true;
        for (const RuntimeLockCheck& check : inputs.lockChecks) {
            if (check.Ok()) continue;
            if (!first) line += ',';
            first = false;
            line += Utf8(check.name);
        }
        line += ')';
    }
    const NeuralRenderResult& result = inputs.result;
    line += " failure=";
    line += NeuralRenderFailureName(result.failure);
    line += " frames=" + std::to_string(result.frameCount) + "/" + std::to_string(result.nativeEvaluations) +
            " verified=" + std::to_string(result.verifiedNeuralFrames) +
            " resets=" + std::to_string(result.historyResets) + " retries=" + std::to_string(result.frameRetries) +
            " cuts=" + std::to_string(result.sceneCuts.Accepted()) +
            " suppressed=" + std::to_string(result.sceneCuts.suppressed);
    return line;
}

std::string SummarizeNeuralColdStartForLog(const NeuralColdStartTimeline& timeline)
{
    // Total first: the acceptance criterion is written against it, and a reader
    // checking a claim must not have to add nine numbers to find it.
    std::string line = "total=" + Seconds(timeline.Total());
    for (uint32_t index = 0; index < kNeuralColdStartPhaseCount; ++index) {
        const auto phase = static_cast<NeuralColdStartPhase>(index);
        line += ' ';
        line += NeuralColdStartPhaseName(phase);
        line += '=';
        line += Seconds(timeline.Phase(phase));
    }
    return line;
}
