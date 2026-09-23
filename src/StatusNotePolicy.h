#pragma once

#include <cwctype>
#include <filesystem>
#include <string>
#include <utility>

// Status-bar notes that are true of one specific thing, and so have to end
// when that thing stops being true rather than whenever the player happens to
// Unload. Both notes below used to share one field Unload clears: a neural
// pre-render loads asynchronously and Unloads on the way, which wiped the
// drop note before it was ever seen, and nothing but a new file cleared the
// decode note, which stayed up over playback that had started again.
namespace status_note {

// Two spellings of one local file: a drop hands over the shell's path and the
// render job hands back std::filesystem::absolute of it, which can differ in
// case, separators and `.` components. Windows paths compare without case.
inline bool SameSource(const std::filesystem::path& a, const std::filesystem::path& b)
{
    if (a.empty() || b.empty()) return false;
    const std::wstring left = a.lexically_normal().wstring(), right = b.lexically_normal().wstring();
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index)
        if (std::towlower(left[index]) != std::towlower(right[index])) return false;
    return true;
}

// "Opened one dropped file; N others were ignored" is true of the file that
// drop opened, for as long as that file is what is loaded - across the Unload
// an asynchronous load does on the way in, and never for another file. A new
// open ends it (Clear), so reopening the same file later does not revive it.
class DropNote {
public:
    void Set(std::wstring text, std::filesystem::path opened)
    {
        text_ = std::move(text);
        source_ = std::move(opened);
    }
    void Clear()
    {
        text_.clear();
        source_.clear();
    }
    // Empty while that file is still loading, and once anything else is on
    // screen: the note says what happened to the file being watched.
    std::wstring Visible(bool loaded, const std::filesystem::path& current) const
    {
        return loaded && !text_.empty() && SameSource(source_, current) ? text_ : std::wstring{};
    }

private:
    std::wstring text_;
    std::filesystem::path source_;
};

// Where Play starts a local file whose playback has nothing queued. A clip
// that played to its end starts over; one whose decode failed part-way is
// retried where it stopped, because starting over from 0 replays everything
// the viewer already watched to reach the same failure - and the note that
// says it stopped is no longer true once playback moves (it is cleared by the
// seek, and set again if the retry fails too).
inline double PlayRestartSeconds(bool decodeStopped, double stoppedAtSeconds)
{
    return decodeStopped && stoppedAtSeconds > 0.0 ? stoppedAtSeconds : 0.0;
}

} // namespace status_note
