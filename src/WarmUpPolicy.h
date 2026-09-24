#pragma once

#include <cstdint>

#include "NeuralRenderTypes.h"

// What a live session is doing before its first rendered second exists. That
// wait measured 16.9-21.9 s from the D press on an RTX 4080 SUPER, and for all
// of it the player said "Buffering neural frames · 0.0 s buffered" beside a
// render chip frozen at "0% · ETA 0:10" - an ETA made from a forecast, for a
// render that had not started. The steps below are the ones the cold start is
// made of (the `Neural cold start:` log line), named in words a viewer can
// follow, so the wait reads as progress rather than as a hang.
namespace warm_up {

enum class Step {
    None,            // not warming up: there is coverage, or no live render
    CheckingCache,   // the render key, which hashes the model store the first time
    CheckingRuntime, // the preflight probe of the neural runtime
    PreparingModel,  // helper up, feature being armed, no frame out yet
    FirstSecond,     // frames are coming out; the first segment is not published
};

inline Step Resolve(bool liveSession, bool jobActive, bool anyCoverage, NeuralRenderPhase phase,
                    uint64_t completedFrames)
{
    if (!liveSession || !jobActive || anyCoverage) return Step::None;
    switch (phase) {
    case NeuralRenderPhase::CheckingCache: return Step::CheckingCache;
    case NeuralRenderPhase::Preflight: return Step::CheckingRuntime;
    default: break;
    }
    return completedFrames == 0 ? Step::PreparingModel : Step::FirstSecond;
}

inline const wchar_t* TextKey(Step step)
{
    switch (step) {
    case Step::CheckingCache: return L"warmup.cache";
    case Step::CheckingRuntime: return L"warmup.runtime";
    case Step::PreparingModel: return L"warmup.model";
    case Step::FirstSecond: return L"warmup.first";
    case Step::None: break;
    }
    return nullptr;
}

} // namespace warm_up
