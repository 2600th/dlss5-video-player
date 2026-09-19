#pragma once

// What to do with a cached render the key matched.
//
// LookupRender has already verified the full SHA-256 of the payload and both
// sidecars before any of this runs, so an entry reaching here is intact. What
// remains is whether it describes the render being asked for, which is settled
// by two different kinds of evidence:
//
//   - the manifest, compared in process against the identity just built, and
//   - an ffprobe of the payload, which needs a child process.
//
// Conflating them cost renders. A probe that could not run - an antivirus
// holding the unsigned ffprobe.exe, or the helper simply not resolving, which
// MediaPipeline reports as a plain "FFmpeg tools are unavailable" failure -
// was read as the probe disagreeing, and the entry was quarantined. The
// staging sweep deletes any `invalid*` name unconditionally on the next
// manager construction, so a user opening their five rendered videos with a
// broken ffprobe lost hours of GPU time that had just been hash-verified as
// intact.
namespace cached_render {

enum class Verdict {
    // Matches the request; hand it to the player.
    Serve,
    // Something that does not need a child process disagreed, or the probe ran
    // and contradicted the manifest. The entry is wrong and may be retired.
    Discard,
    // The probe could not run, so nothing was learned. Do not serve it, and do
    // not destroy it either.
    Unverified,
};

struct Evidence {
    // False when ffprobe could not be run at all, as opposed to running and
    // returning something that disagreed.
    bool probeRan = false;
    // Digests, range and guides: compared in process, so these are known
    // whether or not the probe ran.
    bool manifestMatches = false;
    // Everything below is only meaningful when `probeRan`.
    bool geometryMatches = false;
    bool durationMatches = false;
};

inline Verdict Judge(const Evidence& evidence)
{
    // Needs no child process, so it is decided first and condemns the entry
    // even while ffprobe is unavailable. A stale entry is still retired.
    if (!evidence.manifestMatches) return Verdict::Discard;
    // Past here every remaining question is one only the probe can answer.
    if (!evidence.probeRan) return Verdict::Unverified;
    return (evidence.geometryMatches && evidence.durationMatches) ? Verdict::Serve
                                                                  : Verdict::Discard;
}

} // namespace cached_render
