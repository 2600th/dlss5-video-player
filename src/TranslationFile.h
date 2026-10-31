#pragma once

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Utf8Text.h"

// A translation of the player's strings: one UTF-8 text file per language,
// `lang\<tag>.lang` beside DLSSVideoPlayer.exe, in the plainest form a
// contributor can edit in any editor and review in a diff:
//
//     # Simplified Chinese
//     menu.file = 文件
//     menu.rtx_vsr_unavailable = " — %s"
//
// One `key = text` per line. The key is Localization.h's; the text runs to the
// end of the line, trimmed, and is quoted only when it has to keep a space at
// either end. \n is a line break, \t a tab and \\ a backslash. `#` starts a
// comment line. Nothing in a file can stop the player: a line that is not a
// string is reported in the log and skipped, a key English does not have is
// ignored, and a string whose placeholders do not match English's is dropped,
// because a %s that went missing in translation is a crash in swprintf rather
// than a typo. Every key a file leaves out reads in English.
//
// Which file: the Windows display language, most preferred first (Settings >
// Time & language > Language), each tried from its full tag down to its bare
// language - zh-CN tries zh-Hans-CN, zh-Hans and zh, so the one Simplified
// Chinese file serves China and Singapore alike. `[Interface] Language` in
// DLSSVideoPlayer.ini, or `--lang <tag>`, picks one by name; `en` is English.
namespace translation {

using Map = std::unordered_map<std::wstring, std::wstring>;

inline constexpr std::wstring_view kExtension = L".lang";
inline constexpr std::wstring_view kDirectory = L"lang";

struct Parsed {
    Map strings;
    // "line 12: no '=' - a string is key = text", in file order. Logged, never shown.
    std::vector<std::wstring> problems;
};

namespace detail {

inline std::wstring_view Trim(std::wstring_view text)
{
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L'\r')) text.remove_suffix(1);
    return text;
}

inline bool KeyCharacter(wchar_t character)
{
    return (character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z') ||
           (character >= L'0' && character <= L'9') || character == L'.' || character == L'_' || character == L'-';
}

} // namespace detail

inline Parsed Parse(std::string_view utf8)
{
    Parsed parsed;
    // A BOM is what Notepad writes; it is not part of the first key.
    if (utf8.size() >= 3 && utf8.substr(0, 3) == "\xEF\xBB\xBF") utf8.remove_prefix(3);
    const auto problem = [&](size_t line, std::wstring_view what) {
        parsed.problems.push_back(L"line " + std::to_wstring(line) + L": " + std::wstring(what));
    };
    size_t number = 0;
    while (!utf8.empty()) {
        ++number;
        const size_t end = utf8.find('\n');
        const std::string_view raw = utf8.substr(0, end);
        utf8 = end == std::string_view::npos ? std::string_view{} : utf8.substr(end + 1);
        const std::optional<std::wstring> wide = utf8_text::ToWideStrict(raw);
        if (!wide) { problem(number, L"not UTF-8; save the file as UTF-8"); continue; }
        const std::wstring_view line = detail::Trim(*wide);
        if (line.empty() || line.front() == L'#') continue;
        const size_t equals = line.find(L'=');
        if (equals == std::wstring_view::npos) { problem(number, L"no '=' - a string is key = text"); continue; }
        const std::wstring_view key = detail::Trim(line.substr(0, equals));
        if (key.empty() || !std::all_of(key.begin(), key.end(), detail::KeyCharacter)) {
            problem(number, L"'" + std::wstring(key) + L"' is not a key");
            continue;
        }
        std::wstring_view value = detail::Trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
        std::wstring text;
        bool escapeError = false;
        for (size_t index = 0; index < value.size(); ++index) {
            if (value[index] != L'\\') { text.push_back(value[index]); continue; }
            const wchar_t next = index + 1 < value.size() ? value[index + 1] : L'\0';
            if (next == L'n') text.push_back(L'\n');
            else if (next == L't') text.push_back(L'\t');
            else if (next == L'\\') text.push_back(L'\\');
            else if (next == L'"') text.push_back(L'"');
            else { escapeError = true; break; }
            ++index;
        }
        if (escapeError) { problem(number, L"'" + std::wstring(key) + L"': only \\n, \\t, \\\\ and \\\" are escapes"); continue; }
        if (!parsed.strings.emplace(std::wstring(key), std::move(text)).second)
            problem(number, L"'" + std::wstring(key) + L"' is given twice; the first one is used");
    }
    return parsed;
}

