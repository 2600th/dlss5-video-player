#include "YouTubeResolver.h"

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <type_traits>

#ifdef YOUTUBE_RESOLVER_TESTING
#error The release API compile test must not expose resolver test seams.
#endif

static_assert(std::is_default_constructible_v<YouTubeResolver>);
static_assert(!std::is_copy_constructible_v<YouTubeResolver>);
static_assert(!std::is_constructible_v<YouTubeResolver, std::filesystem::path>);
static_assert(!std::is_constructible_v<YouTubeResolver, std::filesystem::path,
                                       std::chrono::milliseconds>);
static_assert(std::is_same_v<
    decltype(std::declval<YouTubeResolver&>().Resolve(
        std::declval<std::wstring_view>(), std::declval<std::stop_token>())),
    ResolveResult>);
static_assert(std::is_same_v<
    decltype(std::declval<YouTubeResolver&>().Cancel()), void>);

int main()
{
    return 0;
}
