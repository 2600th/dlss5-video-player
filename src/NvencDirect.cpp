#include "NvencDirect.h"

#include "D3D12FenceWait.h"
#include "Log.h"
#include "NvencDirectMux.h"
#include "NvencDirectPolicy.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <set>

using Microsoft::WRL::ComPtr;

// ---- NvencSurfacePool ---------------------------------------------------------

bool NvencSurfacePool::Configure(ID3D12Device* device, ID3D12Fence* captureFence, EncoderPixelFormat format,
                                 uint32_t width, uint32_t height, uint32_t captureSlots)
{
    std::lock_guard lock(mutex_);
    if (!device || !captureFence || format == EncoderPixelFormat::Bgra || !width || !height ||
        (width | height) & 1u)
        return false;
    if (device_.Get() != device || format_ != format || width_ != width || height_ != height) {
        surfaces_.clear();
        busy_.clear();
    }
    device_ = device;
    fence_ = captureFence;
    format_ = format;
    width_ = width;
    height_ = height;
    captureSlots_ = captureSlots;
    return true;
}

bool NvencSurfacePool::Configured() const
{
    std::lock_guard lock(mutex_);
    return device_ != nullptr;
}

bool NvencSurfacePool::Reserve(uint32_t count)
{
    std::lock_guard lock(mutex_);
    if (!device_) return false;
    if (std::find(busy_.begin(), busy_.end(), true) != busy_.end()) return surfaces_.size() >= count;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width_;
    desc.Height = height_;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format_ == EncoderPixelFormat::P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    while (surfaces_.size() < count) {
        ComPtr<ID3D12Resource> surface;
        // COMMON, the state a resource crosses to another engine in: the capture
        // takes it to COPY_DEST and back around its copy, and NVENC reads it there.
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&surface)))) {
            LOG("NVENC direct: could not create encode surface " << surfaces_.size() << " of " << count << ".");
            return false;
        }
        surface->SetName(format_ == EncoderPixelFormat::P010 ? L"Nvenc_Direct_Surface_P010"
                                                             : L"Nvenc_Direct_Surface_NV12");
        surfaces_.push_back(std::move(surface));
        busy_.push_back(false);
    }
    return true;
}

void NvencSurfacePool::Reset()
{
    std::lock_guard lock(mutex_);
    surfaces_.clear();
    busy_.clear();
    fence_.Reset();
    device_.Reset();
}

std::optional<uint32_t> NvencSurfacePool::Acquire(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    const auto free = [this] { return std::find(busy_.begin(), busy_.end(), false) != busy_.end(); };
    if (!freed_.wait_for(lock, timeout, free)) return std::nullopt;
    const auto found = std::find(busy_.begin(), busy_.end(), false);
    *found = true;
    return uint32_t(found - busy_.begin());
}

void NvencSurfacePool::Release(uint32_t index)
{
    {
        std::lock_guard lock(mutex_);
        if (index < busy_.size()) busy_[index] = false;
    }
    freed_.notify_all();
}

void NvencSurfacePool::ReleaseAll()
{
    {
        std::lock_guard lock(mutex_);
        std::fill(busy_.begin(), busy_.end(), false);
    }
    freed_.notify_all();
}

ID3D12Resource* NvencSurfacePool::Surface(uint32_t index) const
{
    std::lock_guard lock(mutex_);
    return index < surfaces_.size() ? surfaces_[index].Get() : nullptr;
}

uint32_t NvencSurfacePool::Count() const { std::lock_guard lock(mutex_); return uint32_t(surfaces_.size()); }
uint32_t NvencSurfacePool::CaptureSlots() const { std::lock_guard lock(mutex_); return captureSlots_; }
ID3D12Device* NvencSurfacePool::Device() const { std::lock_guard lock(mutex_); return device_.Get(); }
ID3D12Fence* NvencSurfacePool::CaptureFence() const { std::lock_guard lock(mutex_); return fence_.Get(); }
EncoderPixelFormat NvencSurfacePool::Format() const { std::lock_guard lock(mutex_); return format_; }
uint32_t NvencSurfacePool::Width() const { std::lock_guard lock(mutex_); return width_; }
uint32_t NvencSurfacePool::Height() const { std::lock_guard lock(mutex_); return height_; }

#if defined(DLSS_VIDEO_PLAYER_HAS_NVENC)

