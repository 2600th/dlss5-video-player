#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

// One SHA-256 of the loaded source per file, rather than one per job.
//
// Every neural job full-hashed the source before doing anything - including
// the prepare-only cache check and every live-session retarget - so a 5 GB
// file cost 3 to 5 seconds of dead air after the viewer pressed render, paid
// again for each job against the same unchanged file.
//
// This is deliberately NOT Sha256FileCached. That memo lives for the whole
// process and its own header says never to use it for user content, because a
// file rewritten with an identical size and timestamp would keep the stale
// digest. This one is owned by the loaded media and forgotten on every load,
// so the window in which a stale digest could be served is a single playback
// session of a single file - and the size and write time still have to match
// within it.
//
// Why that matters more here than for an ordinary cache: the digest is a term
// of the render cache key. A stale one does not merely waste a hash, it
// serves the render of a different file.
namespace source_digest {

struct Entry {
    std::wstring path;
    uintmax_t size{};
    int64_t writeTime{};
    std::string digest;
};

// `compute` returns the digest or nullopt. It is called only when the entry
// does not already describe exactly this file.
template <class Compute>
std::optional<std::string> Lookup(Entry& entry, const std::wstring& path, uintmax_t size,
                                  int64_t writeTime, Compute&& compute)
{
    if (!entry.digest.empty() && entry.path == path && entry.size == size &&
        entry.writeTime == writeTime) {
        return entry.digest;
    }
    // Cleared before the attempt, not after a failure: leaving the previous
    // file's digest in place while the identity fields move on would be the
    // one outcome worse than re-hashing.
    entry = {};
    std::optional<std::string> digest = std::forward<Compute>(compute)();
    if (!digest || digest->empty()) return std::nullopt;
    entry.path = path;
    entry.size = size;
    entry.writeTime = writeTime;
    entry.digest = *digest;
    return digest;
}

// What a new media load does.
inline void Forget(Entry& entry) { entry = {}; }

} // namespace source_digest
