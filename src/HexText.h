#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>

// Hex for log lines and receipts: "0x" and every digit of the value's width,
// lower case. An HRESULT or an NGX result is always eight digits, a LUID or a
// GPU virtual address sixteen.
//
// The receipts have always written NGX results this way ("0xbad00002"), but
// the 45 log sites that formatted with std::hex dropped leading zeros, so
// "result=0x1" in a log could not be found by searching for the receipt's
// "0x00000001". Several also left the stream in hex, so a width or a frame
// number later in the same line came out in hex without a prefix.
namespace hex_text_detail {
template <typename Value>
struct Bits { using type = Value; };
template <typename Value>
    requires std::is_enum_v<Value>
struct Bits<Value> { using type = std::underlying_type_t<Value>; };
} // namespace hex_text_detail

template <typename Value>
    requires(std::is_integral_v<Value> || std::is_enum_v<Value>) && (!std::same_as<Value, bool>)
std::string HexText(Value value)
{
    using Unsigned = std::make_unsigned_t<typename hex_text_detail::Bits<Value>::type>;
    const auto bits = static_cast<Unsigned>(value);
    std::string text = "0x";
    for (int shift = static_cast<int>(sizeof(Unsigned) * 8) - 4; shift >= 0; shift -= 4)
        text.push_back("0123456789abcdef"[(bits >> shift) & 0xFu]);
    return text;
}

// The eight-digit form of an NGX result code, as the preflight receipt writes
// it; the probe reports the same codes in its JSON.
inline std::string HexResultText(uint32_t value) { return HexText(value); }

// The same digits as a wide string, for the diagnostics that are shown to a
// user or carried in a std::wstring reason (the DLSS-G probe's capability
// detail among them).
inline std::wstring HexResultTextWide(uint32_t value)
{
    const std::string text = HexText(value);
    return std::wstring(text.begin(), text.end());
}
