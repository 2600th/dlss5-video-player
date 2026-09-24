#pragma once

#include "ReShadeConfig.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Player-owned RenoDX 6.5.3 neural tuning. Field defaults, key names and value
// ranges follow the add-on's persisted [RenoDX.DLSS5] controls as exercised by
// the MIT-licensed DLSS5-Feeder project, confirmed against the pinned build's
// own overlay strings, and echoed in RenoDX's "active settings" log line.
// Ranges: intensity/localTone/localStructure 0..2, skinStructure Off or
// 0..0.99 (skin_structure below), colorStrength 0..1, preset 0..3, style 0..2,
// passes 1..4.
//
// `passes` and `chainedHistory` arrived with RenoDX 6.x and have no 4.70
// equivalent. Stacking is the one place where an offline video render and a
// game want opposite things: the add-on's own overlay warns a game off extra
// passes because each one costs a full neural evaluate against a frame budget,
// and this renderer has no frame budget - it has a progress bar. The cost is
// render time, which the pace forecast already measures and reports.
struct NeuralSettings {
    float intensity{1.0f};       // NRIntensity
    float localTone{1.0f};       // NRLocalTone
    float localStructure{1.0f};  // NRLocalStructure
    float skinStructure{-1.0f};  // NRSkinStructure; -1 is skin_structure::kOff
    float colorStrength{1.0f};   // NRColorStrength
    int preset{0};               // NRPreset
    int style{0};                // NRStyle
    bool autoMask{true};         // NRAutoMask
    int passes{1};               // NRPasses
    bool chainedHistory{true};   // NRChainedHistory

    friend bool operator==(const NeuralSettings&, const NeuralSettings&) = default;
};

// Skin structure as RenoDX 6.5.3 actually reads NRSkinStructure
// (docs/measurements/knobs-653-20260924): 0.00, 0.25, 0.50 and 0.99 each apply
// the term, while every negative value tried (-1.00, -0.50, -0.01) and exactly
// +1.00 render the default picture byte for byte, and with NRAutoMask=0 no value
// changes anything. The add-on's "-1..1, negative smooths" is not what runs, so
// the control is Off plus 0.00..0.99. Off is written as -1.000000, the value the
// player has always shipped, so the default render and its cache key stay put.
namespace skin_structure {
inline constexpr float kOff = -1.0f;
inline constexpr float kMax = 0.99f;
// The dialog's trackbar: 0 is Off, 1..100 are 0.00..0.99 in steps of 0.01.
inline constexpr int kSliderMax = 100;

inline bool IsOff(float value) { return !(value >= 0.0f && value < 1.0f); }

// What a saved or hand-edited value means now: a value the runtime renders as
// off becomes Off, and anything else is held inside 0..0.99. Idempotent, and it
// changes no rendered byte for any value the table measured.
inline float Normalize(float value)
{
    if (IsOff(value)) return kOff;
    return value > kMax ? kMax : value;
}

inline int SliderPosition(float value)
{
    if (IsOff(value)) return 0;
    const int hundredths = static_cast<int>(Normalize(value) * 100.0f + 0.5f);
    return hundredths + 1;
}

inline float FromSliderPosition(int position)
{
    if (position <= 0) return kOff;
    const int hundredths = position > kSliderMax ? kSliderMax - 1 : position - 1;
    return static_cast<float>(hundredths) / 100.0f;
}
} // namespace skin_structure

// The add-on's normalization governor (NRNormGovernor: 0 off, 1 slew, 2 stable),
// pinned to slew rather than left at the add-on's default of stable. Measured in
// docs/measurements/governor-20260924: at stable a repeat of one render differs
// from itself wherever the governor moves - 50.7 % of the bytes of a real clip
// with lighting changes, three distinct results from three renders on each of
// five clips - because it settles at rates per second of render time, and the
// render cache assumes a render is a function of its key. Off is reproducible
// but brings the pumping back (added frame-to-frame mean-luma change 0.45
// against 0.17 on flash-exposure); slew was identical in every render (four or
// five per clip) and damps as stable does, within 0.02-0.04. On the
// three real captures all three values render the same bytes. Not a setting:
// a render property, keyed through the settings snapshot like any other key.
inline constexpr std::string_view kPinnedNormGovernor = "1";

// Exact-case [RenoDX.DLSS5] overrides in field order, then the pinned governor.
// Floats use RenoDX's six-decimal form ("1.000000"), integers are plain and
// booleans are 0/1.
std::vector<NeuralAddonOverride> NeuralAddonOverridesFor(const NeuralSettings& settings);

// [NeuralSettings] section of the player's DLSSVideoPlayer.ini. Load leaves
// absent keys at their defaults, clamps values into range (a saved skin
// structure through skin_structure::Normalize, so a -0.50 or +1.00 an older
// build saved loads as the Off it always rendered as) and returns false
// when the section holds no recognised key. Save returns false when any key
// could not be written. Relative paths are resolved against the current
// directory before use.
bool LoadNeuralSettings(const std::filesystem::path& ini, NeuralSettings& settings);
bool SaveNeuralSettings(const std::filesystem::path& ini, const NeuralSettings& settings);

// One-line canonical form for logs, e.g.
// "intensity=1.000000 localTone=1.000000 ... preset=0 style=0 autoMask=1".
std::string CanonicalNeuralSettings(const NeuralSettings& settings);
