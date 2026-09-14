#pragma once

// The helper's inbound command channel and the loop that serves jobs on it.
//
// Separated from NeuralWorkerMain so the protocol half can be exercised without
// a GPU: the tests drive a real anonymous pipe into a real CommandChannel and a
// runner that renders nothing, which is the only honest way to test a framing
// loop. NeuralWorkerMain supplies the runner that actually renders.
//
// Header-only because the only two translation units that need it are the
// helper's entry point and its test, and a resident helper's command channel is
// not something the hook-free player links.

#include "NeuralWorkerProtocol.h"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace resident_worker {

using neural_worker_protocol::CommandKind;

// The helper exits by itself after this long with no job. It bounds parked VRAM
// and releases the device; the parent relaunches on the next job, which costs
// the process launch it saved on every job before this one.
inline constexpr std::chrono::seconds kIdleTimeout{30};

struct Command {
    CommandKind kind{};
    // Job only, and only when the payload decoded.
    std::vector<std::wstring> arguments;
    // A Job whose payload DecodeJobArguments refused. The frame's length was
    // still known, so the channel stays in sync and the job is refused rather
    // than attempted with whatever survived the decode.
    bool undecodable{};
};

// Reads command frames off the parent's pipe on its own thread and watches the
// parent's process handle, so a Cancel, a Shutdown, a closed pipe or a dead
// parent stops a running job the instant it is seen instead of waiting for the
// job to finish and the loop to come back around.
//
// A reader thread rather than an overlapped wait: the parent hands over an
// anonymous pipe, and anonymous pipes are synchronous, so there is no event to
// put in a WaitForMultipleObjects. Both threads hold a reference to the shared
// state and the state owns the handles, so a destructor that detaches instead
// of joining a thread still blocked in ReadFile cannot leave a dangling
// reference or close a handle out from under it.
class CommandChannel {
public:
    enum class Wake { Command, Closed, Malformed, Idle, ParentExited };

    // Takes ownership of `commandPipe` (the read end). `parentProcess` is
    // borrowed and may be null; when it is not, its signalling ends the
    // session. Handles inherited from the parent live for the process, so the
    // channel never closes that one.
    CommandChannel(HANDLE commandPipe, HANDLE parentProcess)
        : state_(std::make_shared<State>())
    {
        state_->pipe = commandPipe;
        state_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        state_->quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::shared_ptr<State> state = state_;
        reader_ = std::thread([state] { ReadLoop(*state); });
        if (parentProcess) {
            parent_ = std::thread([state, parentProcess] { WatchParent(*state, parentProcess); });
        }
    }

    CommandChannel(const CommandChannel&) = delete;
    CommandChannel& operator=(const CommandChannel&) = delete;

    ~CommandChannel()
    {
        {
            std::lock_guard lock(state_->mutex);
            state_->quitting = true;
        }
        if (state_->quit) SetEvent(state_->quit);
        // The parent watcher leaves on the quit event. The reader is blocked in
        // ReadFile on a pipe only the parent can end, so it is detached: it
        // holds its own reference to the state and closes the pipe when it
        // finally returns.
        if (parent_.joinable()) parent_.join();
        if (reader_.joinable()) reader_.detach();
    }

