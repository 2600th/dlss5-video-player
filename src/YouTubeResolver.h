#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
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
    double durationSeconds{};
    // Latest selected stream availability, rounded up to avoid premature access.
    int64_t availableAtUnixSeconds{};
    // Height in pixels of the rung yt-dlp actually selected; zero when unknown.
    int selectedHeight{};
    // Bitrate of that rung in kbps: the video stream's own rate for a merged
    // selection, the whole stream's rate for a progressive one, which is what a
    // legacy 360p fallback offers. Zero when yt-dlp advertised neither.
    double videoKbps{};
    // YouTube age gate in years, 18 on age-restricted videos; zero when none or unknown.
    int ageLimit{};
};

bool IsSupportedYouTubeUrl(std::wstring_view value);
std::string CanonicalYouTubeVideoId(std::wstring_view value);
std::string StableYouTubeStreamIdentity(std::wstring_view mediaUrl,
                                        std::wstring_view audioUrl);
std::wstring_view YouTubeResolveErrorMessageKey(ResolveError error);
ResolveResult ParseResolverOutput(std::string_view stdoutBytes, DWORD exitCode);
// yt-dlp's stderr as one log line: at most the bytes captured (the rest are
// only counted in `totalBytes`), line breaks folded to " | ", anything outside
// printable ASCII shown as '?'. Empty when there was nothing.
std::string SummarizeResolverStderr(std::string_view captured, size_t totalBytes);
std::wstring_view YouTubeFormatSelector(YouTubeSourceQuality quality);
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
    // What the last Resolve logged of the helper's stderr.
    std::string lastStderrSummary_;
#endif
    std::mutex resolveMutex_;
    std::mutex stateMutex_;
    HANDLE activeJob_{nullptr};
    bool resolving_{false};
    bool cancelRequested_{false};
};
