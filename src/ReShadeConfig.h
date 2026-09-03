#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

// Applies the complete RenoDX neural-rendering contract used by this player.
// Enabling turns hooks and neural uplift on while explicitly keeping RenoDX's
// own upscaling path off. Disabling only disables the add-on, preserving the
// user's neural tuning for a later normal launch.
std::string UpdateNeuralAddonIni(
    std::string_view ini,
    bool enable);

ConfigUpdate EvaluateNeuralAddonConfigUpdate(
    std::string_view previousIni,
    std::string_view finalIni,
    bool changed,
    bool desiredEnabled);

ConfigUpdate ConfigureNeuralAddon(
    const std::filesystem::path& iniPath,
    bool enable);

// Canonical capture settings: neural add-on enable state and all exact-case
// [RenoDX.DLSS5] entries. Call after ConfigureNeuralAddon(..., true), then again
// after rendering without reconfiguring. Does not change the supplied settings.
// ReShade overlay/preset effects are not sampled by CaptureEvaluatedFrame.
// Throws std::invalid_argument for ambiguous neural entries or embedded NUL.
std::string SnapshotNeuralAddonSettings(std::string_view ini);
std::optional<std::string> ReadNeuralAddonSettingsSnapshot(
    const std::filesystem::path& iniPath, std::wstring* error = nullptr);
