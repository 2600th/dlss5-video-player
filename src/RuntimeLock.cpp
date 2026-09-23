#include "RuntimeLock.h"

#include "NeuralCache.h"
#include "RuntimeLockData.h"
#include "StrictJson.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#pragma comment(lib, "version.lib")

namespace {

using strict_json::JsonParser;
using strict_json::JsonValue;

bool ReadUnsigned(const JsonValue* value, uint64_t& out)
{
    if (!value || value->kind != JsonValue::Kind::Number) return false;
    const std::string_view text = value->text;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

bool ReadString(const JsonValue* value, std::string& out)
{
    if (!value || value->kind != JsonValue::Kind::String) return false;
    out = value->text;
    return true;
}

std::wstring Widen(std::string_view utf8)
{
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), length);
    return wide;
}

bool LowercaseHex64(std::string& hex)
{
    if (hex.size() != 64) return false;
    for (char& c : hex) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        const bool digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!digit) return false;
    }
    return true;
}

std::wstring FileVersionText(const std::filesystem::path& path)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return {};
    std::vector<std::byte> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoBytes = 0;
    if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &infoBytes) || !info ||
        infoBytes < sizeof(VS_FIXEDFILEINFO)) return {};
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L'.' + std::to_wstring(LOWORD(info->dwFileVersionMS)) +
           L'.' + std::to_wstring(HIWORD(info->dwFileVersionLS)) + L'.' + std::to_wstring(LOWORD(info->dwFileVersionLS));
}

// "0.2026.0828.0517" and "0.2026.828.517" name the same VS_FIXEDFILEINFO;
// compare dotted versions numerically when both sides are four numbers.
bool DottedVersion(std::wstring_view text, std::array<uint64_t, 4>& parts)
{
    size_t index = 0;
    size_t pos = 0;
    while (index < 4) {
        const size_t dot = text.find(L'.', pos);
        const std::wstring_view part = text.substr(pos, dot == std::wstring_view::npos ? std::wstring_view::npos : dot - pos);
        if (part.empty() || part.size() > 10) return false;
        uint64_t value = 0;
        for (const wchar_t c : part) {
            if (c < L'0' || c > L'9') return false;
            value = value * 10 + static_cast<uint64_t>(c - L'0');
        }
        parts[index++] = value;
        if (dot == std::wstring_view::npos) break;
        pos = dot + 1;
    }
    return index == 4 && text.find(L'.', pos) == std::wstring_view::npos;
}

bool VersionsMatch(std::wstring_view expected, std::wstring_view actual)
{
    if (expected == actual) return true;
    std::array<uint64_t, 4> lhs{};
    std::array<uint64_t, 4> rhs{};
    return DottedVersion(expected, lhs) && DottedVersion(actual, rhs) && lhs == rhs;
}

} // namespace

std::optional<RuntimeLock> ParseRuntimeLock(std::string_view json)
{
    JsonValue root;
    if (!JsonParser(json).ParseDocument(root) || root.kind != JsonValue::Kind::Object) return std::nullopt;

    RuntimeLock lock;
    uint64_t schema = 0;
    if (!ReadUnsigned(root.Member("schemaVersion"), schema) || schema != 1) return std::nullopt;
    lock.schemaVersion = static_cast<uint32_t>(schema);
    if (!ReadString(root.Member("runtimeVersion"), lock.runtimeVersion)) return std::nullopt;

    const JsonValue* entries = root.Member("entries");
    if (!entries || entries->kind != JsonValue::Kind::Array) return std::nullopt;
    lock.entries.reserve(entries->items.size());
    for (const JsonValue& item : entries->items) {
        if (item.kind != JsonValue::Kind::Object) return std::nullopt;
        RuntimeLockEntry entry;
        std::string destination;
        std::string fileVersion;
        if (!ReadString(item.Member("destination"), destination) || destination.empty()) return std::nullopt;
        if (!ReadUnsigned(item.Member("size"), entry.size)) return std::nullopt;
        if (!ReadString(item.Member("sha256"), entry.sha256) || !LowercaseHex64(entry.sha256)) return std::nullopt;
        if (!ReadString(item.Member("fileVersion"), fileVersion)) return std::nullopt;
        entry.destination = Widen(destination);
        entry.fileVersion = Widen(fileVersion);
        if (entry.destination.empty()) return std::nullopt;
        lock.entries.push_back(std::move(entry));
    }
    return lock;
}

