#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <semaphore>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// A single process-wide worker pool for the per-frame CPU passes.
//
// The readback pixel conversion and the temporal-guide block matching each used to
// create and join up to eight std::jthreads per video frame. Windows thread creation
// costs tens of microseconds, so at 60 fps that was close to a millisecond of pure spawn
// overhead per frame across the two sites. The pool also removes the hard cap of eight
// workers, which left more than half the cores idle on larger CPUs.
//
// Dispatch cost is the whole point here, so two things matter:
//
//  * Each worker has its own semaphore. A dispatch that only needs two ranges releases
//    exactly one worker instead of broadcasting to every thread in the pool. On a 32
//    thread machine an earlier condition_variable_any design cost ~840 us per dispatch,
//    which swamped every pass it was applied to.
//  * Nothing in the hot path waits on a std::stop_token. The stop_token overloads of
//    condition_variable_any register and unregister a stop_callback on every wait, and
//    that alone dominated the handshake.
//
// Bodies passed to ParallelForRanges must not throw. An exception escaping a pool worker
// terminates the process, as it would for any bare std::thread.

#if defined(_MSC_VER)
#pragma warning(push)
// C4324: "structure was padded due to alignment specifier". That padding is the point:
// it keeps the two hot atomics, and each worker's semaphore, on separate cache lines.
#pragma warning(disable : 4324)
#endif

namespace parallel_detail {

// Fixed rather than std::hardware_destructive_interference_size: that constant is
// tuning dependent, warns under GCC, and 64 is correct on every CPU this ships to.
inline constexpr size_t kCacheLine = 64;

// Helpers normally finish while the caller is still draining its own chunks, so a short
// spin avoids parking the caller only to wake it microseconds later. Measured: dropping
// this to zero costs about 10% on guide generation. Only the calling thread spins, so it
// does not steal cores from the ffmpeg children.
inline constexpr int kCallerSpin = 512;

inline bool& InPoolWorkerFlag()
{
    thread_local bool inWorker = false;
    return inWorker;
}

class WorkerPool {
public:
    static WorkerPool& Instance()
    {
        static WorkerPool pool;
        return pool;
    }

    // Run() keeps exactly one task slot, so two threads must never dispatch into the same
    // pool at once. A caller that needs to fan out while another thread is already fanning
    // out builds its own pool instead of sharing this one. Workers park on a semaphore, so
    // an idle second pool costs nothing but its stacks.
    explicit WorkerPool(size_t width = 0) { Construct(width ? width : DefaultWidth()); }

    // Total parallelism a pool takes when its width is left unspecified. One core is left
    // for the decoder queue thread and the ffmpeg child processes.
    static size_t DefaultWidth()
    {
        const unsigned int hardware = std::max(1u, std::thread::hardware_concurrency());
        return hardware > 1u ? size_t(hardware) - 1u : size_t(1);
    }

