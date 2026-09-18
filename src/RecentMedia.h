#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct RecentMediaEntry {
    bool youtube{};
    std::string id;
    std::wstring title;
    std::wstring source;
    std::string sourceKey;
    std::string renderKey;
    int sourceQuality{};

    friend bool operator==(const RecentMediaEntry&, const RecentMediaEntry&) = default;
};

// History owns no media files, and since 2026-09-18 nothing evicts on its
// behalf either. This list is the five-item Recent MENU; the neural cache is a
// work product keyed by source and settings, and tying the second's lifetime to
// the first's meant opening a sixth video deleted the first one's render -
// minutes of GPU time thrown away by a menu rolling over, then spent again the
// next time that video was opened. `Remember` still reports what it dropped and
// what it replaced, because that is the honest description of what it did to
// the list; the player no longer turns those records into deletions. The cache
// is bounded by "Clear neural cache" alone.
class RecentMediaHistory {
public:
    explicit RecentMediaHistory(std::filesystem::path file);

    // A file this program could not have written (bad header, unreadable
    // record, trailing bytes) leaves the current entries untouched. A record
    // that reads but fails validation - a key that is not a hash, a local path
    // that is not on a drive letter, a duplicate - is dropped and the rest are
    // kept. Missing is empty.
    bool Load();
    bool Save() const;
    const std::vector<RecentMediaEntry>& Entries() const { return entries_; }

    // Invalid entries are ignored. Newest first; returns evicted records and
    // records whose nonempty cache keys were replaced. Empty keys mean absent.
    std::vector<RecentMediaEntry> Remember(RecentMediaEntry entry);

private:
    std::filesystem::path file_;
    std::vector<RecentMediaEntry> entries_;
};
