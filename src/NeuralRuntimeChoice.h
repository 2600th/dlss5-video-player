#pragma once

#include "RuntimePolicy.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

// Which DLSS-NR runtime a render loads, and which of its networks (P3.14, P3.5).
//
// neural-runtime/ carries two NR DLLs beside one RenoDX add-on (10.8.0), and the
// add-on loads the one its NRRuntimePath names, resolved against its own folder:
//
//   nvngx_dlssnr.dll               310.9.16, two networks (A and B)
//   310.8.SF-v2/nvngx_dlssnr.dll   310.8.SF-v2, the 0.28 runtime, network A only
//
// Measured in docs/measurements/runtime-310916-20261031 (RTX 5090, 621.01).
// NRPreset reaches DLSSNR.Hint.Render.Preset: on 310.9.16 preset 1 renders
// byte-identical to 310.8.SF-v2 on every clip (network A, the same weights), and
// presets 0, 2 and 3 render identical to each other (network B, NVIDIA's
// lightweight network). So 0 is not "unset": it is whatever the runtime ships
// as its default, which moved from A to B between the two DLLs. The player never
// sends it; A is 1 and B is 2.
//
// The default is A because it is the picture 0.28 made. B is a milder picture:
// on all five clips it lands nearer the source (the knobs distance 79-93 % of
// A's; VMAF v1 82.1 against 78.8 and ColorVideoVDP 8.90 against 8.74 JOD, both
// scored against the source) without freezing (lost share 0.130 against
// 0.140). Nearer the source is not better by itself - the model's change is the
// product - so the quality rule keeps A, and B is the opt-in rung with its cost
// and its distance beside it (kNetworks below).
//
// The runtime follows the GPU. 310.9.16 is offered on RTX 50 only: it is
// NVIDIA's 310.9.16 with a community section of Turing/Ampere/Ada kernels added
// and its signature removed, NVIDIA's own build of it fails feature creation on
// an RTX 3060 Ti with 0xBAD00002 (NeuralScreen v2.6.1), and no RTX 20-40 card
// has run either here. Every other generation keeps 310.8.SF-v2, which 0.28
// shipped to all of them. On RTX 50 the previous runtime stays selectable, so
// "same look as the last version" can be shown rather than claimed.
namespace neural_runtime {

enum class Network { A, B };

struct Runtime {
    std::string_view version;    // receipt, footer and start-screen name
    std::wstring_view file;      // under neural-runtime/: the lock's destination
    std::string_view addonPath;  // NRRuntimePath, relative to the add-on's folder
    bool networkB;               // the DLL carries the lightweight network
};

inline constexpr Runtime kCurrent{"310.9.16", L"nvngx_dlssnr.dll", "nvngx_dlssnr.dll", true};
inline constexpr Runtime kPrevious{"310.8.SF-v2", L"310.8.SF-v2/nvngx_dlssnr.dll",
                                   "310.8.SF-v2\\nvngx_dlssnr.dll", false};

// Whether this GPU is offered 310.9.16 at all; where it is not, the choice of
// runtime is not the user's and the dialog says so.
inline bool OffersCurrent(GpuGeneration gpu) { return gpu == GpuGeneration::Rtx50Blackwell; }

inline const Runtime& Select(GpuGeneration gpu, bool previousRequested)
{
    return OffersCurrent(gpu) && !previousRequested ? kCurrent : kPrevious;
}

// What runs when B is asked of a runtime that has only A: A, and the receipt,
// the key and the footer all say A, because that is the picture.
inline Network Effective(const Runtime& runtime, Network requested)
{
    return runtime.networkB ? requested : Network::A;
}

// NRPreset, which the add-on forwards as DLSSNR.Hint.Render.Preset. Never 0.
inline int PresetValue(Network network) { return network == Network::B ? 2 : 1; }

// [NeuralSettings] Network, the canonical settings and --network: "a" and "b".
inline std::string_view Key(Network network) { return network == Network::B ? "b" : "a"; }
inline std::optional<Network> ParseKey(std::wstring_view text)
{
    if (text == L"a" || text == L"A") return Network::A;
    if (text == L"b" || text == L"B") return Network::B;
    return std::nullopt;
}

// The rungs, default first, each with what it measured against the other. The
// cost is the model's own GPU time per frame (neuralGpuMsP50, RTX 5090, three
// interleaved repeats over five 1080p clips and one 4K clip, spread under
// 0.25 ms), which is the part of a frame the network changes; the rest of the
// pipeline is the same for both, and end-to-end pace on a shared machine moved
// more between repeats than between the networks.
struct NetworkRung {
    Network network;
    std::wstring_view label;
    std::wstring_view cost;
};
inline constexpr std::array<NetworkRung, 2> kNetworks{{
    {Network::A, L"A \u00b7 same look as 0.28 (recommended)", L"Model time 3.8 ms a 1080p frame, 9.6 ms at 4K."},
    {Network::B, L"B \u00b7 lighter: 36% less model time", L"Model time 2.4 ms a 1080p frame, 6.0 ms at 4K; a milder picture, nearer the source."},
}};

inline size_t NetworkIndex(Network network) { return network == Network::B ? 1 : 0; }

// Neural settings' one Network control: the two rungs on 310.9.16 and, after
// them, the previous runtime, which has only A. A GPU that is not offered
// 310.9.16 shows the previous runtime alone, greyed out (ChoiceCount 1).
inline constexpr size_t kPreviousChoice = kNetworks.size();
inline size_t ChoiceCount(GpuGeneration gpu) { return OffersCurrent(gpu) ? kNetworks.size() + 1 : 1; }
inline size_t ChoiceIndex(GpuGeneration gpu, Network network, bool previousRequested)
{
    if (!OffersCurrent(gpu)) return 0;
    return previousRequested ? kPreviousChoice : NetworkIndex(network);
}
// Picking the previous runtime keeps the network asked for, so coming back to
// 310.9.16 returns to it; picking a rung leaves the previous runtime.
inline void ApplyChoice(GpuGeneration gpu, size_t index, Network& network, bool& previousRequested)
{
    if (!OffersCurrent(gpu)) return;
    previousRequested = index >= kPreviousChoice;
    if (index < kNetworks.size()) network = kNetworks[index].network;
}

// "310.9.16: network A": the footer's and the receipt's one-line name for what ran.
inline std::wstring Describe(const Runtime& runtime, Network requested)
{
    std::wstring text(runtime.version.begin(), runtime.version.end());
    text += Effective(runtime, requested) == Network::B ? L" \u00b7 network B" : L" \u00b7 network A";
    return text;
}

} // namespace neural_runtime
