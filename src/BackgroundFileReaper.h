#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

// Deletes files on a thread of its own.
//
// A live session retires its segment files once the joined cache entry serves
// them, and deleting them on the UI thread stalled presentation: on the RTX
// 5090 machine, with the cache under %LOCALAPPDATA%, removing eleven 1.7 MB
// segments another process had just closed took 88-102 ms, playback fell
// 103-113 ms behind and dropped 3 to 6 frames at the moment every render
// finished. Deleting the same number of fresh files from a script took 5 ms,
// so the cost is in whatever the system does to files a helper just wrote,
// and the only fix that does not depend on it is to keep it off the thread
// that presents.
//
// Files are tried in the order given. One that cannot be removed yet - still
// open elsewhere - is tried again after kRetryDelay, up to kAttempts times,
// then left for whoever removes its directory. One already gone counts as
// removed. Nothing is started until the first Remove, and destruction stops
// the thread after the file in hand, abandoning the rest.
class BackgroundFileReaper {
public:
    static constexpr int kAttempts = 5;
    static constexpr std::chrono::milliseconds kRetryDelay{200};

    BackgroundFileReaper() = default;
    BackgroundFileReaper(const BackgroundFileReaper&) = delete;
    BackgroundFileReaper& operator=(const BackgroundFileReaper&) = delete;
    ~BackgroundFileReaper()
    {
        if (thread_.joinable()) {
            thread_.request_stop();
            wake_.notify_all();
            thread_.join();
        }
    }

    void Remove(std::vector<std::filesystem::path> paths)
    {
        if (paths.empty()) return;
        {
            const std::lock_guard lock(mutex_);
            for (auto& path : paths) queue_.push_back({std::move(path), 0});
            if (!thread_.joinable())
                thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
        }
        wake_.notify_all();
    }

    // Waits until nothing is queued or in hand. For tests and for a caller
    // that must know the files are gone; the UI never calls it.
    bool WaitIdle(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return idle_.wait_for(lock, timeout, [this] { return queue_.empty() && !busy_; });
    }

    // Removals that were given up on, for the log and for tests.
    size_t Abandoned() const
    {
        const std::lock_guard lock(mutex_);
        return abandoned_;
    }

private:
    struct Pending {
        std::filesystem::path path;
        int attempts;
    };

    void Run(std::stop_token stop)
    {
        std::unique_lock lock(mutex_);
        while (!stop.stop_requested()) {
            if (queue_.empty()) {
                idle_.notify_all();
                wake_.wait(lock, stop, [this] { return !queue_.empty(); });
                continue;
            }
            Pending pending = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            std::error_code error;
            const bool removed = std::filesystem::remove(pending.path, error) || !error;
            lock.lock();
            busy_ = false;
            if (removed) continue;
            if (++pending.attempts >= kAttempts) {
                ++abandoned_;
                continue;
            }
            // Back of the queue, after a pause, so one held file does not hold
            // up the ones behind it and a retry loop never spins.
            queue_.push_back(std::move(pending));
            wake_.wait_for(lock, stop, kRetryDelay, [] { return false; });
        }
        idle_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable_any idle_;
    std::deque<Pending> queue_;
    bool busy_ = false;
    size_t abandoned_ = 0;
    std::jthread thread_;
};
