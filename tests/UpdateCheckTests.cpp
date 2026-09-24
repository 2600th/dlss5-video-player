#include "UpdateCheck.h"
#include "TrailerThumbnail.h"
#include "TrailerThumbnailPolicy.h"
#include "ExampleVideos.h"
#include "TestSupport.h"

#include <objbase.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

// A trimmed but otherwise faithful /releases body, newest-first with the tags
// this project actually publishes. Every entry is a pre-release, because that
// is all this project ships; nesting (author, assets) and release notes with
// escaped quotes and backslashes are kept so the object split is exercised.
// The oldest entry deliberately mentions "tag_name" inside its body *before*
// its real tag_name, so a parser that ignored member position would report
// v9.9.9 and fail here.
constexpr std::string_view kReleaseFeed = R"json([
  {
    "url": "https://api.github.com/repos/2600th/dlss5-video-player/releases/5",
    "id": 187654325,
    "author": {"login": "2600th", "id": 1, "type": "User"},
    "tag_name": "dlss5-video-player-v0.17.2",
    "name": "0.17.2 \"Neural\"",
    "draft": false,
    "prerelease": true,
    "assets": [{"name": "dlss5-video-player-v0.17.2-win64.zip", "size": 12345}],
    "published_at": "2026-09-01T12:00:00Z"
  },
  {"id": 187654324, "tag_name": "dlss5-video-player-v0.17.1", "draft": false,
   "prerelease": true},
  {"id": 187654323, "tag_name": "dlss5-video-player-v0.17.0", "draft": false,
   "prerelease": true},
  {"id": 187654322, "tag_name": "dlss5-video-player-v0.16.0", "draft": false,
   "prerelease": true},
  {
    "id": 187654321,
    "body": "Path with \\ in it, and prose quoting \"tag_name\": \"dlss5-video-player-v9.9.9\".\r\nDone.",
    "tag_name": "dlss5-video-player-v0.15.0",
    "draft": false,
    "prerelease": true
  }
])json";

// GitHub's ordering is not trusted: the highest version wins wherever it sits.
constexpr std::string_view kOutOfOrderFeed = R"json([
  {"tag_name": "dlss5-video-player-v0.9.0", "draft": false, "prerelease": true},
  {"tag_name": "dlss5-video-player-v0.18.0", "draft": false, "prerelease": true},
  {"tag_name": "dlss5-video-player-v0.17.2", "draft": false, "prerelease": true}
])json";

// The newest entry is a draft, which is not published yet and must be ignored.
constexpr std::string_view kDraftFeed = R"json([
  {"tag_name": "dlss5-video-player-v0.19.0", "draft": true, "prerelease": false},
  {"tag_name": "dlss5-video-player-v0.18.0", "draft": false, "prerelease": true}
])json";

// The only "tag_name" here is quoted inside the release notes, so there is no
// tag at all: an escaped mention must never be promoted to a member.
constexpr std::string_view kEscapedMentionOnly = R"json([
  {"id": 7, "body": "see \"tag_name\": \"dlss5-video-player-v9.9.9\" below", "draft": false,
   "prerelease": true}
])json";

constexpr std::string_view kNoTagMember = R"json([
  {"id": 8, "draft": false, "prerelease": true, "name": "0.18.0"}
])json";

constexpr std::string_view kUnterminated =
    R"json([{"id": 1, "tag_name": "dlss5-video-player-v0.18.0}])json";