    // Blocks until the next queued command, the channel ends, the parent exits
    // or `idle` elapses. `idle` is the remaining idle budget, not a fresh one:
    // a spurious wake must not extend the deadline the caller is tracking.
    Wake Next(std::chrono::milliseconds idle, Command& out)
    {
        const auto deadline = std::chrono::steady_clock::now() + idle;
        for (;;) {
            {
                std::lock_guard lock(state_->mutex);
                if (!state_->queue.empty()) {
                    out = std::move(state_->queue.front());
                    state_->queue.pop_front();
                    return Wake::Command;
                }
                // Ordered by which fact makes the others moot: a dead parent is
                // the end of the session whatever the pipe did, and a frame
                // this version cannot resynchronize from is the end of the pipe.
                if (state_->parentExited) return Wake::ParentExited;
                if (state_->malformed) return Wake::Malformed;
                if (state_->closed) return Wake::Closed;
            }
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) return Wake::Idle;
            const auto milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            const DWORD wait = WaitForSingleObject(
                state_->wake, static_cast<DWORD>(milliseconds > 0 ? milliseconds : 1));
            if (wait == WAIT_TIMEOUT) {
                // Re-checked rather than returned directly: a command may have
                // been queued between the drain above and the timeout.
                std::lock_guard lock(state_->mutex);
                if (state_->queue.empty() && !state_->parentExited && !state_->malformed &&
                    !state_->closed) {
                    return Wake::Idle;
                }
                continue;
            }
            if (wait != WAIT_OBJECT_0) return Wake::Closed;
        }
    }

    // Hands the running job's stop source to the reader and the parent watcher.
    // A stop already pending - Cancel, Shutdown, a closed pipe or a dead parent
    // seen while the loop was between jobs - is applied immediately, so the job
    // starts already cancelled rather than ignoring a request that arrived a
    // moment too early.
    void AttachJobStop(std::stop_source& source)
    {
        std::lock_guard lock(state_->mutex);
        state_->jobStop = &source;
        if (state_->stopPending) source.request_stop();
    }

    void DetachJobStop()
    {
        std::lock_guard lock(state_->mutex);
        state_->jobStop = nullptr;
        state_->stopPending = false;
    }

private:
    struct State {
        HANDLE pipe{};
        HANDLE wake{};
        HANDLE quit{};
        std::mutex mutex;
        std::deque<Command> queue;
        // Borrowed for the length of one job by the loop that owns it.
        std::stop_source* jobStop{};
        bool stopPending{};
        bool closed{};
        bool malformed{};
        bool parentExited{};
        bool quitting{};

        ~State()
        {
            if (pipe) CloseHandle(pipe);
            if (wake) CloseHandle(wake);
            if (quit) CloseHandle(quit);
        }
    };

    static void RequestStop(State& state)
    {
        state.stopPending = true;
        if (state.jobStop) state.jobStop->request_stop();
    }

    static bool ReadExactly(HANDLE pipe, void* data, size_t bytes)
    {
        auto* cursor = static_cast<std::byte*>(data);
        while (bytes) {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(bytes > MAXDWORD ? MAXDWORD : bytes);
            if (!ReadFile(pipe, cursor, chunk, &read, nullptr) || !read) return false;
            cursor += read;
            bytes -= read;
        }
        return true;
    }

    static void ReadLoop(State& state)
    {
        using namespace neural_worker_protocol;
        for (;;) {
            WireHeader header{};
            if (!ReadExactly(state.pipe, &header, sizeof(header))) {
                std::lock_guard lock(state.mutex);
                state.closed = true;
                // The parent's write end is gone, so nothing will ever cancel
                // the running job but this.
                RequestStop(state);
                if (state.wake) SetEvent(state.wake);
                return;
            }
            // A header this version cannot vouch for leaves the stream at an
            // unknown offset: there is no resynchronizing from it, and guessing
            // is how a v5 stream gets read as a v6 one.
            const bool framed = header.magic == kMagic && header.version == kVersion &&
                                IsKnownCommand(header.kind) &&
                                header.payloadBytes <= kMaximumPayloadBytes &&
                                (header.kind == static_cast<uint16_t>(CommandKind::Job) ||
                                 header.payloadBytes == 0);
            if (!framed) {
                std::lock_guard lock(state.mutex);
                state.malformed = true;
                RequestStop(state);
                if (state.wake) SetEvent(state.wake);
                return;
            }
            Command command;
            command.kind = static_cast<CommandKind>(header.kind);
            if (header.payloadBytes) {
                std::vector<std::byte> payload(header.payloadBytes);
                if (!ReadExactly(state.pipe, payload.data(), payload.size())) {
                    std::lock_guard lock(state.mutex);
                    state.closed = true;
                    RequestStop(state);
                    if (state.wake) SetEvent(state.wake);
                    return;
                }
                auto arguments = DecodeJobArguments(payload);
                if (arguments) command.arguments = std::move(*arguments);
                else command.undecodable = true;
            } else if (command.kind == CommandKind::Job) {
                command.undecodable = true;
            }
            {
                std::lock_guard lock(state.mutex);
                if (command.kind == CommandKind::Cancel || command.kind == CommandKind::Shutdown)
                    RequestStop(state);
                state.queue.push_back(std::move(command));
                if (state.wake) SetEvent(state.wake);
            }
        }
    }

    static void WatchParent(State& state, HANDLE parentProcess)
    {
        const HANDLE handles[2]{parentProcess, state.quit};
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0) return;
        std::lock_guard lock(state.mutex);
        state.parentExited = true;
        RequestStop(state);
        if (state.wake) SetEvent(state.wake);
    }

    std::shared_ptr<State> state_;
    std::thread reader_;
    std::thread parent_;
};

