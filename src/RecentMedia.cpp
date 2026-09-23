#include "RecentMedia.h"

#include "AtomicFile.h"
#include "Utf8Text.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

namespace {

constexpr size_t kMaximumEntries = 5;
constexpr size_t kMaximumFileBytes = 1024 * 1024;
constexpr size_t kMaximumTextLength = 32768;
constexpr std::string_view kHeader = "DLSSRecentMedia 1";

std::optional<std::string> Utf8(std::wstring_view text)
{
    if (text.empty()) return std::string{};
    if (text.size() > kMaximumTextLength || text.find(L'\0') != text.npos) return {};
    return utf8_text::FromWideStrict(text);
}

std::optional<std::wstring> Wide(std::string_view text)
{
    if (text.empty()) return std::wstring{};
    if (text.size() > kMaximumTextLength * 4 || text.find('\0') != text.npos) return {};
    auto result = utf8_text::ToWideStrict(text);
    if (!result || result->size() > kMaximumTextLength) return {};
    return result;
}

bool ValidKey(std::string_view key)
{
    return key.empty() || (key.size() == 64 && std::ranges::all_of(key, [](char value) {
        return (value >= 'a' && value <= 'f') || (value >= '0' && value <= '9');
    }));
}

bool Normalize(RecentMediaEntry& entry)
{
    if (!ValidKey(entry.sourceKey) || !ValidKey(entry.renderKey) ||
        entry.sourceQuality < 0 || entry.sourceQuality > 3 || !Utf8(entry.title)) return false;
    if (entry.youtube) {
        // The resolver provides the canonical video ID, never a signed stream
        // URL: exactly eleven characters of the URL-safe alphabet, the same
        // shape IsSupportedYouTubeUrl accepts.
        if (entry.id.size() != 11 ||
            !std::ranges::all_of(entry.id, [](char value) {
                return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                    (value >= '0' && value <= '9') || value == '-' || value == '_';
            })) return false;
        entry.source = L"https://www.youtube.com/watch?v=" +
            std::wstring(entry.id.begin(), entry.id.end());
    } else {
        if (entry.source.empty() || !entry.sourceKey.empty() || !Utf8(entry.source)) return false;
        std::error_code error;
        auto path = std::filesystem::absolute(std::filesystem::path(entry.source), error);
        if (error) return false;
        entry.source = path.lexically_normal().make_preferred().wstring();
        auto id = Utf8(entry.source);
        if (!id) return false;
        entry.id = std::move(*id);
    }
    return true;
}

// A local entry read back from disk may only name a drive-letter path. The
// history file is plain text in the cache folder, and opening a recent entry
// touches its path before anything is played; a UNC root planted there would
// have the player authenticate to an arbitrary host on a click. An entry
// recorded from a UNC open in this session is still offered until the player
// exits, and dropped by the next load.
bool DriveRooted(const std::filesystem::path& path)
{
    const auto root = path.root_name().wstring();
    return root.size() == 2 && root[1] == L':' &&
        ((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')) &&
        !path.root_directory().empty();
}

bool SameVideo(const RecentMediaEntry& left, const RecentMediaEntry& right)
{
    if (left.youtube != right.youtube) return false;
    if (left.youtube) return left.id == right.id;
    return CompareStringOrdinal(left.source.data(), static_cast<int>(left.source.size()),
        right.source.data(), static_cast<int>(right.source.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

RecentMediaHistory::RecentMediaHistory(std::filesystem::path file) : file_(std::move(file)) {}

bool RecentMediaHistory::Load()
{
    std::error_code error;
    if (!std::filesystem::exists(file_, error)) {
        if (error) return false;
        entries_.clear();
        return true;
    }
    const auto size = std::filesystem::file_size(file_, error);
    if (error || size == 0 || size > kMaximumFileBytes) return false;
    std::ifstream file(file_, std::ios::binary);
    if (!file) return false;
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
        file.peek() != std::char_traits<char>::eof()) return false;

    std::istringstream input(bytes);
    std::string header;
    size_t count{};
    if (!std::getline(input, header) || header != kHeader || !(input >> count) ||
        count > kMaximumEntries) return false;
    // A record the parser cannot read is a file this program did not write and
    // the whole load fails; a record that parses but no longer passes
    // validation is just that record, so it is dropped and the rest kept.
    std::vector<RecentMediaEntry> loaded;
    for (size_t index = 0; index < count; ++index) {
        RecentMediaEntry entry;
        int youtube{};
        std::string title, source;
        if (!(input >> youtube >> std::quoted(entry.id) >> std::quoted(title) >>
                std::quoted(source) >> std::quoted(entry.sourceKey) >>
                std::quoted(entry.renderKey) >> entry.sourceQuality)) return false;
        if (youtube != 0 && youtube != 1) continue;
        entry.youtube = youtube != 0;
        auto wideTitle = Wide(title), wideSource = Wide(source);
        if (!wideTitle || !wideSource) continue;
        entry.title = std::move(*wideTitle);
        entry.source = std::move(*wideSource);
        const auto storedId = entry.id;
        const auto storedSource = entry.source;
        if (!Normalize(entry) || entry.id != storedId || entry.source != storedSource ||
            (!entry.youtube && !DriveRooted(entry.source)) ||
            std::ranges::any_of(loaded, [&](const auto& other) { return SameVideo(entry, other); }))
            continue;
        loaded.push_back(std::move(entry));
    }
    input >> std::ws;
    if (!input.eof()) return false;
    entries_ = std::move(loaded);
    return true;
}

bool RecentMediaHistory::Save() const
{
    std::ostringstream output;
    output << kHeader << '\n' << entries_.size() << '\n';
    for (const auto& entry : entries_) {
        const auto title = Utf8(entry.title), source = Utf8(entry.source);
        if (!title || !source) return false;
        output << (entry.youtube ? 1 : 0) << ' ' << std::quoted(entry.id) << ' ' <<
            std::quoted(*title) << ' ' << std::quoted(*source) << ' ' <<
            std::quoted(entry.sourceKey) << ' ' << std::quoted(entry.renderKey) << ' ' <<
            entry.sourceQuality << '\n';
    }
    const auto bytes = output.str();
    if (bytes.size() > kMaximumFileBytes || file_.empty()) return false;

    // Only the temporary this call created is ever removed, never the history
    // itself or any source or render cache file.
    return static_cast<bool>(atomic_file::Replace(file_, bytes));
}

std::vector<RecentMediaEntry> RecentMediaHistory::Remember(RecentMediaEntry entry)
{
    if (!Normalize(entry)) return {};
    std::vector<RecentMediaEntry> removed;
    auto existing = std::ranges::find_if(entries_, [&](const auto& other) {
        return SameVideo(entry, other);
    });
    if (existing != entries_.end()) {
        if ((!existing->sourceKey.empty() && existing->sourceKey != entry.sourceKey) ||
            (!existing->renderKey.empty() && existing->renderKey != entry.renderKey))
            removed.push_back(*existing);
        entries_.erase(existing);
    }
    entries_.insert(entries_.begin(), std::move(entry));
    if (entries_.size() > kMaximumEntries) {
        removed.push_back(std::move(entries_.back()));
        entries_.pop_back();
    }
    return removed;
}
