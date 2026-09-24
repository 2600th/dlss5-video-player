#pragma once

#include "HdrToneMap.h"

#include <cstdint>
#include <vector>

// Runs hdr_tonemap's integer path over whole P010 frames: as a D3D11 compute
// pass when the machine has a GPU that will run it, on the CPU otherwise. The
// two produce the same bytes (HdrToneMap.h), so the choice is logged and never
// keyed. One device serves every decoder in the process; a frame waits for the
// one before it, which at 4K is a few milliseconds.
namespace hdr_tonemap {

// What one source is tone mapped with: its table and its matrix. Built once per
// open; `id` tells the GPU when the table it holds is another source's.
struct Table {
    uint64_t id = 0;
    hdr_policy::HdrSignal signal = hdr_policy::HdrSignal::Pq;
    double peakNits = 0.0;
    bool fullRange = false;
    std::vector<uint32_t> lut;  // BuildLut: nodes, then encode thresholds
    Matrix matrix;
};
Table MakeTable(hdr_policy::HdrSignal signal, double peakNits, bool fullRange);

enum class Path { Gpu, Cpu };

// `p010` holds P010FrameBytes(w, h); `bgra` receives w * h * 4. w and h even.
Path ToneMapFrame(const Table& table, const uint8_t* p010, uint32_t w, uint32_t h, uint8_t* bgra);
// The CPU path alone, which is also what ToneMapFrame falls back to.
void ToneMapFrameCpu(const Table& table, const uint8_t* p010, uint32_t w, uint32_t h, uint8_t* bgra);
// The GPU path alone, for the test that holds the two to the same bytes. False
// when there is no GPU path on this machine or it failed.
bool ToneMapFrameGpu(const Table& table, const uint8_t* p010, uint32_t w, uint32_t h, uint8_t* bgra);

} // namespace hdr_tonemap