// WIN32_LEAN_AND_MEAN leaves the crypto headers out of windows.h.
#include <bcrypt.h>
#include <wincrypt.h>
#include <mscat.h>
#include <softpub.h>
#include <wintrust.h>

namespace {

using PfnCreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using PfnMaxSupportedVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);

struct Api {
    NV_ENCODE_API_FUNCTION_LIST fn{};
    uint32_t driverVersion{};
    std::string failure;
    bool ok{};
};

std::string HexHash(std::span<const uint8_t> hash)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string text;
    for (const uint8_t byte : hash) {
        text.push_back(kDigits[byte >> 4]);
        text.push_back(kDigits[byte & 15]);
    }
    return text;
}

// A driver file is signed through the driver package's catalog rather than in
// its own body - nvEncodeAPI64.dll reports SignatureType Catalog, signer
// "Microsoft Windows Hardware Compatibility Publisher" - so WinVerifyTrust has to
// be handed the catalog that lists the file's hash. An embedded signature is
// tried first in case a later driver adds one.
bool SignedFile(const std::filesystem::path& file)
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = file.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    if (status == ERROR_SUCCESS) return true;

    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool verified = false;
    // SHA-256 catalogs first, then the SHA-1 ones older packages still carry.
    for (const wchar_t* algorithm : {BCRYPT_SHA256_ALGORITHM, static_cast<const wchar_t*>(nullptr)}) {
        HCATADMIN admin = nullptr;
        if (!CryptCATAdminAcquireContext2(&admin, nullptr, algorithm, nullptr, 0)) continue;
        DWORD hashBytes = 0;
        std::vector<uint8_t> hash;
        if (CryptCATAdminCalcHashFromFileHandle2(admin, handle, &hashBytes, nullptr, 0) || hashBytes) {
            hash.resize(hashBytes);
            if (!CryptCATAdminCalcHashFromFileHandle2(admin, handle, &hashBytes, hash.data(), 0)) hash.clear();
        }
        HCATINFO catalog = hash.empty() ? nullptr
            : CryptCATAdminEnumCatalogFromHash(admin, hash.data(), hashBytes, 0, nullptr);
        if (catalog) {
            CATALOG_INFO info{};
            info.cbStruct = sizeof(info);
            if (CryptCATCatalogInfoFromContext(catalog, &info, 0)) {
                const std::string tag = HexHash(hash);
                const std::wstring memberTag(tag.begin(), tag.end());
                WINTRUST_CATALOG_INFO catalogInfo{};
                catalogInfo.cbStruct = sizeof(catalogInfo);
                catalogInfo.pcwszCatalogFilePath = info.wszCatalogFile;
                catalogInfo.pcwszMemberFilePath = file.c_str();
                catalogInfo.pcwszMemberTag = memberTag.c_str();
                catalogInfo.hMemberFile = handle;
                catalogInfo.pbCalculatedFileHash = hash.data();
                catalogInfo.cbCalculatedFileHash = hashBytes;
                catalogInfo.hCatAdmin = admin;
                WINTRUST_DATA catalogData{};
                catalogData.cbStruct = sizeof(catalogData);
                catalogData.dwUIChoice = WTD_UI_NONE;
                catalogData.fdwRevocationChecks = WTD_REVOKE_NONE;
                catalogData.dwUnionChoice = WTD_CHOICE_CATALOG;
                catalogData.pCatalog = &catalogInfo;
                catalogData.dwStateAction = WTD_STATEACTION_VERIFY;
                catalogData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
                status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &catalogData);
                catalogData.dwStateAction = WTD_STATEACTION_CLOSE;
                WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &catalogData);
                verified = status == ERROR_SUCCESS;
            }
            CryptCATAdminReleaseCatalogContext(admin, catalog, 0);
        }
        CryptCATAdminReleaseContext(admin, 0);
        if (verified) break;
    }
    CloseHandle(handle);
    return verified;
}

