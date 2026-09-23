#include "SubtitleOverlay.h"

#include "HardErrorSuppression.h"
#include "KillOnCloseJob.h"
#include "Log.h"
#include "MediaTools.h"
#include "Utf8Text.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

// A child's standard error: the stats lines that stamp each frame, and the
// tail of anything else for the log line that explains a failure.
struct ErrorStream {
    std::mutex mutex;
    std::condition_variable arrived;
    std::deque<double> stamps;
    std::string tail;
    bool closed = false;

    void Line(std::string_view line)
    {
        if (const auto stamp = subtitle::ParseStatsLine(line)) {
            std::lock_guard lock(mutex);
            stamps.push_back(*stamp);
        } else if (!line.empty()) {
            std::lock_guard lock(mutex);
            tail.append(line).push_back(' ');
            if (tail.size() > 2048) tail.erase(0, tail.size() - 2048);
        }
        arrived.notify_all();
    }
};

} // namespace

struct SubtitleOverlay::Session {
    HANDLE job = nullptr, process = nullptr, output = nullptr, errors = nullptr;
    ErrorStream stderrStream;
    std::thread stderrReader;

    ~Session()
    {
        if (job) TerminateJobObject(job, 1);
        if (process) {
            if (WaitForSingleObject(process, 2000) != WAIT_OBJECT_0)
                LOG("Subtitles: a child did not exit within 2 s of being terminated.");
        }
        // The pipe breaks when the child's end goes, which ends the reader.
        if (stderrReader.joinable()) stderrReader.join();
        for (HANDLE handle : {output, errors, process, job})
            if (handle) CloseHandle(handle);
    }
    DWORD ExitCode() const
    {
        DWORD code = 1;
        if (!process || !GetExitCodeProcess(process, &code)) return 1;
        return code;
    }
    std::string Tail()
    {
        std::lock_guard lock(stderrStream.mutex);
        return stderrStream.tail;
    }
};

SubtitleOverlay::SubtitleOverlay(fs::path helperDirectory) : m_helperDirectory(std::move(helperDirectory))
{
    std::error_code error;
    m_extractDirectory = fs::temp_directory_path(error) / (L"DLSS5-subtitles-" + std::to_wstring(GetCurrentProcessId()));
    m_worker = std::thread([this] { Run(); });
}

SubtitleOverlay::~SubtitleOverlay()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        InterruptLocked(true);
    }
    m_wake.notify_all();
    m_progress.notify_all();
    if (m_worker.joinable()) m_worker.join();
    std::error_code error;
    if (!m_extracted.empty()) fs::remove_all(m_extractDirectory, error);
}

fs::path SubtitleOverlay::FFmpeg() const
{
    return media_tools::FindTool(m_helperDirectory, L"ffmpeg.exe", media_tools::Fallback::SearchPath);
}

fs::path SubtitleOverlay::FFprobe() const
{
    return media_tools::FindTool(m_helperDirectory, L"ffprobe.exe", media_tools::Fallback::SearchPath);
}

void SubtitleOverlay::InterruptLocked(bool force)
{
    if (m_activeJob && (force || m_phase == Phase::Rendering)) TerminateJobObject(m_activeJob, 1);
    m_progress.notify_all();
}

void SubtitleOverlay::Discover(const std::wstring& media, bool lookBeside)
{
    {
        std::lock_guard lock(m_mutex);
        m_probeRequests.emplace_back(media, lookBeside);
        // The worker probes between sessions, so a running one is ended; the
        // player shows what the probe answers with a new Show anyway.
        if (m_showing) {
            ++m_generation;
            m_frames.clear();
            m_ended = false;
        }
        InterruptLocked(true);
    }
    m_wake.notify_all();
}

std::optional<subtitle::Discovery> SubtitleOverlay::TakeDiscovery()
{
    std::lock_guard lock(m_mutex);
    if (m_discoveries.empty()) return std::nullopt;
    subtitle::Discovery taken = std::move(m_discoveries.front());
    m_discoveries.pop_front();
    return taken;
}

