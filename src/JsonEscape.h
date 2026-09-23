#pragma once

#include <string>
#include <string_view>

// The body of a JSON string literal for UTF-8 `text`, without the quotes: '"'
// and '\\' escaped, \b \f \n \r \t in their short forms, and every other
// control character as \u00XX. Everything else, including bytes >= 0x80, is
// copied, so valid UTF-8 in is valid JSON out.
//
// There were two, and they disagreed. The cache manifest's copy returned an
// EMPTY STRING for a control character without a short form, so the whole
// field - an environment term of the render identity, say - was written as ""
// and read back as a different, valid value. The preflight receipt's copy
// escaped it, but as \u0008 and \u000c where the manifest wrote \b and \f.
// This one escapes everything and keeps the manifest's bytes for every string
// it could already write; the receipt's JSON changes only for \b and \f, to
// the equivalent short form.
inline std::string JsonEscape(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const unsigned char character : text) {
        switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (character < 0x20) {
                constexpr char digits[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += digits[character >> 4];
                escaped += digits[character & 0xF];
            } else {
                escaped += static_cast<char>(character);
            }
        }
    }
    return escaped;
}
