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
// What a job costs before it renders anything: helper launch, runtime, feature
// arm and preroll, measured at about 7.3 s across the sessions in docs. A hole
// narrower than this is cheaper to watch on the original than to render.
inline constexpr double kColdStartSeconds = 7.3;
// A seek backwards is always out of coverage, but a tiny backwards nudge is
// usually a rounding artifact of frame snapping rather than a real seek.
inline constexpr double kBackwardSlack = 0.5;
// A viewer tapping the seek key moves the playhead about once a second, and
// each move used to cancel the running job and restart it on the new hole.
// Measured across six presses 900 ms apart: without this, five restarts and
// not one segment rendered during the burst; with it, the running job is left
// alone and publishes six, and one retarget fires after the last press. What
// it buys is render throughput while the viewer is moving.
//
// It is NOT free for the viewer. A restart reaches its first segment in about
// 1.1 s, so this defers the frames at the destination by roughly the settle
// window: measured 0.5 s after the last press without it against 2.5 s with
// it. The original plays there in the meantime, which is why the trade is
// worth taking - and why this is not sized "under" the restart cost, it is
// about equal to it.
//
// A physically held key is not this case: Windows auto-repeats at about 30 Hz
// after its half-second delay, and those seeks queue, so the in-flight rule
// already coalesces them. This owns the tapped key and the clicked button.
inline constexpr double kSeekSettleSeconds = 1.0;

// Seconds of uninterrupted playback one buffer fill should buy when the render
// runs SLOWER than real time. A frame-generated source is the case: a 2x
// conversion doubles the frames the render has to produce without changing the
// clock they have to arrive by, and 2560x1440 at 119.88 fps measured 0.814x
// real time on an RTX 5090.
//
// Below real time the buffer DRAINS while playback runs - the lead falls by
// (1 - ratio) every second - so no fixed cushion prevents a rebuffer. It only
// decides how often one happens, and the total watching time is fixed by the
// ratio whatever this is set to. What it buys is fewer interruptions: at
// 0.814x the old two-second resume bought about eleven seconds of playback
// before the next stall, which is the "plays a few seconds then pauses" cycle.
// A minute is long enough to stop reading as stuttering.
inline constexpr double kSustainSeconds = 60.0;
// ...but not at any price. The fill that buys it is time spent watching a
// buffering panel, and past this a viewer would rather have the original.
inline constexpr double kMaxRefillWaitSeconds = 20.0;
// Cushion ceiling, whatever the arithmetic says.
inline constexpr double kMaxLead = 30.0;

// The cushion a sub-real-time render needs. Two bounds, and the smaller wins:
// what buys kSustainSeconds of playback, and what can be refilled inside
// kMaxRefillWaitSeconds.
inline double SustainedLead(double realtimeRatio)
{
    const double drainPerSecond = 1.0 - realtimeRatio;
    const double buysASustainedRun = drainPerSecond * kSustainSeconds;
    const double refillableInTime = kMaxRefillWaitSeconds * realtimeRatio;
    return std::clamp(std::min(buysASustainedRun, refillableInTime), kStartLead, kMaxLead);
}

// Lead to require before the first attach, given how fast this GPU renders
// relative to real time (`LiveRenderForecast::realtimeRatio`). The 4 s cushion
// is sized for a card that barely keeps up; on one that renders three times
// faster the buffer refills faster than playback drains it, so the same cushion
// only makes the user wait. An unknown pace keeps the full cushion.
//
// The sub-real-time arm is the one this function was missing: it could only
// ever SHRINK the cushion, so the case that needs a bigger one - the render
// losing ground on every frame played - got the same four seconds as a card
// that keeps up exactly.
inline double StartLead(double realtimeRatio, double startLead = kStartLead)
{
    if (!(realtimeRatio > 0.0)) return startLead;
    if (realtimeRatio >= 3.0) return std::min(startLead, 1.0);
    if (realtimeRatio >= 1.5) return std::min(startLead, 2.0);
    if (realtimeRatio >= 1.0) return startLead;
    return std::max(startLead, SustainedLead(realtimeRatio));
}

