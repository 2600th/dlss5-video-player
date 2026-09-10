#include "UpdateCheck.h"
#include "TestSupport.h"

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

} // namespace

int main()
{
    semantic_versions_accept_triples_and_release_tags();
    release_feeds_yield_the_highest_non_draft_tag();
    malformed_feeds_yield_no_tag();
    notices_require_a_newer_undismissed_release();
    check_scheduling_respects_interval_and_clock_skew();
    if (test_support::failure_count != 0) return 1;
    std::cout << "Update check tests passed\n";
    return 0;
}