// What one job did, as far as the loop needs to care.
enum class JobOutcome {
    Completed,
    // Reported a Result with `cancelled` set. The helper stays resident: a
    // cancel is the user's choice, not a broken process.
    Cancelled,
    // Reported a failed Result. The process must not serve another job: the
    // session log's failure lines are scanned session-wide, and a removed
    // device or a stalled GPU leaves state no reset here can describe.
    Invalidated,
    // Reported a failed Result for a job the protocol refused. The helper stays
    // resident: a job that does not parse is the parent's mistake, not evidence
    // that this process is broken.
    Refused,
    // The metadata pipe is gone; there is nobody left to report to.
    WriteFailed,
};

enum class ResidentExit {
    Shutdown,
    Idle,
    ParentExited,
    Closed,
    Malformed,
    WriteFailed,
    JobInvalidated,
};

// Serves jobs until one of the ResidentExit reasons. `runner` must provide:
//   bool Ready();                                             // answer Hello
//   JobOutcome Job(std::span<const std::wstring>, std::stop_token);
//   bool Refuse(std::wstring_view detail);                    // failed Result
template <class Runner>
ResidentExit RunResidentLoop(CommandChannel& channel, Runner& runner,
                             std::chrono::milliseconds idle =
                                 std::chrono::duration_cast<std::chrono::milliseconds>(kIdleTimeout))
{
    auto deadline = std::chrono::steady_clock::now() + idle;
    for (;;) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) return ResidentExit::Idle;
        Command command;
        switch (channel.Next(std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
                             command)) {
            case CommandChannel::Wake::Idle: return ResidentExit::Idle;
            case CommandChannel::Wake::Closed: return ResidentExit::Closed;
            case CommandChannel::Wake::Malformed: return ResidentExit::Malformed;
            case CommandChannel::Wake::ParentExited: return ResidentExit::ParentExited;
            case CommandChannel::Wake::Command: break;
        }
        switch (command.kind) {
            case CommandKind::Hello:
                if (!runner.Ready()) return ResidentExit::WriteFailed;
                break;
            case CommandKind::Cancel:
                // The reader already stopped the job this was meant for, so a
                // Cancel that reaches the loop is one for a job that has
                // already reported. Nothing to do, and the helper stays.
                break;
            case CommandKind::Shutdown:
                return ResidentExit::Shutdown;
            case CommandKind::Job: {
                if (command.undecodable) {
                    if (!runner.Refuse(L"The helper could not decode the job's argument vector."))
                        return ResidentExit::WriteFailed;
                    deadline = std::chrono::steady_clock::now() + idle;
                    break;
                }
                std::stop_source stop;
                channel.AttachJobStop(stop);
                const JobOutcome outcome = runner.Job(command.arguments, stop.get_token());
                channel.DetachJobStop();
                // The idle budget is "no job for this long", so it runs from the
                // end of the last one.
                deadline = std::chrono::steady_clock::now() + idle;
                if (outcome == JobOutcome::WriteFailed) return ResidentExit::WriteFailed;
                if (outcome == JobOutcome::Invalidated) return ResidentExit::JobInvalidated;
                break;
            }
        }
    }
}

} // namespace resident_worker