    ~WorkerPool()
    {
        stopping_.store(true, std::memory_order_release);
        for (auto& worker : workers_) worker->go.release();
        for (auto& worker : workers_) {
            if (worker->thread.joinable()) worker->thread.join();
        }
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Total parallelism including the calling thread.
    size_t Width() const noexcept { return workers_.size() + 1u; }

    // Runs task(chunk) for chunk in [0, chunks) and returns once all have finished.
    // A second thread dispatching while a fan-out is in flight runs its own task on the
    // calling thread instead of sharing the single task slot: the slot's lambda lives on
    // the first dispatcher's stack, and overwriting it would hand helpers a dangling
    // pointer. Serial is the safe answer; the caller that wants real overlap builds its
    // own pool (see the constructor).
    void Run(size_t chunks, const std::function<void(size_t)>& task)
    {
        if (chunks == 0) return;
        if (chunks == 1 || workers_.empty() || busy_.exchange(true, std::memory_order_acq_rel)) {
            for (size_t chunk = 0; chunk < chunks; ++chunk) task(chunk);
            return;
        }

        // The caller takes one share of the work, so only wake enough helpers to cover
        // the rest. A two range split must not cost thirty thread wakeups.
        const size_t helpers = std::min(chunks - 1u, workers_.size());

        task_ = &task;
        chunkCount_ = chunks;
        nextChunk_.store(0, std::memory_order_relaxed);
        pending_.store(helpers, std::memory_order_release);
        for (size_t i = 0; i < helpers; ++i) workers_[i]->go.release();

        RunChunks(task, chunks);

        // Helpers usually finish while the caller is still draining its own chunks, so
        // spin briefly before parking.
        for (int spin = 0; spin < kCallerSpin; ++spin) {
            if (pending_.load(std::memory_order_acquire) == 0) break;
            std::this_thread::yield();
        }
        for (;;) {
            const size_t remaining = pending_.load(std::memory_order_acquire);
            if (remaining == 0) break;
            pending_.wait(remaining, std::memory_order_acquire);
        }
        task_ = nullptr;
        chunkCount_ = 0;
        busy_.store(false, std::memory_order_release);
    }

private:
    struct alignas(kCacheLine) Worker {
        std::binary_semaphore go{0};
        std::thread thread;
    };

    void Construct(size_t width)
    {
        workers_.reserve(width - 1u);
        for (size_t i = 0; i + 1u < width; ++i) {
            auto worker = std::make_unique<Worker>();
            Worker* raw = worker.get();
            try {
                raw->thread = std::thread([this, raw] { WorkerLoop(raw); });
            } catch (const std::system_error&) {
                break;  // Fewer workers than requested is fine; the caller still runs.
            }
            workers_.push_back(std::move(worker));
        }
    }

    void RunChunks(const std::function<void(size_t)>& task, size_t chunks)
    {
        const bool previous = InPoolWorkerFlag();
        InPoolWorkerFlag() = true;
        for (;;) {
            const size_t chunk = nextChunk_.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= chunks) break;
            task(chunk);
        }
        InPoolWorkerFlag() = previous;
    }

    void WorkerLoop(Worker* self)
    {
        for (;;) {
            self->go.acquire();
            if (stopping_.load(std::memory_order_acquire)) return;
            const std::function<void(size_t)>* task = task_;
            const size_t chunks = chunkCount_;
            if (task) RunChunks(*task, chunks);
            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) pending_.notify_all();
        }
    }

    // Written by the dispatching thread before helpers are released and read by them
    // after, so the semaphore release/acquire pair provides the ordering.
    const std::function<void(size_t)>* task_ = nullptr;
    size_t chunkCount_ = 0;
    alignas(kCacheLine) std::atomic<size_t> nextChunk_{0};
    alignas(kCacheLine) std::atomic<size_t> pending_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> busy_{false};
    std::vector<std::unique_ptr<Worker>> workers_;
};

}  // namespace parallel_detail

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Splits [0, count) into contiguous ranges and invokes body(begin, end) on each, fanning
// out across the given pool. Pass a pool other than the default one only when this call
// may overlap a dispatch made by another thread; see WorkerPool's constructor.
template <class Body>
void ParallelForRangesIn(parallel_detail::WorkerPool& pool, size_t count,
                         size_t minItemsPerRange, Body&& body)
{
    if (count == 0) return;

    size_t ranges = 1;
    if (minItemsPerRange > 0 && !parallel_detail::InPoolWorkerFlag())
        ranges = std::min<size_t>(pool.Width(), std::max<size_t>(1u, count / minItemsPerRange));

    if (ranges <= 1) {
        body(size_t{0}, count);
        return;
    }

    const size_t perRange = (count + ranges - 1) / ranges;
    ranges = (count + perRange - 1) / perRange;
    const std::function<void(size_t)> task = [&](size_t range) {
        const size_t begin = range * perRange;
        body(begin, std::min(count, begin + perRange));
    };
    pool.Run(ranges, task);
}

// minItemsPerRange keeps small workloads on the calling thread: the split only happens
// when there is enough work for at least two ranges of that size. Contiguous ranges are
// used rather than strided indices because every caller here streams memory.
template <class Body>
void ParallelForRanges(size_t count, size_t minItemsPerRange, Body&& body)
{
    ParallelForRangesIn(parallel_detail::WorkerPool::Instance(), count, minItemsPerRange,
                        std::forward<Body>(body));
}
