#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "NeuralCoverage.h"

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
//
// A seek in flight refuses: the playhead the lead was measured against is the
// one playback is leaving. With coverage in regions a seek can legitimately
// target unrendered video, and attaching at the position being abandoned made
// the player attach, refuse the pending seek because that target is a hole,
// hand playback back, and attach again - 28 times in seventeen seconds of a
// driven session.
inline bool ShouldAttach(const SessionView& view, double startLead = kStartLead)
{
    if (view.attached || view.seeking) return false;
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

// What a finished job leaves an active session to play. The question is not how
// the job ended but whether anything reached the segment index playback is
// bound to, because a job can succeed without appending a single segment: one
// whose render key is already published returns that entry as a cache hit in
// about 50 ms and renders nothing. The index then stays empty with `Finished()`
// set, which is the one state where every other decision here says "wait" -
// `ShouldAttach` sees a finished session with zero lead, `NeedsRebase` sees a
// playhead inside the range, and the player sits behind the buffering panel
// until the user gives up. The entry covers the session's whole range, so it is
// playable immediately and better than anything the session could have done;
// with no entry there is nothing to show and the original has to come back.
// 0.21.2 fixed the same family for coverage that starts one frame late; this is
// coverage that never arrives at all.
enum class CompletedSessionPlan {
    Segments,       // keep playing what the session published
    PublishedEntry, // nothing was published to the index; play the cache entry instead
    Stop,           // nothing to play: end the session and hand the original back
};

struct CompletedSession {
    bool covered = false;        // at least one finalized segment reached the index
    bool ok = false;             // the job reported success
    bool publishedEntry = false; // a complete entry covering the session's range exists
};

inline CompletedSessionPlan PlanForCompletedSession(const CompletedSession& session)
{
    // Coverage outranks the verdict: a job that failed after publishing
    // segments still leaves seconds of picture on screen, and taking those away
    // is worse than keeping them.
    if (session.covered) return CompletedSessionPlan::Segments;
    if (session.ok && session.publishedEntry) return CompletedSessionPlan::PublishedEntry;
    return CompletedSessionPlan::Stop;
}

// True once a rebuffer can end.
inline bool ShouldResume(const SessionView& view, double resumeLead = kResumeLead)
{
    return view.finished || Lead(view) >= resumeLead;
}

// Whether the running job should be pointed at a different hole. `target` is the
// hole it is filling; `wanted` is the hole the playhead needs, which is what
// NextRenderTarget answers.
//
// This replaces the rebase that used to stop the session and restart it at the
// playhead. Coverage is a set of regions now, so moving the render is all that
// is needed - nothing rendered is thrown away, and the user can be anywhere in
// the video, including inside a region an earlier job already finished. Unlike
// the rebase this also applies while playback is attached: a viewer who seeks
// back onto rendered frames still wants the render working where they are.
//
// A job filling the hole the playhead is in is rendering towards the viewer, so
// waiting beats restarting - up to the same budget: a job that has fallen more
// than `aheadBudget` seconds behind the playhead will not catch up, and starting
// again at the playhead costs one job startup instead of that wait.
//
// `frameDuration100ns` is the slack on "same hole". The two spans are never bit
// identical: a hole shrinks as the job publishes into it, and the job's own
// range was snapped to a frame boundary when it started. A first version
// compared the starts for equality, and a five-tick snap residual - coverage
// ending at 4999995 against a range starting at 5000000 - read as different
// work on every tick: the driven session cancelled and relaunched its helper
// four times in 110 ms, and one of those part-rendered jobs is where a stray
// half-second region came from.
inline bool ShouldRetarget(const SessionView& view, CoverageSpan target, CoverageSpan wanted,
                           int64_t frameDuration100ns, double aheadBudget = kRebaseAhead)
{
    if (view.seeking) return false;
    if (wanted.Empty()) return false;
    const int64_t position100ns = static_cast<int64_t>(std::llround(view.positionSec * 1e7));
    // Checked before the identity test below, which would otherwise hold for
    // every hole the viewer is standing in and pin a job that cannot catch up.
    if (target.Contains(position100ns)) {
        const double reach = std::max(double(target.start100ns) * 1e-7, view.headSec);
        return view.positionSec > reach + aheadBudget;
    }
    // The same hole, still being filled: leave the job alone.
    if (wanted.start100ns + frameDuration100ns >= target.start100ns &&
        wanted.end100ns <= target.end100ns)
        return false;
    // A backward nudge of a frame or two is frame snapping, not a seek, and the
    // hole it lands in is the one being filled anyway; anything else is the job
    // working somewhere the playhead is not.
    return view.positionSec + kBackwardSlack < double(target.start100ns) * 1e-7 ||
           view.positionSec >= double(target.end100ns) * 1e-7;
}

// Video seconds covered per second of wall clock. Meaningless until the job's
// fixed startup cost stops dominating, so early samples report nothing.
inline double RealtimeRatio(double coveredSec, double elapsedSec, double settleSec = 8.0)
{
    if (elapsedSec < settleSec || !(coveredSec > 0.0)) return 0.0;
    return coveredSec / elapsedSec;
}

} // namespace live_session
