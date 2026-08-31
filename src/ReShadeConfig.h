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

ConfigUpdate EvaluateNeuralAddonConfigUpdate(
    std::string_view previousIni,
    std::string_view finalIni,
    bool changed,
    bool desiredEnabled);

ConfigUpdate ConfigureNeuralAddon(
    const std::filesystem::path& iniPath,
    bool enable);