// Never bundled and never looked for beside the executable: the encoder is a
// driver component, so the system directory's copy - which the driver install
// links into place from its driver-store package - is the only one loaded, and
// only once its signature checks out.
Api LoadApi()
{
    Api api;
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH) { api.failure = "the system directory could not be read"; return api; }
    const std::filesystem::path file = std::filesystem::path(system) / L"nvEncodeAPI64.dll";
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        api.failure = "nvEncodeAPI64.dll is not in the driver install";
        return api;
    }
    if (!SignedFile(file)) {
        api.failure = "nvEncodeAPI64.dll failed its signature check";
        return api;
    }
    const HMODULE module = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { api.failure = "nvEncodeAPI64.dll could not be loaded"; return api; }
    const auto maxVersion = reinterpret_cast<PfnMaxSupportedVersion>(
        GetProcAddress(module, "NvEncodeAPIGetMaxSupportedVersion"));
    const auto create = reinterpret_cast<PfnCreateInstance>(GetProcAddress(module, "NvEncodeAPICreateInstance"));
    if (!maxVersion || !create) { api.failure = "nvEncodeAPI64.dll exports no NVENC entry points"; return api; }
    if (maxVersion(&api.driverVersion) != NV_ENC_SUCCESS) {
        api.failure = "NvEncodeAPIGetMaxSupportedVersion failed";
        return api;
    }
    // The driver answers (major << 4) | minor.
    constexpr uint32_t kBuilt = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (api.driverVersion < kBuilt) {
        api.failure = "the driver's NVENC API is " + std::to_string(api.driverVersion >> 4) + "." +
                      std::to_string(api.driverVersion & 15) + ", older than the 13.1 this build uses";
        return api;
    }
    api.fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create(&api.fn) != NV_ENC_SUCCESS || !api.fn.nvEncOpenEncodeSessionEx || !api.fn.nvEncInitializeEncoder ||
        !api.fn.nvEncEncodePicture || !api.fn.nvEncLockBitstream || !api.fn.nvEncRegisterResource ||
        !api.fn.nvEncMapInputResource || !api.fn.nvEncGetEncodePresetConfigEx || !api.fn.nvEncGetSequenceParams ||
        !api.fn.nvEncDestroyEncoder) {
        api.failure = "NvEncodeAPICreateInstance returned an incomplete function list";
        return api;
    }
    // The module stays loaded for the process: every session's function
    // pointers point into it.
    api.ok = true;
    return api;
}

const Api& SharedApi()
{
    static const Api api = LoadApi();
    return api;
}

const char* StatusName(NVENCSTATUS status)
{
    switch (status) {
    case NV_ENC_SUCCESS: return "SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE: return "NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE: return "INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE: return "INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST: return "DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PTR: return "INVALID_PTR";
    case NV_ENC_ERR_INVALID_EVENT: return "INVALID_EVENT";
    case NV_ENC_ERR_INVALID_PARAM: return "INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL: return "INVALID_CALL";
    case NV_ENC_ERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED: return "ENCODER_NOT_INITIALIZED";
    case NV_ENC_ERR_UNSUPPORTED_PARAM: return "UNSUPPORTED_PARAM";
    case NV_ENC_ERR_LOCK_BUSY: return "LOCK_BUSY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER: return "NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_INVALID_VERSION: return "INVALID_VERSION";
    case NV_ENC_ERR_MAP_FAILED: return "MAP_FAILED";
    case NV_ENC_ERR_NEED_MORE_INPUT: return "NEED_MORE_INPUT";
    case NV_ENC_ERR_ENCODER_BUSY: return "ENCODER_BUSY";
    case NV_ENC_ERR_EVENT_NOT_REGISTERD: return "EVENT_NOT_REGISTERED";
    case NV_ENC_ERR_GENERIC: return "GENERIC";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY: return "INCOMPATIBLE_CLIENT_KEY";
    case NV_ENC_ERR_UNIMPLEMENTED: return "UNIMPLEMENTED";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED: return "RESOURCE_REGISTER_FAILED";
    case NV_ENC_ERR_RESOURCE_NOT_REGISTERED: return "RESOURCE_NOT_REGISTERED";
    case NV_ENC_ERR_RESOURCE_NOT_MAPPED: return "RESOURCE_NOT_MAPPED";
    default: break;
    }
    return "UNKNOWN";
}

// NVENC keeps the surfaces of the frames it has not coded yet. How many it
// keeps is set by the configuration (NvencDirectPolicy.h), and this many more
// pictures are left submitted but unlocked while the next frame is still
// arriving, so NVENC always has the next encode queued behind the current one.
constexpr uint32_t kLockDepth = 2;

} // namespace

bool nvenc_direct::DriverAvailable(std::string* reason)
{
    const Api& api = SharedApi();
    if (reason) *reason = api.failure;
    return api.ok;
}

