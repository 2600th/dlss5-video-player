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
//  result{...}}. Single line, no trailing newline.
std::string BuildNeuralRenderReceiptJson(const NeuralRenderReceiptInputs& inputs);

// One log line: gpu/driver/reshade/renodx/nr/feature18 (with the feature's
// own NGX result and the diagnosed cause when the probe classified one) from
// the preflight JSON plus lock status, failure kind and frame counts.
std::string SummarizeNeuralReceiptForLog(const NeuralRenderReceiptInputs& inputs);
