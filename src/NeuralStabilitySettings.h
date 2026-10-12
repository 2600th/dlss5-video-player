#pragma once

#include "AmdLmxxfPolicy.h"
#include "NeuralRenderTypes.h"
#include "TemporalStabilityPolicy.h"

#include <string>
#include <string_view>

// Where the temporal stability rung is written down, on the two sides of it that
// are not the pass itself: the settings document an AMD job's cache key is a
// digest of, and the INI key each backend remembers its rung under.
//
// Both are here, away from their call sites, because both are one fact with two
// spellings waiting to happen and a second spelling fails silently. The two
// switches are different features with different defaults - AMD's is this
// player's own accumulation over the runtime's output, where Off is not the
// neutral setting but the arm that flickers; NVIDIA's is the add-on's own ladder
// in its capture path, opt-in and Off until a comparison says otherwise
// (AmdLmxxfPolicy.h) - so a machine, which has one backend and so reads exactly
// one of these keys, must never be handed the other's answer.

// The AMD job's settings document, which is what the cache key's settings term
// is a digest of.
//
// There is no INI here and nothing to rewrite per job: this backend's knobs
// travel in the request, and the ones it has are the evaluator's - where its
// guides come from, whether the runtime's own temporal model runs, and how much
// of the host-side accumulation is blended back in. So this document says
// exactly that, in the terms the worker will use, rather than transcribing the
// ReShade panel the backend never reads. Those panel values are deliberately not
// hashed in either: a key term that changes no pixel would retire a render every
// time a slider moved, and the job's log line names them as asked-for instead.
//
// The two temporal entries are different features and both belong here.
// `temporal=` is the runtime's own model, which needs guides and the
// recording-lease lifecycle; `accumulation=` is this player's own pass over the
// runtime's output (TemporalStabilityPolicy.h), which needs neither. Each is a
// different picture, so each is a term.
//
// `accumulation` is written only off its Off value. Off is not "less of the
// pass" - it is the pass not running at all, which is the exact picture and the
// exact key this backend published before the pass existed, so a job that asks
// for it has to keep that key. Every other rung is spelled out, because two
// rungs blend differently and must never share an entry.
inline std::string AmdNeuralSettingsDocument(TemporalStability accumulation)
{
    std::string document = "backend=amd-lmxxf\n"
                           "temporal=off\n"
                           "motion=constant(0,0)\n"
                           "depth=constant(1)\n";
    if (accumulation != TemporalStability::Off)
        document += "accumulation=" + std::string(TemporalStabilityName(accumulation)) + "\n";
    return document;
}

// The key, under [Temporal], that `backend` remembers its rung under.
//
// Two keys and not one, because they are two switches: the answer a build with a
// single switch for both wrote down is NVIDIA's, and it must not become the rung
// an AMD render takes. Spelling this out at each call site - the INI read, the
// INI write, the parse - is how the two keys end up disagreeing about which
// backend is which.
inline const wchar_t* StabilitySettingsKey(NeuralBackend backend) noexcept
{
    return backend == NeuralBackend::Amd ? L"AmdStability" : L"Stability";
}

// The rung `backend` takes when its key holds nothing, or holds something that
// does not parse. Off on NVIDIA, where the pass is opt-in; this backend's own
// default on AMD, where it is not.
//
// One function rather than a literal per use, so the text the INI lookup falls
// back to and the rung the parse falls back to cannot drift apart. Malformed
// text falls back rather than failing: an INI a user has edited by hand is not a
// reason to refuse to play a video.
inline TemporalStability StabilitySettingsDefault(NeuralBackend backend) noexcept
{
    return backend == NeuralBackend::Amd ? amd_lmxxf::kAmdDefaultStability
                                        : TemporalStability::Off;
}

inline TemporalStability SavedStability(NeuralBackend backend, std::string_view saved) noexcept
{
    return ParseTemporalStability(saved).value_or(StabilitySettingsDefault(backend));
}
