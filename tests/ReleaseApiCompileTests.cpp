#include "AudioPlayer.h"
#include "VideoDecoder.h"
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

// The decoder's and the audio player's fault-injection seams compile into the
// release unguarded: a default-constructed Settings is production, so every
// injection has to be off by default, at compile time where the defaults are
// literal and at run time for the Settings object itself (whose strings keep it
// out of a constant expression).
namespace {
constexpr VideoDecoder::Settings::FaultInjection kVideoFaults{};
static_assert(kVideoFaults.resume == VideoDecoder::FailureStage::None);

constexpr AudioPlayer::Settings::FaultInjection kAudioFaults{};
static_assert(!kAudioFaults.disableAudioDevice);
static_assert(!kAudioFaults.failTerminateJob);
static_assert(!kAudioFaults.failInitialProcessWait);
static_assert(!kAudioFaults.failGetExitCodeProcess);
static_assert(!kAudioFaults.failFinalProcessWait);
static_assert(!kAudioFaults.failInitialReaderWait);
static_assert(!kAudioFaults.failFinalReaderWait);

bool ProductionDefaults()
{
    const VideoDecoder::Settings video{};
    const AudioPlayer::Settings audio{};
    return video.faults.resume == VideoDecoder::FailureStage::None &&
        !audio.faults.disableAudioDevice && !audio.faults.failTerminateJob &&
        !audio.faults.failInitialProcessWait && !audio.faults.failGetExitCodeProcess &&
        !audio.faults.failFinalProcessWait && !audio.faults.failInitialReaderWait &&
        !audio.faults.failFinalReaderWait;
}
} // namespace

int main()
{
    return ProductionDefaults() ? 0 : 1;
}
