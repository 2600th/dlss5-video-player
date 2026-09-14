#pragma once

// Neural render receipt: the durable record of what a render ran against
// (preflight probe, runtime lock verification, request) and what it produced
// (result, evidence, timing). Written beside the cached video and digested
// into the manifest so a replay can prove its provenance.

#include "OfflineNeuralRenderer.h"
#include "RuntimeLock.h"

#include <chrono>
#include <string>
#include <vector>

struct NeuralRenderReceiptInputs {
    std::string preflightJson; // worker preflight receipt, verbatim; empty when no probe ran
    std::vector<RuntimeLockCheck> lockChecks;
    NeuralRenderRequest request;
    NeuralRenderResult result;
    std::string renderKey;
    std::string settingsDigest;
    std::string runtimeDigest;
    std::chrono::system_clock::time_point started;
    std::chrono::system_clock::time_point finished;
};

// {"schema":1, jobId, renderKey, settingsDigest, runtimeDigest, started,
//  finished, request{...}, preflight:<object|null>, lock{satisfied,checks[]},
//  result{..., timing{...}, coldStartMicroseconds{total, request, preflight,
//  launch, helperStart, runtimeReady, neuralInit, featureArm, firstOutput,
//  attach}}}. A cold-start phase that did not happen is null, never 0.
// Single line, no trailing newline.
std::string BuildNeuralRenderReceiptJson(const NeuralRenderReceiptInputs& inputs);

// One log line: gpu/driver/reshade/renodx/nr/feature18 (with the feature's
// own NGX result and the diagnosed cause when the probe classified one) from
// the preflight JSON plus lock status, failure kind, frame counts and the
// scene-cut tally.
std::string SummarizeNeuralReceiptForLog(const NeuralRenderReceiptInputs& inputs);

// One log line: the measured request-to-picture total and every cold-start
// phase, in seconds to millisecond resolution, with "-" for a phase that did
// not happen. Emitted once per render so a user's log carries the whole
// breakdown without the receipt file.
std::string SummarizeNeuralColdStartForLog(const NeuralColdStartTimeline& timeline);
