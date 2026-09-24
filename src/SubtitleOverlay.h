#pragma once

#include "SubtitlePolicy.h"

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace subtitle {

// One picture of the overlay: premultiplied BGRA at the canvas size, shown from
// `pts` (subtitle time) until the next frame's. No pixels when nothing is drawn.
struct Frame {
    double pts = 0.0;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
    // Every non-zero pixel of `bgra` is inside this: found by the reader, off the
    // UI thread, so the renderer uploads only what changes (see NonZeroBounds).
    PixelBox drawn;

    bool Empty() const { return bgra.empty(); }
};

// What a probe found in and beside one file.
struct Discovery {
    std::wstring media;
    std::vector<Track> tracks;
    // A subtitle file beside the video, its full path; empty when none.
    std::wstring sidecar;
    // The container's start time: where its own timestamps begin when the
    // player's clock reads zero.
    double origin = 0.0;
    // For a subtitle file in a legacy code page, the charenc it needs
    // (subtitle::CharencFor); empty for everything else.
    std::wstring charenc;
    // ffprobe answered. A file it could not read has no tracks either way,
    // but only this says why.
    bool probed = false;
};

} // namespace subtitle

// The subtitle overlay's worker: probes files for subtitle streams, and runs
// the ffmpeg child that draws the chosen one at the picture's size, keeping
// the few frames around the clock the player asks about.
//
// Nothing here blocks its caller. Every request is posted to one worker
// thread, which owns every child; a request that makes a running child
// pointless terminates its job, which is what ends the worker's blocking read
// of the child's pipe. The UI thread asks FrameAt on every tick, which only
// takes a lock and moves a pointer.
class SubtitleOverlay {
public:
    struct Source {
        // The video (an embedded stream) or a subtitle file the viewer chose.
        std::wstring path;
        int stream = 0;
        bool external = false;
        std::string codec;
        double origin = 0.0;
        // Only a text file the viewer picked needs one; see subtitle::CharencFor.
        std::wstring charenc;

        friend bool operator==(const Source&, const Source&) = default;
    };
    struct Canvas {
        uint32_t width = 0, height = 0;
        uint32_t videoWidth = 0, videoHeight = 0;
        double rate = 24.0;
        double duration = 0.0;

        friend bool operator==(const Canvas&, const Canvas&) = default;
    };

    explicit SubtitleOverlay(std::filesystem::path helperDirectory = {});
    ~SubtitleOverlay();
    SubtitleOverlay(const SubtitleOverlay&) = delete;
    SubtitleOverlay& operator=(const SubtitleOverlay&) = delete;

    // Lists the subtitle streams of `media` - and, with lookBeside, the file
    // beside it the player loads by itself. Each answer arrives through
    // TakeDiscovery, once, in the order they were asked for; the caller tells
    // them apart by Discovery::media.
    void Discover(const std::wstring& media, bool lookBeside);
    std::optional<subtitle::Discovery> TakeDiscovery();

    // Draws `source` on `canvas` from subtitle time `at`.
    void Show(const Source& source, const Canvas& canvas, double at);
    // The clock jumped to `at`. Frames already read answer it when they can
    // (subtitle::QueueAnswers); otherwise the child starts over there.
    void Seek(double at);
    void Hide();
    bool Showing() const;
    const Source* CurrentSource() const { return m_showing ? &m_shownSource : nullptr; }
    const Canvas& CurrentCanvas() const { return m_shownCanvas; }

    // The frame on screen at subtitle time `at`, or null while none is known.
    // The same pointer comes back until the picture changes.
    std::shared_ptr<const subtitle::Frame> FrameAt(double at);
    // Whether FrameAt(at) is final: a later change is already read, or the child
    // has drawn everything there is. Until then a change at `at` may still come.
    bool Settled(double at) const;

    // Frames kept ahead of the clock. Each is a canvas of BGRA; at 3840x2160
    // that is 33 MB, so the child is held a couple of changes ahead and no more.
    static constexpr size_t kFramesAhead = 2;

private:
    enum class Phase { Idle, Extracting, Rendering };
    struct Session;

    void Run();
    subtitle::Discovery Probe(const std::wstring& media, bool lookBeside);
    void RunSession(uint64_t generation);
    std::optional<std::wstring> Extracted(const Source& source, uint64_t generation);
    bool Spawn(const std::filesystem::path& exe, const std::wstring& arguments, Session& session, bool captureOutput);
    // Ends the running child, if it is doing something `force` - or a render
    // - says is now pointless. Called with the lock held.
    void InterruptLocked(bool force);
    std::filesystem::path FFmpeg() const;
    std::filesystem::path FFprobe() const;

    const std::filesystem::path m_helperDirectory;
    std::filesystem::path m_extractDirectory;
    // Worker-only: text streams already copied out, by (video, stream).
    std::map<std::pair<std::wstring, int>, std::filesystem::path> m_extracted;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;     // the worker: a request, or stop
    std::condition_variable m_progress; // the session: room ahead, or a request
    bool m_stop = false;

    std::deque<std::pair<std::wstring, bool>> m_probeRequests;
    std::deque<subtitle::Discovery> m_discoveries;

    // What the UI asked for. m_generation moves on every request that makes
    // the frames held so far wrong; the worker's session is m_sessionGeneration.
    bool m_showing = false;
    Source m_shownSource;
    Canvas m_shownCanvas;
    double m_startAt = 0.0;
    uint64_t m_generation = 0;
    uint64_t m_sessionGeneration = 0;

    // The running child, so a request can end it, and what it is doing.
    HANDLE m_activeJob = nullptr;
    Phase m_phase = Phase::Idle;

    std::deque<std::shared_ptr<const subtitle::Frame>> m_frames;
    bool m_ended = false;
    double m_consumedAt = 0.0;

    std::thread m_worker;
};
