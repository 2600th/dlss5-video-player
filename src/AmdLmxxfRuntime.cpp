#include "AmdLmxxfRuntime.h"

#include <system_error>
#include <utility>

namespace amd_lmxxf {
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

    api_.struct_size = sizeof(api_);
    const auto getApiFn = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(getApi);
    int32_t result = getApiFn(LMXXF_NR_ABI_VERSION, &api_);
    if (result == static_cast<int32_t>(LMXXF_NR_INVALID_ARGUMENT)) {
        api_ = {};
        api_.struct_size = LMXXF_NR_API_V1_SIZE;
        result = getApiFn(LMXXF_NR_ABI_VERSION, &api_);
    }
    if (result != static_cast<int32_t>(LMXXF_NR_OK)) {
        if (error) {
            *error = FailureText(RuntimeFailure::GetApiFailed,
                                 L"status=" + std::to_wstring(result));
        }
        Unload();
        return false;
    }

    if (const ApiProblem problem = ValidateApiTable(api_); problem != ApiProblem::None) {
        if (error) {
            *error = FailureText(RuntimeFailure::InvalidApiTable,
                                 WideAscii(ApiProblemName(problem)));
        }
        Unload();
        return false;
    }
    return true;
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
