#include "FrameGenBackend.h"

#include "AmdFrameGenBackend.h"
#include "DLSSGBackend.h"

#include <memory>

// NVIDIA adapters keep DLSS-G (NVidia NGX): that path is measured on an RTX 5090
// and is the only one that admits the feature there. Every other adapter - the
// AMD GPUs this build targets, plus Intel or a software fallback - gets the
// vendor-neutral optical-flow backend, which needs no NGX and no nvngx_dlssg.dll.
std::unique_ptr<IFrameGenBackend> CreateFrameGenBackend(bool preferNvidia)
{
    if (preferNvidia) return std::make_unique<DLSSGBackend>();
    return std::make_unique<AmdFrameGenBackend>();
}