// A truncated body is not trusted even though the first object is complete.
constexpr std::string_view kTruncatedAfterGoodEntry = R"json([
  {"tag_name": "dlss5-video-player-v0.17.2", "draft": false, "prerelease": true},
  {"tag_name": "dlss5-video-player-v0.18.0)json";

// What /releases/latest answers for this repository: an object, not an array.
constexpr std::string_view kNotFound =
    R"json({"message": "Not Found", "documentation_url": "https://docs.github.com/rest"})json";

bool Is(const std::optional<SemanticVersion>& value, uint32_t major, uint32_t minor,
        uint32_t patch)
{
    return value && value->major == major && value->minor == minor && value->patch == patch;
}

std::string TagOf(const std::optional<std::string>& tag)
{
    return tag.value_or(std::string("<none>"));
}

void semantic_versions_accept_triples_and_release_tags()
{
    CHECK(Is(ParseSemanticVersion("0.17.2"), 0, 17, 2));
    CHECK(Is(ParseSemanticVersion("v0.17.2"), 0, 17, 2));
    CHECK(Is(ParseSemanticVersion("V0.17.2"), 0, 17, 2));
    CHECK(Is(ParseSemanticVersion("dlss5-video-player-v0.17.2"), 0, 17, 2));
    // Deliberate: the "v" stays optional after the prefix, exactly as it is
    // without one, so a tag cut without the "v" still reads as a version.
    CHECK(Is(ParseSemanticVersion("dlss5-video-player-0.17.2"), 0, 17, 2));
    CHECK(Is(ParseSemanticVersion("v10.0.0"), 10, 0, 0));
    CHECK(Is(ParseSemanticVersion("4294967295.1.2"), 4294967295u, 1, 2));

    constexpr std::string_view rejected[] = {
        "", "v", "1", "1.2", "1.2.", ".1.2", "1..2", "1.2.3.4", "1.2.3-beta.1", "0.18.0-beta.1",
        "0.18.0+1", "1.2.3 ", " 1.2.3", "1.2.-3", "abc", "v1.2.x", "4294967296.0.0",
        // The prefix is matched case-sensitively, and a suffix is still a
        // suffix once the prefix is gone.
        "DLSS5-Video-Player-v0.17.2", "dlss5-video-player-v0.18.0-beta.1",
        "dlss5-video-player-", "dlss5-video-player-v1.2", "dlss5-video-player-dlss5-v1.2.3",
    };
    for (const auto text : rejected) {
        CHECK(!ParseSemanticVersion(text));
    }

    const SemanticVersion current{0, 17, 2};
    const SemanticVersion next{0, 18, 0};
    CHECK_EQ(std::wstring(L"0.18.0"), FormatSemanticVersion(next));
    CHECK(current < next);
}

void release_feeds_yield_the_highest_non_draft_tag()
{
    // Pre-releases must count: filtering them out is what made the check inert.
    CHECK_EQ(std::string("dlss5-video-player-v0.17.2"), TagOf(ParseNewestReleaseTag(kReleaseFeed)));
    CHECK_EQ(std::string("dlss5-video-player-v0.18.0"),
             TagOf(ParseNewestReleaseTag(kOutOfOrderFeed)));
    CHECK_EQ(std::string("dlss5-video-player-v0.18.0"), TagOf(ParseNewestReleaseTag(kDraftFeed)));
    // A single unprefixed tag is still understood, and comes back verbatim.
    CHECK_EQ(std::string("v0.18.0"),
             TagOf(ParseNewestReleaseTag(R"json([{"tag_name": "v0.18.0"}])json")));

    // 65 characters is one past the tag cap, so that release is unusable; 64
    // is accepted. Both tags parse as versions, isolating the length rule.
    const std::string longTag = "v" + std::string(59, '0') + "1.2.3";
    const std::string maximumTag = "v" + std::string(58, '0') + "1.2.3";
    CHECK(!ParseNewestReleaseTag("[{\"tag_name\": \"" + longTag + "\"}]"));
    CHECK_EQ(maximumTag, TagOf(ParseNewestReleaseTag("[{\"tag_name\": \"" + maximumTag + "\"}]")));
}

void malformed_feeds_yield_no_tag()
{
    CHECK(!ParseNewestReleaseTag(kEscapedMentionOnly));
    CHECK(!ParseNewestReleaseTag(kNoTagMember));
    CHECK(!ParseNewestReleaseTag(kUnterminated));
    CHECK(!ParseNewestReleaseTag(kTruncatedAfterGoodEntry));
    CHECK(!ParseNewestReleaseTag(kNotFound));
    CHECK(!ParseNewestReleaseTag("[]"));
    CHECK(!ParseNewestReleaseTag("[ \r\n ]"));
    CHECK(!ParseNewestReleaseTag(""));
    CHECK(!ParseNewestReleaseTag("   "));
    CHECK(!ParseNewestReleaseTag(R"json("dlss5-video-player-v9.9.9")json"));
    // Only unparseable tags: nothing to offer, but no crash either.
    CHECK(!ParseNewestReleaseTag(R"json([{"tag_name": "nightly"}, {"tag_name": "v1.2"}])json"));
}

void notices_require_a_newer_undismissed_release()
{
    const SemanticVersion expected{0, 18, 1};
    const auto newer = EvaluateUpdateNotice("0.18.0", "dlss5-video-player-v0.18.1", "");
    CHECK(newer.has_value());
    if (newer) {
        CHECK_EQ(expected, newer->latest);
        // Verbatim, because this string is the dismissal key.
        CHECK_EQ(std::string("dlss5-video-player-v0.18.1"), newer->tag);
    }

    CHECK(!EvaluateUpdateNotice("0.18.0", "dlss5-video-player-v0.18.0", ""));
    CHECK(!EvaluateUpdateNotice("0.18.0", "dlss5-video-player-v0.17.2", ""));
    CHECK(!EvaluateUpdateNotice("0.18.0", "dlss5-video-player-v0.18.1",
                                "dlss5-video-player-v0.18.1"));
    CHECK(EvaluateUpdateNotice("0.18.0", "dlss5-video-player-v0.18.1",
                               "dlss5-video-player-v0.18.0").has_value());
    CHECK(!EvaluateUpdateNotice("nightly", "dlss5-video-player-v0.18.1", ""));
    CHECK(!EvaluateUpdateNotice("0.18.0", "dlss5-video-player-v0.19.0-rc.1", ""));
}

void check_scheduling_respects_interval_and_clock_skew()
{
    constexpr int64_t now = 1'757'500'000;
    constexpr int64_t interval = kUpdateCheckIntervalSeconds;
    CHECK(DecideUpdateCheck(false, now - interval, now, interval) == UpdateCheckDecision::Disabled);
    CHECK(DecideUpdateCheck(false, 0, now, interval) == UpdateCheckDecision::Disabled);
    CHECK(DecideUpdateCheck(true, now - 60, now, interval) == UpdateCheckDecision::UseCache);
    CHECK(DecideUpdateCheck(true, now - interval + 1, now, interval) ==
          UpdateCheckDecision::UseCache);
    CHECK(DecideUpdateCheck(true, now - interval, now, interval) == UpdateCheckDecision::Fetch);
    CHECK(DecideUpdateCheck(true, 0, now, interval) == UpdateCheckDecision::Fetch);
    CHECK(DecideUpdateCheck(true, now + interval * 30, now, interval) ==
          UpdateCheckDecision::Fetch);
}


// ---- Trailer thumbnails (TrailerThumbnail.h) ------------------------------
//
// No case here touches the network: the worker takes its fetch as a function,
// and these hand it one that fails, blocks or answers from memory. The real
// fetch from i.ytimg.com is checked by hand against the player's log.

// A 32x24 JPEG: a red 32x18 picture with 3-pixel black bars above and below,
// which is how hqdefault.jpg letterboxes a 16:9 video inside 4:3.
constexpr unsigned char kLetterboxJpeg[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x02, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x0f, 0x4c, 0x61, 0x76, 0x63, 0x36, 0x33, 0x2e, 0x31,
    0x2e, 0x31, 0x30, 0x31, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x08, 0x10, 0x10, 0x13, 0x10, 0x13,
    0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x1a, 0x18, 0x1a, 0x1b, 0x1b, 0x1b, 0x1a, 0x1a, 0x1a, 0x1a,
    0x1b, 0x1b, 0x1b, 0x1d, 0x1d, 0x1d, 0x22, 0x22, 0x22, 0x1d, 0x1d, 0x1d, 0x1b, 0x1b, 0x1d, 0x1d,
    0x20, 0x20, 0x22, 0x22, 0x25, 0x26, 0x25, 0x23, 0x23, 0x22, 0x23, 0x26, 0x26, 0x28, 0x28, 0x28,
    0x30, 0x30, 0x2e, 0x2e, 0x38, 0x38, 0x3a, 0x45, 0x45, 0x53, 0xff, 0xc4, 0x00, 0x60, 0x00, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x06, 0x07, 0x01, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x04, 0x05, 0x06, 0x07, 0x10, 0x00, 0x01, 0x05, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x02, 0x15, 0xd2, 0x91, 0x52,
    0x11, 0x00, 0x01, 0x02, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x03, 0x15, 0x13, 0x51, 0xd2, 0x52, 0xd1, 0xa1, 0x91, 0xff, 0xc0, 0x00, 0x11,
    0x08, 0x00, 0x18, 0x00, 0x20, 0x03, 0x01, 0x12, 0x00, 0x02, 0x12, 0x00, 0x03, 0x12, 0x00, 0xff,
    0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xcc, 0x2b, 0xda,
    0xf5, 0x3e, 0xa6, 0x47, 0x45, 0x33, 0xd5, 0x44, 0xf7, 0x60, 0x26, 0x83, 0x09, 0x63, 0x27, 0x7d,
    0x4d, 0xa5, 0x38, 0x15, 0x7b, 0x5e, 0xa7, 0xd4, 0xc8, 0xe8, 0x74, 0xf5, 0x51, 0x3d, 0xd8, 0x09,
    0x31, 0x09, 0x63, 0x27, 0x7d, 0x4d, 0xa5, 0x38, 0x15, 0x7b, 0x5e, 0xa7, 0xd4, 0xc8, 0xe8, 0x74,
    0xf5, 0x51, 0x3d, 0xd8, 0x09, 0x31, 0x09, 0x63, 0x27, 0x7d, 0x4d, 0xa5, 0x38, 0x15, 0x7b, 0x5e,
    0xa7, 0xd4, 0xc8, 0xe8, 0x74, 0xf5, 0x51, 0x3d, 0xd8, 0x09, 0x31, 0x09, 0x63, 0x27, 0x7d, 0x4d,
    0xa5, 0x38, 0x15, 0x83, 0x5e, 0x67, 0xc4, 0xd1, 0x1c, 0x1d, 0x21, 0x55, 0x4f, 0x74, 0x36, 0x26,
    0x22, 0xcc, 0x62, 0xef, 0x89, 0xb8, 0xcf, 0x8b, 0x1b, 0x06, 0xbc, 0xcf, 0x89, 0xa2, 0x38, 0x53,
    0x21, 0x55, 0x4f, 0x74, 0x36, 0x34, 0x18, 0xb3, 0x18, 0xbb, 0xe2, 0x6e, 0x33, 0xe2, 0xc6, 0xc1,
    0xaf, 0x33, 0xe2, 0x68, 0x8e, 0x14, 0xc8, 0x55, 0x53, 0xdd, 0x0d, 0x8d, 0x06, 0x2c, 0xc6, 0x2e,
    0xf8, 0x9b, 0x8c, 0xf8, 0xb1, 0xb0, 0x6b, 0xcc, 0xf8, 0x9a, 0x23, 0x85, 0x32, 0x15, 0x54, 0xf7,
    0x43, 0x63, 0x41, 0x8b, 0x31, 0x8b, 0xbe, 0x26, 0xe3, 0x3e, 0x3f, 0xff, 0xd9
};
std::string_view LetterboxJpeg()
{
    return {reinterpret_cast<const char*>(kLetterboxJpeg), sizeof(kLetterboxJpeg)};
}

void trailer_thumbnail_urls_are_built_and_allowlisted()
{
    using namespace trailer_thumbnail;
    CHECK(ThumbnailUrl(L"VQRLujxTm3c", Variant::MaxRes) == L"https://i.ytimg.com/vi/VQRLujxTm3c/maxresdefault.jpg");
    CHECK(ThumbnailUrl(L"0wFNN1f6hF8", Variant::High) == L"https://i.ytimg.com/vi/0wFNN1f6hF8/hqdefault.jpg");
    CHECK(RequestPath(L"trvIyyFt_MM", Variant::MaxRes) == L"/vi/trvIyyFt_MM/maxresdefault.jpg");
    CHECK(AllowedThumbnailUrl(ThumbnailUrl(L"vPS-pgg3scE", Variant::MaxRes)));
    CHECK(AllowedThumbnailUrl(ThumbnailUrl(L"vPS-pgg3scE", Variant::High)));
    // Nothing but the one host, over HTTPS, with the two names and no extras.
    for (const wchar_t* refused : {
             L"http://i.ytimg.com/vi/VQRLujxTm3c/maxresdefault.jpg",
             L"https://i.ytimg.com.example.com/vi/VQRLujxTm3c/maxresdefault.jpg",
             L"https://img.youtube.com/vi/VQRLujxTm3c/maxresdefault.jpg",
             L"https://i.ytimg.com:443/vi/VQRLujxTm3c/maxresdefault.jpg",
             L"https://user@i.ytimg.com/vi/VQRLujxTm3c/maxresdefault.jpg",
             L"https://i.ytimg.com/vi/VQRLujxTm3c/maxresdefault.jpg?x=1",
             L"https://i.ytimg.com/vi/VQRLujxTm3c/maxresdefault.jpg#x",
             L"https://i.ytimg.com/vi/VQRLujxTm3c/sddefault.jpg",
             L"https://i.ytimg.com/vi/VQRLujxTm3c/",
             L"https://i.ytimg.com/vi/../../x/maxresdefault.jpg",
             L"https://i.ytimg.com/vi/VQRLujxTm3/maxresdefault.jpg",
             L"https://i.ytimg.com/vi/VQRLujx.m3c/maxresdefault.jpg",
             L"https://i.ytimg.com/vi_webp/VQRLujxTm3c/maxresdefault.webp",
             L""}) {
        CHECK(!AllowedThumbnailUrl(refused));
    }
    CHECK(ThumbnailUrl(L"../evil/xx", Variant::MaxRes).empty());
    CHECK(RequestPath(L"VQRLujxTm3", Variant::High).empty());
    // Only the curated spelling yields an id.
    CHECK(VideoIdFromWatchUrl(L"https://www.youtube.com/watch?v=VQRLujxTm3c") == std::wstring(L"VQRLujxTm3c"));
    CHECK(!VideoIdFromWatchUrl(L"https://www.youtube.com/watch?v=VQRLujxTm3c&t=10"));
    CHECK(!VideoIdFromWatchUrl(L"https://youtu.be/VQRLujxTm3c"));
    CHECK(!VideoIdFromWatchUrl(L"http://www.youtube.com/watch?v=VQRLujxTm3c"));
    // And every curated entry has one, so every trailer tile can get a picture.
    for (const ExampleVideo& example : kExampleVideos) {
        const auto id = VideoIdFromWatchUrl(example.url);
        REQUIRE(id.has_value());
        CHECK(AllowedThumbnailUrl(ThumbnailUrl(*id, Variant::MaxRes)));
        CHECK(TrailerThumbnails::CachePath(L"C:\\cache", *id) ==
              std::filesystem::path(L"C:\\cache\\thumbs\\" + *id + L".jpg"));
    }
    CHECK(TrailerThumbnails::CachePath({}, L"VQRLujxTm3c").empty());
    CHECK(TrailerThumbnails::CachePath(L"C:\\cache", L"..\\..\\x").empty());
}

void trailer_thumbnail_cache_is_refreshed_after_thirty_days()
{
    using namespace trailer_thumbnail;
    constexpr int64_t now = 1'790'000'000;
    CHECK(Decide(true, std::nullopt, now) == CacheDecision::Fetch);
    CHECK(Decide(true, now - 60, now) == CacheDecision::UseCache);
    CHECK(Decide(true, now - kRefreshSeconds + 1, now) == CacheDecision::UseCache);
    CHECK(Decide(true, now - kRefreshSeconds, now) == CacheDecision::Refresh);
    // A stamp from the future is stale, not fresh for ever.
    CHECK(Decide(true, now + 3600, now) == CacheDecision::Refresh);
    CHECK(Decide(false, now - 60, now) == CacheDecision::CacheOnly);
    CHECK(Decide(false, now - 10 * kRefreshSeconds, now) == CacheDecision::CacheOnly);
    CHECK(Decide(false, std::nullopt, now) == CacheDecision::Placeholder);
    CHECK_EQ(int64_t{30} * 24 * 60 * 60, kRefreshSeconds);
}

void trailer_thumbnail_bytes_are_checked_and_cropped()
{
    using namespace trailer_thumbnail;
    CHECK(LooksLikeJpeg(LetterboxJpeg()));
    CHECK(!LooksLikeJpeg("<!doctype html><html>"));
    CHECK(!LooksLikeJpeg(std::string_view("\xFF\xD8", 2)));
    CHECK(!LooksLikeJpeg(std::string(kMaximumBytes + 1, '\xFF')));
    // hqdefault's letterbox comes off; a 16:9 picture is kept whole.
    const auto hq = CoverCrop(480, 360);
    CHECK(hq.x == 0 && hq.y == 45 && hq.width == 480 && hq.height == 270);
    const auto maxres = CoverCrop(1280, 720);
    CHECK(maxres.x == 0 && maxres.y == 0 && maxres.width == 1280 && maxres.height == 720);
    const auto wide = CoverCrop(1000, 100);
    CHECK(wide.x == 411 && wide.y == 0 && wide.width == 177 && wide.height == 100);
    CHECK(CoverCrop(0, 5).width == 0);
}

void trailer_thumbnail_decodes_to_a_letterbox_free_tile()
{
    const auto picture = DecodeTrailerThumbnail(LetterboxJpeg(), 16);
    REQUIRE(picture != nullptr);
    CHECK_EQ(LONG{16}, picture->size.cx);
    CHECK_EQ(LONG{9}, picture->size.cy);
    REQUIRE(picture->bgra.size() == size_t{16} * 9 * 4);
    // BGRA: red is byte 2. The top and bottom rows would be the black bars
    // had the crop not taken them off.
    const auto red = [&](int x, int y) { return int(picture->bgra[(size_t(y) * 16 + x) * 4 + 2]); };
    const auto green = [&](int x, int y) { return int(picture->bgra[(size_t(y) * 16 + x) * 4 + 1]); };
    CHECK(red(8, 4) > 170 && green(8, 4) < 80);
    CHECK(red(8, 0) > 120);
    CHECK(red(8, 8) > 120);
    CHECK(DecodeTrailerThumbnail("<!doctype html>", 16) == nullptr);
    CHECK(DecodeTrailerThumbnail(LetterboxJpeg().substr(0, 40), 16) == nullptr);
    CHECK(DecodeTrailerThumbnail(LetterboxJpeg(), 0) == nullptr);
}

std::filesystem::path FreshDirectory(const wchar_t* name)
{
    const auto directory = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    return directory;
}

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds limit)
{
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

std::vector<std::wstring> CuratedIds()
{
    std::vector<std::wstring> ids;
    for (const ExampleVideo& example : kExampleVideos)
        if (auto id = trailer_thumbnail::VideoIdFromWatchUrl(example.url)) ids.push_back(*id);
    return ids;
}

// Offline, and a fetch that never answers: the screen's thread gets straight
// back, the tiles stay on their placeholders, nothing is cached, and closing
// the player does not wait out a timeout.
void trailer_thumbnails_offline_or_hung_leave_the_placeholder_without_blocking()
{
    const auto root = FreshDirectory(L"trailer-thumbs-offline");
    const auto ids = CuratedIds();
    REQUIRE(!ids.empty());
    {
        std::atomic<int> calls{};
        TrailerThumbnails thumbnails;
        const auto started = std::chrono::steady_clock::now();
        thumbnails.Start(ids,
                         {root, true, 64,
                          [&](std::wstring_view, std::stop_token) {
                              ++calls;
                              return TrailerThumbnailFetch{false, {}, L"WinHttpSendRequest failed (0x00002ee7)."};
                          }},
                         nullptr, 0);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(200));
        CHECK(thumbnails.Started());
        REQUIRE(WaitFor([&] { return thumbnails.Finished(); }, std::chrono::seconds(10)));
        CHECK_EQ(int(ids.size()), calls.load());
        for (const auto& id : ids) CHECK(thumbnails.Picture(id) == nullptr);
        CHECK(!std::filesystem::exists(root / L"thumbs" / (ids[0] + L".jpg")));
        // Once per process: a second start is a no-op.
        thumbnails.Start(ids,
                         {root, true, 64,
                          [&](std::wstring_view, std::stop_token) {
                              ++calls;
                              return TrailerThumbnailFetch{};
                          }},
                         nullptr, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK_EQ(int(ids.size()), calls.load());
    }
    {
        std::atomic<bool> entered{};
        TrailerThumbnails thumbnails;
        const auto started = std::chrono::steady_clock::now();
        thumbnails.Start(ids,
                         {root, true, 64,
                          [&](std::wstring_view, std::stop_token stop) {
                              entered = true;
                              while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                              return TrailerThumbnailFetch{};
                          }},
                         nullptr, 0);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(200));
        REQUIRE(WaitFor([&] { return entered.load(); }, std::chrono::seconds(5)));
        CHECK(thumbnails.Picture(ids[0]) == nullptr);
        CHECK(!thumbnails.Finished());
        const auto stopping = std::chrono::steady_clock::now();
        thumbnails.Stop();
        CHECK(std::chrono::steady_clock::now() - stopping < std::chrono::seconds(1));
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// A fresh picture on disk is used without a request; a stale one is shown and
// kept when the refresh fails; a fetched one is shown and cached byte for byte;
// with fetching off, nothing is requested at all.
void trailer_thumbnails_serve_the_cache_and_keep_stale_pictures()
{
    const auto root = FreshDirectory(L"trailer-thumbs-cache");
    const std::wstring id = L"VQRLujxTm3c";
    const auto path = TrailerThumbnails::CachePath(root, id);
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(LetterboxJpeg().data(), std::streamsize(LetterboxJpeg().size()));
    }
    const auto run = [&](bool enabled, TrailerThumbnails::Fetcher fetch) {
        auto thumbnails = std::make_unique<TrailerThumbnails>();
        thumbnails->Start({id}, {root, enabled, 32, std::move(fetch)}, nullptr, 0);
        CHECK(WaitFor([&] { return thumbnails->Finished(); }, std::chrono::seconds(10)));
        return thumbnails;
    };
    std::atomic<int> calls{};
    const auto failing = [&](std::wstring_view, std::stop_token) {
        ++calls;
        return TrailerThumbnailFetch{false, {}, L"offline"};
    };

    {
        const auto fresh = run(true, failing);
        CHECK_EQ(0, calls.load());
        const auto picture = fresh->Picture(id);
        REQUIRE(picture != nullptr);
        CHECK_EQ(LONG{32}, picture->size.cx);
        CHECK_EQ(LONG{18}, picture->size.cy);
    }
    {
        const auto stale = std::chrono::clock_cast<std::chrono::file_clock>(std::chrono::system_clock::now() -
                                                                           std::chrono::hours(24 * 31));
        std::filesystem::last_write_time(path, stale);
        const auto refreshed = run(true, failing);
        CHECK_EQ(1, calls.load());
        CHECK(refreshed->Picture(id) != nullptr);
    }
    {
        calls = 0;
        const auto off = run(false, failing);
        CHECK_EQ(0, calls.load());
        CHECK(off->Picture(id) != nullptr);
    }
    std::filesystem::remove(path);
    {
        const auto offEmpty = run(false, failing);
        CHECK_EQ(0, calls.load());
        CHECK(offEmpty->Picture(id) == nullptr);
    }
    {
        const auto fetched = run(true, [&](std::wstring_view requested, std::stop_token) {
            ++calls;
            CHECK(requested == id);
            return TrailerThumbnailFetch{true, std::string(LetterboxJpeg()), {}};
        });
        CHECK_EQ(1, calls.load());
        CHECK(fetched->Picture(id) != nullptr);
        std::ifstream in(path, std::ios::binary);
        const std::string cached((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(cached == LetterboxJpeg());
    }
    {
        // Something that is not a JPEG is neither shown nor cached.
        calls = 0;
        std::filesystem::remove(path);
        const auto html = run(true, [&](std::wstring_view, std::stop_token) {
            ++calls;
            return TrailerThumbnailFetch{true, "<!doctype html>", {}};
        });
        CHECK_EQ(1, calls.load());
        CHECK(html->Picture(id) == nullptr);
        CHECK(!std::filesystem::exists(path));
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

} // namespace

int main()
{
    // DecodeTrailerThumbnail runs WIC on the calling thread, as the worker does on its own.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    semantic_versions_accept_triples_and_release_tags();
    release_feeds_yield_the_highest_non_draft_tag();
    malformed_feeds_yield_no_tag();
    notices_require_a_newer_undismissed_release();
    check_scheduling_respects_interval_and_clock_skew();
    trailer_thumbnail_urls_are_built_and_allowlisted();
    trailer_thumbnail_cache_is_refreshed_after_thirty_days();
    trailer_thumbnail_bytes_are_checked_and_cropped();
    trailer_thumbnail_decodes_to_a_letterbox_free_tile();
    trailer_thumbnails_offline_or_hung_leave_the_placeholder_without_blocking();
    trailer_thumbnails_serve_the_cache_and_keep_stale_pictures();
    if (test_support::failure_count != 0) return 1;
    std::cout << "Update check tests passed\n";
    return 0;
}
