#pragma once

#include "NeuralSettings.h"

#include <cstddef>
#include <string_view>

// Named starting points for the six neural controls.
//
// The controls are the right surface for someone who already knows what each
// one does. They are the wrong first contact: the repository's own issue #1 is
// "So what resolution do I have to give to this?", from someone who never got
// as far as the controls. A preset answers "what should I pick" in one click
// and leaves every slider reachable afterwards.
//
// Two rules this list keeps.
//
// The default is the best-looking setting, and it is also the recommended one.
// Nothing here trades picture quality for speed - every preset is one neural
// evaluation over the same frame at the same resolution, so they all cost the
// same. That is worth saying because it is the question a "preset" usually
// raises: the speed/quality trade in this player lives in Encoder settings
// (the NVENC preset, measured at 0.12 VMAF between p5 and p7 on ordinary
// content and 0.53 on noise-heavy), not here.
//
// No preset claims a quality number. The descriptions say which control moves
// and why, because that is what is actually known; inventing "sharper by X"
// would be off-voice against every other measured claim in this project.
namespace neural_presets {

struct Preset {
    // Stable key written to the ini, so a renamed label does not silently
    // change what a user had selected.
    std::string_view key;
    std::string_view label;
    // One line, shown beside the choice. Says what moves, not how good it is.
    std::string_view description;
    NeuralSettings settings;
};

// `Natural` first: it is the shipped default, and the order is the order the
// UI offers them in.
inline constexpr Preset kPresets[] = {
    {"natural", "Natural (recommended)",
     "Every control at its default. The render the project measures and the "
     "screenshots were taken with.",
     NeuralSettings{}},

    {"detail-only", "Detail only",
     "Keeps the source colour and lets the model change structure alone - "
     "NRColorStrength to 0. For material whose grade you do not want touched.",
     NeuralSettings{1.0f, 1.0f, 1.0f, -1.0f, 0.0f, 0, 0, true}},

    {"gentle", "Gentle",
     "Half intensity and softer local structure. For faces and film grain, "
     "where a full-strength render is the one that reads as waxy.",
     NeuralSettings{0.5f, 1.0f, 0.5f, 0.0f, 1.0f, 0, 0, true}},

    {"strong", "Strong",
     "Raised intensity and local structure. For heavily compressed sources - "
     "a low-bitrate stream - where the default leaves the artefacts visible.",
     NeuralSettings{1.5f, 1.25f, 1.5f, -1.0f, 1.0f, 0, 0, true}},
};

inline constexpr size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

// The shipped default, and the one a first run gets.
inline constexpr size_t kDefaultPresetIndex = 0;

// The preset whose settings these are, or kPresetCount when the user has moved
// a control away from all of them. "Custom" is a real state, not a failure:
// the presets are starting points and editing one afterwards is the expected
// path.
inline size_t IndexOf(const NeuralSettings& settings)
{
    for (size_t index = 0; index < kPresetCount; ++index)
        if (kPresets[index].settings == settings) return index;
    return kPresetCount;
}

// Lookup by the key persisted in the ini. kPresetCount for an unknown key, so
// a file written by a later version does not silently land on a preset that
// means something else.
inline size_t IndexOfKey(std::string_view key)
{
    for (size_t index = 0; index < kPresetCount; ++index)
        if (kPresets[index].key == key) return index;
    return kPresetCount;
}

} // namespace neural_presets
