#pragma once

#include "AmdLmxxfPolicy.h"
#include "NeuralRenderTypes.h"
#include "NeuralSettings.h"
#include "TemporalStabilityPolicy.h"

#include <algorithm>
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
//
// `passes` follows the same rule and for the same reason (default 1 writes
// nothing): the count is the one panel value this backend stacks the model
// with, so a render at two passes is a different picture and a different key,
// while 1 keys as every render before the control was wired keyed.
// The pass count a backend renders when nothing was saved. NVIDIA's add-on
// defaults to one pass and so does this player on it; the AMD backend defaults
// to the measured combination - three passes, prediction on, later-pass skip on
// - because stacking is where that backend's look lives and the skip is what
// keeps the ghosting at a single pass's level while fitting the frame budget.
inline constexpr uint32_t PassesSettingsDefault(NeuralBackend backend) noexcept
{
    return backend == NeuralBackend::Amd ? 3u : 1u;
}

inline std::string AmdNeuralSettingsDocument(TemporalStability accumulation, uint32_t passes = 1,
                                             bool skinProtect = false, bool multiPassPredict = true,
                                             bool laterPassSkip = false, uint32_t style = 1)
{
    std::string document = "backend=amd-lmxxf\n"
                           "temporal=off\n"
                           "motion=constant(0,0)\n"
                           "depth=constant(1)\n";
    if (accumulation != TemporalStability::Off)
        document += "accumulation=" + std::string(TemporalStabilityName(accumulation)) + "\n";
    if (passes != 1)
        document += "passes=" + std::to_string(passes) + "\n";
    // The style, off its default 1 - the runtime's own built-in answer and the
    // picture this backend keyed before the switch existed. Unlike the pass
    // count it belongs to the request rather than to a bring-up alone: a moved
    // style is a different picture at the same cost, so it is a key term and a
    // reuse term (the helper relaunches on it because the digest moves).
    if (style != 1)
        document += "style=" + std::to_string(style) + "\n";
    // Only with a count: the runtime blends the skin mask in only when later
    // passes exist, so at one pass this switch renders exactly what an off
    // switch renders, and a key term that claims otherwise would retire
    // coverage for a picture this backend would render the same either way.
    if (skinProtect && passes > 1)
        document += "skin-protect=1\n";
    // The prediction is a three-pass question: at any other count the runtime
    // never asks, so the key says nothing there whatever the switch says. At
    // three, the OFF spelling is the one written - on is the runtime's default
    // and the picture this backend keyed before the switch existed.
    if (passes == 3 && !multiPassPredict)
        document += "mp-predict=0\n";
    // Same rule as the prediction: a later-pass question, so the key says nothing
    // at one pass, and the ON spelling is the term because off is the built-in
    // default this backend keyed before the switch existed.
    if (passes > 1 && laterPassSkip)
        document += "mp-skip=1\n";
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

// The NeuralSettings term of a RETAINED live session's identity: what `backend`'s
// render actually reads out of the add-on's panel, spelled as the caller would
// spell it on the backend where all of it is read.
//
// On NVIDIA this is the whole canonical string, because every value on the panel
// reaches a pixel there (BackendReadsNeuralSettings). On AMD the run-time switches
// do - the pass count and the style (both the runtime's keys, passed in beside
// the panel's settings because the add-on does not own them) plus the switches
// that go with the count - and nothing else off the panel. The rung travels in
// the temporal term of the same key (CanonicalTemporalSettings); the panel's
// other values are still unreachable here, and hashing them would retire
// coverage a restart would have rendered identically - the exact waste the
// settings document's comment already ruled out for the render key, one key over.
//
// Keep this in lockstep with what RunAmdJob actually copies into the evaluator's
// config. A value added here before it reaches a pixel is a key term that lies;
// a value that reaches a pixel without being added here retires rendered work
// every time a slider moves.
inline std::string CanonicalNeuralSettingsForBackend(NeuralBackend backend,
                                                     const NeuralSettings& settings,
                                                     bool amdSkinProtect = false,
                                                     bool amdMultiPassPredict = true,
                                                     bool amdMultiPassSkip = false,
                                                     uint32_t amdStyle = 1)
{
    if (backend == NeuralBackend::Amd) {
        const uint32_t passes = std::clamp(settings.passes, 1, 4);
        // The skin switch lives beside the count in the player's own state
        // rather than in NeuralSettings - it is the runtime's, not the add-on's
        // - so it arrives as its own argument, and it counts only above one pass,
        // which is the same rule AmdNeuralSettingsDocument applies to it. In step
        // or coverage from one arm is served to the other. The style travels the
        // same road for the same reason: DLSS5_STYLE is the runtime's key, not
        // the add-on's NRStyle that NeuralSettings carries.
        std::string term = "passes=" + std::to_string(passes);
        if (amdSkinProtect && passes > 1) term += " skin-protect=1";
        if (passes == 3 && !amdMultiPassPredict) term += " mp-predict=0";
        if (passes > 1 && amdMultiPassSkip) term += " mp-skip=1";
        if (amdStyle != 1) term += " style=" + std::to_string(amdStyle);
        return term;
    }
    return CanonicalNeuralSettings(settings);
}
