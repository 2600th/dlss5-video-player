#include "RuntimeLock.h"

#include "NeuralCache.h"
#include "RuntimeLockData.h"

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

// Minimal strict JSON document model: enough for the lock file and nothing
// more. Numbers keep their source text so integers are converted exactly.
struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind{Kind::Null};
    bool boolean{};
    std::string text; // Number: source text; String: decoded UTF-8
    std::vector<JsonValue> items; // Array items or Object values
    std::vector<std::string> keys; // Object keys, parallel to items

    const JsonValue* Member(std::string_view key) const
    {
        if (kind != Kind::Object) return nullptr;
        for (size_t index = 0; index < keys.size(); ++index) {
            if (keys[index] == key) return &items[index];
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool ParseDocument(JsonValue& out)
    {
        SkipWhitespace();
        if (!ParseValue(out, 0)) return false;
        SkipWhitespace();
        return pos_ == text_.size();
    }

private:
    static constexpr size_t kMaxDepth = 32;

    std::string_view text_;
    size_t pos_{};

    bool AtEnd() const { return pos_ >= text_.size(); }
    char Peek() const { return text_[pos_]; }

    void SkipWhitespace()
    {
        while (!AtEnd() && (Peek() == ' ' || Peek() == '\t' || Peek() == '\n' || Peek() == '\r')) ++pos_;
    }

    bool Consume(char expected)
    {
        if (AtEnd() || Peek() != expected) return false;
        ++pos_;
        return true;
    }

    bool ConsumeLiteral(std::string_view literal)
    {
        if (text_.substr(pos_, literal.size()) != literal) return false;
        pos_ += literal.size();
        return true;
    }

    bool ParseValue(JsonValue& out, size_t depth)
    {
        if (AtEnd() || depth > kMaxDepth) return false;
        switch (Peek()) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"':
                out.kind = JsonValue::Kind::String;
                return ParseString(out.text);
            case 't':
                out.kind = JsonValue::Kind::Bool;
                out.boolean = true;
                return ConsumeLiteral("true");
            case 'f':
                out.kind = JsonValue::Kind::Bool;
                out.boolean = false;
                return ConsumeLiteral("false");
            case 'n':
                out.kind = JsonValue::Kind::Null;
                return ConsumeLiteral("null");
            default: return ParseNumber(out);
        }
    }

    bool ParseObject(JsonValue& out, size_t depth)
    {
        out.kind = JsonValue::Kind::Object;
        if (!Consume('{')) return false;
        SkipWhitespace();
        if (Consume('}')) return true;
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (AtEnd() || Peek() != '"' || !ParseString(key)) return false;
            SkipWhitespace();
            if (!Consume(':')) return false;
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.keys.push_back(std::move(key));
            out.items.push_back(std::move(value));
            SkipWhitespace();
            if (Consume(',')) continue;
            return Consume('}');
        }
    }

    bool ParseArray(JsonValue& out, size_t depth)
    {
        out.kind = JsonValue::Kind::Array;
        if (!Consume('[')) return false;
        SkipWhitespace();
        if (Consume(']')) return true;
        for (;;) {
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.items.push_back(std::move(value));
            SkipWhitespace();
            if (Consume(',')) continue;
            return Consume(']');
        }
    }

    bool ParseHex4(uint32_t& code)
    {
        if (pos_ + 4 > text_.size()) return false;
        code = 0;
        for (size_t index = 0; index < 4; ++index) {
            const char c = text_[pos_ + index];
            uint32_t digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
            else return false;
            code = (code << 4) | digit;
        }
        pos_ += 4;
        return true;
    }

    static void AppendUtf8(std::string& out, uint32_t code)
    {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool ParseString(std::string& out)
    {
        if (!Consume('"')) return false;
        for (;;) {
            if (AtEnd()) return false;
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false; // raw control characters are invalid
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (AtEnd()) return false;
            const char escape = text_[pos_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t code = 0;
                    if (!ParseHex4(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        uint32_t low = 0;
                        if (!ConsumeLiteral("\\u") || !ParseHex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return false; // lone low surrogate
                    }
                    AppendUtf8(out, code);
                    break;
                }
                default: return false;
            }
        }
    }

    bool ParseNumber(JsonValue& out)
    {
        out.kind = JsonValue::Kind::Number;
        const size_t start = pos_;
        Consume('-');
        if (AtEnd()) return false;
        if (Peek() == '0') {
            ++pos_;
        } else if (Peek() >= '1' && Peek() <= '9') {
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
        } else {
            return false;
        }
        if (Consume('.')) {
            const size_t fractionStart = pos_;
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
            if (pos_ == fractionStart) return false;
        }
        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            ++pos_;
            if (!Consume('+')) Consume('-');
            const size_t exponentStart = pos_;
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
            if (pos_ == exponentStart) return false;
        }
        out.text.assign(text_.substr(start, pos_ - start));
        return true;
    }
};

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