struct NvencDirectEncoder::Impl {
    const Api* api{};
    void* session{};
    NvencSurfacePool* pool{};
    EncoderSpec spec{};
    nvenc_direct::Rational rate{};
    nvenc_direct::Plan plan{};
    NV_ENC_INITIALIZE_PARAMS init{};
    NV_ENC_CONFIG config{};
    std::filesystem::path helperDirectory, output, intermediate;
    nvenc_direct::mkv::Writer writer;
    std::string detail;
    // Per pool surface, registered the first time a frame uses it.
    std::vector<NV_ENC_REGISTERED_PTR> registered;
    struct Output {
        ComPtr<ID3D12Resource> buffer;
        NV_ENC_REGISTERED_PTR registered{};
        NV_ENC_INPUT_PTR mapped{};
        NV_ENC_OUTPUT_RESOURCE_D3D12 resource{};
        bool busy{};
    };
    std::vector<Output> outputs;
    uint32_t nextOutput{};
    // A frame NVENC has been handed and not yet coded: the surface it holds and
    // the mapping that holds it. Keyed by frame index, which is what a locked
    // picture's timestamp names.
    struct Input {
        uint32_t surface{};
        NV_ENC_INPUT_PTR mapped{};
        NV_ENC_INPUT_RESOURCE_D3D12 resource{};
    };
    std::map<uint64_t, Input> inputs;
    std::set<uint64_t> coded;
    uint64_t codedThrough{};
    struct Submission {
        uint32_t output{};
        uint64_t fenceValue{};
    };
    // Submitted and answered NEED_MORE_INPUT: NVENC will fill these outputs once
    // a later frame lets it code them.
    std::deque<Submission> waiting;
    // Encodes NVENC has been told to run, oldest first; their pictures come out
    // of the outputs in this order.
    std::deque<Submission> issued;
    ComPtr<ID3D12Fence> outputFence;
    uint64_t outputFenceValue{};
    HANDLE event{};
    uint64_t frames{};
    bool active{};

    ~Impl() { Destroy(); }

    EncodeError Fail(std::string what, EncodeError error = EncodeError::WriteFailed)
    {
        detail = std::move(what);
        LOG("NVENC direct: " << detail);
        return error;
    }

    EncodeError Fail(const char* call, NVENCSTATUS status, EncodeError error = EncodeError::WriteFailed)
    {
        return Fail(std::string(call) + " returned " + StatusName(status), error);
    }

    // Unmaps and unregisters everything, destroys the session, and gives back
    // every surface a frame of this session still holds.
    void Destroy()
    {
        if (session && api) {
            for (auto& [frame, input] : inputs)
                if (input.mapped) api->fn.nvEncUnmapInputResource(session, input.mapped);
            for (Output& out : outputs) {
                if (out.mapped) api->fn.nvEncUnmapInputResource(session, out.mapped);
                if (out.registered) api->fn.nvEncUnregisterResource(session, out.registered);
            }
            for (NV_ENC_REGISTERED_PTR surface : registered)
                if (surface) api->fn.nvEncUnregisterResource(session, surface);
            api->fn.nvEncDestroyEncoder(session);
        }
        session = nullptr;
        if (pool) for (auto& [frame, input] : inputs) pool->Release(input.surface);
        inputs.clear();
        coded.clear();
        outputs.clear();
        registered.clear();
        waiting.clear();
        issued.clear();
        outputFence.Reset();
        if (event) { CloseHandle(event); event = nullptr; }
        active = false;
    }

    bool WaitOutput(uint64_t value)
    {
        const auto result = d3d12_renderer_detail::WaitForGPUFenceCompletion(
            value, GetTickCount64(), d3d12_renderer_detail::RenderFenceWaitMilliseconds,
            [&] { return outputFence->GetCompletedValue(); },
            [&](uint64_t v) { return outputFence->SetEventOnCompletion(v, event); },
            [&](DWORD timeout) { return WaitForSingleObject(event, timeout); });
        return result == d3d12_renderer_detail::FenceWaitResult::Completed;
    }

    // Gives back the surfaces of every frame NVENC has finished with. A frame is
    // released once it and every frame before it have been coded: NVENC codes
    // out of order within a group of B-frames, and a surface is only safe to
    // overwrite once the frames that could still reference it as input are done.
    void ReleaseCoded(uint64_t frame)
    {
        coded.insert(frame);
        while (!coded.empty() && *coded.begin() == codedThrough) {
            coded.erase(coded.begin());
            ++codedThrough;
        }
        while (!inputs.empty() && inputs.begin()->first < codedThrough) {
            Input& input = inputs.begin()->second;
            if (input.mapped) api->fn.nvEncUnmapInputResource(session, input.mapped);
            pool->Release(input.surface);
            inputs.erase(inputs.begin());
        }
    }

