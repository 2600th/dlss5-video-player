#pragma once

#include <algorithm>
#include <cstdint>

// Decisions an active neural session makes on the UI thread, separated from the
// player so they can be tested without a window, a GPU or a render helper.
//
// A session is one long render job that publishes finalized segments while it
// runs. Playback follows the render head: it attaches once enough is buffered,
// rebuffers when it catches up, and rebases (stop + restart at the new
// position) when the user seeks somewhere the head will not reach soon.
namespace live_session {

// Seconds of rendered video that must exist ahead of the playhead before
// playback starts, and the smaller amount that ends a rebuffer.
inline constexpr double kStartLead = 4.0;
inline constexpr double kResumeLead = 2.0;
// Restarting a session costs a fresh job startup (~7.3 s) plus the lead-in, and
// a running job covers roughly a second of video per second, so waiting is
// cheaper than restarting for any target the head reaches inside that budget.
inline constexpr double kRebaseAhead = 15.0;
// A seek backwards is always out of coverage, but a tiny backwards nudge is
// usually a rounding artifact of frame snapping rather than a real seek.
inline constexpr double kBackwardSlack = 0.5;

// Lead to require before the first attach, given how fast this GPU renders
// relative to real time (`LiveRenderForecast::realtimeRatio`). The 4 s cushion
// is sized for a card that barely keeps up; on one that renders three times
// faster the buffer refills faster than playback drains it, so the same cushion
// only makes the user wait. An unknown pace keeps the full cushion.
inline double StartLead(double realtimeRatio, double startLead = kStartLead)
{
    if (!(realtimeRatio > 0.0)) return startLead;
    if (realtimeRatio >= 3.0) return std::min(startLead, 1.0);
    if (realtimeRatio >= 1.5) return std::min(startLead, 2.0);
    return startLead;
}

struct SessionView {
    double positionSec = 0.0;   // where playback is
    double rangeStartSec = 0.0; // where the job began rendering
    double headSec = 0.0;       // newest rendered timestamp, 0 when nothing landed
    bool attached = false;      // playback is running on the rendered segments
    bool finished = false;      // the job published its last segment
    bool seeking = false;       // a seek is in flight or the user is scrubbing
};

// Rendered seconds sitting ahead of the playhead.
inline double Lead(const SessionView& view)
{
    return view.headSec > 0.0 ? std::max(0.0, view.headSec - view.positionSec) : 0.0;
}

// True once the head is far enough ahead to start playing. A finished job never
// grows again, so any coverage at all is enough to start.
inline bool ShouldAttach(const SessionView& view, double startLead = kStartLead)
{
    if (view.attached) return false;
    const double lead = Lead(view);
    return lead >= startLead || (view.finished && lead > 0.0);
}

// Attaching needs a finalized segment that contains the playhead, which is not
// the same thing as having a lead: adopted coverage can start after the
// playhead, or a seek can land in a gap. When that happens the lead keeps
// growing, `ShouldAttach` keeps saying yes, the attach keeps failing, and the
// player sits behind the buffering panel with a full buffer and nothing to show
// for it. After a few consecutive failures the session is in the wrong place,
// so restart it at the playhead instead of retrying forever.
inline constexpr int kAttachFailureLimit = 20;

inline bool ShouldRebaseStalledAttach(const SessionView& view, int consecutiveFailures,
                                      int failureLimit = kAttachFailureLimit)
{
    if (view.attached || view.seeking) return false;
    if (consecutiveFailures < failureLimit) return false;
    return Lead(view) > 0.0;
}

// Where the player should join the render. The session starts at the snapped
// playhead, but the first segment it finalizes can begin a frame or two later -
// one 30 fps clip toggled on at 12.033 s published coverage from 12.066 s.
// Joining at the playhead then found no segment containing it, so the player
// waited behind a filling buffer for a frame that would never be published, and
// the recovery above restarted the same session at the same place forever.
// Coverage that starts later is the frame playback continues from; coverage that
// starts earlier is history, so the playhead stands.
inline int64_t AttachPosition100ns(int64_t position100ns, int64_t rangeStart100ns,
                                   int64_t coverageStart100ns)
{
    const int64_t floor = std::max(position100ns, rangeStart100ns);
    return coverageStart100ns > floor ? coverageStart100ns : floor;
}

// True once a rebuffer can end.
inline bool ShouldResume(const SessionView& view, double resumeLead = kResumeLead)
{
    return view.finished || Lead(view) >= resumeLead;
}

// True when the playhead left the part of the timeline this job will render
// soon enough to be worth waiting for.
inline bool NeedsRebase(const SessionView& view, double aheadBudget = kRebaseAhead)
{
    if (view.attached || view.seeking) return false;
    if (view.positionSec + kBackwardSlack < view.rangeStartSec) return true;
    return view.positionSec > std::max(view.rangeStartSec, view.headSec) + aheadBudget;
}

// Video seconds covered per second of wall clock. Meaningless until the job's
// fixed startup cost stops dominating, so early samples report nothing.
inline double RealtimeRatio(double coveredSec, double elapsedSec, double settleSec = 8.0)
{
    if (elapsedSec < settleSec || !(coveredSec > 0.0)) return 0.0;
    return coveredSec / elapsedSec;
}

} // namespace live_session
