#include "NeuralSettings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <optional>
#include <string_view>

namespace {

constexpr const wchar_t* kSection = L"NeuralSettings";

std::string FormatFloat(float value)
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.6f", static_cast<double>(value));
    return buffer;
}

std::wstring AbsoluteIni(const std::filesystem::path& ini)
{
    // The profile API treats relative names as Windows-directory files.
    std::error_code error;
    const auto absolute = std::filesystem::absolute(ini, error);
    return (error ? ini : absolute).wstring();
}

std::optional<std::wstring> ReadKey(const std::wstring& path, const wchar_t* key)
{
    constexpr wchar_t sentinel[] = L"\x01";
    wchar_t buffer[128]{};
    const DWORD length = GetPrivateProfileStringW(kSection, key, sentinel, buffer,
                                                  static_cast<DWORD>(std::size(buffer)),
                                                  path.c_str());
    if (std::wstring_view(buffer, length) == sentinel) return std::nullopt;
    return std::wstring(buffer, length);
}

bool ReadFloat(const std::wstring& path, const wchar_t* key, float& value, float low,
               float high)
{
    const auto text = ReadKey(path, key);
    if (!text) return false;
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(text->c_str(), &end);
    if (end && end != text->c_str() && std::isfinite(parsed))
        value = std::clamp(static_cast<float>(parsed), low, high);
    return true;
}

bool ReadInt(const std::wstring& path, const wchar_t* key, int& value, int low, int high)
{
    const auto text = ReadKey(path, key);
    if (!text) return false;
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text->c_str(), &end, 10);
    if (end && end != text->c_str()) value = std::clamp(static_cast<int>(parsed), low, high);
    return true;
}

bool ReadBool(const std::wstring& path, const wchar_t* key, bool& value)
{
    int number = value ? 1 : 0;
    if (!ReadInt(path, key, number, 0, 1)) return false;
    value = number == 1;
    return true;
}

bool WriteKey(const std::wstring& path, const wchar_t* key, const std::string& value)
{
    const std::wstring wide(value.begin(), value.end()); // ASCII digits only.
    return WritePrivateProfileStringW(kSection, key, wide.c_str(), path.c_str()) != FALSE;
}

} // namespace

std::vector<NeuralAddonOverride> NeuralAddonOverridesFor(const NeuralSettings& settings)
{
    return {
        {"NRIntensity", FormatFloat(settings.intensity)},
        {"NRLocalTone", FormatFloat(settings.localTone)},
        {"NRLocalStructure", FormatFloat(settings.localStructure)},
        {"NRSkinStructure", FormatFloat(settings.skinStructure)},
        {"NRColorStrength", FormatFloat(settings.colorStrength)},
        {"NRPreset", std::to_string(settings.preset)},
        {"NRStyle", std::to_string(settings.style)},
        {"NRAutoMask", settings.autoMask ? "1" : "0"},
    };
}

bool LoadNeuralSettings(const std::filesystem::path& ini, NeuralSettings& settings)
{
    const std::wstring path = AbsoluteIni(ini);
    const bool present[]{
        ReadFloat(path, L"Intensity", settings.intensity, 0.0f, 2.0f),
        ReadFloat(path, L"LocalTone", settings.localTone, 0.0f, 2.0f),
        ReadFloat(path, L"LocalStructure", settings.localStructure, 0.0f, 2.0f),
        ReadFloat(path, L"SkinStructure", settings.skinStructure, -1.0f, 1.0f),
        ReadFloat(path, L"ColorStrength", settings.colorStrength, 0.0f, 1.0f),
        ReadInt(path, L"Preset", settings.preset, 0, 3),
        ReadInt(path, L"Style", settings.style, 0, 2),
        ReadBool(path, L"AutoMask", settings.autoMask),
    };
    return std::ranges::any_of(present, [](bool value) { return value; });
}

bool SaveNeuralSettings(const std::filesystem::path& ini, const NeuralSettings& settings)
{
    const std::wstring path = AbsoluteIni(ini);
    const bool written[]{
        WriteKey(path, L"Intensity", FormatFloat(settings.intensity)),
        WriteKey(path, L"LocalTone", FormatFloat(settings.localTone)),
        WriteKey(path, L"LocalStructure", FormatFloat(settings.localStructure)),
        WriteKey(path, L"SkinStructure", FormatFloat(settings.skinStructure)),
        WriteKey(path, L"ColorStrength", FormatFloat(settings.colorStrength)),
        WriteKey(path, L"Preset", std::to_string(settings.preset)),
        WriteKey(path, L"Style", std::to_string(settings.style)),
        WriteKey(path, L"AutoMask", settings.autoMask ? "1" : "0"),
    };
    return std::ranges::all_of(written, [](bool value) { return value; });
}

std::string CanonicalNeuralSettings(const NeuralSettings& settings)
{
    return "intensity=" + FormatFloat(settings.intensity) +
        " localTone=" + FormatFloat(settings.localTone) +
        " localStructure=" + FormatFloat(settings.localStructure) +
        " skinStructure=" + FormatFloat(settings.skinStructure) +
        " colorStrength=" + FormatFloat(settings.colorStrength) +
        " preset=" + std::to_string(settings.preset) +
        " style=" + std::to_string(settings.style) +
        " autoMask=" + (settings.autoMask ? "1" : "0");
}
