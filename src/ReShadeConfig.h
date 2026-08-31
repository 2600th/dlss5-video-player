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

// Throws std::invalid_argument when the INI contains an embedded NUL or more
// than one DisabledAddons key in its [ADDON] section.
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