void SubtitleOverlay::Show(const Source& source, const Canvas& canvas, double at)
{
    {
        std::lock_guard lock(m_mutex);
        // A copy running for this same stream is kept going: only its first
        // use waits for it, and a resize or a seek in the meantime changes
        // nothing about what it copies.
        const bool sameSource = m_showing && m_shownSource == source;
        m_showing = true;
        m_shownSource = source;
        m_shownCanvas = canvas;
        m_startAt = std::max(0.0, at);
        m_consumedAt = at;
        ++m_generation;
        m_frames.clear();
        m_ended = false;
        InterruptLocked(!sameSource);
    }
    m_wake.notify_all();
}

void SubtitleOverlay::Seek(double at)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_showing) return;
        m_consumedAt = at;
        std::vector<double> pts;
        for (const auto& frame : m_frames) pts.push_back(frame->pts);
        if (m_sessionGeneration == m_generation && subtitle::QueueAnswers(pts, m_ended, at)) {
            m_progress.notify_all();
            return;
        }
        m_startAt = std::max(0.0, at);
        ++m_generation;
        m_frames.clear();
        m_ended = false;
        InterruptLocked(false);
    }
    m_wake.notify_all();
}

void SubtitleOverlay::Hide()
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_showing) return;
        m_showing = false;
        ++m_generation;
        m_frames.clear();
        m_ended = false;
        InterruptLocked(true);
    }
    m_wake.notify_all();
}

bool SubtitleOverlay::Showing() const
{
    std::lock_guard lock(m_mutex);
    return m_showing;
}

std::shared_ptr<const subtitle::Frame> SubtitleOverlay::FrameAt(double at)
{
    std::lock_guard lock(m_mutex);
    if (!m_showing || at < 0.0) return nullptr;
    m_consumedAt = at;
    size_t shown = subtitle::kNoTrack;
    for (size_t index = 0; index < m_frames.size() && m_frames[index]->pts <= at; ++index) shown = index;
    if (shown == subtitle::kNoTrack) return nullptr;
    if (shown > 0) {
        // The frames before the one on screen are done with; dropping them is
        // what lets the worker read the next change.
        m_frames.erase(m_frames.begin(), m_frames.begin() + std::ptrdiff_t(shown));
        m_progress.notify_all();
    }
    return m_frames.front();
}

bool SubtitleOverlay::Settled(double at) const
{
    std::lock_guard lock(m_mutex);
    if (!m_showing) return true;
    std::vector<double> pts;
    for (const auto& frame : m_frames) pts.push_back(frame->pts);
    return m_sessionGeneration == m_generation && (subtitle::QueueAnswers(pts, m_ended, at) || (m_ended && pts.empty()));
}

void SubtitleOverlay::Run()
{
    std::unique_lock lock(m_mutex);
    while (!m_stop) {
        m_wake.wait(lock, [&] {
            return m_stop || !m_probeRequests.empty() || (m_showing && m_generation != m_sessionGeneration);
        });
        if (m_stop) break;
        if (!m_probeRequests.empty()) {
            const auto request = m_probeRequests.front();
            m_probeRequests.pop_front();
            lock.unlock();
            subtitle::Discovery found = Probe(request.first, request.second);
            lock.lock();
            m_discoveries.push_back(std::move(found));
            continue;
        }
        // A drag across the timeline asks for a new start every few
        // milliseconds; a child per step would be spawned only to be killed.
        // The last request of a burst is the one that is run.
        for (uint64_t seen = m_generation;;) {
            if (m_wake.wait_for(lock, std::chrono::milliseconds(60), [&] {
                    return m_stop || !m_probeRequests.empty() || m_generation != seen;
                })) {
                if (m_stop || !m_probeRequests.empty()) break;
                seen = m_generation;
                continue;
            }
            break;
        }
        if (m_stop || !m_probeRequests.empty() || !m_showing) continue;
        const uint64_t generation = m_generation;
        m_sessionGeneration = generation;
        lock.unlock();
        RunSession(generation);
        lock.lock();
    }
}

