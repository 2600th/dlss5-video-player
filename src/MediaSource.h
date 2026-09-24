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

// What a source stream declares about its own colour: ffprobe's color_space,
// color_range, color_primaries and color_transfer, mapped onto the values this
// pipeline can act on. Every field has an explicit Unspecified because an
// undeclared stream is the case these types exist to name - the GPU NV12 source
// conversion needs a stated matrix and a stated range, and Unspecified is never
// read as "BT.709 limited", which is what it used to be assumed to be.
//
// Other means "declared, and not one of the listed values". It is kept apart
// from Unspecified so a refusal can say which of the two it saw: "this file
// says BT.2020" and "this file says nothing" are different diagnoses.
enum class ColorMatrix {
    Unspecified,
    Bt709,
    // ffprobe's bt470bg and smpte170m: the same coefficients, 625- and
    // 525-line, and the shader has one set for both.
    Bt601,
    // ffprobe's bt2020nc, the HDR10 and HLG matrix. Named because the decoder's
    // own tone map (HdrToneMap.h) implements exactly this one; the NV12 pass
    // does not, so it refuses it like Other.
    Bt2020Ncl,
    Other,
};

enum class ColorRange {
    Unspecified,
    Limited,  // ffprobe "tv": Y in 16..235, chroma in 16..240
    Full,     // ffprobe "pc": Y in 0..255, chroma centred on 128 over 0..255
};

// Bt2020, Pq (ffprobe "smpte2084") and Hlg ("arib-std-b67") are the three
// values that make a stream HDR; they are named so the decoder can tone map
// exactly those streams (HdrPolicy.h) instead of lumping them into Other.
enum class ColorPrimaries { Unspecified, Bt709, Bt470bg, Smpte170m, Other, Bt2020 };
enum class ColorTransfer { Unspecified, Bt709, Smpte170m, Other, Pq, Hlg };

struct SourceColorDescription {
    ColorMatrix matrix{ColorMatrix::Unspecified};
    ColorRange range{ColorRange::Unspecified};
    ColorPrimaries primaries{ColorPrimaries::Unspecified};
    ColorTransfer transfer{ColorTransfer::Unspecified};
};

// The conversions D3D12Renderer's NV12 source pass implements: one per (matrix,
// range) pair it has coefficients and a range mapping for. This enum IS the gate
// on the GPU source-conversion path - Unsupported means the decoder delivers BGRA
// and ffmpeg converts on the CPU instead, which costs pipe bandwidth and never
// costs correctness. There is deliberately no "nearest variant": a source that
// declares a matrix the pass cannot honour is refused, not rounded to BT.709.
//
// Primaries and transfer are carried in the description and named in the refusal
// log line, but they do not select a conversion. The pass produces R'G'B' from
// Y'CbCr, which is exactly a matrix and a range mapping; neither it nor the CPU
// conversion it falls back to re-maps primaries or re-applies a transfer, so
// gating on those two would refuse sources the fallback handles no better.
enum class SourceNv12Conversion {
    Unsupported,
    Bt709Limited,
    Bt709Full,
    Bt601Limited,
    Bt601Full,
};

inline SourceNv12Conversion SourceNv12ConversionFor(const SourceColorDescription& color)
{
    const bool full = color.range == ColorRange::Full;
    if (!full && color.range != ColorRange::Limited) return SourceNv12Conversion::Unsupported;
    switch (color.matrix) {
        case ColorMatrix::Bt709:
            return full ? SourceNv12Conversion::Bt709Full : SourceNv12Conversion::Bt709Limited;
        case ColorMatrix::Bt601:
            return full ? SourceNv12Conversion::Bt601Full : SourceNv12Conversion::Bt601Limited;
        case ColorMatrix::Unspecified:
        case ColorMatrix::Bt2020Ncl:
        case ColorMatrix::Other:
            break;
    }
    return SourceNv12Conversion::Unsupported;
}
