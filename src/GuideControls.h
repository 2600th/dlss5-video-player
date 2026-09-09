#pragma once

#include <optional>
#include <string>
#include <string_view>

// Per-guide enable switches for the reconstructed temporal guides. A disabled
// guide is still bound to NGX (the D3D12 contract rejects null resources) but
// carries a neutral value: zero motion, flat depth. Ablations vary
// these one at a time under identical warm-up conditions.
struct GuideControls {
    bool motionVectors{true};
    bool depth{true};

    friend bool operator==(const GuideControls&, const GuideControls&) = default;

    constexpr bool IsDefault() const noexcept { return motionVectors && depth; }
};

// Canonical form used for the worker argument, the cache identity term and
// log receipts: "mv=1,depth=1". Parsing accepts only that exact shape.
inline std::string CanonicalGuideControls(const GuideControls& controls)
{
    std::string text = "mv=";
    text += controls.motionVectors ? '1' : '0';
    text += ",depth=";
    text += controls.depth ? '1' : '0';
    return text;
}

inline std::optional<GuideControls> ParseGuideControls(std::string_view text)
{
    constexpr std::string_view mv = "mv=";
    constexpr std::string_view depth = ",depth=";
    if (text.size() != mv.size() + 1 + depth.size() + 1) return std::nullopt;
    if (text.substr(0, mv.size()) != mv) return std::nullopt;
    size_t offset = mv.size();
    auto flag = [&](bool& value) {
        const char character = text[offset++];
        if (character != '0' && character != '1') return false;
        value = character == '1';
        return true;
    };
    GuideControls controls;
    if (!flag(controls.motionVectors)) return std::nullopt;
    if (text.substr(offset, depth.size()) != depth) return std::nullopt;
    offset += depth.size();
    if (!flag(controls.depth)) return std::nullopt;
    return controls;
}
