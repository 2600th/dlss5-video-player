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

struct ResolveResult {
    bool ok{false};
    std::wstring mediaUrl;
    ResolveError error{ResolveError::None};
    std::wstring detail;
};

bool IsSupportedYouTubeUrl(std::wstring_view value);
ResolveResult ParseResolverOutput(std::string_view stdoutBytes, DWORD exitCode);
#ifdef YOUTUBE_RESOLVER_TESTING
std::wstring QuoteWindowsArgument(std::wstring_view argument);
std::vector<std::wstring> BuildYouTubeResolverArguments(
    const std::filesystem::path& helperDirectory,
    std::wstring_view youtubeUrl);
#endif

class YouTubeResolver {
public:
    YouTubeResolver();
    ~YouTubeResolver();

    YouTubeResolver(const YouTubeResolver&) = delete;
    YouTubeResolver& operator=(const YouTubeResolver&) = delete;

    ResolveResult Resolve(std::wstring_view youtubeUrl, std::stop_token stop);
    void Cancel();

#ifdef YOUTUBE_RESOLVER_TESTING
    enum class FailureStage {
        None,
        PipeSetup,
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
