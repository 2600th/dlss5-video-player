#pragma once

// Runtime lock: the exact set of neural-runtime files (size, SHA-256, file
// version) a render is allowed to run against. The lock is embedded at build
// time from packaging/runtime-lock.json and verified against the staged
// runtime directory before every render so a drifted file is named, not
// silently rendered with.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

struct RuntimeLockEntry {
    std::wstring destination;
    uint64_t size{};
    std::string sha256; // lowercase hex
    std::wstring fileVersion;

    friend bool operator==(const RuntimeLockEntry&, const RuntimeLockEntry&) = default;
};

struct RuntimeLock {
    uint32_t schemaVersion{};
    std::string runtimeVersion;
    std::vector<RuntimeLockEntry> entries;

    friend bool operator==(const RuntimeLock&, const RuntimeLock&) = default;
};

// Strict parse of the lock document. Returns nullopt for malformed JSON, an
// unsupported schemaVersion, a missing/ill-typed required field, an empty
// destination or a sha256 that is not 64 hex digits.
std::optional<RuntimeLock> ParseRuntimeLock(std::string_view json);

// The lock compiled into this binary (packaging/runtime-lock.json). Parsed
// once; a build whose embedded lock does not parse terminates on first use.
const RuntimeLock& EmbeddedRuntimeLock();

struct RuntimeLockCheck {
    std::wstring name;
    bool present{};
    bool sizeMatches{};
    bool hashMatches{};
    // Informational only: community builds of the runtime may lack a version
    // resource, so a version difference is reported but never fails Ok().
    bool versionMatches{};
    uint64_t actualSize{};
    std::string actualSha256; // lowercase hex, empty when absent/unreadable
    std::wstring actualFileVersion;

    bool Ok() const noexcept { return present && sizeMatches && hashMatches; }
};

// One check per lock entry, in lock order. `stop` aborts hashing; aborted
// entries report present with an empty hash and hashMatches == false.
std::vector<RuntimeLockCheck> VerifyRuntimeLock(const std::filesystem::path& runtimeDirectory,
                                                const RuntimeLock& lock,
                                                std::stop_token stop = {});

bool RuntimeLockSatisfied(std::span<const RuntimeLockCheck> checks);

// Human-readable list of every failing file and what differs ("dxgi.dll:
// missing; sl.dlss.dll: size 1 != 421504, hash mismatch"). Empty when the
// lock is satisfied.
std::wstring DescribeRuntimeLockDrift(std::span<const RuntimeLockCheck> checks);

// Every loadable module in the top level of `runtimeDirectory` that the lock
// does not name: a *.dll the loader would find beside the helper, or a
// *.addon / *.addon32 / *.addon64 the ReShade proxy loads from `AddonPath=.`.
// VerifyRuntimeLock only looks at the files it knows, so a stray add-on was
// loaded into feature 18's process while the digest, the cache key and the
// receipt still named the locked runtime. Names are compared case-insensitively
// and returned sorted; anything else (NeuralWorker.exe, the INIs, logs, *.bak
// backups, ngx_logs/) is not a module and is ignored. nullopt when the
// directory cannot be listed, which the caller must treat as a refusal.
std::optional<std::vector<std::wstring>> FindUnlockedRuntimeModules(const std::filesystem::path& runtimeDirectory,
                                                                    const RuntimeLock& lock);
