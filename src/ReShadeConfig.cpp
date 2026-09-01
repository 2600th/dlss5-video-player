#include "ReShadeConfig.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {

constexpr std::string_view kNeuralAddonCanonical =
    "DLSS 5 Neural Rendering@renodx-dlss5.addon64";
constexpr std::string_view kNeuralAddonRegisteredName = "DLSS 5 Neural Rendering";
constexpr std::string_view kNeuralAddonAtFilename = "@renodx-dlss5.addon64";
constexpr std::string_view kNeuralAddonLegacyFilename = "renodx-dlss5.addon64";
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

struct Line {
    size_t begin{};
    size_t contentEnd{};
    size_t end{};
};

struct ParsedUpdate {
    bool malformed{false};
    std::string error;
    std::string content;
    bool addonEnabled{true};
};

struct ReadResult {
    bool ok{false};
    std::string content;
    std::wstring error;
};

struct WriteResult {
    bool ok{false};
    std::wstring error;
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

std::string_view StripUtf8Bom(std::string_view value, bool firstLine)
{
    if (firstLine && value.starts_with(kUtf8Bom)) {
        value.remove_prefix(kUtf8Bom.size());
    }
    return value;
}

bool IsSection(std::string_view line, std::string_view sectionName, bool firstLine)
{
    line = Trim(StripUtf8Bom(line, firstLine));
    return line.size() >= 2 && line.front() == '[' && line.back() == ']'
        && Trim(line.substr(1, line.size() - 2)) == sectionName;
}

bool IsKey(std::string_view line, std::string_view keyName, size_t& valueStart)
{
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos || Trim(line.substr(0, equals)) != keyName) {
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

std::string_view LineEnding(std::string_view ini, const Line& line)
{
    return ini.substr(line.contentEnd, line.end - line.contentEnd);
}

std::string DetectTargetSectionLineEnding(
    std::string_view ini,
    const std::vector<Line>& lines,
    size_t addonHeader,
    size_t addonSectionEnd)
{
    if (addonHeader != std::string_view::npos) {
        const std::string_view headerEnding = LineEnding(ini, lines[addonHeader]);
        if (!headerEnding.empty()) return std::string(headerEnding);
        for (size_t index = addonHeader + 1; index < lines.size() && lines[index].begin < addonSectionEnd; ++index) {
            const std::string_view ending = LineEnding(ini, lines[index]);
            if (!ending.empty()) return std::string(ending);
        }
    }
    return DetectLineEnding(ini);
}

bool IsNeuralAddon(std::string_view addonName)
{
    return addonName == kNeuralAddonCanonical;
}

bool IsManagedNeuralAlias(std::string_view entry)
{
    entry = Trim(entry);
    return EqualsIgnoreCase(entry, kNeuralAddonCanonical)
        || EqualsIgnoreCase(entry, kNeuralAddonRegisteredName)
        || EqualsIgnoreCase(entry, kNeuralAddonAtFilename)
        || EqualsIgnoreCase(entry, kNeuralAddonLegacyFilename);
}

bool IsReShadeDisabledNeuralToken(std::string_view entry)
{
    entry = Trim(entry);
    return entry == kNeuralAddonCanonical
        || entry == kNeuralAddonRegisteredName
        || entry == kNeuralAddonAtFilename;
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

    const bool neuralAddon = IsNeuralAddon(addonName);
    size_t targetCount = 0;
    for (const std::string_view entry : entries) {
        if (neuralAddon ? IsManagedNeuralAlias(entry) : Trim(entry) == addonName) {
            ++targetCount;
        }
    }

    if (!disabled && targetCount == 0) {
        return std::string(list);
    }
    if (disabled && targetCount == 1) {
        for (const std::string_view entry : entries) {
            if ((neuralAddon ? IsManagedNeuralAlias(entry) : Trim(entry) == addonName)
                && Trim(entry) == addonName) {
                return std::string(list);
            }
        }
    }
    if (disabled && targetCount == 0) {
        return list.empty() ? std::string(addonName) : std::string(list) + ',' + std::string(addonName);
    }

    std::vector<std::string> retained;
    bool keptTarget = false;
    for (const std::string_view entry : entries) {
        const bool isTarget = neuralAddon ? IsManagedNeuralAlias(entry) : Trim(entry) == addonName;
        if (!isTarget) {
            retained.emplace_back(entry);
        } else if (disabled && !keptTarget) {
            retained.emplace_back(addonName);
            keptTarget = true;
        }
    }
    std::string joined;
    for (size_t index = 0; index < retained.size(); ++index) {
        if (index != 0) joined.push_back(',');
        joined.append(retained[index]);
    }
    return joined;
}

bool ListContainsAddon(std::string_view list, std::string_view addonName)
{
    for (size_t begin = 0;;) {
        const size_t comma = list.find(',', begin);
        const std::string_view entry = list.substr(
            begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
        if (IsNeuralAddon(addonName)
                ? IsReShadeDisabledNeuralToken(entry)
                : Trim(entry) == addonName) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        begin = comma + 1;
    }
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
        const std::string_view trimmed = Trim(StripUtf8Bom(content, index == 0));
        const bool isAddonSection = IsSection(content, "ADDON", index == 0);
        const bool isAnySection = !trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']';
        if (isAnySection) {
            if (inAddonSection && addonSectionEnd == ini.size()) {
                addonSectionEnd = line.begin;
            }
            inAddonSection = isAddonSection;
            if (isAddonSection && addonHeader == std::string_view::npos) {
                addonHeader = index;
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

    const std::string lineEnding = DetectTargetSectionLineEnding(ini, lines, addonHeader, addonSectionEnd);
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
    const std::string_view originalList = line.substr(valueStart);
    const bool addonEnabled = !ListContainsAddon(originalList, addonName);
    const std::string updatedList = UpdateList(originalList, addonName, disabled);
    if (updatedList == line.substr(valueStart)) {
        return {false, {}, std::string(ini), addonEnabled};
    }

    std::string content(ini);
    content.replace(disabledLine.begin + valueStart, disabledLine.contentEnd - (disabledLine.begin + valueStart), updatedList);
    return {false, {}, std::move(content), addonEnabled};
}

std::wstring Win32Error(std::wstring_view operation)
{
    return std::wstring(operation) + L" failed (Win32 error " + std::to_wstring(GetLastError()) + L")";
}

ReadResult ReadIniFile(const std::filesystem::path& iniPath)
{
    std::error_code sizeError;
    const uintmax_t fileSize = std::filesystem::file_size(iniPath, sizeError);
    if (sizeError || fileSize > static_cast<uintmax_t>(std::string{}.max_size())
        || fileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return {false, {}, L"Unable to determine a readable ReShade.ini size"};
    }

    std::ifstream input(iniPath, std::ios::binary);
    if (!input) {
        return {false, {}, L"Unable to open ReShade.ini"};
    }
    std::string content(static_cast<size_t>(fileSize), '\0');
    if (!content.empty()) {
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
    }
    if (!input || input.gcount() != static_cast<std::streamsize>(content.size())) {
        return {false, {}, L"Unable to read ReShade.ini"};
    }
    if (input.peek() != std::char_traits<char>::eof() || input.bad()) {
        return {false, {}, L"ReShade.ini changed while it was being read"};
    }
    input.close();
    if (input.fail()) {
        return {false, {}, L"Unable to close ReShade.ini after reading"};
    }
    return {true, std::move(content), {}};
}

WriteResult WriteUpdatedIni(const std::filesystem::path& iniPath, std::string_view content)
{
    const std::filesystem::path directory = iniPath.has_parent_path() ? iniPath.parent_path() : std::filesystem::current_path();
    std::wstring temporary(MAX_PATH, L'\0');
    const std::wstring prefix = L"RDX";
    if (GetTempFileNameW(directory.c_str(), prefix.c_str(), 0, temporary.data()) == 0) {
        return {false, Win32Error(L"Creating ReShade.ini temporary file")};
    }
    temporary.resize(std::wcslen(temporary.c_str()));

    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const std::wstring error = Win32Error(L"Opening ReShade.ini temporary file");
        DeleteFileW(temporary.c_str());
        return {false, error};
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
        return {false, L"Writing ReShade.ini temporary file failed (Win32 error " + std::to_wstring(writeError) + L")"};
    }

    if (!MoveFileExW(temporary.c_str(), iniPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::wstring error = Win32Error(L"Replacing ReShade.ini");
        DeleteFileW(temporary.c_str());
        return {false, error};
    }
    return {true, {}};
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

ConfigUpdate EvaluateNeuralAddonConfigUpdate(
    std::string_view previousIni,
    std::string_view finalIni,
    bool changed,
    bool desiredEnabled)
{
    const ParsedUpdate previous = ParseAndUpdate(previousIni, kNeuralAddonCanonical, !desiredEnabled);
    if (previous.malformed) {
        return {false, changed, false, false, std::wstring(previous.error.begin(), previous.error.end())};
    }
    const ParsedUpdate final = ParseAndUpdate(finalIni, kNeuralAddonCanonical, !desiredEnabled);
    if (final.malformed) {
        return {false, changed, previous.addonEnabled, false, std::wstring(final.error.begin(), final.error.end())};
    }
    if (final.addonEnabled != desiredEnabled) {
        return {false, changed, previous.addonEnabled, final.addonEnabled,
            L"Observed neural add-on state does not match the requested state"};
    }
    return {true, changed, previous.addonEnabled, final.addonEnabled, {}};
}

ConfigUpdate ConfigureNeuralAddon(const std::filesystem::path& iniPath, bool enable)
{
    const ReadResult originalRead = ReadIniFile(iniPath);
    if (!originalRead.ok) {
        return {false, false, false, false, originalRead.error};
    }

    const ParsedUpdate updated = ParseAndUpdate(originalRead.content, kNeuralAddonCanonical, !enable);
    if (updated.malformed) {
        return {false, false, false, false, std::wstring(updated.error.begin(), updated.error.end())};
    }
    if (updated.content == originalRead.content) {
        return EvaluateNeuralAddonConfigUpdate(originalRead.content, originalRead.content, false, enable);
    }

    const WriteResult write = WriteUpdatedIni(iniPath, updated.content);
    if (!write.ok) {
        return {false, false, updated.addonEnabled, false, write.error};
    }
    const ReadResult finalRead = ReadIniFile(iniPath);
    if (!finalRead.ok) {
        return {false, true, updated.addonEnabled, false,
            L"Unable to verify updated ReShade.ini: " + finalRead.error};
    }
    return EvaluateNeuralAddonConfigUpdate(originalRead.content, finalRead.content, true, enable);
}
