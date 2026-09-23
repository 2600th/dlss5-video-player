#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Runs a capture's readback copy off the render loop, and the wait for the GPU to finish
// writing it.
//
// The copy is around 10 MiB of memcpy per frame and, taken inline, it was the largest
// single item in the loop. It depends on nothing the loop does next, so it happens here
// while the loop decodes, builds guides and submits the following frame. So does the wait
// on the capture's fence, which used to block the loop at every Post. The readback slot
// the view points into stays reserved by the renderer until Join returns, so the GPU
// cannot land the next capture on top of the memory being copied.
//
// The worker thread is created on first use and lives for the job. Spawning one per frame
// was never an option: Windows thread creation costs tens of microseconds, which is the
// same reason the parallel passes share a pool rather than spawning.
//
// `View` is the readback description the copy reads from and `Copy` is a callable
// `void(View&, std::vector<uint8_t>&)`; it may record what happened in the view (how
// the wait went, how long it took), and Join hands that view back with the bytes, so
// what the renderer must learn about the copy travels with it rather than beside it.
// Both are template parameters so the lifecycle can be exercised without a D3D12
// device: the Post/Join/Shutdown state machine is where this class has historically
// gone wrong, and it is independent of what the copy does.
template <class View, class Copy>
class DeferredCaptureWorker {
public:
    DeferredCaptureWorker() = default;
    explicit DeferredCaptureWorker(Copy copy) : m_copy(std::move(copy)) {}
    DeferredCaptureWorker(const DeferredCaptureWorker&) = delete;
    DeferredCaptureWorker& operator=(const DeferredCaptureWorker&) = delete;
    ~DeferredCaptureWorker() { Shutdown(); }

    // True while a posted copy has not been joined, and therefore while a readback slot
    // is still spoken for.
    bool Posted() const { std::scoped_lock lock(m_mutex); return m_posted; }

    // Hands the copy to the worker. `scratch` is recycled as the destination buffer, so
    // the full-frame allocation does not repeat every frame.
    void Post(const View& view, std::vector<uint8_t>&& scratch)
    {
        // std::thread::joinable() stays true for a thread that has already
        // returned, so a finished worker has to be joined before another can
        // take its place. Without this, Post saw a joinable corpse, started
        // nothing, and notified a condition variable nobody was waiting on.
        bool finished = false;
        { std::scoped_lock lock(m_mutex); finished = m_finished; }
        if (finished && m_worker.joinable()) m_worker.join();
        {
            std::scoped_lock lock(m_mutex);
            // Cleared here rather than in Shutdown: one instance serves several
            // jobs, and a latch left set by the Release between them made the
            // next worker exit after a single copy.
            m_quit = false;
            m_finished = false;
            m_view = view; m_pixels = std::move(scratch); m_posted = true; m_busy = true;
        }
        if (!m_worker.joinable()) m_worker = std::thread([this] { Loop(); });
        m_wake.notify_one();
    }

    // Blocks until the posted copy has finished and moves its bytes into `pixels`, and
    // the view as the copy left it into `view` when one is asked for. False when nothing
    // was posted, which leaves both alone.
    bool Join(std::vector<uint8_t>& pixels, View* view = nullptr)
    {
        std::unique_lock lock(m_mutex);
        if (!m_posted) return false;
        m_idle.wait(lock, [this] { return !m_busy; });
        pixels = std::move(m_pixels); m_pixels.clear(); m_posted = false;
        if (view) *view = m_view;
        return true;
    }

    void Shutdown()
    {
        if (!m_worker.joinable()) return;
        { std::scoped_lock lock(m_mutex); m_quit = true; }
        m_wake.notify_one();
        m_worker.join();
    }

private:
    void Loop()
    {
        for (;;) {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_busy || m_quit; });
            // A copy already posted is finished before quitting, so a Join racing the
            // shutdown still gets its bytes rather than blocking forever.
            if (!m_busy) { m_finished = true; return; }
            View view = m_view;
            std::vector<uint8_t> pixels = std::move(m_pixels);
            lock.unlock();
            m_copy(view, pixels);
            lock.lock();
            m_view = view; m_pixels = std::move(pixels); m_busy = false;
            lock.unlock();
            m_idle.notify_one();
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_wake, m_idle;
    Copy m_copy{};
    View m_view{};
    std::vector<uint8_t> m_pixels;
    bool m_posted = false, m_busy = false, m_quit = false;
    // Set by the worker under the mutex as it returns, so Post can tell a
    // running worker from a joinable corpse.
    bool m_finished = false;
    std::thread m_worker;
};