// The same question for ending a rebuffer, and the more important one: the
// first attach happens once, this happens every time the buffer runs dry.
// Resuming a sub-real-time render on the small cushion is what turns one stall
// into a cycle of them - it plays until the drain has eaten those two seconds
// and stops again.
inline double ResumeLead(double realtimeRatio, double resumeLead = kResumeLead)
{
    if (!(realtimeRatio > 0.0)) return resumeLead;
    if (realtimeRatio >= 1.0) return resumeLead;
    return std::max(resumeLead, SustainedLead(realtimeRatio));
}

struct SessionView {
    double positionSec = 0.0;   // where playback is
    double rangeStartSec = 0.0; // where the job began rendering
    double headSec = 0.0;       // newest rendered timestamp, 0 when nothing landed
    bool attached = false;      // playback is running on the rendered segments
    bool finished = false;      // the job published its last segment
    bool seeking = false;       // a seek is in flight or the user is scrubbing
    bool paused = false;        // the playhead is not moving, so a narrow hole
                                // in front of it is still worth rendering
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

// Whether the cache entry a finished job published can be offered as the
// session's converted video. An entry is the join of ONE job's segments,
// labelled with that job's range, while "Save converted video" writes the
// entry out under the SESSION's range - so the two only agree when a single
// job rendered every frame of that range. A session that filled its range in
// several jobs has several entries and none of them is the film: one 90 s
// session offered its last 12 s hole under the film's name and exported that.
// `frameDuration100ns` is the same sub-frame slack UncoveredSpans applies: an
// integer-frame head lands a few ticks short of a fractional rate's declared
// end, and that residual is coverage, not a missing frame.
inline bool ExportableEntry(const std::vector<CoverageSpan>& covered, CoverageSpan range,
                            CoverageSpan entry, int64_t frameDuration100ns)
{
    if (range.Empty() || entry.Empty()) return false;
    if (!UncoveredSpans(covered, range, frameDuration100ns).empty()) return false;
    return entry.start100ns <= range.start100ns + frameDuration100ns &&
           entry.end100ns + frameDuration100ns >= range.end100ns;
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
//
// `jobHeadSec` is how far the running job has actually rendered inside its own
// target. It is deliberately NOT `view.headSec`, which is the end of the region
// around the PLAYHEAD and therefore zero whenever the playhead sits in a hole:
// with that, a viewer who seeks one second ahead of a render head looked like a
// viewer the job would never reach, and the helper was restarted - seven
// seconds of startup - instead of waited out for one.
inline bool ShouldRetarget(const SessionView& view, CoverageSpan target, CoverageSpan wanted,
                           int64_t frameDuration100ns, double jobHeadSec,
                           double aheadBudget = kRebaseAhead)
{
    if (view.seeking) return false;
    if (wanted.Empty()) return false;
    // The job has rendered its whole hole and is on its way to publishing: the
    // completion path starts the next one a tick later. Cancelling here throws
    // away the cache entry and receipt it was about to promote for work that is
    // already done - one driven session did exactly that, five seconds after the
    // job finished [100.1,113) s, because the completion message had not been
    // processed yet and the job still counted as running.
    if (jobHeadSec >= double(target.end100ns) * 1e-7) return false;
    const int64_t position100ns = static_cast<int64_t>(std::llround(view.positionSec * 1e7));
    // Checked before everything below, which would otherwise hold for every hole
    // the viewer is standing in and pin a job that cannot catch up.
    if (target.Contains(position100ns)) {
        const double reach = std::max(double(target.start100ns) * 1e-7, jobHeadSec);
        return view.positionSec > reach + aheadBudget;
    }
    // The same hole, still being filled: leave the job alone.
    if (wanted.start100ns + frameDuration100ns >= target.start100ns &&
        wanted.end100ns <= target.end100ns)
        return false;
    // A hole narrower than a job startup is not worth taking a running job away
    // for: the startup is paid twice, once for the sliver and once to come back,
    // and by the time its first frame exists the playhead has left it - a hole is
    // bounded by coverage, so the viewer crosses it straight onto rendered
    // frames. One driven session traded a job rendering [38.6,104.4) for a
    // one-second hole. A PAUSED viewer is the exception: that playhead is not
    // going anywhere, so the frame in front of them is worth rendering however
    // narrow the hole around it. Nothing is stranded either way - the session
    // starts on that hole as soon as the running job ends, because this rule
    // only governs taking a job away.
    if (!view.paused && wanted.Width() < static_cast<int64_t>(kColdStartSeconds * 1e7)) return false;
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
