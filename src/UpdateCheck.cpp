#include "UpdateCheck.h"

#include <windows.h>
#include <winhttp.h>

#include <charconv>

#ifndef DLSS_VIDEO_PLAYER_VERSION
#define DLSS_VIDEO_PLAYER_VERSION "0.0.0"
#endif
#define UPDATE_CHECK_WIDEN_INNER(text) L##text
#define UPDATE_CHECK_WIDEN(text) UPDATE_CHECK_WIDEN_INNER(text)

namespace {

constexpr size_t kMaximumVersionCharacters = 64;
constexpr size_t kMaximumTagCharacters = 64;
constexpr size_t kMaximumBodyBytes = 256 * 1024;
// The feed is asked for ten releases; the extra slack absorbs a larger page
// without ever letting a hostile body turn the scan into unbounded work.
constexpr size_t kMaximumReleaseObjects = 32;
// A release object nests a few levels at most (author, assets, reactions).
// Anything deeper is not a release feed, so refuse it instead of walking it.
constexpr size_t kMaximumNestingDepth = 16;
constexpr DWORD kTimeoutMilliseconds = 8000;

// The one definition of this project's tag shape. Tags read as
// "dlss5-video-player-v0.18.0"; the prefix is stripped before version parsing
// but never before storing or comparing a tag.
constexpr std::string_view kReleaseTagPrefix = "dlss5-video-player-";

constexpr std::wstring_view kUserAgent =
    L"DLSSVideoPlayer/" UPDATE_CHECK_WIDEN(DLSS_VIDEO_PLAYER_VERSION)
    L" (+https://github.com/2600th/dlss5-video-player)";
constexpr std::wstring_view kApiHost = L"api.github.com";
// /releases, not /releases/latest: that endpoint skips pre-releases, and every
// release here is one, so it answers 404. Ten entries is more than enough to
// find the newest version even if a stale tag was published out of order.
constexpr std::wstring_view kApiPath = L"/repos/2600th/dlss5-video-player/releases?per_page=10";
constexpr std::wstring_view kRequestHeaders =
    L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28";

bool JsonSpace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

// Offset just past the colon of "key": ... , or npos. The quoted key must sit
// where a member name can legally sit ('{' or ',' before it), which keeps the
// same text quoted inside a release body from being mistaken for the member.
size_t FindMemberValue(std::string_view json, std::string_view key)
{
    for (size_t match = json.find(key); match != std::string_view::npos;
         match = json.find(key, match + 1)) {
        if (match == 0 || json[match - 1] != '"') continue;
        size_t after = match + key.size();
        if (after >= json.size() || json[after] != '"') continue;
        size_t before = match - 1;
        while (before > 0 && JsonSpace(json[before - 1])) --before;
        if (before == 0 || (json[before - 1] != '{' && json[before - 1] != ',')) continue;
        ++after;
        while (after < json.size() && JsonSpace(json[after])) ++after;
        if (after >= json.size() || json[after] != ':') continue;
        return after + 1;
    }
    return std::string_view::npos;
}

bool MemberIsTrue(std::string_view json, std::string_view key)
{
    const size_t value = FindMemberValue(json, key);
    if (value == std::string_view::npos) return false;
    size_t position = value;
    while (position < json.size() && JsonSpace(json[position])) ++position;
    return json.substr(position).starts_with("true");
}

// Decodes the quoted string starting at or after position. Escapes that cannot
// appear in a release tag collapse to '?' rather than failing the whole scan.
std::optional<std::string> ReadMemberString(std::string_view json, size_t position,
                                            size_t maximumCharacters)
{
    while (position < json.size() && JsonSpace(json[position])) ++position;
    if (position >= json.size() || json[position++] != '"') return {};
    std::string decoded;
    while (position < json.size()) {
        const unsigned char character = static_cast<unsigned char>(json[position++]);
        if (character == '"') return decoded;
        if (character < 0x20) return {};
        if (decoded.size() >= maximumCharacters) return {};
        if (character != '\\') {
            decoded.push_back(static_cast<char>(character));
            continue;
        }
        if (position >= json.size()) return {};
        const char escape = json[position++];
        if (escape == '"' || escape == '\\' || escape == '/') decoded.push_back(escape);
        else if (escape == 'b' || escape == 'f' || escape == 'n' || escape == 'r' ||
                 escape == 't') decoded.push_back('?');
        else if (escape == 'u') {
            if (position + 4 > json.size()) return {};
            for (size_t index = 0; index < 4; ++index) {
                const char hex = json[position++];
                if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') ||
                      (hex >= 'A' && hex <= 'F'))) return {};
            }
            decoded.push_back('?');
        } else return {};
    }
    return {};
}

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : handle_(handle) {}
    ~InternetHandle() { reset(); }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    InternetHandle(InternetHandle&& other) noexcept : handle_(other.release()) {}
    InternetHandle& operator=(InternetHandle&& other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }

    HINTERNET get() const { return handle_; }
    HINTERNET release()
    {
        const HINTERNET value = handle_;
        handle_ = nullptr;
        return value;
    }
    void reset(HINTERNET value = nullptr)
    {
        if (handle_) WinHttpCloseHandle(handle_);
        handle_ = value;
    }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_{nullptr};
};

