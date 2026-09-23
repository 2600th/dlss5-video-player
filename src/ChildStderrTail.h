#pragma once

#include <windows.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

// The last few KiB a helper process wrote to stderr, for the log line that
// explains why it failed. ffprobe and ffmpeg used to write stderr to NUL, so a
// probe that failed logged an exit code and a decode that died mid-file
// logged nothing at all: "exitCode=1" and "Invalid data found when processing
// input" are the same line to a reader who only gets the first.
//
// Drained on a thread of its own, never by the thread reading stdout. An
// anonymous pipe has no overlapped mode, and a child that fills its stderr
// pipe blocks on the write - so a chatty child whose stderr nobody read would
// stop producing frames, which is a worse failure than the one this exists to
// explain. The thread keeps only the tail, so memory is bounded whatever the
// child writes.
class ChildStderrTail {
public:
    static constexpr size_t kCapacity = 4096;

    ChildStderrTail() = default;
    ~ChildStderrTail() { Stop(); }
    ChildStderrTail(const ChildStderrTail&) = delete;
    ChildStderrTail& operator=(const ChildStderrTail&) = delete;

    // The inheritable write end for STARTUPINFO::hStdError, or nullptr when no
    // pipe could be made - the caller then keeps NUL, as before.
    HANDLE Open(SECURITY_ATTRIBUTES& inheritable)
    {
        if (read_ || write_) return nullptr;
        if (!CreatePipe(&read_, &write_, &inheritable, 0)) { read_ = write_ = nullptr; return nullptr; }
        SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0);
        return write_;
    }

    // After CreateProcess, whether or not it succeeded: drops this process's
    // copy of the write end, so the pipe breaks when the child's does, and
    // starts draining when there is a child to drain.
    void Start(bool childStarted)
    {
        if (write_) { CloseHandle(write_); write_ = nullptr; }
        if (!read_) return;
        if (!childStarted) { CloseHandle(read_); read_ = nullptr; return; }
        try {
            thread_ = std::thread([this] { Drain(); });
        } catch (...) {
            // Unread, the pipe would fill and stall the child. Closing it makes
            // the child's writes fail instead, which is what NUL used to be.
            CloseHandle(read_); read_ = nullptr;
        }
    }

    // What the child wrote, after up to `waitMs` for its last bytes to arrive.
    // A child that has exited has closed its end, so the drain ends promptly;
    // the wait only bounds a grandchild that inherited the handle.
    std::string Collect(DWORD waitMs)
    {
        if (thread_.joinable()) WaitForSingleObject(thread_.native_handle(), waitMs);
        std::scoped_lock lock(mutex_);
        return tail_.size() > kCapacity ? tail_.substr(tail_.size() - kCapacity) : tail_;
    }

    // True the first time only: one child's failure is logged once, however
    // many reads go on to observe it.
    bool TakeReport()
    {
        const bool first = !reported_;
        reported_ = true;
        return first;
    }

    // Idempotent. Unparks a drain blocked in ReadFile (a killed child's pipe
    // can outlive it through an inherited handle) and joins it.
    void Stop()
    {
        if (thread_.joinable()) {
            DWORD waited;
            do {
                CancelSynchronousIo(thread_.native_handle());
                waited = WaitForSingleObject(thread_.native_handle(), 1);
            } while (waited == WAIT_TIMEOUT);
            thread_.join();
        }
        if (read_) { CloseHandle(read_); read_ = nullptr; }
        if (write_) { CloseHandle(write_); write_ = nullptr; }
    }

    // Keeps the last `capacity` bytes of `tail` + `chunk`. Trimmed in halves so
    // a steady trickle does not shift the buffer on every write.
    static void Append(std::string& tail, std::string_view chunk, size_t capacity = kCapacity)
    {
        if (chunk.size() >= capacity) { tail.assign(chunk.substr(chunk.size() - capacity)); return; }
        tail.append(chunk);
        if (tail.size() > 2 * capacity) tail.erase(0, tail.size() - capacity);
    }

    // The tail as log text: the last `capacity` bytes, control characters
    // other than newlines replaced, blank lines dropped, each remaining line
    // prefixed so a grep for the helper finds all of it.
    static std::string ForLog(std::string_view raw, std::string_view prefix, size_t capacity = kCapacity)
    {
        if (raw.size() > capacity) raw = raw.substr(raw.size() - capacity);
        std::string text;
        std::string line;
        const auto flush = [&] {
            const size_t first = line.find_first_not_of(' ');
            if (first != std::string::npos) {
                if (!text.empty()) text += '\n';
                text.append(prefix).append(line, first, std::string::npos);
            }
            line.clear();
        };
        for (const char c : raw) {
            if (c == '\n' || c == '\r') { flush(); continue; }
            const unsigned char u = static_cast<unsigned char>(c);
            line += (u < 0x20 || u == 0x7F) ? ' ' : c;
        }
        flush();
        return text;
    }

private:
    void Drain()
    {
        char buffer[1024];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(read_, buffer, sizeof(buffer), &got, nullptr) || got == 0) break;
            std::scoped_lock lock(mutex_);
            Append(tail_, std::string_view(buffer, got));
        }
    }

    HANDLE read_{};
    HANDLE write_{};
    std::thread thread_;
    std::mutex mutex_;
    std::string tail_;
    bool reported_{};
};
