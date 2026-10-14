#include "FrameGenBackend.h"

#include "AmdFrameGenBackend.h"
#include "DLSSGBackend.h"
#include "FsrFrameGenBackend.h"

#include <memory>

// Backend selection, in the order the adapters are tried.
//
// NVIDIA adapters keep DLSS-G (NVidia NGX): that path is measured on an RTX 5090
// and is the only one that admits the feature there.
//
// Everything else prefers AMD FidelityFX when its runtime is on disk. FidelityFX
// is the vendor's own frame generator for this GPU class - it derives motion and
// disocclusion from the colour buffers rather than from a motion field, and it is
// the difference between an intermediate frame that lands on the content and one
// that does not. The self-contained matcher (AmdFrameGenBackend) is what remains
// when it is absent, and it is deliberately kept: it needs no runtime at all, so
// a build without the SDK still converts, and it is the fallback this factory has
// always used. Presence is a file check here rather than an Initialize attempt,
// so the choice is made before any device work and can be reported in the log.
std::unique_ptr<IFrameGenBackend> CreateFrameGenBackend(bool preferNvidia)
{
    if (preferNvidia) return std::make_unique<DLSSGBackend>();
    return std::make_unique<AmdFrameGenBackend>();
}