    EncodeError LockOldest()
    {
        const Submission submission = issued.front();
        issued.pop_front();
        Output& out = outputs[submission.output];
        // The D3D12 interface does not synchronize the lock itself: the output
        // fence is what says the picture is in the buffer.
        if (!WaitOutput(submission.fenceValue)) return Fail("an encoded picture did not complete in time");
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = &out.resource;
        NVENCSTATUS status = api->fn.nvEncLockBitstream(session, &lock);
        if (status != NV_ENC_SUCCESS) return Fail("nvEncLockBitstream", status);
        const uint64_t frame = lock.outputTimeStamp;
        // hevc_nvenc flags only an IDR picture as a keyframe; an I picture that
        // is not one is not a random-access point for the index either.
        const bool keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
        const bool written = writer.Write(
            std::span<const uint8_t>(static_cast<const uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes),
            nvenc_direct::FrameMilliseconds(frame, rate), keyframe);
        status = api->fn.nvEncUnlockBitstream(session, &out.resource);
        if (!written) return Fail("the encoded picture could not be written to " + intermediate.string());
        if (status != NV_ENC_SUCCESS) return Fail("nvEncUnlockBitstream", status);
        if (out.mapped) {
            api->fn.nvEncUnmapInputResource(session, out.mapped);
            out.mapped = nullptr;
        }
        out.busy = false;
        ReleaseCoded(frame);
        return EncodeError::None;
    }

    EncodeError LockAll()
    {
        while (!issued.empty()) {
            const EncodeError error = LockOldest();
            if (error != EncodeError::None) return error;
        }
        return EncodeError::None;
    }
};

NvencDirectEncoder::NvencDirectEncoder() : impl_(std::make_unique<Impl>()) {}
NvencDirectEncoder::~NvencDirectEncoder() { Cancel(); }

const std::string& NvencDirectEncoder::Detail() const { return impl_->detail; }

