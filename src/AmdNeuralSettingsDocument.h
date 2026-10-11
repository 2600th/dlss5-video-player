#pragma once

#include "NeuralSettings.h"

#include <cstdio>
#include <string>

// The AMD job's settings document: what the staged neural-settings.ini holds,
// and what the cache key's settings term is a digest of.
//
// There is no INI on this backend and nothing to rewrite per job - the runtime
// loads no proxy and reads no ReShade.ini - so this document says what the job
// actually renders with, in the terms the worker uses, rather than transcribing
// the ReShade panel the backend never reads.
//
// What is NOT in it, deliberately, and in both directions: Local tone, Local
// structure, Skin structure, Style, Preset, Passes, Auto mask, Chained history.
// Those reach the model through the add-on's own keys in a ReShade.ini this
// backend never loads, so hashing them would retire a render every time a
// slider moved without changing one byte - and hashing none of them is only
// honest while this backend cannot read them at all. The job's log line names
// them as asked-for instead (NeuralJobRun::AnswerFromCache).
//
// The two strength terms are the exception, and that is the whole point of them
// being here: they are the model's INPUT rather than the add-on's tuning. On
// this backend they reach LmxxfNrFrameInfo::transfer_strength and color_strength
// directly, so they change every frame of a render, and a render made with one
// is not the render made with another. Without them in the key a moved slider
// would be answered from the cache entry of the previous value.
//
// Written only when they are off their defaults, so a render at the defaults
// has the same document it had before they were wired up - and therefore the
// same key, and every AMD cache entry already on disk stays valid.
inline std::string AmdNeuralSettingsDocument(const NeuralSettings& settings)
{
    std::string document =
        "backend=amd-lmxxf\n"
        "temporal=off\n"
        "motion=constant(0,0)\n"
        "depth=constant(1)\n";
    // Six decimals, the form the add-on's own keys and this codebase's settings
    // dumps both use (NeuralSettings.cpp's FormatFloat), so a document and a
    // "settings=" log line are read the same way. This is a key term rather than
    // a wire value, so the round-trip float text BuildWorkerArguments uses for
    // the command line is not what belongs here: two texts that differ only in
    // spelling would be two cache identities for one render.
    const auto format = [](float value) {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "%.6f", static_cast<double>(value));
        return std::string(buffer);
    };
    if (settings.intensity != 1.0f) document += "transfer=" + format(settings.intensity) + "\n";
    if (settings.colorStrength != 1.0f) document += "color=" + format(settings.colorStrength) + "\n";
    return document;
}
