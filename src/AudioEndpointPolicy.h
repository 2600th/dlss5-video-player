#pragma once

#include <mmdeviceapi.h>

#include <atomic>
#include <string_view>

// Which endpoint notifications mean "the stream this player is on has gone".
//
// Recovery used to be polled: the renderer noticed only when a call returned
// AUDCLNT_E_DEVICE_INVALIDATED, which covers an endpoint that disappears and
// nothing else. Three things it cannot see:
//
//  * A default-device change. The old endpoint keeps working, so the film
//    plays on out of the device the user just stopped using.
//  * A mix-format change under a running stream. The stream was opened at the
//    old format and ffmpeg is still producing it.
//  * A driver that stops asking for data without erroring. Every call
//    succeeds; the film just goes quiet.
//
// The decisions are separated from the COM callbacks so they can be argued
// with in a test rather than by unplugging things.
namespace audio_endpoint {

// A new default render endpoint. Roles matter: the player follows the console
// and multimedia roles, and deliberately not communications - that is the
// headset role, and following it would move a film's audio the moment someone
// took a call.
//
// An empty `ourId` means Open has not run, so nothing can be about us yet; a
// blank id must not match every notification. An empty `newId` means there is
// no default left at all, which is a loss rather than a no-op.
inline bool DefaultChangeAffectsUs(EDataFlow flow, ERole role, std::wstring_view newId,
                                   std::wstring_view ourId)
{
    if (ourId.empty()) return false;
    if (flow != eRender) return false;
    if (role != eConsole && role != eMultimedia) return false;
    // Already the one being rendered to. Restarting would be an audible gap
    // in exchange for nothing.
    return newId != ourId;
}

inline bool StateChangeAffectsUs(std::wstring_view changedId, DWORD newState,
                                 std::wstring_view ourId)
{
    if (ourId.empty() || changedId != ourId) return false;
    return newState != DEVICE_STATE_ACTIVE;
}

// `isDeviceFormatKey` is whether the changed property was
// PKEY_AudioEngine_DeviceFormat. The caller compares the key, because a
// PROPERTYKEY does not belong in a policy that a test has to construct.
inline bool FormatChangeAffectsUs(std::wstring_view changedId, std::wstring_view ourId,
                                  bool isDeviceFormatKey)
{
    if (ourId.empty() || changedId != ourId) return false;
    return isDeviceFormatKey;
}

// The door between the OS's notification threads and the renderer they
// report to.
//
// The OS calls endpoint notifications on its own threads, at any moment up to
// and possibly past the Unregister call, and nothing documents that
// Unregister waits for a callback already running. The back pointer used to
// be a plain pointer cleared under the renderer's mutex while callbacks read
// it with no lock at all - a data race - and Close held that mutex across
// Unregister while every handler takes it, which deadlocks if Unregister does
// wait.
//
// Each call counts itself in `inFlight_` BEFORE reading the owner, and Detach
// clears the owner and then waits for the count to drain. Both sides are
// sequentially consistent, so a call either counted itself before Detach
// looked at the count - and Detach waits for it - or reads the null Detach
// stored and touches nothing. Once Detach returns, no call can reach the
// owner, which is what makes it safe to unregister with no lock held and
// destroy the owner afterwards. Detach must not be called while holding a
// lock the handlers take, and a handler must never call Detach.
template <typename Owner>
class CallbackGate {
public:
    explicit CallbackGate(Owner* owner) : owner_(owner) {}

    template <typename Handler>
    void Forward(Handler&& handler)
    {
        inFlight_.fetch_add(1);
        if (Owner* const owner = owner_.load()) handler(*owner);
        if (inFlight_.fetch_sub(1) == 1) inFlight_.notify_all();
    }

    void Detach()
    {
        owner_.store(nullptr);
        for (unsigned running = inFlight_.load(); running; running = inFlight_.load())
            inFlight_.wait(running);
    }

private:
    std::atomic<Owner*> owner_;
    std::atomic<unsigned> inFlight_{0};
};

} // namespace audio_endpoint

// The guard for a sink that stopped asking for data without erroring.
namespace audio_sink {

// Kodi treats 1100 ms without the render event as a dead sink, for drivers
// that stop signalling silently. The engine period here is 22 ms, so this is
// fifty missed periods - far past an underrun and far short of anything a
// viewer would sit through.
inline constexpr double kDeadSeconds = 1.1;

struct State {
    double lastSignalSeconds = 0.0;
    bool have = false;
};

// `signalled` is whether the endpoint asked for data this time round.
// `playing` is false whenever it is not expected to: a paused stream never
// signals, and declaring one dead would restart the pipeline every time
// somebody paused for a minute.
inline bool Dead(State& state, double nowSeconds, bool signalled, bool playing)
{
    if (signalled || !state.have) {
        state.have = true;
        state.lastSignalSeconds = nowSeconds;
        return false;
    }
    // Carried forward while paused, so a long pause does not arrive at the
    // resume already counted as a death.
    if (!playing) {
        state.lastSignalSeconds = nowSeconds;
        return false;
    }
    return nowSeconds - state.lastSignalSeconds > kDeadSeconds;
}

} // namespace audio_sink
