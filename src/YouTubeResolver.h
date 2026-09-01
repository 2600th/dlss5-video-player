#pragma once

#include <windows.h>

#include <string>
#include <string_view>

enum class ResolveError {
    None,
    InvalidUrl,
    HelperMissing,
    StartFailed,
    TimedOut,
    Cancelled,
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