// The file form of one string: the inverse of Parse, so a template written
// from English reads back as English.
inline std::wstring FormatValue(std::wstring_view text)
{
    std::wstring value;
    for (const wchar_t character : text) {
        if (character == L'\n') value += L"\\n";
        else if (character == L'\t') value += L"\\t";
        else if (character == L'\\') value += L"\\\\";
        else value.push_back(character);
    }
    const bool quoted = !value.empty() && (value.front() == L' ' || value.back() == L' ' ||
                                           (value.front() == L'"' && value.back() == L'"'));
    return quoted ? L"\"" + value + L"\"" : value;
}

// Every string, sorted by key, with a header: what `--lang-template` writes for
// a translator to start from.
inline std::string FormatFile(const Map& strings, std::wstring_view header)
{
    const std::map<std::wstring, std::wstring> sorted(strings.begin(), strings.end());
    std::wstring text;
    for (std::wstring_view rest = header; !rest.empty();) {
        const size_t end = rest.find(L'\n');
        text += L"# " + std::wstring(rest.substr(0, end)) + L"\r\n";
        rest = end == std::wstring_view::npos ? std::wstring_view{} : rest.substr(end + 1);
    }
    for (const auto& [key, value] : sorted) text += key + L" = " + FormatValue(value) + L"\r\n";
    return utf8_text::FromWide(text);
}

// The printf conversions in a string, in order, plus every tab: what a
// translation must carry over unchanged. "%%" is a literal and not one. A tab
// is the column a menu row's measured value sits in.
inline std::vector<std::wstring> Placeholders(std::wstring_view text)
{
    std::vector<std::wstring> found;
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\t') { found.emplace_back(L"\t"); continue; }
        if (text[index] != L'%') continue;
        if (index + 1 < text.size() && text[index + 1] == L'%') { ++index; continue; }
        size_t end = index + 1;
        while (end < text.size() && std::wstring_view(L"-+ #0123456789.*").find(text[end]) != std::wstring_view::npos) ++end;
        while (end < text.size() && std::wstring_view(L"hlLzjt").find(text[end]) != std::wstring_view::npos) ++end;
        if (end < text.size() && std::wstring_view(L"diouxXeEfFgGaAcsSp").find(text[end]) != std::wstring_view::npos) {
            found.emplace_back(text.substr(index, end - index + 1));
            index = end;
        }
    }
    return found;
}

// What a translation gets wrong against English, one line each: keys English
// does not have, keys it leaves out (only when `complete` is asked for, which
// is the rule for a file the project ships), and strings whose placeholders do
// not survive. Sorted, so a report is stable.
inline std::vector<std::wstring> CheckAgainst(const Map& english, const Map& translation, bool complete)
{
    std::vector<std::wstring> problems;
    for (const auto& [key, text] : translation) {
        const auto source = english.find(key);
        if (source == english.end()) { problems.push_back(key + L": not an English key"); continue; }
        if (Placeholders(source->second) != Placeholders(text))
            problems.push_back(key + L": its placeholders differ from English's");
    }
    if (complete)
        for (const auto& [key, text] : english)
            if (!translation.contains(key)) problems.push_back(key + L": missing");
    std::sort(problems.begin(), problems.end());
    return problems;
}

// The strings a running player may use from a parsed file: English keys only,
// and only where the placeholders match, so nothing a file says can make a
// format call read an argument that is not there.
inline Map Usable(const Map& english, Map translation)
{
    for (auto it = translation.begin(); it != translation.end();) {
        const auto source = english.find(it->first);
        if (source == english.end() || Placeholders(source->second) != Placeholders(it->second)) it = translation.erase(it);
        else ++it;
    }
    return translation;
}

// A string made longer the way a translation makes it longer - bracketed, so a
// clipped end shows, and padded by `expansion` of its length - with its
// placeholders, line breaks and tabs where they were. What the layout tests
// draw the longest strings with.
inline std::wstring Pseudo(std::wstring_view text, double expansion)
{
    const size_t tab = text.find(L'\t');
    const std::wstring_view label = text.substr(0, tab);
    const size_t padding = static_cast<size_t>(double(label.size()) * expansion + 0.999);
    std::wstring longer = L"[" + std::wstring(label) + std::wstring(padding, L'é') + L"]";
    if (tab != std::wstring_view::npos) longer += std::wstring(text.substr(tab));
    return longer;
}