EncodeError NvencDirectEncoder::Start(NvencSurfacePool& pool, const EncoderSpec& spec,
                                      const std::filesystem::path& output,
                                      const std::filesystem::path& helperDirectory)
{
    Cancel();
    Impl& s = *impl_;
    s.detail.clear();
    const Api& api = SharedApi();
    if (!api.ok) return s.Fail(api.failure, EncodeError::StartFailed);
    const bool tenBit = EncoderQualityIsTenBit(spec.quality);
    if (spec.kind != EncoderKind::HevcNvenc || !pool.Configured() ||
        pool.Format() != (tenBit ? EncoderPixelFormat::P010 : EncoderPixelFormat::Nv12) ||
        pool.Format() != spec.pixelFormat || pool.Width() != spec.width || pool.Height() != spec.height ||
        output.empty() || !std::isfinite(spec.fps) || spec.fps <= 0.0)
        return s.Fail("the render is not one the direct path encodes", EncodeError::InvalidSpecification);
    s.api = &api;
    s.pool = &pool;
    s.spec = spec;
    s.output = output;
    s.helperDirectory = helperDirectory;
    s.rate = nvenc_direct::ChildFrameRate(spec.fps);
    if (s.rate.num <= 0 || s.rate.den <= 0) return s.Fail("the frame rate has no rational form", EncodeError::InvalidSpecification);

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device = pool.Device();
    open.apiVersion = NVENCAPI_VERSION;
    NVENCSTATUS status = api.fn.nvEncOpenEncodeSessionEx(&open, &s.session);
    if (status != NV_ENC_SUCCESS) {
        s.session = nullptr;
        return s.Fail("nvEncOpenEncodeSessionEx", status, EncodeError::StartFailed);
    }
    if (tenBit) {
        NV_ENC_CAPS_PARAM caps{};
        caps.version = NV_ENC_CAPS_PARAM_VER;
        caps.capsToQuery = NV_ENC_CAPS_SUPPORT_10BIT_ENCODE;
        int supported = 0;
        if (api.fn.nvEncGetEncodeCaps(s.session, NV_ENC_CODEC_HEVC_GUID, &caps, &supported) != NV_ENC_SUCCESS ||
            !supported) {
            s.Destroy();
            return s.Fail("this GPU's NVENC has no 10-bit HEVC", EncodeError::StartFailed);
        }
    }
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    status = api.fn.nvEncGetEncodePresetConfigEx(s.session, NV_ENC_CODEC_HEVC_GUID,
                                                 nvenc_direct::PresetGuid(spec.nvencPreset),
                                                 NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset);
    if (status != NV_ENC_SUCCESS) {
        s.Destroy();
        return s.Fail("nvEncGetEncodePresetConfigEx", status, EncodeError::StartFailed);
    }
    s.config = preset.presetCfg;
    s.init = NV_ENC_INITIALIZE_PARAMS{};
    s.plan = nvenc_direct::ApplyHevcNvencOptions(spec, s.rate, s.init, s.config);
    status = api.fn.nvEncInitializeEncoder(s.session, &s.init);
    if (status != NV_ENC_SUCCESS) {
        s.Destroy();
        return s.Fail("nvEncInitializeEncoder", status, EncodeError::StartFailed);
    }
    const uint32_t surfaces = nvenc_direct::SurfacePoolSize(pool.CaptureSlots(), s.plan.frameIntervalP,
                                                            s.plan.lookahead, kLockDepth);
    if (!pool.Reserve(surfaces)) {
        s.Destroy();
        return s.Fail("the encode surfaces could not be allocated", EncodeError::StartFailed);
    }
    s.registered.assign(pool.Count(), nullptr);

    // Output buffers: readback memory NVENC writes each picture into, registered
    // once for the session.
    ID3D12Device* device = pool.Device();
    const uint32_t outputBytes = nvenc_direct::OutputBufferBytes(spec.width, spec.height, tenBit);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = outputBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    s.outputs.resize(nvenc_direct::OutputBufferCount(s.plan.frameIntervalP, s.plan.lookahead, kLockDepth));
    for (Impl::Output& out : s.outputs) {
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&out.buffer)))) {
            s.Destroy();
            return s.Fail("an output bitstream buffer could not be allocated", EncodeError::StartFailed);
        }
        out.buffer->SetName(L"Nvenc_Direct_Bitstream");
        NV_ENC_REGISTER_RESOURCE reg{};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.width = outputBytes;
        reg.height = 1;
        reg.resourceToRegister = out.buffer.Get();
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_U8;
        reg.bufferUsage = NV_ENC_OUTPUT_BITSTREAM;
        status = api.fn.nvEncRegisterResource(s.session, &reg);
        if (status != NV_ENC_SUCCESS) {
            s.Destroy();
            return s.Fail("nvEncRegisterResource (bitstream)", status, EncodeError::StartFailed);
        }
        out.registered = reg.registeredResource;
    }
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.outputFence)))) {
        s.Destroy();
        return s.Fail("the output fence could not be created", EncodeError::StartFailed);
    }
    s.outputFenceValue = 0;
    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.event) {
        s.Destroy();
        return s.Fail("the output fence event could not be created", EncodeError::StartFailed);
    }

    // The parameter sets, which Matroska carries as codec private data.
    std::vector<uint8_t> header(1024);
    uint32_t headerBytes = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = uint32_t(header.size());
    payload.spsppsBuffer = header.data();
    payload.outSPSPPSPayloadSize = &headerBytes;
    status = api.fn.nvEncGetSequenceParams(s.session, &payload);
    if (status != NV_ENC_SUCCESS || !headerBytes || headerBytes > header.size()) {
        s.Destroy();
        return s.Fail("nvEncGetSequenceParams", status, EncodeError::StartFailed);
    }
    header.resize(headerBytes);
    // In the temp directory and never beside the output, for the reason
    // ConcatenateMedia's list lives there: the output's folder is a staging folder
    // that is renamed whole into the published cache entry, so a hand-off file
    // whose removal failed would travel into renders/<key>/ with it.
    std::error_code tempError;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(tempError);
    if (tempError) {
        s.Destroy();
        return s.Fail("no temp directory for the hand-off file", EncodeError::StartFailed);
    }
    static std::atomic_uint64_t sequence{};
    s.intermediate = temp / (L"dlss-nvenc-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1)) + L".mkv");
    if (!s.writer.Open(s.intermediate, spec.width, spec.height, s.rate, header)) {
        s.Destroy();
        return s.Fail("could not create " + s.intermediate.string(), EncodeError::StartFailed);
    }
    s.frames = 0;
    s.codedThrough = 0;
    s.nextOutput = 0;
    s.active = true;
    LOG("NVENC direct session: " << spec.width << "x" << spec.height << (tenBit ? " Main10 P010" : " Main NV12")
        << " p" << std::clamp<uint32_t>(spec.nvencPreset, 1u, 7u) << " at " << s.rate.num << "/" << s.rate.den
        << " fps, frameIntervalP " << s.plan.frameIntervalP << ", lookahead " << s.plan.lookahead
        << ", " << pool.Count() << " surfaces, " << s.outputs.size() << " bitstream buffers of "
        << outputBytes / 1024u << " KiB; driver NVENC API " << (api.driverVersion >> 4) << "."
        << (api.driverVersion & 15) << ".");
    return EncodeError::None;
}

