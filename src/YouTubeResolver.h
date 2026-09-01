#pragma once

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

struct YouTubeResolverTestAccess;

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
std::wstring QuoteWindowsArgument(std::wstring_view argument);
std::vector<std::wstring> BuildYouTubeResolverArguments(
    const std::filesystem::path& helperDirectory,
    std::wstring_view youtubeUrl);

class YouTubeResolver {
public:
    YouTubeResolver();
    ~YouTubeResolver();

    YouTubeResolver(const YouTubeResolver&) = delete;
    YouTubeResolver& operator=(const YouTubeResolver&) = delete;

    ResolveResult Resolve(std::wstring_view youtubeUrl, std::stop_token stop);
    void Cancel();

private:
    friend struct YouTubeResolverTestAccess;

    struct Settings {
        std::filesystem::path helperDirectory;
        std::chrono::milliseconds deadline{std::chrono::seconds{45}};
        std::chrono::milliseconds pollInterval{std::chrono::milliseconds{25}};
        std::chrono::milliseconds shutdownWait{std::chrono::seconds{2}};
    };

    explicit YouTubeResolver(Settings settings);

    Settings settings_;
    std::mutex resolveMutex_;
    std::mutex stateMutex_;
    HANDLE activeJob_{nullptr};
    bool resolving_{false};
    bool cancelRequested_{false};
};
