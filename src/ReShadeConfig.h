#pragma once

#include <filesystem>
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