EncodeError NvencDirectEncoder::Encode(std::span<const uint8_t> tokenBytes, bool moreQueued)
{
    Impl& s = *impl_;
    if (!s.active) return EncodeError::WriteFailed;
    NvencDirectToken token;
    if (!ReadNvencDirectToken(tokenBytes, token) || token.surface >= s.registered.size()) {
        return s.Fail("a frame arrived without a valid encode surface", EncodeError::InvalidFrame);
    }
    const Api& api = *s.api;
    // Every output is attached to one submission until its picture is locked;
    // the count is sized so the one due next is only ever taken by a picture
    // NVENC has already been told to encode.
    while (s.outputs[s.nextOutput].busy) {
        if (s.issued.empty()) return s.Fail("no bitstream buffer came free");
        const EncodeError error = s.LockOldest();
        if (error != EncodeError::None) return error;
    }
    NVENCSTATUS status;
    if (!s.registered[token.surface]) {
        NV_ENC_REGISTER_RESOURCE reg{};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.width = s.spec.width;
        reg.height = s.spec.height;
        reg.resourceToRegister = s.pool->Surface(token.surface);
        reg.bufferFormat = s.init.bufferFormat;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        status = api.fn.nvEncRegisterResource(s.session, &reg);
        if (status != NV_ENC_SUCCESS) return s.Fail("nvEncRegisterResource (surface)", status);
        s.registered[token.surface] = reg.registeredResource;
    }
    const uint64_t frame = s.frames;
    Impl::Input& input = s.inputs[frame];
    input.surface = token.surface;
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = s.registered[token.surface];
    status = api.fn.nvEncMapInputResource(s.session, &map);
    if (status != NV_ENC_SUCCESS) return s.Fail("nvEncMapInputResource (surface)", status);
    input.mapped = map.mappedResource;
    input.resource = {};
    input.resource.version = NV_ENC_INPUT_RESOURCE_D3D12_VER;
    input.resource.pInputBuffer = map.mappedResource;
    // NVENC waits on the GPU for the capture's own fence; nothing here waits.
    input.resource.inputFencePoint.version = NV_ENC_FENCE_POINT_D3D12_VER;
    input.resource.inputFencePoint.pFence = s.pool->CaptureFence();
    input.resource.inputFencePoint.waitValue = token.fenceValue;
    input.resource.inputFencePoint.bWait = 1;

    const uint32_t outputIndex = s.nextOutput;
    Impl::Output& out = s.outputs[outputIndex];
    NV_ENC_MAP_INPUT_RESOURCE outMap{};
    outMap.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    outMap.registeredResource = out.registered;
    status = api.fn.nvEncMapInputResource(s.session, &outMap);
    if (status != NV_ENC_SUCCESS) return s.Fail("nvEncMapInputResource (bitstream)", status);
    out.mapped = outMap.mappedResource;
    out.resource = {};
    out.resource.version = NV_ENC_OUTPUT_RESOURCE_D3D12_VER;
    out.resource.pOutputBuffer = outMap.mappedResource;
    out.resource.outputFencePoint.version = NV_ENC_FENCE_POINT_D3D12_VER;
    out.resource.outputFencePoint.pFence = s.outputFence.Get();
    out.resource.outputFencePoint.signalValue = ++s.outputFenceValue;
    out.resource.outputFencePoint.bSignal = 1;
    out.busy = true;
    s.nextOutput = (s.nextOutput + 1u) % uint32_t(s.outputs.size());

    // What nvenc_send_frame fills in for a frame with no side data and no forced
    // picture type.
    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = s.spec.width;
    pic.inputHeight = s.spec.height;
    pic.inputPitch = s.spec.width;
    pic.encodePicFlags = 0;
    pic.frameIdx = uint32_t(frame);
    pic.inputTimeStamp = frame;
    pic.inputBuffer = &input.resource;
    pic.outputBitstream = &out.resource;
    pic.bufferFmt = s.init.bufferFormat;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.codecPicParams.hevcPicParams.sliceMode = s.config.encodeCodecConfig.hevcConfig.sliceMode;
    pic.codecPicParams.hevcPicParams.sliceModeData = s.config.encodeCodecConfig.hevcConfig.sliceModeData;
    s.waiting.push_back({outputIndex, out.resource.outputFencePoint.signalValue});
    ++s.frames;
    status = api.fn.nvEncEncodePicture(s.session, &pic);
    if (status == NV_ENC_SUCCESS) {
        s.issued.insert(s.issued.end(), s.waiting.begin(), s.waiting.end());
        s.waiting.clear();
    } else if (status != NV_ENC_ERR_NEED_MORE_INPUT) {
        return s.Fail("nvEncEncodePicture", status);
    }
    while (!s.issued.empty() && (s.issued.size() > kLockDepth || !moreQueued)) {
        const EncodeError error = s.LockOldest();
        if (error != EncodeError::None) return error;
    }
    return EncodeError::None;
}

