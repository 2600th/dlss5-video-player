#pragma once

#include "ReShadeConfig.h"

#include <filesystem>
#include <string>
#include <vector>

// Player-owned RenoDX 4.70 neural tuning. Field defaults, key names and value
// ranges follow the add-on's persisted [RenoDX.DLSS5] controls as exercised by
// the MIT-licensed DLSS5-Feeder project and echoed in RenoDX's
// "active settings" log line. Ranges: intensity/localTone/localStructure 0..2,
// skinStructure -1..1, colorStrength 0..1, preset 0..3, style 0..2.
struct NeuralSettings {
    float intensity{1.0f};       // NRIntensity
    float localTone{1.0f};       // NRLocalTone
    float localStructure{1.0f};  // NRLocalStructure
    float skinStructure{-1.0f};  // NRSkinStructure
    float colorStrength{1.0f};   // NRColorStrength
    int preset{0};               // NRPreset
    int style{0};                // NRStyle
    bool autoMask{true};         // NRAutoMask

    friend bool operator==(const NeuralSettings&, const NeuralSettings&) = default;
};

// Exact-case [RenoDX.DLSS5] overrides in field order. Floats use RenoDX's
// six-decimal form ("1.000000"), integers are plain and booleans are 0/1.
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
