# Third-Party Notices

This player links no third-party libraries at build time. It can, however, load
one vendor runtime at run time, and that runtime's licence travels with the
distribution, as its terms require.

## AMD FidelityFX (optional, loaded at run time)

- Component: `amd_fidelityfx_framegeneration_dx12.dll` and
  `amd_fidelityfx_upscaler_dx12.dll`, and the `amd_fidelityfx_loader_dx12.dll`
  that loads either effect beside itself.
- Source: <https://github.com/GPUOpen-Libraries-AndSDKs/FidelityFX-SDK>
  (`Kits/FidelityFX/signedbin`).
- Copyright (C) Advanced Micro Devices, Inc.
- Licence: the FidelityFX SDK licence (`docs/license.md` in that repository).
  It grants redistribution **in binary form only**, on three conditions this
  project satisfies:
  1. No reverse engineering, decompilation or disassembly of the binary. This
     project reaches it only through its published C API
     (`ffxCreateContext` / `ffxDispatch` / `ffxDestroyContext`) and its published
     descriptor structures; nothing inspects or modifies the binary itself. The
     descriptor layouts are transcribed in `src/FsrFrameGenAbi.h` from the
     public headers, and the version constant is checked by the runtime, so a
     version mismatch surfaces as an error rather than as a patched call.
  2. Redistributions must reproduce the copyright notice, the permission notice
     and the disclaimers - that is what this file is.
  3. The software is provided "AS IS", with all warranties disclaimed.

  The licence's source-redistribution term does not apply here: no FidelityFX
  source is vendored into this repository, which is why the ABI is declared
  locally rather than included from the SDK.

- Used for: two effects, each optional on its own. AMD FidelityFX Frame
  Generation, the AMD GPU frame-generation backend
  (`src/FsrFrameGenBackend.cpp`); absent, the player falls back to its own
  optical-flow backend (`src/AmdFrameGenBackend.cpp`), which needs no runtime.
  And FSR 4, the top tier of the playback upscaler
  (`src/FsrUpscaleBackend.cpp`); absent, the upscale falls to FSR 1's EASU and
  then to Catmull-Rom with CAS, both the player's own shaders
  (`src/AmdUpscaleShader.h`), so upscaling and every other feature work without
  the runtime either way.

- FSR, FidelityFX and AMD are trademarks of Advanced Micro Devices, Inc.

## What this is not

The `MPCVR-FSR-FrameGen` project (a DirectShow video renderer with FSR Frame
Generation support) pointed at this same DLL while this player was being written,
and it is GPL v3. That licence covers **its** video-renderer code, not the DLL,
which is AMD's and separately licensed. No code was taken from that project; the
only thing reused was the observation that the DLL can be loaded by name and
driven through the FFX C API.

## NVIDIA components (not redistributed)

`nvngx_dlss.dll` and `nvngx_dlssg.dll` are produced by this repository's build
from the NVIDIA DLSS SDK, which is not redistributed here. They are required only
by the NVIDIA NGX paths (DLSS-G frame generation and DLSS super-resolution);
neither is loaded on an AMD adapter.