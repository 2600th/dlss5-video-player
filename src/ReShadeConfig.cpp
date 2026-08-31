#include "ReShadeConfig.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

struct Line {
    size_t begin{};
    size_t contentEnd{};
    size_t end{};
};

struct ParsedUpdate {
    bool malformed{false};
    std::string error;
    std::string content;
};

std::string_view Trim(std::string_view value)
{
    const size_t first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        const unsigned char leftCharacter = static_cast<unsigned char>(left[index]);
        const unsigned char rightCharacter = static_cast<unsigned char>(right[index]);
        if (std::tolower(leftCharacter) != std::tolower(rightCharacter)) {
            return false;
        }
    }
    return true;
}

std::vector<Line> SplitLines(std::string_view ini)
{
    std::vector<Line> lines;
    for (size_t begin = 0; begin < ini.size();) {
        size_t contentEnd = begin;
        while (contentEnd < ini.size() && ini[contentEnd] != '\r' && ini[contentEnd] != '\n') {
            ++contentEnd;
        }
        size_t end = contentEnd;
        if (end < ini.size() && ini[end] == '\r') {
            ++end;
            if (end < ini.size() && ini[end] == '\n') {
                ++end;
            }
        } else if (end < ini.size()) {
            ++end;
        }
        lines.push_back({begin, contentEnd, end});
        begin = end;
    }
    return lines;
}

bool IsSection(std::string_view line, std::string_view sectionName)
{
    line = Trim(line);
    return line.size() >= 2 && line.front() == '[' && line.back() == ']'
        && EqualsIgnoreCase(Trim(line.substr(1, line.size() - 2)), sectionName);
}

bool IsKey(std::string_view line, std::string_view keyName, size_t& valueStart)
{
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos || !EqualsIgnoreCase(Trim(line.substr(0, equals)), keyName)) {
        return false;
    }
    valueStart = equals + 1;
    return true;
}

std::string DetectLineEnding(std::string_view ini)
{
    for (size_t index = 0; index < ini.size(); ++index) {
        if (ini[index] == '\r') {
            return index + 1 < ini.size() && ini[index + 1] == '\n' ? "\r\n" : "\r";
        }
        if (ini[index] == '\n') {
            return "\n";
        }
    }
    return "\r\n";
}

std::string Join(const std::vector<std::string_view>& values)
{
    std::string joined;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            joined.push_back(',');
        }
        joined.append(values[index]);
    }
    return joined;
}

std::string UpdateList(std::string_view list, std::string_view addonName, bool disabled)
{
    std::vector<std::string_view> entries;
    for (size_t begin = 0;;) {
        const size_t comma = list.find(',', begin);
        entries.push_back(list.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }

    size_t targetCount = 0;
    for (const std::string_view entry : entries) {
        if (entry == addonName) {
            ++targetCount;
        }
    }

    if ((!disabled && targetCount == 0) || (disabled && targetCount == 1)) {
        return std::string(list);
    }
    if (disabled && targetCount == 0) {
        return list.empty() ? std::string(addonName) : std::string(list) + ',' + std::string(addonName);
    }

    std::vector<std::string_view> retained;
    bool keptTarget = false;
    for (const std::string_view entry : entries) {
        if (entry != addonName) {
            retained.push_back(entry);
        } else if (disabled && !keptTarget) {
            retained.push_back(entry);
            keptTarget = true;
        }
    }
    return Join(retained);
}

ParsedUpdate ParseAndUpdate(std::string_view ini, std::string_view addonName, bool disabled)
{
    if (ini.find('\0') != std::string_view::npos) {
        return {true, "INI contains an embedded NUL"};
    }

    const std::vector<Line> lines = SplitLines(ini);
    size_t addonHeader = std::string_view::npos;
    size_t addonSectionEnd = ini.size();
    std::vector<size_t> disabledKeys;
    bool inAddonSection = false;

    for (size_t index = 0; index < lines.size(); ++index) {
        const Line& line = lines[index];
        const std::string_view content = ini.substr(line.begin, line.contentEnd - line.begin);
        if (IsSection(content, "ADDON")) {
            if (inAddonSection) {
                addonSectionEnd = line.begin;
                inAddonSection = false;
            }
            if (addonHeader == std::string_view::npos) {
                addonHeader = index;
                inAddonSection = true;
            }
            continue;
        }
        const std::string_view trimmed = Trim(content);
        if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']') {
            if (inAddonSection) {
                addonSectionEnd = line.begin;
                inAddonSection = false;
            }
            continue;
        }
        if (inAddonSection) {
            size_t valueStart = 0;
            if (IsKey(content, "DisabledAddons", valueStart)) {
                disabledKeys.push_back(index);
            }
        }
    }

    if (disabledKeys.size() > 1) {
        return {true, "[ADDON] contains duplicate DisabledAddons keys"};
    }

    const std::string lineEnding = DetectLineEnding(ini);
    if (addonHeader == std::string_view::npos) {
        std::string content(ini);
        if (!content.empty() && content.back() != '\r' && content.back() != '\n') {
            content.append(lineEnding);
        }
        content.append("[ADDON]");
        content.append(lineEnding);
        content.append("DisabledAddons=");
        if (disabled) {
            content.append(addonName);
        }
        content.append(lineEnding);
        return {false, {}, std::move(content)};
    }

    if (disabledKeys.empty()) {
        std::string content(ini);
        if (addonSectionEnd > 0 && content[addonSectionEnd - 1] != '\r' && content[addonSectionEnd - 1] != '\n') {
            content.insert(addonSectionEnd, lineEnding);
            addonSectionEnd += lineEnding.size();
        }
        std::string key = "DisabledAddons=";
        if (disabled) {
            key.append(addonName);
        }
        key.append(lineEnding);
        content.insert(addonSectionEnd, key);
        return {false, {}, std::move(content)};
    }

    const Line& disabledLine = lines[disabledKeys.front()];
    const std::string_view line = ini.substr(disabledLine.begin, disabledLine.contentEnd - disabledLine.begin);
    size_t valueStart = 0;
    IsKey(line, "DisabledAddons", valueStart);
    const std::string updatedList = UpdateList(line.substr(valueStart), addonName, disabled);
    if (updatedList == line.substr(valueStart)) {
        return {false, {}, std::string(ini)};
    }

    std::string content(ini);
    content.replace(disabledLine.begin + valueStart, disabledLine.contentEnd - (disabledLine.begin + valueStart), updatedList);
    return {false, {}, std::move(content)};
}

