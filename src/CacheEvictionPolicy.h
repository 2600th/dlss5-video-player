#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// What the render cache may delete, and when.
//
// There was no eviction at all. RemoveSource and RemoveRender existed with no
// production caller, and the only reclamation was Clear(), which is all or
// nothing. Meanwhile the key deliberately retires entries wholesale:
// applicationVersion, driverVersion, modelStoreDigest, runtimeDigest and the
// manifest schema are all key terms, so one NVIDIA driver update changes every
// key at once. A user with 40 GB of renders takes the update, all 40 GB
// becomes unreachable, everything re-renders, the cache reaches 80 GB - and
// the only remedy on offer destroys the new renders too.
//
// Two rules, applied in that order.
//
// An entry whose manifest can no longer be reused is dead whatever the disk
// looks like. The schema gate already refuses it on lookup, so keeping it
// costs space and buys nothing. This costs the user nothing and always runs.
//
// Everything else is evicted only when the disk is actually under pressure,
// least recently used first. Not a fixed size cap: a cap has to be either so
// small it deletes renders while the disk is half empty, or so large it never
// fires on the machine that needed it. Re-rendering a film costs minutes to
// hours and free space costs nothing until it runs out, so the trigger is the
// thing that actually hurts - running out - rather than a number someone
// guessed.
namespace cache_eviction {

// The free-space floor the cache will try to leave on the volume. Chosen to be
// larger than any single render this player produces, so a machine that hits
// the floor still has room to finish what it is doing.
inline constexpr uintmax_t kDefaultFreeFloorBytes = uintmax_t{20} * 1024 * 1024 * 1024;

struct Entry {
    std::string key;
    uintmax_t bytes{};
    // Larger is more recent. A file time is fine; only the ordering is used.
    int64_t lastUsed{};
    // The manifest passes this build's reuse gate. A false here means the
    // entry can never be served again.
    bool reusable{};
    // A job is reading or writing this entry right now. Never evicted:
    // removing it under a running render is worse than running out of disk.
    bool active{};
};

struct Plan {
    std::vector<std::string> evict;
    uintmax_t freedBytes{};
    // False when everything evictable was chosen and the floor is still not
    // met. The caller reports it; it must not retry in a loop that cannot
    // succeed.
    bool floorMet{};
};

// `freeBytes` is the volume's current free space, `freeFloorBytes` the amount
// to try to leave. A floor of zero disables pressure eviction, leaving only
// the dead entries - the switch for anyone who would rather manage the cache
// by hand.
inline Plan PlanEviction(std::span<const Entry> entries, uintmax_t freeBytes,
                         uintmax_t freeFloorBytes)
{
    Plan plan;
    uintmax_t freed = 0;

    for (const Entry& entry : entries) {
        if (entry.active || entry.reusable) continue;
        plan.evict.push_back(entry.key);
        freed += entry.bytes;
    }

    if (freeBytes + freed >= freeFloorBytes) {
        plan.freedBytes = freed;
        plan.floorMet = true;
        return plan;
    }

    // Still short. Take the reusable entries oldest-first until the floor is
    // met, and stop there rather than emptying the cache.
    std::vector<const Entry*> candidates;
    for (const Entry& entry : entries)
        if (!entry.active && entry.reusable) candidates.push_back(&entry);
    std::ranges::sort(candidates, [](const Entry* left, const Entry* right) {
        if (left->lastUsed != right->lastUsed) return left->lastUsed < right->lastUsed;
        return left->key < right->key;   // stable answer for equal timestamps
    });

    for (const Entry* entry : candidates) {
        if (freeBytes + freed >= freeFloorBytes) break;
        plan.evict.push_back(entry->key);
        freed += entry->bytes;
    }

    plan.freedBytes = freed;
    plan.floorMet = freeBytes + freed >= freeFloorBytes;
    return plan;
}

} // namespace cache_eviction
