#pragma once

#include <windows.h>

#include <climits>
#include <optional>
#include <string>
#include <string_view>

// UTF-16 <-> UTF-8, in the two forms the code needs, named for what each does
// with text that is not well formed (an unpaired surrogate in a file name, a
// Latin-1 byte in ffmpeg's output). NarrowText.h is the ASCII counterpart.
//
// Fifteen private converters existed in src/ (and three in the GPU smokes).
// They agreed on UTF-8 but not on failure: some answered "", some
// "<wide-string conversion failed>", and ffmpeg's diagnostics were decoded
// strictly, so one byte that was not UTF-8 - a tag in a legacy code page,
// which ffmpeg prints raw - threw away the whole message the user was about
// to be shown. CrashDump.h keeps its own call: its handler must not allocate.
namespace utf8_text {

// For text a person reads, and for identities that have always been digested
// this way: an unpaired surrogate becomes U+FFFD. Never fails for input below
// INT_MAX units; empty in, empty out.
inline std::string FromWide(std::wstring_view text)
{
    if (text.empty() || text.size() > INT_MAX) return {};
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string utf8(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), length, utf8.data(), size, nullptr, nullptr) != size)
        return {};
    return utf8;
}

// For what is stored and read back: refuses text that is not well formed
// rather than storing a replacement that no longer names the original.
inline std::optional<std::string> FromWideStrict(std::wstring_view text)
{
    if (text.empty()) return std::string{};
    if (text.size() > INT_MAX) return std::nullopt;
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return std::nullopt;
    std::string utf8(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, utf8.data(), size, nullptr,
                            nullptr) != size) return std::nullopt;
    return utf8;
}

// For diagnostics from other programs: a byte sequence that is not UTF-8
// becomes U+FFFD and the rest of the message survives.
inline std::wstring ToWide(std::string_view text)
{
    if (text.empty() || text.size() > INT_MAX) return {};
    const int length = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), size) != size) return {};
    return wide;
}

// For stored or protocol text: refuses bytes that are not UTF-8.
inline std::optional<std::wstring> ToWideStrict(std::string_view text)
{
    if (text.empty()) return std::wstring{};
    if (text.size() > INT_MAX) return std::nullopt;
    const int length = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (size <= 0) return std::nullopt;
    std::wstring wide(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(), size) != size)
        return std::nullopt;
    return wide;
}

} // namespace utf8_text
