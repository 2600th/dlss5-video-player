#include "MediaSource.h"

DecoderOpenPolicy DecoderPolicyForSource(MediaSourceKind sourceKind)
{
    return sourceKind == MediaSourceKind::YouTube
        ? DecoderOpenPolicy::FfmpegOnly
        : DecoderOpenPolicy::FfmpegThenMediaFoundation;
}
