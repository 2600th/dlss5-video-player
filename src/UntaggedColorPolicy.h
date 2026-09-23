#pragma once

#include "MediaSource.h"

#include <cstdint>

// Which matrix an UNDECLARED video is decoded with.
//
// A stream that states no colour matrix still has to be converted to RGB with
// one. ffmpeg's converter picks BT.601 for every such stream, whatever its
// size, and every export this player writes is converted back with BT.709 and
// tagged BT.709 (BuildEncoderArguments). For an untagged HD source the two
// disagreed: the file came back with its Y'CbCr moved - measured on a 1280x720
// smptehdbars clip through frame generation alone, the cyan bar's Y went 133
// -> 155 - and the stand-in clip FrameGenerationSmoke once generated untagged
// showed a 10.1 px brightness-centroid shift between source and passthrough
// frames that tagging it BT.709 took down to 0.66 px.
//
// The rule is the one players follow for a stream that says nothing: HD and
// larger is BT.709, standard definition is BT.601. The boundary is mpv's -
// wider than 1279 or taller than 576 - so a 1280x544 scope film is HD and a
// 720x576 PAL master is not. A declared matrix is always honoured as declared,
// and an undeclared range stays ffmpeg's own limited-range reading, which is
// already what players assume. Photos and GIFs are not video: a JPEG is
// BT.601 by definition (JFIF) and the decoder reports it so, and a GIF or PNG
// is RGB already, so neither is given a matrix it did not declare.
//
// Only the one case that changes anything is reported: an undeclared matrix on
// an HD video. SD undeclared is BT.601 either way, so ffmpeg's default already
// is the rule there and nothing about those decodes, or their cache keys, moves.
inline bool UntaggedSourceDecodesAsBt709(const SourceColorDescription& declared, uint32_t width,
                                         uint32_t height, bool stillOrAnimation)
{
    if (stillOrAnimation || declared.matrix != ColorMatrix::Unspecified) return false;
    return width > 1279 || height > 576;
}

// The cache-key term a render of such a source carries: its input pixels are
// the BT.709 reading now, where a render made before this rule saw BT.601.
// Empty for every other source, whose key does not move.
inline const char* UntaggedColorIdentityTerm(bool decodesAsBt709)
{
    return decodesAsBt709 ? "|untagged-hd-bt709-v1" : "";
}
