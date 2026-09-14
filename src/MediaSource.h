#pragma once

// Describes where media comes from and which decoder backend that permits.
// These live here rather than in UiLayout so the decode layer does not
// depend on the UI layer.

enum class MediaSourceKind {
    LocalFile,
    YouTube,
};

enum class DecoderOpenPolicy {
    FfmpegThenMediaFoundation,
    FfmpegOnly,
};

DecoderOpenPolicy DecoderPolicyForSource(MediaSourceKind sourceKind);
