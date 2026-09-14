#pragma once

#include <cstdint>

// Two halves of the same question: which GPU this process runs on, and which
// one it actually got. GpuPreference.cpp exports the driver hints that answer
// the first; the identity below is how anything downstream checks the second.
//
// DXGI identifies an adapter three ways and only one of them is an identity.
// The description repeats between two identical cards and is localized in
// parts; the vendor/device pair names a model, not a part. The LUID is unique
// per physical adapter for the lifetime of the boot, which makes it the only
// field that can answer "is the adapter this device was created on the adapter
// the policy classified". It arrives as two halves; packing it keeps both the
// comparison and the log format free of windows.h, which RuntimePolicy.h is
// deliberately compiled without.

// Packs DXGI_ADAPTER_DESC1::AdapterLuid. HighPart is signed, so the cast
// through uint32_t is what keeps its bit pattern out of LowPart's half
// instead of sign-extending over it.
constexpr uint64_t PackAdapterLuid(int32_t highPart, uint32_t lowPart) noexcept
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(highPart)) << 32) | lowPart;
}

// Zero is not an adapter: DXGI never issues that LUID, so it is what a
// default-constructed DetectedGpu and a failed GetDesc call both carry, and
// two of them must not agree with each other any more than one of them agrees
// with a real adapter.
enum class AdapterMatch { Same, Different, Unknown };

constexpr AdapterMatch CompareAdapterLuids(uint64_t left, uint64_t right) noexcept
{
    if (left == 0 || right == 0) return AdapterMatch::Unknown;
    return left == right ? AdapterMatch::Same : AdapterMatch::Different;
}
