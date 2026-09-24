#pragma once

#include "MediaPipeline.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

// Direct NVENC encoding of the neural capture (P3.7).
//
// The capture path used to read every frame back to the CPU and pipe it to an
// ffmpeg child running hevc_nvenc, which copied it straight back onto the GPU for
// NVENC. At 4K that is 12-25 MB a frame through PCIe twice and through a pipe
// once. Here the capture copies its planes into an NV12 or P010 surface NVENC
// reads in place through its D3D12 interface, waiting on the capture's own fence,
// and the ffmpeg child is kept for what it is good at: the muxing
// (RemuxVideoStream), the codecs NVENC does not do, and the fallback whenever
// this path cannot start. NvencDirectPolicy.h says why the bits come out the same.

// What a direct capture resolves to in place of pixels: the surface the frame
// was copied into and the capture fence value NVENC waits on before it reads it.
// It travels through the job exactly where the pixels did, so it is a fixed
// size and RunJob's frame-size check still means something.
struct NvencDirectToken {
    static constexpr uint32_t kMagic = 0x5444564Eu;  // "NVDT"
    uint32_t magic{kMagic};
    uint32_t surface{};
    uint64_t fenceValue{};
};
static_assert(sizeof(NvencDirectToken) == 16);

inline void WriteNvencDirectToken(const NvencDirectToken& token, std::vector<uint8_t>& bytes)
{
    bytes.resize(sizeof(token));
    std::memcpy(bytes.data(), &token, sizeof(token));
}

inline bool ReadNvencDirectToken(std::span<const uint8_t> bytes, NvencDirectToken& token)
{
    if (bytes.size() != sizeof(token)) return false;
    std::memcpy(&token, bytes.data(), sizeof(token));
    return token.magic == NvencDirectToken::kMagic;
}

// The surfaces the capture copies into and NVENC reads from, on the renderer's
// device. The render thread takes one per capture (Acquire) and the encoder's
// thread gives it back once NVENC has coded that frame (Release), so the pool is
// the only thing the two share, and it is what holds the capture back when the
// encoder falls behind - a wait for a free surface replaces the old wait for
// room in the pipe.
class NvencSurfacePool {
public:
    // Fixes the device, the capture fence NVENC waits on, and the surface layout.
    // `captureSlots` is the renderer's capture ring depth, the part of the pool
    // NvencDirectPolicy's sizing charges to frames the render thread still holds.
    bool Configure(ID3D12Device* device, ID3D12Fence* captureFence, EncoderPixelFormat format,
                   uint32_t width, uint32_t height, uint32_t captureSlots);
    bool Configured() const;
    // Grows the pool to at least `count` surfaces. Only between attempts, with
    // nothing acquired: a grown pool renumbers nothing, but the renderer and the
    // encoder must not be mid-frame while it changes.
    bool Reserve(uint32_t count);
    // Drops every surface, for a device that is going away.
    void Reset();
    // A free surface, waiting up to `timeout` for the encoder to give one back.
    std::optional<uint32_t> Acquire(std::chrono::milliseconds timeout);
    void Release(uint32_t index);
    // Between attempts: a cancelled or failed attempt can leave surfaces taken
    // by frames that never reached an encoder.
    void ReleaseAll();

    ID3D12Resource* Surface(uint32_t index) const;
    uint32_t Count() const;
    uint32_t CaptureSlots() const;
    ID3D12Device* Device() const;
    ID3D12Fence* CaptureFence() const;
    EncoderPixelFormat Format() const;
    uint32_t Width() const;
    uint32_t Height() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable freed_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> surfaces_;
    std::vector<bool> busy_;
    EncoderPixelFormat format_{EncoderPixelFormat::Nv12};
    uint32_t width_{}, height_{}, captureSlots_{};
};

namespace nvenc_direct {

// Whether this machine's driver offers the NVENC API the build was compiled
// against (13.1): nvEncodeAPI64.dll from the system directory, catalog-signed,
// reporting a maximum API version at least that new. Answered once per process;
// `reason` says what failed.
bool DriverAvailable(std::string* reason = nullptr);

} // namespace nvenc_direct

// One NVENC session writing one file. Start and Encode run on whichever thread
// feeds it - the render thread starts it, the encoder adapter's feeder thread
// encodes - and Finish/Cancel on the thread that owns the job; none of them
// runs concurrently with another.
class NvencDirectEncoder {
public:
    NvencDirectEncoder();
    ~NvencDirectEncoder();
    NvencDirectEncoder(const NvencDirectEncoder&) = delete;
    NvencDirectEncoder& operator=(const NvencDirectEncoder&) = delete;

    // Opens and configures the session on the pool's device, grows the pool to
    // what the configuration needs and opens the hand-off file (NvencDirectMux.h)
    // in the temp directory. `helperDirectory` is where ffmpeg.exe is found for
    // the final mux; empty means beside this module, as for the encoder child.
    EncodeError Start(NvencSurfacePool& pool, const EncoderSpec& spec, const std::filesystem::path& output,
                      const std::filesystem::path& helperDirectory = {});
    // Submits one captured frame. `moreQueued` says another is already waiting:
    // when none is, every picture NVENC can finish is locked and written now, so
    // the surfaces behind them go back to the capture instead of waiting for a
    // frame that the capture cannot produce without them.
    EncodeError Encode(std::span<const uint8_t> token, bool moreQueued);
    // Drains the session, writes the file and muxes it into `output`.
    EncodeError Finish(std::stop_token stop);
    void Cancel();
    // The last failure, for the log.
    const std::string& Detail() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