std::wstring Win32Error(std::wstring_view operation)
{
    return std::wstring(operation) + L" failed (Win32 error " + std::to_wstring(GetLastError()) + L")";
}

ConfigUpdate WriteUpdatedIni(const std::filesystem::path& iniPath, std::string_view content, bool addonEnabled)
{
    const std::filesystem::path directory = iniPath.has_parent_path() ? iniPath.parent_path() : std::filesystem::current_path();
    std::wstring temporary(MAX_PATH, L'\0');
    const std::wstring prefix = L"RDX";
    if (GetTempFileNameW(directory.c_str(), prefix.c_str(), 0, temporary.data()) == 0) {
        return {false, false, false, Win32Error(L"Creating ReShade.ini temporary file")};
    }
    temporary.resize(std::wcslen(temporary.c_str()));

    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const std::wstring error = Win32Error(L"Opening ReShade.ini temporary file");
        DeleteFileW(temporary.c_str());
        return {false, false, false, error};
    }

    bool wrote = true;
    DWORD written = 0;
    size_t offset = 0;
    while (offset < content.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(content.size() - offset, MAXDWORD));
        if (!WriteFile(file, content.data() + offset, chunk, &written, nullptr) || written != chunk) {
            wrote = false;
            break;
        }
        offset += written;
    }
    const bool flushed = wrote && FlushFileBuffers(file) != FALSE;
    const DWORD writeError = wrote && !flushed ? GetLastError() : (wrote ? ERROR_SUCCESS : GetLastError());
    CloseHandle(file);
    if (!wrote || !flushed) {
        DeleteFileW(temporary.c_str());
        return {false, false, false, L"Writing ReShade.ini temporary file failed (Win32 error " + std::to_wstring(writeError) + L")"};
    }

    if (!MoveFileExW(temporary.c_str(), iniPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::wstring error = Win32Error(L"Replacing ReShade.ini");
        DeleteFileW(temporary.c_str());
        return {false, false, false, error};
    }
    return {true, true, addonEnabled, {}};
}

} // namespace

std::string UpdateDisabledAddonsIni(std::string_view ini, std::string_view addonName, bool disabled)
{
    const ParsedUpdate updated = ParseAndUpdate(ini, addonName, disabled);
    if (updated.malformed) {
        throw std::invalid_argument(updated.error);
    }
    return updated.content;
}

ConfigUpdate ConfigureNeuralAddon(const std::filesystem::path& iniPath, bool enable)
{
    std::ifstream input(iniPath, std::ios::binary);
    if (!input) {
        return {false, false, false, L"Unable to open ReShade.ini"};
    }
    const std::string original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return {false, false, false, L"Unable to read ReShade.ini"};
    }
    input.close();

    constexpr std::string_view neuralAddon = "renodx-dlss5.addon64";
    const ParsedUpdate updated = ParseAndUpdate(original, neuralAddon, !enable);
    if (updated.malformed) {
        return {false, false, false, std::wstring(updated.error.begin(), updated.error.end())};
    }
    if (updated.content == original) {
        return {true, false, enable, {}};
    }
    return WriteUpdatedIni(iniPath, updated.content, enable);
}
