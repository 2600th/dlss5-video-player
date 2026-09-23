#pragma once

#include "SceneCut.h"

#include <optional>
#include <string>
#include <string_view>

// The render's temporal choices: how readily a scene cut resets the neural pass's
// history (P2.12). Every field changes output pixels, so the whole struct travels
// the way GuideControls does - one canonical string on the helper's command line,
// a cache-key term, and a receipt field - and every field defaults to what the
// renderer did before the setting existed.
//
// Kept apart from NeuralSettings on purpose: those are the RenoDX add-on's own
// [RenoDX.DLSS5] controls, written into ReShade.ini and digested with it, while
// these are decisions this player's own guide generator takes.
struct TemporalSettings {
    scene_cut::Sensitivity sceneCuts{scene_cut::Sensitivity::Default};

    friend bool operator==(const TemporalSettings&, const TemporalSettings&) = default;

    constexpr bool IsDefault() const noexcept { return *this == TemporalSettings{}; }
};

// Canonical form for the worker argument and the receipt: "cuts=default".
// Parsing accepts only that exact shape, the way ParseGuideControls does.
inline std::string CanonicalTemporalSettings(const TemporalSettings& settings)
{
    return "cuts=" + std::string(scene_cut::SensitivityName(settings.sceneCuts));
}

inline std::optional<TemporalSettings> ParseTemporalSettings(std::string_view text)
{
    constexpr std::string_view cuts = "cuts=";
    if (text.substr(0, cuts.size()) != cuts) return std::nullopt;
    const auto sensitivity = scene_cut::ParseSensitivity(text.substr(cuts.size()));
    if (!sensitivity) return std::nullopt;
    TemporalSettings settings;
    settings.sceneCuts = *sensitivity;
    return settings;
}

// The cache-key term, appended to NeuralRenderPipelineIdentity by the caller. Empty
// at the defaults, so every render published before these settings existed keeps
// the exact key it was published under; any other rung is spelled out, because two
// rungs reset the history on different frames and must never share an entry.
inline std::string TemporalPipelineTerm(const TemporalSettings& settings)
{
    std::string term;
    if (settings.sceneCuts != scene_cut::Sensitivity::Default)
        term += "|cuts-" + std::string(scene_cut::SensitivityName(settings.sceneCuts));
    return term;
}
