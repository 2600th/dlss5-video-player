#include "AmdLmxxfRuntime.h"

#include "PlatformPaths.h"

#include <iterator>
#include <system_error>
#include <utility>

namespace amd_lmxxf {

std::filesystem::path ResolveRuntimeDirectory(const std::filesystem::path& explicitDirectory)
{
    namespace fs = std::filesystem;
    const auto holdsRuntime = [](const fs::path& directory) {
        if (directory.empty()) return false;
        std::error_code error;
        return fs::is_regular_file(directory / L"LmxxfNrRuntime.dll", error) && !error;
    };
    if (!explicitDirectory.empty())
        return holdsRuntime(explicitDirectory) ? explicitDirectory : fs::path{};
    // Not getenv: the name is ASCII but the value is a Windows path, so the wide
    // API is the one that cannot mangle it. Retried because the value can change
    // between the size query and the read.
    std::wstring fromEnvironment;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD needed = GetEnvironmentVariableW(L"DLSS5_AMD_RUNTIME_DIR", nullptr, 0);
        if (needed == 0) break;
        fromEnvironment.assign(needed, L'\0');
        const DWORD length =
            GetEnvironmentVariableW(L"DLSS5_AMD_RUNTIME_DIR", fromEnvironment.data(), needed);
        if (length < needed) {
            fromEnvironment.resize(length);
            break;
        }
        fromEnvironment.clear();
    }
    if (!fromEnvironment.empty()) {
        const fs::path candidate(fromEnvironment);
        if (holdsRuntime(candidate)) return candidate;
    }
    const fs::path module = platform_paths::ModuleDirectory().value_or(fs::path{});
    if (!module.empty()) {
        // A packaged helper sits in `neural-runtime/` beside the player, and the
        // runtime tree ships beside the player, not inside the helper - so the
        // first candidate is that parent. The two above it are the checkout
        // layout, where the tree is at the repository root and the executable is
        // two directories below it: build/<config>[/neural-runtime].
        const fs::path base = module.filename() == L"neural-runtime" ? module.parent_path() : module;
        for (const fs::path& candidate : {base / L"amd-neural-runtime",
                                          base.parent_path() / L"amd-neural-runtime",
                                          base.parent_path().parent_path() / L"amd-neural-runtime"})
            if (holdsRuntime(candidate)) return candidate;
    }
    return {};
}

