#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

// What the audio helpers said on stderr, kept for the one time it matters.
//
// The ffmpeg audio child and the ffprobe track probe both had stderr sent to
// NUL, so every decode failure arrived in the log as a bare exit code - or,
// for a helper that died mid-stream, as a clock that stopped. Their stderr now
// goes to a pipe, and the last few KiB are kept here and written out once,
// when the helper ends badly.
//
// Bounded rather than rate-limited: the reader drains the pipe as it goes, so
// a helper that floods stderr can never block on a full pipe, and the flood
// costs a fixed 4 KiB however long it runs. The end of the output is the part
// worth keeping - ffmpeg says why it gave up last.
namespace audio_stderr {

inline constexpr size_t kTailBytes = 4096;

class Tail {
public:
    void Append(const char* bytes, size_t count)
    {
        if (!count) return;
        if (count >= kTailBytes) {
            text_.assign(bytes + (count - kTailBytes), kTailBytes);
            truncated_ = true;
            return;
        }
        text_.append(bytes, count);
        if (text_.size() > kTailBytes) {
            text_.erase(0, text_.size() - kTailBytes);
            truncated_ = true;
        }
    }

    bool Empty() const { return text_.find_first_not_of(" \t\r\n") == std::string::npos; }

    // One log line: the helper's non-blank lines joined with " | ", with an
    // ellipsis in front when the start was dropped. Control bytes other than
    // tabs are shown as '?' rather than written raw into the log.
    std::string Line() const
    {
        std::string line = truncated_ ? "..." : "";
        size_t start = 0;
        while (start < text_.size()) {
            size_t end = text_.find_first_of("\r\n", start);
            if (end == std::string::npos) end = text_.size();
            // Blank lines, and the blanks around a line, carry nothing.
            const size_t first = text_.find_first_not_of(" \t", start);
            if (first != std::string::npos && first < end) {
                const size_t last = text_.find_last_not_of(" \t", end - 1);
                if (!line.empty()) line += line == "..." ? " " : " | ";
                for (size_t at = first; at <= last; ++at) {
                    const auto character = static_cast<unsigned char>(text_[at]);
                    line.push_back(character < 0x20 && character != '\t' ? '?' : text_[at]);
                }
            }
            start = end + 1;
        }
        return line;
    }

private:
    std::string text_;
    bool truncated_ = false;
};

// Whether an exited audio child's stderr is worth a log line. Zero is a normal
// end, and so is -22 on this command line: `-map 0:a:N?` maps nothing on a
// video-only source, which is a silent film rather than a failure, and the
// player already says so in words. STILL_ACTIVE is not an exit at all.
inline bool ReportOnExit(DWORD exitCode)
{
    return exitCode != 0 && exitCode != STILL_ACTIVE && exitCode != DWORD(-22);
}

} // namespace audio_stderr
