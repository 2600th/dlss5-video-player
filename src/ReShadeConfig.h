#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

struct ConfigUpdate {
    bool ok{false};
    bool changed{false};
    bool previousAddonEnabled{false};
    bool addonEnabled{false};
    std::wstring error;
};

// Models ReShade 6.8's exact-case [ADDON]/DisabledAddons lookup and UTF-8 BOM
// handling. Throws std::invalid_argument for embedded NUL or duplicate exact
// DisabledAddons keys.
std::string UpdateDisabledAddonsIni(
    std::string_view ini,
    std::string_view addonName,
    bool disabled);

// Exact-case [RenoDX.DLSS5] key/value written after the managed contract keys.
// Keys must be non-empty, trimmed, not start a section or comment, and contain
// no '='; neither side may contain NUL or line breaks. UpdateNeuralAddonIni
// throws std::invalid_argument otherwise.
using NeuralAddonOverride = std::pair<std::string, std::string>;

// Applies the complete RenoDX neural-rendering contract used by this player.
// Enabling turns hooks and neural uplift on while explicitly keeping RenoDX's
// own upscaling path off, then writes each override into [RenoDX.DLSS5],
// replacing an existing exact-case key or appending at the section end.
// Disabling only disables the add-on, preserving the user's neural tuning for a
// later normal launch; overrides are not applied while disabling.
std::string UpdateNeuralAddonIni(
    std::string_view ini,
    bool enable,
    std::span<const NeuralAddonOverride> overrides = {});

ConfigUpdate EvaluateNeuralAddonConfigUpdate(
    std::string_view previousIni,
    std::string_view finalIni,
    bool changed,
    bool desiredEnabled);

ConfigUpdate ConfigureNeuralAddon(
    const std::filesystem::path& iniPath,
    bool enable,
    std::span<const NeuralAddonOverride> overrides = {});

// Canonical capture settings: neural add-on enable state and all exact-case
// [RenoDX.DLSS5] entries. Call after ConfigureNeuralAddon(..., true), then again
// after rendering without reconfiguring. Does not change the supplied settings.
// ReShade overlay/preset effects are not sampled by CaptureEvaluatedFrame.
// Throws std::invalid_argument for ambiguous neural entries or embedded NUL.
std::string SnapshotNeuralAddonSettings(std::string_view ini);
std::optional<std::string> ReadNeuralAddonSettingsSnapshot(
    const std::filesystem::path& iniPath, std::wstring* error = nullptr);