EncodeError NvencDirectEncoder::Finish(std::stop_token stop)
{
    Impl& s = *impl_;
    if (!s.active) return EncodeError::FinishFailed;
    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    const NVENCSTATUS status = s.api->fn.nvEncEncodePicture(s.session, &eos);
    if (status != NV_ENC_SUCCESS) {
        s.Fail("nvEncEncodePicture (end of stream)", status);
        Cancel();
        return EncodeError::FinishFailed;
    }
    s.issued.insert(s.issued.end(), s.waiting.begin(), s.waiting.end());
    s.waiting.clear();
    if (s.LockAll() != EncodeError::None || !s.inputs.empty()) {
        if (s.detail.empty()) s.Fail("frames were left uncoded at the end of the stream");
        Cancel();
        return EncodeError::FinishFailed;
    }
    const uint64_t frames = s.frames;
    s.Destroy();
    if (!s.writer.Close()) {
        s.Fail("could not finish " + s.intermediate.string());
        s.writer.Abandon();
        return EncodeError::FinishFailed;
    }
    const EncodeError muxed = RemuxVideoStream(s.helperDirectory, s.intermediate, s.output, stop);
    s.writer.Abandon();  // deletes the intermediate either way
    if (muxed != EncodeError::None) {
        std::error_code ignored;
        std::filesystem::remove(s.output, ignored);
        if (muxed != EncodeError::Cancelled) s.Fail("the final mux into " + s.output.string() + " failed", muxed);
        return muxed;
    }
    LOG("NVENC direct: " << frames << " frames encoded and muxed into " << s.output.filename().string() << ".");
    return EncodeError::None;
}

void NvencDirectEncoder::Cancel()
{
    if (!impl_) return;
    impl_->Destroy();
    impl_->writer.Abandon();
}

#else  // DLSS_VIDEO_PLAYER_HAS_NVENC

bool nvenc_direct::DriverAvailable(std::string* reason)
{
    if (reason) *reason = "the build has no nvEncodeAPI.h";
    return false;
}

struct NvencDirectEncoder::Impl {
    std::string detail{"the build has no nvEncodeAPI.h"};
};

NvencDirectEncoder::NvencDirectEncoder() : impl_(std::make_unique<Impl>()) {}
NvencDirectEncoder::~NvencDirectEncoder() = default;
const std::string& NvencDirectEncoder::Detail() const { return impl_->detail; }
EncodeError NvencDirectEncoder::Start(NvencSurfacePool&, const EncoderSpec&, const std::filesystem::path&,
                                      const std::filesystem::path&)
{
    return EncodeError::StartFailed;
}
EncodeError NvencDirectEncoder::Encode(std::span<const uint8_t>, bool) { return EncodeError::WriteFailed; }
EncodeError NvencDirectEncoder::Finish(std::stop_token) { return EncodeError::FinishFailed; }
void NvencDirectEncoder::Cancel() {}

#endif
