#pragma once

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#ifdef YOUTUBE_RESOLVER_TESTING
struct YouTubeResolverTestAccess;
#endif

enum class ResolveError {
    None,
    InvalidUrl,
    HelperMissing,
    StartFailed,
    TimedOut,
    Cancelled,
    OutputTooLarge,
    ExtractionFailed,
    InvalidOutput,
};

enum class YouTubeSourceQuality {
    Auto,
    P2160,
    P1440,
    P1080,
};

struct ResolveResult {
    bool ok{false};
    std::wstring mediaUrl;
    std::wstring audioUrl;
    ResolveError error{ResolveError::None};
    std::wstring detail;
};

struct YouTubeFormatAvailability {
    bool valid{};
    bool autoAvailable{true};
    bool p2160{};
    bool p1440{};
    bool p1080{};
};

inline constexpr size_t kMaximumYouTubeFormatMetadataBytes = 4u * 1024u * 1024u;

bool IsSupportedYouTubeUrl(std::wstring_view value);
std::string CanonicalYouTubeVideoId(std::wstring_view value);
std::string StableYouTubeStreamIdentity(std::wstring_view mediaUrl,
                                        std::wstring_view audioUrl);
std::wstring_view YouTubeResolveErrorMessageKey(ResolveError error);
ResolveResult ParseResolverOutput(std::string_view stdoutBytes, DWORD exitCode);
std::wstring_view YouTubeFormatSelector(YouTubeSourceQuality quality);
YouTubeFormatAvailability ParseYouTubeFormatMetadata(std::string_view json);
#ifdef YOUTUBE_RESOLVER_TESTING
std::wstring QuoteWindowsArgument(std::wstring_view argument);
std::vector<std::wstring> BuildYouTubeResolverArguments(
    const std::filesystem::path& helperDirectory,
    std::wstring_view youtubeUrl,
    YouTubeSourceQuality quality);
#endif

class YouTubeResolver {
public:
    YouTubeResolver();
    ~YouTubeResolver();

    YouTubeResolver(const YouTubeResolver&) = delete;
    YouTubeResolver& operator=(const YouTubeResolver&) = delete;

    ResolveResult Resolve(std::wstring_view youtubeUrl, std::stop_token stop);
    ResolveResult Resolve(std::wstring_view youtubeUrl, YouTubeSourceQuality quality,
                          std::stop_token stop);
    void Cancel();

#ifdef YOUTUBE_RESOLVER_TESTING
    enum class FailureStage {
        None,
        PipeHandlesOwned,
        JobAssignment,
        Resume,
        PipeRead,
    };
#endif

private:
#ifdef YOUTUBE_RESOLVER_TESTING
    friend struct YouTubeResolverTestAccess;

    struct Settings {
        std::filesystem::path helperDirectory;
        std::chrono::milliseconds deadline{std::chrono::seconds{45}};
        std::chrono::milliseconds pollInterval{std::chrono::milliseconds{25}};
        std::chrono::milliseconds shutdownWait{std::chrono::seconds{2}};
        FailureStage failureStage{FailureStage::None};
    };

    explicit YouTubeResolver(Settings settings);
#endif

    std::filesystem::path helperDirectory_;
    std::chrono::milliseconds deadline_{std::chrono::seconds{45}};
    std::chrono::milliseconds pollInterval_{std::chrono::milliseconds{25}};
    std::chrono::milliseconds shutdownWait_{std::chrono::seconds{2}};
#ifdef YOUTUBE_RESOLVER_TESTING
    FailureStage failureStage_{FailureStage::None};
#endif
    std::mutex resolveMutex_;
    std::mutex stateMutex_;
    HANDLE activeJob_{nullptr};
    bool resolving_{false};
    bool cancelRequested_{false};
};
