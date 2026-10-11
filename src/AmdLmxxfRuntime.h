#pragma once

#include "AmdLmxxfApi.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace amd_lmxxf {

enum class RuntimeFailure
{
    None,
    MissingModule,
    MissingExport,
    GetApiFailed,
    InvalidApiTable,
    InvalidCapabilities,
    CreateFailed,
    PrepareFailed,
};

constexpr std::wstring_view RuntimeFailureName(RuntimeFailure failure) noexcept
{
    switch (failure) {
        case RuntimeFailure::None: return L"none";
        case RuntimeFailure::MissingModule: return L"missing-module";
        case RuntimeFailure::MissingExport: return L"missing-export";
        case RuntimeFailure::GetApiFailed: return L"get-api-failed";
        case RuntimeFailure::InvalidApiTable: return L"invalid-api-table";
        case RuntimeFailure::InvalidCapabilities: return L"invalid-capabilities";
        case RuntimeFailure::CreateFailed: return L"create-failed";
        case RuntimeFailure::PrepareFailed: return L"prepare-failed";
    }
    return L"unknown";
}

// Where LmxxfNrRuntime.dll and its module tree are expected to be. The same
// search order the ffmpeg helpers use (MediaTools.h / FindToolIn) and for the
// same reason: an explicit answer wins, then the layouts a checkout and a
// packaged build actually have. Empty when none of them holds the module, which
// the caller reports rather than papering over - a job that silently ran against
// a different runtime tree would make every other check meaningless.
//
// The environment variable is named after the compile definition the AMD tests
// build against, so one name means one thing across both.
std::filesystem::path ResolveRuntimeDirectory(const std::filesystem::path& explicitDirectory = {});

// Owns one loaded LmxxfNrRuntime module and at most one live context. The
// runtime globally records its last error, so all calls on one module must be
// serialized by the owning worker or evaluator.
class Runtime
{
public:
    Runtime() = default;
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&& other) noexcept;
    Runtime& operator=(Runtime&& other) noexcept;

    // `runtimeDirectory` must contain LmxxfNrRuntime.dll. Reparse points and
    // directories are refused so a checked package cannot redirect the load
    // outside the runtime tree.
    bool Load(const std::filesystem::path& runtimeDirectory, std::wstring* error);
    void Unload();

    bool QueryCapabilities(std::wstring* error);
    bool CreateSession(void* device, void* queue, const std::filesystem::path& assetsDirectory,
                       uint32_t flags, std::wstring* error);
    bool PrepareSession(std::wstring* error);
    void ResetSession();

    bool Loaded() const noexcept { return module_ != nullptr; }
    bool SessionCreated() const noexcept { return context_ != nullptr; }
    bool TimingAvailable() const noexcept { return api_.GetTimings != nullptr; }

    const LmxxfNrApi& Api() const noexcept { return api_; }
    const LmxxfNrCapabilities& Capabilities() const noexcept { return capabilities_; }
    void* Context() const noexcept { return context_; }
    const std::filesystem::path& ModulePath() const noexcept { return modulePath_; }

    std::string LastError() const;
    std::string Status() const;
    bool GetTimings(LmxxfNrTimings& out, std::string* error);

private:
    void ResetHandles() noexcept;
    void MoveFrom(Runtime& other) noexcept;

    HMODULE module_{};
    LmxxfNrApi api_{};
    LmxxfNrCapabilities capabilities_{};
    void* context_{};
    std::wstring assetsDirectory_;
    std::filesystem::path modulePath_;
};

} // namespace amd_lmxxf
