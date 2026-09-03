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

// History owns no media files. The caller must check returned records against
// current entries and active playback/jobs before evicting owned cache keys.
class RecentMediaHistory {
public:
    explicit RecentMediaHistory(std::filesystem::path file);

    // A malformed file leaves the current entries untouched. Missing is empty.
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