namespace {

std::wstring FailureText(RuntimeFailure failure, std::wstring_view detail)
{
    std::wstring text(L"LmxxfNrRuntime: ");
    text += RuntimeFailureName(failure);
    if (!detail.empty()) {
        text += L": ";
        text += detail;
    }
    return text;
}

bool IsRegularFileNoReparse(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

std::wstring WideAscii(std::string_view text)
{
    std::wstring wide;
    wide.reserve(text.size());
    for (const char character : text) wide.push_back(static_cast<wchar_t>(character));
    return wide;
}

} // namespace

Runtime::~Runtime()
{
    Unload();
}

Runtime::Runtime(Runtime&& other) noexcept
{
    MoveFrom(other);
}

Runtime& Runtime::operator=(Runtime&& other) noexcept
{
    if (this == &other) return *this;
    Unload();
    MoveFrom(other);
    return *this;
}

void Runtime::MoveFrom(Runtime& other) noexcept
{
    module_ = std::exchange(other.module_, nullptr);
    api_ = other.api_;
    capabilities_ = other.capabilities_;
    context_ = std::exchange(other.context_, nullptr);
    assetsDirectory_ = std::move(other.assetsDirectory_);
    modulePath_ = std::move(other.modulePath_);
    other.api_ = {};
    other.capabilities_ = {};
}

void Runtime::ResetHandles() noexcept
{
    api_ = {};
    capabilities_ = {};
    context_ = nullptr;
    assetsDirectory_.clear();
    modulePath_.clear();
}

void Runtime::Unload()
{
    ResetSession();
    if (module_ != nullptr) {
        FreeLibrary(module_);
        module_ = nullptr;
    }
    ResetHandles();
}

bool Runtime::Load(const std::filesystem::path& runtimeDirectory, std::wstring* error)
{
    Unload();
    std::error_code absoluteError;
    modulePath_ = std::filesystem::absolute(runtimeDirectory / L"LmxxfNrRuntime.dll",
                                            absoluteError);
    if (absoluteError) modulePath_ = runtimeDirectory / L"LmxxfNrRuntime.dll";
    if (!IsRegularFileNoReparse(modulePath_)) {
        if (error) *error = FailureText(RuntimeFailure::MissingModule, modulePath_.wstring());
        modulePath_.clear();
        return false;
    }

    module_ = LoadLibraryExW(modulePath_.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module_ == nullptr) {
        if (error) *error = FailureText(RuntimeFailure::MissingModule, modulePath_.wstring());
        modulePath_.clear();
        return false;
    }

    const FARPROC getApi = GetProcAddress(module_, "LmxxfNrGetApi");
    if (getApi == nullptr) {
        if (error) *error = FailureText(RuntimeFailure::MissingExport, modulePath_.wstring());
        Unload();
        return false;
    }

    // One call, one contract, per version. The table this header declares is the
    // table the runtime must hand back: the size is part of the ABI, and the
    // runtime rejects anything else rather than truncating. What varies between
    // packs is the version integer - the 2026-10 pack ships an ABI 6 runtime
    // that refuses 3 outright (UNSUPPORTED_ABI) while keeping the same 176-byte
    // table and the same struct sizes - so the version is negotiated, newest
    // first, and the table is judged after the call rather than before it.
    // Retrying with a shorter legacy table is still not a downgrade path: the
    // runtime answers INVALID_ARGUMENT, and the tail it never wrote would be
    // indistinguishable from a populated one.
    const auto getApiFn = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(getApi);
    int32_t lastStatus = static_cast<int32_t>(LMXXF_NR_OK);
    uint32_t lastRejectedVersion = 0;
    ApiProblem tableProblem = ApiProblem::None;
    uint32_t tableProblemVersion = 0;
    for (const uint32_t version : kAcceptedAbiVersions) {
        api_ = {};
        api_.struct_size = sizeof(api_);
        const int32_t result = getApiFn(version, &api_);
        if (result != static_cast<int32_t>(LMXXF_NR_OK)) {
            if (lastRejectedVersion == 0) {
                lastRejectedVersion = version;
                lastStatus = result;
            }
            continue;
        }
        const ApiProblem problem = ValidateApiTable(api_);
        if (problem != ApiProblem::None) {
            if (tableProblemVersion == 0) {
                tableProblemVersion = version;
                tableProblem = problem;
            }
            api_ = {};
            continue;
        }
        return true;
    }
    if (error) {
        std::wstring detail;
        if (tableProblemVersion != 0) {
            detail = L"abi_version " + std::to_wstring(tableProblemVersion) +
                     L" was accepted but its table is " + WideAscii(ApiProblemName(tableProblem));
            *error = FailureText(RuntimeFailure::InvalidApiTable, detail);
        } else {
            detail = L"status=" + std::to_wstring(lastStatus);
            detail += L" (" + WideAscii(LmxxfNrStatusName(lastStatus)) + L")";
            detail += L" for every abi_version this build speaks (";
            for (size_t i = 0; i < std::size(kAcceptedAbiVersions); ++i) {
                if (i != 0) detail += L", ";
                detail += std::to_wstring(kAcceptedAbiVersions[i]);
            }
            detail += L"); it wants a ";
            detail += std::to_wstring(sizeof(LmxxfNrApi));
            detail += L"-byte table";
            *error = FailureText(RuntimeFailure::GetApiFailed, detail);
        }
    }
    Unload();
    return false;
}

bool Runtime::QueryCapabilities(std::wstring* error)
{
    if (!Loaded()) {
        if (error) *error = FailureText(RuntimeFailure::MissingModule, L"module not loaded");
        return false;
    }
    capabilities_ = {};
    capabilities_.struct_size = sizeof(capabilities_);
    const int32_t result = api_.QueryCapabilities(&capabilities_);
    if (result != static_cast<int32_t>(LMXXF_NR_OK)) {
        if (error) {
            *error = FailureText(RuntimeFailure::InvalidCapabilities,
                                 L"status=" + std::to_wstring(result));
        }
        return false;
    }
    if (const ApiProblem problem = ValidateCapabilities(capabilities_);
        problem != ApiProblem::None) {
        if (error) {
            *error = FailureText(RuntimeFailure::InvalidCapabilities,
                                 WideAscii(ApiProblemName(problem)));
        }
        return false;
    }
    return true;
}

bool Runtime::CreateSession(void* device, void* queue,
                            const std::filesystem::path& assetsDirectory,
                            uint32_t flags, std::wstring* error)
{
    ResetSession();
    if (!Loaded() || device == nullptr || queue == nullptr) {
        if (error) *error = FailureText(RuntimeFailure::CreateFailed, L"missing dependency");
        return false;
    }

    assetsDirectory_ = assetsDirectory.wstring();
    LmxxfNrCreateInfo info = MakeCreateInfo(device, queue, assetsDirectory_.c_str(), flags);
    const int32_t result = api_.Create(&info, &context_);
    if (result != static_cast<int32_t>(LMXXF_NR_OK) || context_ == nullptr) {
        if (error) {
            *error = FailureText(RuntimeFailure::CreateFailed,
                                 L"status=" + std::to_wstring(result));
        }
        if (context_ != nullptr) {
            api_.Destroy(context_);
            context_ = nullptr;
        }
        assetsDirectory_.clear();
        return false;
    }
    return true;
}

bool Runtime::PrepareSession(std::wstring* error)
{
    if (context_ == nullptr) {
        if (error) *error = FailureText(RuntimeFailure::PrepareFailed, L"session not created");
        return false;
    }
    const int32_t result = api_.PrepareSession(context_);
    if (result != static_cast<int32_t>(LMXXF_NR_OK)) {
        if (error) {
            *error = FailureText(RuntimeFailure::PrepareFailed,
                                 L"status=" + std::to_wstring(result));
        }
        return false;
    }
    return true;
}

void Runtime::ResetSession()
{
    if (context_ != nullptr && api_.Destroy != nullptr) {
        api_.Destroy(context_);
        context_ = nullptr;
    }
    assetsDirectory_.clear();
}

std::string Runtime::LastError() const
{
    if (!Loaded() || api_.GetLastError == nullptr) return {};
    char buffer[1024]{};
    if (api_.GetLastError(buffer, static_cast<uint32_t>(sizeof(buffer))) !=
        static_cast<int32_t>(LMXXF_NR_OK))
        return {};
    return buffer;
}

std::string Runtime::Status() const
{
    if (!Loaded() || api_.GetStatus == nullptr) return {};
    char buffer[1024]{};
    if (api_.GetStatus(context_, buffer, static_cast<uint32_t>(sizeof(buffer))) !=
        static_cast<int32_t>(LMXXF_NR_OK))
        return {};
    return buffer;
}

bool Runtime::GetTimings(LmxxfNrTimings& out, std::string* error)
{
    out = MakeTimings();
    if (!Loaded() || context_ == nullptr) {
        if (error) *error = "timings requested before the session was created";
        return false;
    }
    if (api_.GetTimings == nullptr) {
        if (error) *error = "this runtime ABI has no GetTimings";
        return false;
    }
    const int32_t result = api_.GetTimings(context_, &out);
    if (result != static_cast<int32_t>(LMXXF_NR_OK)) {
        if (error) *error = "GetTimings status=" + std::to_string(result);
        return false;
    }
    return true;
}

} // namespace amd_lmxxf
