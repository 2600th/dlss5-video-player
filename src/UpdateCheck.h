#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

// Release discovery for the GitHub project. Everything except the fetch is
// pure so the decision logic stays testable without a network or a clock.

struct SemanticVersion {
    uint32_t major{}, minor{}, patch{};
    auto operator<=>(const SemanticVersion&) const = default;
};

// Accepts "0.17.2", "v0.17.2" and this project's own release-tag spelling
// "dlss5-video-player-v0.17.2". Exactly three numeric components and no
// leftover text: "0.18.0-beta.1" and "0.18.0+1" are not releases we offer.
std::optional<SemanticVersion> ParseSemanticVersion(std::string_view text);
std::wstring FormatSemanticVersion(SemanticVersion version);

// Picks the highest-versioned "tag_name" out of a GitHub /releases array body,
// returned exactly as written so the caller can use it as a dismissal key.
// The /releases/latest endpoint cannot be used: it ignores pre-releases, and
// every release of this project is flagged as one, so it answers 404. Drafts
// are skipped here; pre-releases deliberately are not.
std::optional<std::string> ParseNewestReleaseTag(std::string_view json);

struct UpdateNotice {
    SemanticVersion latest{};
    std::string tag;
};

// A notice appears only for a strictly newer, parseable, undismissed tag.
std::optional<UpdateNotice> EvaluateUpdateNotice(std::string_view currentVersion,
                                                 std::string_view latestTag,
                                                 std::string_view dismissedTag);

enum class UpdateCheckDecision { Disabled, UseCache, Fetch };

// A missing or future lastCheckedUnix forces a fetch: a skewed clock must not
// stall the check forever.
UpdateCheckDecision DecideUpdateCheck(bool enabled, int64_t lastCheckedUnix, int64_t nowUnix,
                                      int64_t intervalSeconds);

struct UpdateFetchResult {
    bool ok{};
    std::string tag;
    std::wstring error;
};

// One blocking HTTPS GET through WinHTTP, shared by the release check and the
// start screen's trailer thumbnails so both keep the same transport rules:
// TLS only (WINHTTP_FLAG_SECURE), the system proxy, one timeout per stage, a
// body cap enforced while reading, and a stop that closes the request handle
// under a blocked call. `host`, `path` and `headers` are copied, so views of
// temporaries are fine. With followRedirects off a 3xx is returned as a
// status and never followed, which is what keeps a host allowlist honest.
struct HttpsGetRequest {
    std::wstring_view host;
    std::wstring_view path;
    std::wstring_view headers;
    size_t maximumBodyBytes{};
    uint32_t timeoutMilliseconds{8000};
    bool followRedirects{true};
};

struct HttpsGetResult {
    // Cancelled carries no error: a stop is not a failure to report.
    enum class Outcome { Ok, Cancelled, TransportFailed, HttpStatus, TooLarge };
    Outcome outcome{Outcome::Cancelled};
    uint32_t status{};
    std::string body;
    std::wstring error;   // TransportFailed only: "<stage> failed (0x...)."
};

HttpsGetResult HttpsGet(const HttpsGetRequest& request, std::stop_token stop);

// Blocking HTTPS GET of the release array. Holds no global state, so it is
// safe to run on a std::jthread. Cancellation returns ok=false with an empty
// error.
UpdateFetchResult FetchLatestReleaseTag(std::stop_token stop);

inline constexpr std::wstring_view kUpdateReleasesPageUrl =
    L"https://github.com/2600th/dlss5-video-player/releases/latest";
inline constexpr int64_t kUpdateCheckIntervalSeconds = 24 * 60 * 60;