bool SubtitleOverlay::Spawn(const fs::path& exe, const std::wstring& arguments, Session& session, bool captureOutput)
{
    if (exe.empty()) {
        LOG("Subtitles: FFmpeg's tools were not found; no subtitles.");
        return false;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE outputWrite = nullptr, errorWrite = nullptr;
    // A frame is width*height*4 bytes; a large pipe lets the child write most
    // of one before it waits for the reader.
    if (captureOutput) {
        if (!CreatePipe(&session.output, &outputWrite, &sa, 1u << 20)) return false;
        SetHandleInformation(session.output, HANDLE_FLAG_INHERIT, 0);
    }
    if (!CreatePipe(&session.errors, &errorWrite, &sa, 64 * 1024)) {
        if (outputWrite) CloseHandle(outputWrite);
        return false;
    }
    SetHandleInformation(session.errors, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = outputWrite ? outputWrite : nul;
    si.hStdError = errorWrite;
    std::wstring command = L"\"" + exe.wstring() + L"\" " + arguments;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    session.job = CreateKillOnCloseJob();
    PROCESS_INFORMATION pi{};
    BOOL started = FALSE;
    {
        const ScopedHardErrorSuppression noHardErrorDialog;
        started = session.job && CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                                                CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    }
    const DWORD startError = GetLastError();
    if (outputWrite) CloseHandle(outputWrite);
    CloseHandle(errorWrite);
    if (nul) CloseHandle(nul);
    if (!started) {
        LOG("Subtitles: CreateProcess(" << exe.filename().string() << ") failed winerr=" << startError);
        return false;
    }
    session.process = pi.hProcess;
    if (!AssignProcessToJobObject(session.job, pi.hProcess) || ResumeThread(pi.hThread) == DWORD(-1)) {
        LOG("Subtitles: the child could not be put in its job winerr=" << GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        return false;
    }
    CloseHandle(pi.hThread);
    session.stderrReader = std::thread([&session] {
        std::string pending;
        char buffer[4096];
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(session.errors, buffer, DWORD(sizeof(buffer)), &got, nullptr) || !got) break;
            pending.append(buffer, got);
            for (size_t end; (end = pending.find('\n')) != std::string::npos;) {
                std::string_view line(pending.data(), end);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                session.stderrStream.Line(line);
                pending.erase(0, end + 1);
            }
            if (pending.size() > 65536) pending.clear();
        }
        if (!pending.empty()) session.stderrStream.Line(pending);
        std::lock_guard lock(session.stderrStream.mutex);
        session.stderrStream.closed = true;
        session.stderrStream.arrived.notify_all();
    });
    return true;
}

subtitle::Discovery SubtitleOverlay::Probe(const std::wstring& media, bool lookBeside)
{
    subtitle::Discovery found;
    found.media = media;
    if (lookBeside) {
        // Only a file on disk has a folder to look in.
        std::error_code error;
        const fs::path video(media);
        if (fs::is_regular_file(video, error)) {
            std::vector<std::wstring> names;
            for (fs::directory_iterator it(video.parent_path(), error), end; !error && it != end; it.increment(error)) {
                const std::wstring name = it->path().filename().wstring();
                if (subtitle::IsSidecarExtension(it->path().extension().wstring())) names.push_back(name);
                if (names.size() > 4096) break;
            }
            if (const auto sidecar = subtitle::FindSidecar(video.filename().wstring(), names))
                found.sidecar = (video.parent_path() / *sidecar).wstring();
        }
    }
    const bool network = _wcsnicmp(media.c_str(), L"https://", 8) == 0 || _wcsnicmp(media.c_str(), L"http://", 7) == 0;
    if (network) return found;
    Session session;
    const std::wstring arguments =
        L"-v error -select_streams s "
        L"-show_entries stream=codec_name:stream_tags=language,title:"
        L"stream_disposition=default,forced,hearing_impaired,visual_impaired,comment:format=start_time "
        L"-of default=noprint_wrappers=0 -i \"" + media + L"\"";
    if (!Spawn(FFprobe(), arguments, session, true)) return found;
    // Bounded like the audio track probe: a track list is a few hundred bytes,
    // and a probe that does not answer in ten seconds is not going to.
    std::string text;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    char buffer[8192];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(session.output, nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) {
            if (GetTickCount64() >= deadline) break;
            {
                std::lock_guard lock(m_mutex);
                if (m_stop) break;
            }
            Sleep(2);
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(session.output, buffer, std::min<DWORD>(available, DWORD(sizeof(buffer))), &got, nullptr) || !got) break;
        if (text.size() + got > (1u << 20)) break;
        text.append(buffer, got);
    }
    WaitForSingleObject(session.process, deadline > GetTickCount64() ? DWORD(deadline - GetTickCount64()) : 0);
    if (session.ExitCode() != 0) {
        LOG("Subtitles: ffprobe could not list the subtitle streams (exit " << session.ExitCode() << "): " << session.Tail());
        return found;
    }
    found.probed = true;
    std::istringstream lines(text);
    std::string line;
    subtitle::Track current;
    bool inStream = false;
    const auto flag = [](const std::string& value) { return value == "1"; };
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[STREAM]") { current = {}; current.subtitleIndex = int(found.tracks.size()); inStream = true; continue; }
        if (line == "[/STREAM]") { if (inStream) found.tracks.push_back(current); inStream = false; continue; }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        if (!inStream) {
            if (key == "start_time") {
                try { const double start = std::stod(value); if (std::isfinite(start)) found.origin = start; } catch (...) {}
            }
            continue;
        }
        if (key == "codec_name") current.codec = value;
        else if (key == "TAG:language") current.language = value;
        else if (key == "TAG:title") current.title = value;
        else if (key == "DISPOSITION:default") current.isDefault = flag(value);
        else if (key == "DISPOSITION:forced") current.forced = flag(value);
        else if (key == "DISPOSITION:hearing_impaired") current.hearingImpaired = flag(value);
        else if (key == "DISPOSITION:visual_impaired") current.visualImpaired = flag(value);
        else if (key == "DISPOSITION:comment") current.comment = flag(value);
    }
    // A text file the viewer picked is read here, not by the player: the first
    // 64 KB say what encoding it is in.
    if (!lookBeside && !found.tracks.empty() && found.tracks.front().Drawn() == subtitle::Kind::Text) {
        std::ifstream file(fs::path(media), std::ios::binary);
        std::vector<uint8_t> head(65536);
        file.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
        head.resize(size_t(std::max<std::streamsize>(0, file.gcount())));
        const subtitle::TextEncoding encoding = subtitle::DetectTextEncoding(head);
        found.charenc = subtitle::CharencFor(encoding, GetACP());
        if (!found.charenc.empty())
            LOG("Subtitles: the file is not UTF-8; reading it as " << utf8_text::FromWide(found.charenc) << '.');
    }
    LOG("Subtitles: " << found.tracks.size() << " subtitle stream(s)"
        << (found.sidecar.empty() ? "" : ", and a subtitle file beside the video") << '.');
    return found;
}

