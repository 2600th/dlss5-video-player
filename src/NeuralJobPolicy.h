#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "CachedRenderVerdict.h"
#include "MediaPipeline.h"
#include "NeuralCache.h"
#include "NeuralRenderTypes.h"
#include "NeuralSegmentIndex.h"
#include "RuntimePolicy.h"

// The decisions a player neural job makes between its I/O steps (main.cpp's
// NeuralJobRun): whether a range fits the source, what a cached entry has to
// agree with to be served, which segments a live run publishes, and whether
// the published file is the render the helper reported. Pure, so they can be
// tested without a runtime, a cache or a GPU.
namespace neural_job {

// Frames per live segment file for `seconds` of a source at `fps`; never 0.
inline uint32_t SegmentFrames(double fps, double seconds)
{
    return static_cast<uint32_t>(std::max<long>(1, std::lround(fps * seconds)));
}

// A range must start inside the source and end after it starts. Whole()
// always fits.
inline bool RangeOutsideSource(const NeuralRenderRange& range, int64_t sourceDuration100ns)
{
    return !range.Whole() && (range.start100ns < 0 || range.end100ns <= range.start100ns ||
                              range.start100ns >= sourceDuration100ns);
}

// A range render is exactly [start,end) long; a whole render matches the source.
inline int64_t ExpectedDuration100ns(const NeuralRenderRange& range, int64_t sourceDuration100ns)
{
    return range.Whole() ? sourceDuration100ns : range.end100ns - range.start100ns;
}

// One frame, rounded up: how far a cached payload's duration may sit from
// the duration this job expects.
inline int64_t FrameDurationTolerance100ns(double fps)
{
    return static_cast<int64_t>(std::ceil(10000000.0 / fps));
}

// What this job knows about the render it would make, for comparing a cached
// entry against.
struct Expected {
    std::string sourceDigest, runtimeDigest, settingsDigest;
    NeuralRenderRange range;
    std::string guides;
    uint32_t width{}, height{};
    int64_t duration100ns{};
    int64_t frameDurationTolerance100ns{};
};

// Split by what each piece of evidence needs. The manifest is compared in
// process; everything else needs ffprobe to have run. Reading a probe that
// could not run as a probe that disagreed quarantined - and then deleted -
// entries whose payload had just been hash-verified as intact.
// `manifest.frameCount` is nonzero: LookupRender refuses an entry without one.
inline cached_render::Evidence CachedRenderEvidence(const NeuralCacheManifest& manifest, const ProbeResult& probe,
                                                    const Expected& expected)
{
    const int64_t durationTolerance =
        std::max<int64_t>(1, manifest.duration100ns / static_cast<int64_t>(manifest.frameCount) + 1);
    return cached_render::Evidence{
        probe.ok,
        manifest.sourceDigest == expected.sourceDigest && manifest.runtimeDigest == expected.runtimeDigest &&
            manifest.settingsDigest == expected.settingsDigest &&
            manifest.rangeStart100ns == expected.range.start100ns &&
            manifest.rangeEnd100ns == expected.range.end100ns && manifest.guides == expected.guides,
        probe.width == expected.width && probe.height == expected.height && probe.width == manifest.width &&
            probe.height == manifest.height,
        std::llabs(probe.duration100ns - manifest.duration100ns) <= durationTolerance &&
            std::llabs(probe.duration100ns - expected.duration100ns) <= expected.frameDurationTolerance100ns};
}

// The encoder a render entry's manifest names.
inline const char* CacheEncoderName(EncoderKind encoder)
{
    return encoder == EncoderKind::HevcNvenc ? "hevc_nvenc" : encoder == EncoderKind::Ffv1 ? "ffv1" : "h264_software";
}

// The files one live run published, in timeline order.
//
// The entry is keyed, labelled and proven by THIS run: its range, its frame
// count, its evidence counters. So it must contain exactly the segments this
// run published. A resumed session hands earlier coverage to the next job for
// playback to keep reading, and joining that in too produced a file longer
// than the label - one session joined 46 files of 2622 frames and 87.4 s
// against a result of 1647 frames and 54.9 s, and the gate correctly refused
// the render it had just finished.
//
// Selected by run id rather than by position: segments are held sorted by
// timestamp, so a run that filled a hole behind an earlier region is not the
// tail of the index, and a positional slice would take the wrong files.
inline std::vector<std::filesystem::path> SegmentsOfRun(const NeuralSegmentIndex& index, uint64_t runId)
{
    std::vector<std::filesystem::path> parts;
    for (size_t position = 0; position < index.Count(); ++position)
        if (const auto segment = index.At(position); segment && segment->runId == runId)
            parts.push_back(segment->path);
    return parts;
}

// The published file is the render the helper reported: its geometry, its
// frame count, and three durations - probed, reported and requested - that
// agree within `tolerance100ns` (JoinedMediaDurationTolerance100ns).
inline bool PublishedProbeMatches(const ProbeResult& probe, uint32_t width, uint32_t height, uint64_t frameCount,
                                  int64_t resultDuration100ns, int64_t expectedDuration100ns, int64_t tolerance100ns)
{
    return probe.ok && probe.width == width && probe.height == height && probe.frameCount == frameCount &&
           NeuralPublishDurationsMatch(probe.duration100ns, resultDuration100ns, expectedDuration100ns,
                                       tolerance100ns);
}

} // namespace neural_job
