#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Three ways to put wide text into a narrow string, named for what each does
// with the characters that do not fit: refuse, encode, or mangle.
//
// The codebase had both, unnamed and three lines apart in the same subsystem:
// NeuralWorker.cpp returned an empty string for anything outside ASCII while
// NeuralWorkerMain.cpp substituted '?'. Neither was wrong for its own caller -
// one builds identities, one writes log lines - but nothing said so, and
// reaching for the wrong one is silent.
//
// It had already gone wrong once. PreflightIdentity concatenated the strict
// form of the GPU and driver names, so two different cards whose names carry a
// non-ASCII character both narrowed to "" and produced the SAME identity: a
// "this GPU cannot do neural rendering" verdict probed on one card was then
// served for the other. A lossy identity is worse than no identity, because it
// compares equal.
namespace narrow_text {

// Refuses anything outside ASCII. For identity: a cache key, a protocol
// field, a filename, anything that will be compared for equality. Returning
// nullopt rather than a mangled string is the point - the caller has to decide
// what an unrepresentable name means, and "treat it as the same as every other
// unrepresentable name" is never the answer.
inline std::optional<std::string> StrictAscii(std::wstring_view text)
{
    std::string narrow;
    narrow.reserve(text.size());
    for (const wchar_t character : text) {
        if (character > 0x7F) return std::nullopt;
        narrow.push_back(static_cast<char>(character));
    }
    return narrow;
}

// Lossless: every input produces a distinct output, and the output is narrow
// ASCII. For identity, which is what StrictAscii's nullopt cannot express
// well - refusing a name means refusing the feature to everyone whose GPU is
// called something with a trademark sign in it, and re-running a five second
// preflight probe on every launch.
//
// ASCII passes through so an identity stays readable in a file; '%' escapes
// itself, and anything else becomes %XXXX of its UTF-16 code unit. Surrogate
// pairs encode as their two units, which is fine here: the requirement is
// injectivity, not a canonical Unicode form.
inline std::string IdentityEncoded(std::wstring_view text)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(text.size());
    for (const wchar_t character : text) {
        if (character == L'%') { encoded += "%%"; continue; }
        if (character < 0x80) { encoded.push_back(static_cast<char>(character)); continue; }
        const auto unit = static_cast<uint16_t>(character);
        encoded.push_back('%');
        encoded.push_back(kHex[(unit >> 12) & 0xF]);
        encoded.push_back(kHex[(unit >> 8) & 0xF]);
        encoded.push_back(kHex[(unit >> 4) & 0xF]);
        encoded.push_back(kHex[unit & 0xF]);
    }
    return encoded;
}

// Substitutes '?' for anything outside ASCII. For human-readable log lines
// only, where losing the message is worse than mangling one character of a
// path.
inline std::string LossyAscii(std::wstring_view text)
{
    std::string narrow;
    narrow.reserve(text.size());
    for (const wchar_t character : text)
        narrow.push_back(character < 128 ? static_cast<char>(character) : '?');
    return narrow;
}

} // namespace narrow_text
