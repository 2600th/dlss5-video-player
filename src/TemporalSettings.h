#pragma once

#include "SceneCut.h"
#include "TemporalStabilityPolicy.h"

#include <optional>
#include <string>
#include <string_view>

// The render's temporal choices: how readily a scene cut resets the neural pass's
// history (P2.12), and how much of the motion-compensated history the capture
// blends back in (P2.5). Every field changes output pixels, so the whole struct
// travels the way GuideControls does - one canonical string on the helper's
// command line, a cache-key term, and a receipt field - and every field defaults
// to what the renderer did before the setting existed.
//
// Kept apart from NeuralSettings on purpose: those are the RenoDX add-on's own
// [RenoDX.DLSS5] controls, written into ReShade.ini and digested with it, while
// these are decisions this player's own guide generator and capture take.
struct TemporalSettings {
    scene_cut::Sensitivity sceneCuts{scene_cut::Sensitivity::Default};
    TemporalStability stability{TemporalStability::Off};

    friend bool operator==(const TemporalSettings&, const TemporalSettings&) = default;

    constexpr bool IsDefault() const noexcept { return *this == TemporalSettings{}; }
};

// Canonical form for the worker argument and the receipt:
// "cuts=default,stability=off". Parsing accepts only that exact shape, the way
// ParseGuideControls does.
inline std::string CanonicalTemporalSettings(const TemporalSettings& settings)
{
    return "cuts=" + std::string(scene_cut::SensitivityName(settings.sceneCuts)) +
           ",stability=" + std::string(TemporalStabilityName(settings.stability));
}

inline std::optional<TemporalSettings> ParseTemporalSettings(std::string_view text)
{
    constexpr std::string_view cuts = "cuts=";
    constexpr std::string_view stability = ",stability=";
    if (text.substr(0, cuts.size()) != cuts) return std::nullopt;
    text.remove_prefix(cuts.size());
    const size_t separator = text.find(',');
    if (separator == std::string_view::npos) return std::nullopt;
    const auto sensitivity = scene_cut::ParseSensitivity(text.substr(0, separator));
    text.remove_prefix(separator);
    if (!sensitivity || text.substr(0, stability.size()) != stability) return std::nullopt;
    const auto level = ParseTemporalStability(text.substr(stability.size()));
    if (!level) return std::nullopt;
    TemporalSettings settings;
    settings.sceneCuts = *sensitivity;
    settings.stability = *level;
    return settings;
}

// The cache-key term, appended to NeuralRenderPipelineIdentity by the caller. Empty
// at the defaults, so every render published before these settings existed keeps
// the exact key it was published under; any other rung is spelled out, because two
// rungs reset or blend differently and must never share an entry. The stability
// term carries a version: the pass is an algorithm of this player's, and a change
// to it has to be able to retire what the previous one cached.
inline std::string TemporalPipelineTerm(const TemporalSettings& settings)
{
    std::string term;
    if (settings.sceneCuts != scene_cut::Sensitivity::Default)
        term += "|cuts-" + std::string(scene_cut::SensitivityName(settings.sceneCuts));
    if (settings.stability != TemporalStability::Off)
        term += "|stability-" + std::string(TemporalStabilityName(settings.stability)) + "-v1";
    return term;
}