const RuntimeLock& EmbeddedRuntimeLock()
{
    // A build whose embedded lock does not parse has no runtime identity; the
    // empty lock is never satisfied (see RuntimeLockSatisfied), so a broken
    // build refuses every render instead of rendering unverified.
    static const RuntimeLock lock = ParseRuntimeLock(kRuntimeLockJson).value_or(RuntimeLock{});
    return lock;
}

std::vector<RuntimeLockCheck> VerifyRuntimeLock(const std::filesystem::path& runtimeDirectory,
                                                const RuntimeLock& lock,
                                                std::stop_token stop)
{
    std::vector<RuntimeLockCheck> checks;
    checks.reserve(lock.entries.size());
    for (const RuntimeLockEntry& entry : lock.entries) {
        RuntimeLockCheck check;
        check.name = entry.destination;
        const std::filesystem::path path = runtimeDirectory / entry.destination;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            checks.push_back(std::move(check));
            continue;
        }
        check.present = true;
        const uintmax_t size = std::filesystem::file_size(path, error);
        if (!error) {
            check.actualSize = static_cast<uint64_t>(size);
            check.sizeMatches = check.actualSize == entry.size;
        }
        if (!stop.stop_requested()) {
            // Memoised: BuildRuntimeDigest hashed this identical set moments ago.
            if (auto digest = Sha256FileCached(path, stop)) {
                check.actualSha256 = std::move(*digest);
                for (char& c : check.actualSha256) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                // Entries built by ParseRuntimeLock are lowercase already; a
                // hand-built entry may carry the packaging file's uppercase.
                check.hashMatches = check.actualSha256.size() == entry.sha256.size() &&
                                    std::equal(entry.sha256.begin(), entry.sha256.end(), check.actualSha256.begin(),
                                               [](char lhs, char rhs) {
                                                   return std::tolower(static_cast<unsigned char>(lhs)) == rhs;
                                               });
            }
        }
        check.actualFileVersion = FileVersionText(path);
        check.versionMatches = VersionsMatch(entry.fileVersion, check.actualFileVersion);
        checks.push_back(std::move(check));
    }
    return checks;
}

bool RuntimeLockSatisfied(std::span<const RuntimeLockCheck> checks)
{
    if (checks.empty()) return false;
    for (const RuntimeLockCheck& check : checks) {
        if (!check.Ok()) return false;
    }
    return true;
}

std::wstring DescribeRuntimeLockDrift(std::span<const RuntimeLockCheck> checks)
{
    std::wstring description;
    for (const RuntimeLockCheck& check : checks) {
        if (check.Ok()) continue;
        if (!description.empty()) description += L"; ";
        description += check.name;
        description += L": ";
        if (!check.present) {
            description += L"missing";
            continue;
        }
        bool first = true;
        auto separate = [&] {
            if (!first) description += L", ";
            first = false;
        };
        if (!check.sizeMatches) {
            separate();
            description += L"size " + std::to_wstring(check.actualSize);
        }
        if (!check.hashMatches) {
            separate();
            description += check.actualSha256.empty() ? L"unreadable" : L"hash mismatch";
        }
    }
    return description;
}

std::optional<std::vector<std::wstring>> FindUnlockedRuntimeModules(const std::filesystem::path& runtimeDirectory,
                                                                    const RuntimeLock& lock)
{
    const auto lower = [](std::wstring text) {
        for (wchar_t& c : text) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        return text;
    };
    std::vector<std::wstring> locked;
    locked.reserve(lock.entries.size());
    for (const RuntimeLockEntry& entry : lock.entries) locked.push_back(lower(entry.destination));

    std::vector<std::wstring> unlocked;
    std::error_code error;
    std::filesystem::directory_iterator it(runtimeDirectory, error);
    if (error) return std::nullopt;
    for (const std::filesystem::directory_iterator end{}; it != end; it.increment(error)) {
        if (error) return std::nullopt;
        std::error_code typeError;
        // Follows links: a symlinked DLL is loaded exactly like a copied one.
        if (!it->is_regular_file(typeError) || typeError) continue;
        const std::wstring name = it->path().filename().wstring();
        const std::wstring extension = lower(it->path().extension().wstring());
        if (extension != L".dll" && extension != L".addon" && extension != L".addon32" && extension != L".addon64")
            continue;
        if (std::find(locked.begin(), locked.end(), lower(name)) == locked.end()) unlocked.push_back(name);
    }
    if (error) return std::nullopt;
    std::sort(unlocked.begin(), unlocked.end());
    return unlocked;
}