// The files to try for one language tag, most specific first. `script` is the
// tag's writing system as Windows reports it ("Hans" for zh-CN), or empty:
// Windows names Chinese by region, the files name it by script, and the
// script is what decides which file a reader can read.
inline std::vector<std::wstring> FileCandidates(std::wstring_view tag, std::wstring_view script)
{
    std::vector<std::wstring> parts;
    for (std::wstring_view rest = tag; !rest.empty();) {
        const size_t dash = rest.find_first_of(L"-_");
        parts.emplace_back(rest.substr(0, dash));
        rest = dash == std::wstring_view::npos ? std::wstring_view{} : rest.substr(dash + 1);
    }
    std::vector<std::wstring> candidates;
    const auto add = [&](std::wstring candidate) {
        if (!candidate.empty() && std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
            candidates.push_back(std::move(candidate));
    };
    if (parts.empty()) return candidates;
    // A script subtag is four letters ("Hans"); one already in the tag wins.
    const auto isScript = [](const std::wstring& part) { return part.size() == 4 && std::iswalpha(part[0]); };
    const bool hasScript = parts.size() > 1 && isScript(parts[1]);
    std::wstring joined = parts[0];
    for (size_t index = 1; index < parts.size(); ++index) joined += L"-" + parts[index];
    add(joined);
    if (!hasScript && !script.empty()) {
        std::wstring withScript = parts[0] + L"-" + std::wstring(script);
        for (size_t index = 1; index < parts.size(); ++index) withScript += L"-" + parts[index];
        add(withScript);
        add(parts[0] + L"-" + std::wstring(script));
    }
    for (size_t count = parts.size() - 1; count >= 1; --count) {
        std::wstring prefix = parts[0];
        for (size_t index = 1; index < count; ++index) prefix += L"-" + parts[index];
        add(prefix);
    }
    return candidates;
}

// The Windows display languages, most preferred first ("zh-CN", "en-US").
inline std::vector<std::wstring> DisplayLanguages()
{
    ULONG count = 0, size = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size == 0) return {};
    std::wstring buffer(size, L'\0');
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size)) return {};
    std::vector<std::wstring> languages;
    for (const wchar_t* at = buffer.c_str(); *at; at += wcslen(at) + 1) languages.emplace_back(at);
    return languages;
}

// "Hans" for zh-CN, "Latn" for en-US: the first script Windows lists for it.
inline std::wstring ScriptOf(const std::wstring& tag)
{
    wchar_t scripts[64]{};
    if (!GetLocaleInfoEx(tag.c_str(), LOCALE_SSCRIPTS, scripts, static_cast<int>(std::size(scripts)))) return {};
    std::wstring script(scripts);
    return script.substr(0, script.find(L';'));
}

struct Loaded {
    Map strings;              // the usable strings, empty for English
    std::wstring language;    // the file's tag, "en" when none was found
    std::filesystem::path file;
    std::vector<std::wstring> problems;
};

inline std::optional<std::string> ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// The first file found for `requested` (a tag from the ini or the command line)
// or, when that is empty, for the display languages in order. English is the
// answer when the first language that matters is English, or when nothing is
// found: an English user with Chinese second in the list gets English.
inline Loaded Load(const std::filesystem::path& directory, std::wstring_view requested, const Map& english)
{
    Loaded loaded;
    loaded.language = L"en";
    std::vector<std::wstring> languages;
    if (!requested.empty()) languages.emplace_back(requested);
    else languages = DisplayLanguages();
    for (const std::wstring& language : languages) {
        const std::wstring script = requested.empty() ? ScriptOf(language) : std::wstring();
        for (const std::wstring& candidate : FileCandidates(language, script)) {
            if (_wcsicmp(candidate.c_str(), L"en") == 0) return loaded;
            const std::filesystem::path file = directory / (candidate + std::wstring(kExtension));
            const std::optional<std::string> bytes = ReadFile(file);
            if (!bytes) continue;
            Parsed parsed = Parse(*bytes);
            loaded.problems = std::move(parsed.problems);
            for (std::wstring& problem : CheckAgainst(english, parsed.strings, false)) loaded.problems.push_back(std::move(problem));
            loaded.strings = Usable(english, std::move(parsed.strings));
            loaded.language = candidate;
            loaded.file = file;
            return loaded;
        }
    }
    return loaded;
}

} // namespace translation