std::optional<std::wstring> SubtitleOverlay::Extracted(const Source& source, uint64_t generation)
{
    const auto key = std::make_pair(source.path, source.stream);
    if (const auto it = m_extracted.find(key); it != m_extracted.end()) return it->second.wstring();
    std::error_code error;
    fs::create_directories(m_extractDirectory, error);
    const fs::path output = m_extractDirectory / (std::to_wstring(m_extracted.size()) + L".mks");
    Session session;
    {
        std::lock_guard lock(m_mutex);
        if (m_stop || generation != m_generation) return std::nullopt;
        m_phase = Phase::Extracting;
    }
    const auto started = std::chrono::steady_clock::now();
    const bool spawned = Spawn(FFmpeg(), subtitle::ExtractArguments(source.path, source.stream, source.codec, output.wstring()), session, false);
    {
        std::lock_guard lock(m_mutex);
        m_activeJob = spawned ? session.job : nullptr;
        if (!spawned) m_phase = Phase::Idle;
    }
    if (!spawned) return std::nullopt;
    WaitForSingleObject(session.process, INFINITE);
    {
        std::lock_guard lock(m_mutex);
        m_activeJob = nullptr;
        m_phase = Phase::Idle;
    }
    {
        std::lock_guard lock(m_mutex);
        if (m_stop || generation != m_generation) { fs::remove(output, error); return std::nullopt; }
    }
    if (session.ExitCode() != 0) {
        LOG("Subtitles: the text stream could not be copied out (exit " << session.ExitCode() << "): " << session.Tail());
        fs::remove(output, error);
        return std::nullopt;
    }
    LOG("Subtitles: text stream " << source.stream << " copied out in "
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s.");
    m_extracted[key] = output;
    return output.wstring();
}

void SubtitleOverlay::RunSession(uint64_t generation)
{
    Source source;
    Canvas canvas;
    {
        std::lock_guard lock(m_mutex);
        if (generation != m_generation) return;
        source = m_shownSource;
        canvas = m_shownCanvas;
    }
    const subtitle::Kind kind = subtitle::KindForCodec(source.codec);
    if (kind == subtitle::Kind::Unsupported || !subtitle::CanvasUsable(canvas.width, canvas.height)) {
        std::lock_guard lock(m_mutex);
        if (generation == m_generation) m_ended = true;
        return;
    }
    std::wstring input = source.path;
    double origin = source.origin;
    int stream = source.stream;
    if (kind == subtitle::Kind::Text && !source.external) {
        const auto extracted = Extracted(source, generation);
        if (!extracted) {
            std::lock_guard lock(m_mutex);
            if (generation == m_generation) m_ended = true;
            return;
        }
        // The copy starts at zero on the player's clock and holds one stream.
        input = *extracted;
        origin = 0.0;
        stream = 0;
    }
    double start = 0.0;
    {
        std::lock_guard lock(m_mutex);
        // A seek or resize while the stream was being copied out moved the
        // generation on without ending anything; this session takes it over.
        if (m_stop || !m_showing || !(m_shownSource == source)) return;
        generation = m_generation;
        m_sessionGeneration = generation;
        canvas = m_shownCanvas;
        start = m_startAt;
    }
    if (!subtitle::CanvasUsable(canvas.width, canvas.height)) return;
    subtitle::RenderCommand command;
    command.input = input;
    command.stream = stream;
    command.kind = kind;
    command.charenc = source.charenc;
    command.width = canvas.width;
    command.height = canvas.height;
    command.videoWidth = canvas.videoWidth;
    command.videoHeight = canvas.videoHeight;
    command.rate = canvas.rate;
    command.start = start;
    command.origin = origin;
    command.duration = canvas.duration;

    Session session;
    const auto started = std::chrono::steady_clock::now();
    const bool spawned = Spawn(FFmpeg(), subtitle::RenderArguments(command), session, true);
    {
        std::lock_guard lock(m_mutex);
        if (!spawned) {
            if (generation == m_generation) m_ended = true;
            return;
        }
        m_activeJob = session.job;
        m_phase = Phase::Rendering;
        if (m_stop || generation != m_generation) InterruptLocked(true);
    }
    const size_t frameBytes = size_t(canvas.width) * canvas.height * 4u;
    bool first = true, failed = false, lastEmpty = false;
    std::shared_ptr<const subtitle::Frame> last;
    for (;;) {
        {
            // Held a few changes ahead of the clock and no further: the child
            // then blocks on its pipe, which is what keeps it from drawing
            // the whole film at once into memory.
            std::unique_lock lock(m_mutex);
            m_progress.wait(lock, [&] {
                if (m_stop || generation != m_generation) return true;
                size_t ahead = 0;
                for (const auto& frame : m_frames) if (frame->pts > m_consumedAt) ++ahead;
                return ahead < kFramesAhead;
            });
            if (m_stop || generation != m_generation) break;
        }
        auto frame = std::make_shared<subtitle::Frame>();
        frame->width = canvas.width;
        frame->height = canvas.height;
        frame->bgra.resize(frameBytes);
        size_t filled = 0;
        while (filled < frameBytes) {
            DWORD got = 0;
            const DWORD want = DWORD(std::min<size_t>(frameBytes - filled, 1u << 20));
            if (!ReadFile(session.output, frame->bgra.data() + filled, want, &got, nullptr) || !got) break;
            filled += got;
        }
        if (filled < frameBytes) {
            if (filled) failed = true;
            break;
        }
        // Its time: the stats line ffmpeg writes just before the frame itself.
        std::optional<double> stamp;
        {
            std::unique_lock lock(session.stderrStream.mutex);
            session.stderrStream.arrived.wait_for(lock, std::chrono::seconds(2), [&] {
                return !session.stderrStream.stamps.empty() || session.stderrStream.closed;
            });
            if (!session.stderrStream.stamps.empty()) {
                stamp = session.stderrStream.stamps.front();
                session.stderrStream.stamps.pop_front();
            }
        }
        if (!stamp) { failed = true; LOG("Subtitles: a frame arrived without its time; stopping this track."); break; }
        frame->pts = *stamp - origin;
        // The first canvas frame is the picture at the start, whatever the
        // canvas time base rounded its stamp to; a paused player asks for
        // exactly the start and has to get it.
        if (first && kind == subtitle::Kind::Text) frame->pts = std::min(frame->pts, start);
        // Most frames are "nothing on screen", and holding 8 MB of zeroes for
        // each would be most of the memory this uses.
        const uint8_t* pixels = frame->bgra.data();
        bool any = false;
        for (size_t at = 3; at < frameBytes; at += 4) if (pixels[at]) { any = true; break; }
        if (!any) std::vector<uint8_t>().swap(frame->bgra);
        // ffmpeg repeats a bitmap picture at the end of its display; the same
        // picture again is not a change.
        const bool duplicate = last && ((!any && lastEmpty) ||
                                        (any && !lastEmpty && last->bgra == frame->bgra));
        if (first) {
            LOG("Subtitles: first frame " << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
                << " s after the child started (" << canvas.width << "x" << canvas.height << ", "
                << (kind == subtitle::Kind::Text ? "text" : "bitmap") << ", from " << start << " s).");
            first = false;
        }
        if (duplicate) continue;
        lastEmpty = !any;
        last = frame;
        std::lock_guard lock(m_mutex);
        if (generation != m_generation) break;
        m_frames.push_back(std::move(frame));
        // Frames already behind the one on screen are never shown again.
        size_t shown = 0;
        for (size_t index = 0; index < m_frames.size() && m_frames[index]->pts <= m_consumedAt; ++index) shown = index;
        if (shown > 0) m_frames.erase(m_frames.begin(), m_frames.begin() + std::ptrdiff_t(shown));
    }
    WaitForSingleObject(session.process, 200);
    std::lock_guard lock(m_mutex);
    m_activeJob = nullptr;
    m_phase = Phase::Idle;
    if (generation != m_generation || m_stop) return;
    m_ended = true;
    const DWORD code = session.ExitCode();
    if (failed || (code != 0 && code != STILL_ACTIVE)) {
        std::string tail;
        {
            std::lock_guard errors(session.stderrStream.mutex);
            tail = session.stderrStream.tail;
        }
        LOG("Subtitles: the subtitle child stopped (exit " << code << ")" << (tail.empty() ? "" : ": ") << tail);
    }
}