std::wstring HexValue(DWORD value)
{
    wchar_t buffer[] = L"0x00000000";
    for (size_t index = 9; index >= 2; --index) {
        buffer[index] = L"0123456789abcdef"[value & 0xFu];
        value >>= 4;
    }
    return std::wstring(buffer, 10);
}

UpdateFetchResult TransportFailure(std::wstring_view stage)
{
    UpdateFetchResult result;
    result.error = std::wstring(stage) + L" failed (" + HexValue(GetLastError()) + L").";
    return result;
}

} // namespace

std::optional<SemanticVersion> ParseSemanticVersion(std::string_view text)
{
    // Release tags carry the project name, so "dlss5-video-player-v0.18.0"
    // must read as 0.18.0. The prefix is matched case-sensitively because it
    // is emitted by the release tooling, never typed by a user.
    if (text.starts_with(kReleaseTagPrefix)) text.remove_prefix(kReleaseTagPrefix.size());
    if (text.starts_with('v') || text.starts_with('V')) text.remove_prefix(1);
    if (text.empty() || text.size() > kMaximumVersionCharacters) return {};
    uint32_t components[3]{};
    size_t position = 0;
    for (size_t index = 0; index < 3; ++index) {
        const size_t begin = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') ++position;
        if (position == begin) return {};
        const auto parsed = std::from_chars(text.data() + begin, text.data() + position,
                                            components[index]);
        if (parsed.ec != std::errc{}) return {};
        if (index == 2) break;
        if (position >= text.size() || text[position] != '.') return {};
        ++position;
    }
    // A pre-release or build suffix is left over here, and is never an update.
    if (position != text.size()) return {};
    return SemanticVersion{components[0], components[1], components[2]};
}

std::wstring FormatSemanticVersion(SemanticVersion version)
{
    return std::to_wstring(version.major) + L'.' + std::to_wstring(version.minor) + L'.' +
           std::to_wstring(version.patch);
}

namespace {

// Folds one release object into the running best. Drafts are ignored, but a
// pre-release is NOT: every release this project publishes is flagged
// "prerelease": true, so filtering them out is precisely what made the update
// check inert. Do not "fix" that back. An unreadable or unparseable tag is
// merely skipped -- one odd entry must not blind the whole feed.
void ConsiderRelease(std::string_view object, std::optional<SemanticVersion>& best,
                     std::string& bestTag)
{
    if (MemberIsTrue(object, "draft")) return;
    const size_t value = FindMemberValue(object, "tag_name");
    if (value == std::string_view::npos) return;
    const auto tag = ReadMemberString(object, value, kMaximumTagCharacters);
    if (!tag || tag->empty()) return;
    const auto version = ParseSemanticVersion(*tag);
    if (!version) return;
    // Strictly greater: a tie keeps the earlier entry, which is GitHub's own
    // ordering and therefore the more recently published of the two.
    if (best && *version <= *best) return;
    best = version;
    bestTag = *tag;
}

} // namespace

