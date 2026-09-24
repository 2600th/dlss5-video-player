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
// Ranges: intensity/localTone/localStructure 0..2, skinStructure -1..1,
// colorStrength 0..1, preset 0..3, style 0..2, passes 1..4.
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
    float skinStructure{-1.0f};  // NRSkinStructure
    float colorStrength{1.0f};   // NRColorStrength
    int preset{0};               // NRPreset
    int style{0};                // NRStyle
    bool autoMask{true};         // NRAutoMask
    int passes{1};               // NRPasses
    bool chainedHistory{true};   // NRChainedHistory

    friend bool operator==(const NeuralSettings&, const NeuralSettings&) = default;
};

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
// absent keys at their defaults, clamps values into range and returns false
// when the section holds no recognised key. Save returns false when any key
// could not be written. Relative paths are resolved against the current
// directory before use.
bool LoadNeuralSettings(const std::filesystem::path& ini, NeuralSettings& settings);
bool SaveNeuralSettings(const std::filesystem::path& ini, const NeuralSettings& settings);

// One-line canonical form for logs, e.g.
// "intensity=1.000000 localTone=1.000000 ... preset=0 style=0 autoMask=1".
std::string CanonicalNeuralSettings(const NeuralSettings& settings);