std::optional<std::string> ParseNewestReleaseTag(std::string_view json)
{
    size_t position = 0;
    while (position < json.size() && JsonSpace(json[position])) ++position;
    // Only the array form is understood. An object here means an error payload
    // ({"message": "Not Found"}), which must not be mined for a tag.
    if (position >= json.size() || json[position] != '[') return {};
    ++position;

    std::optional<SemanticVersion> best;
    std::string bestTag;
    size_t objects = 0;
    size_t depth = 0;
    size_t objectBegin = 0;
    bool inString = false;
    // Brace-depth walk, string-aware: braces, brackets and quotes inside a
    // JSON string (release notes are full of them) must not move the depth, so
    // the split below cuts on real object boundaries only.
    for (; position < json.size(); ++position) {
        const char character = json[position];
        if (inString) {
            if (character == '\\') {
                // Consume the escaped character so a trailing \" cannot be
                // mistaken for the closing quote.
                if (++position >= json.size()) return {};
            } else if (character == '"') {
                inString = false;
            }
            continue;
        }
        if (character == '"') {
            inString = true;
        } else if (character == '{' || character == '[') {
            if (depth == 0 && character == '{') objectBegin = position;
            if (++depth > kMaximumNestingDepth) return {};
        } else if (character == '}' || character == ']') {
            if (depth == 0) break; // The array's own ']' ends the walk.
            --depth;
            if (depth != 0 || character != '}') continue;
            ConsiderRelease(json.substr(objectBegin, position - objectBegin + 1), best, bestTag);
            if (++objects >= kMaximumReleaseObjects) break;
        }
    }
    // A body cut off mid-string is truncated, and a truncated feed is not
    // trustworthy even if an earlier entry already parsed.
    if (inString) return {};
    if (!best) return {};
    // Verbatim: the caller stores this as the dismissal key and compares it
    // against the stored one character for character.
    return bestTag;
}

std::optional<UpdateNotice> EvaluateUpdateNotice(std::string_view currentVersion,
                                                 std::string_view latestTag,
                                                 std::string_view dismissedTag)
{
    const auto current = ParseSemanticVersion(currentVersion);
    const auto latest = ParseSemanticVersion(latestTag);
    if (!current || !latest || *latest <= *current) return {};
    if (latestTag == dismissedTag) return {};
    return UpdateNotice{*latest, std::string(latestTag)};
}

UpdateCheckDecision DecideUpdateCheck(bool enabled, int64_t lastCheckedUnix, int64_t nowUnix,
                                      int64_t intervalSeconds)
{
    if (!enabled) return UpdateCheckDecision::Disabled;
    // Never checked, or a stamp from the future: fetch rather than wait out a
    // window that a corrected clock would never close.
    if (lastCheckedUnix <= 0 || lastCheckedUnix > nowUnix) return UpdateCheckDecision::Fetch;
    return nowUnix - lastCheckedUnix >= intervalSeconds ? UpdateCheckDecision::Fetch
                                                        : UpdateCheckDecision::UseCache;
}

UpdateFetchResult FetchLatestReleaseTag(std::stop_token stop)
{
    UpdateFetchResult result;
    if (stop.stop_requested()) return result;

    InternetHandle session(WinHttpOpen(kUserAgent.data(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return TransportFailure(L"WinHttpOpen");
    if (!WinHttpSetTimeouts(session.get(), kTimeoutMilliseconds, kTimeoutMilliseconds,
                            kTimeoutMilliseconds, kTimeoutMilliseconds)) {
        return TransportFailure(L"WinHttpSetTimeouts");
    }

    InternetHandle connection(WinHttpConnect(session.get(), kApiHost.data(),
                                             INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) return TransportFailure(L"WinHttpConnect");

    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", kApiPath.data(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE));
    if (!request) return TransportFailure(L"WinHttpOpenRequest");
    if (!WinHttpAddRequestHeaders(request.get(), kRequestHeaders.data(),
                                  static_cast<DWORD>(kRequestHeaders.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        return TransportFailure(L"WinHttpAddRequestHeaders");
    }

    if (stop.stop_requested()) return result;
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return TransportFailure(L"WinHttpSendRequest");
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        return TransportFailure(L"WinHttpReceiveResponse");
    }

    DWORD status = 0;
    DWORD statusSize = static_cast<DWORD>(sizeof(status));
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        return TransportFailure(L"WinHttpQueryHeaders");
    }
    if (status != HTTP_STATUS_OK) {
        result.error = L"GitHub returned HTTP status " + std::to_wstring(status) + L".";
        return result;
    }

    std::string body;
    for (;;) {
        if (stop.stop_requested()) return result;
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            return TransportFailure(L"WinHttpQueryDataAvailable");
        }
        if (available == 0) break;
        if (body.size() + available > kMaximumBodyBytes) {
            result.error = L"The release feed was larger than the 256 KiB limit.";
            return result;
        }
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), body.data() + offset, available, &read)) {
            return TransportFailure(L"WinHttpReadData");
        }
        body.resize(offset + read);
        if (read == 0) break;
    }

    const auto tag = ParseNewestReleaseTag(body);
    if (!tag) {
        result.error = L"The release feed did not name a usable release tag.";
        return result;
    }
    result.ok = true;
    result.tag = *tag;
    return result;
}
