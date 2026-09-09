#include "RuntimePolicy.h"

#include <cwctype>
#include <string>

#include <dxgi1_6.h>

namespace {

constexpr uint32_t kNvidiaVendorId = 0x10DE;
constexpr std::wstring_view kSafeModeArgument = L"--safe-mode";
constexpr std::wstring_view kBootstrapMarkerArgument = L"--addon-bootstrap-restarted";
// RTX 4080 SUPER, driver 610.47: 15.31 ms/frame over 738 frames of a 1080p30
// live session, 1.22x the reference cost. Measured by segment arrivals, the
// same method as the reference numbers in PlaybackTiming.h.
constexpr double kAdaRenderPacePrior = 1.22;

bool ContainsCaseInsensitive(std::wstring_view text, std::wstring_view needle)
{
    if (needle.empty() || text.size() < needle.size()) {
        return needle.empty();
    }

    for (size_t offset = 0; offset <= text.size() - needle.size(); ++offset) {
        bool matches = true;
        for (size_t index = 0; index < needle.size(); ++index) {
            if (std::towlower(text[offset + index]) != std::towlower(needle[index])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

void AppendQuotedArgument(std::wstring& commandLine, std::wstring_view argument)
{
    commandLine.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            commandLine.append(backslashes * 2 + 1, L'\\');
            commandLine.push_back(character);
        } else {
            commandLine.append(backslashes, L'\\');
            commandLine.push_back(character);
        }
        backslashes = 0;
    }
    commandLine.append(backslashes * 2, L'\\');
    commandLine.push_back(L'"');
}

std::vector<std::wstring> SanitizeRestartArguments(
    const std::vector<std::wstring>& arguments,
    bool requireSafeMode,
    bool requireBootstrapMarker)
{
    std::vector<std::wstring> sanitized;
    bool hasSafeMode = false;
    for (const std::wstring& argument : arguments) {
        if (argument == kBootstrapMarkerArgument) {
            continue;
        }
        if (argument == kSafeModeArgument) {
            if (!hasSafeMode) {
                sanitized.push_back(argument);
                hasSafeMode = true;
            }
            continue;
        }
        sanitized.push_back(argument);
    }
    if (requireSafeMode && !hasSafeMode) {
        sanitized.emplace_back(kSafeModeArgument);
    }
    if (requireBootstrapMarker) {
        sanitized.emplace_back(kBootstrapMarkerArgument);
    }
    return sanitized;
}

} // namespace

GpuGeneration ClassifyGpu(uint32_t vendorId, std::wstring_view description)
{
    if (vendorId != kNvidiaVendorId) {
        return GpuGeneration::Unsupported;
    }
    if (ContainsCaseInsensitive(description, L"GeForce RTX 20")) {
        return GpuGeneration::Rtx20Turing;
    }
    if (ContainsCaseInsensitive(description, L"GeForce RTX 30")) {
        return GpuGeneration::Rtx30Ampere;
    }
    if (ContainsCaseInsensitive(description, L"GeForce RTX 40")) {
        return GpuGeneration::Rtx40Ada;
    }
    if (ContainsCaseInsensitive(description, L"GeForce RTX 50")) {
        return GpuGeneration::Rtx50Blackwell;
    }
    if (ContainsCaseInsensitive(description, L"RTX")) {
        return GpuGeneration::OtherRtx;
    }
    return GpuGeneration::OtherNvidia;
}

const char* GpuGenerationPathName(GpuGeneration generation) noexcept
{
    switch (generation) {
        case GpuGeneration::Rtx20Turing: return "rtx20";
        case GpuGeneration::Rtx30Ampere: return "rtx30";
        case GpuGeneration::Rtx40Ada: return "rtx40";
        case GpuGeneration::Rtx50Blackwell: return "rtx50";
        case GpuGeneration::OtherRtx: return "rtx";
        case GpuGeneration::OtherNvidia:
        case GpuGeneration::Unsupported: break;
    }
    return "unsupported";
}

bool NeuralAddonDesired(GpuGeneration gpu, bool safeMode)
{
    if (safeMode) return false;
    switch (gpu) {
        case GpuGeneration::Rtx20Turing:
        case GpuGeneration::Rtx30Ampere:
        case GpuGeneration::Rtx40Ada:
        case GpuGeneration::Rtx50Blackwell:
        case GpuGeneration::OtherRtx:
            return true;
        case GpuGeneration::OtherNvidia:
        case GpuGeneration::Unsupported:
            break;
    }
    return false;
}

double RenderPacePrior(GpuGeneration generation) noexcept
{
    switch (generation) {
        case GpuGeneration::Rtx50Blackwell: return 1.0;
        // RTX 4080 SUPER, driver 610.47, 1920x1080 live session: measured
        // against the same segment-arrival method as the reference numbers.
        case GpuGeneration::Rtx40Ada: return kAdaRenderPacePrior;
        case GpuGeneration::Rtx20Turing:
        case GpuGeneration::Rtx30Ampere:
        case GpuGeneration::OtherRtx:
        case GpuGeneration::OtherNvidia:
        case GpuGeneration::Unsupported: break;
    }
    return 0.0;
}

NeuralRenderDefaults ResolveNeuralRenderDefaults(
    bool neuralAddonConfigured,
    bool outputExplicit,
    uint32_t requestedWidth,
    uint32_t requestedHeight)
{
    if (neuralAddonConfigured && !outputExplicit) {
        return {1920, 1080};
    }
    return {requestedWidth, requestedHeight};
}

NeuralRuntimeLayout ClassifyNeuralRuntimeLayout(
    bool hasReShadeConfig,
    bool hasReShadeProxy,
    bool hasNeuralAddon,
    bool hasNeuralRuntime)
{
    const unsigned presentCount = static_cast<unsigned>(hasReShadeConfig) +
                                  static_cast<unsigned>(hasReShadeProxy) +
                                  static_cast<unsigned>(hasNeuralAddon) +
                                  static_cast<unsigned>(hasNeuralRuntime);
    if (presentCount == 0) return NeuralRuntimeLayout::Absent;
    if (presentCount == 4) return NeuralRuntimeLayout::Complete;
    return NeuralRuntimeLayout::Incomplete;
}

DetectedGpu DetectHighPerformanceGpu()
{
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return {};
    }

    for (UINT index = 0;; ++index) {
        IDXGIAdapter4* adapter = nullptr;
        const HRESULT result = factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result)) {
            break;
        }

        DXGI_ADAPTER_DESC3 adapterDescription{};
        const HRESULT descriptionResult = adapter->GetDesc3(&adapterDescription);
        if (FAILED(descriptionResult) || (adapterDescription.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0) {
            adapter->Release();
            continue;
        }
        DetectedGpu detected{ClassifyGpu(adapterDescription.VendorId, adapterDescription.Description),
                             adapterDescription.Description};
        detected.vendorId = adapterDescription.VendorId;
        detected.deviceId = adapterDescription.DeviceId;
        detected.dedicatedVideoMemoryBytes = adapterDescription.DedicatedVideoMemory;
        LARGE_INTEGER driver{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) {
            detected.driverVersion = std::to_wstring(driver.HighPart >> 16) + L'.' +
                                     std::to_wstring(driver.HighPart & 0xFFFF) + L'.' +
                                     std::to_wstring(driver.LowPart >> 16) + L'.' +
                                     std::to_wstring(driver.LowPart & 0xFFFF);
        }
        adapter->Release();
        factory->Release();
        return detected;
    }

    factory->Release();
    return {};
}

BootstrapAction DecideBootstrap(
    bool desiredEnabled,
    bool configEnabled,
    bool alreadyRestarted,
    bool updateSucceeded)
{
    if (!updateSucceeded) {
        return BootstrapAction::Fail;
    }
    if (desiredEnabled == configEnabled) {
        return BootstrapAction::Continue;
    }
    return alreadyRestarted ? BootstrapAction::Fail : BootstrapAction::Relaunch;
}

BootstrapAction DecideBootstrapFromObservedUpdate(
    bool desiredEnabled,
    bool previousEnabled,
    bool currentEnabled,
    bool alreadyRestarted,
    bool updateSucceeded)
{
    if (!updateSucceeded || currentEnabled != desiredEnabled) {
        return BootstrapAction::Fail;
    }
    return DecideBootstrap(desiredEnabled, previousEnabled, alreadyRestarted, true);
}

RuntimeArguments ParseRuntimeArguments(int argc, const wchar_t* const* argv)
{
    RuntimeArguments result;
    if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
        result.error = L"Unable to parse the process command line";
        return result;
    }

    bool hasSafeMode = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            result.error = L"The parsed process command line contained an invalid argument";
            result.userArguments.clear();
            return result;
        }
        const std::wstring argument(argv[index]);
        if (argument == kBootstrapMarkerArgument) {
            result.addonBootstrapRestarted = true;
            continue;
        }
        if (argument == kSafeModeArgument) {
            result.safeMode = true;
            if (hasSafeMode) {
                continue;
            }
            hasSafeMode = true;
        }
        result.userArguments.push_back(argument);
    }
    result.ok = true;
    return result;
}

std::vector<std::wstring> BuildBootstrapRelaunchArguments(
    const std::vector<std::wstring>& userArguments)
{
    return SanitizeRestartArguments(userArguments, false, true);
}

std::vector<std::wstring> BuildSafeModeRestartArguments(
    const std::vector<std::wstring>& userArguments)
{
    return SanitizeRestartArguments(userArguments, true, false);
}

SafeModeRestartOutcome ExecuteAdvancedSafeModeRestart(
    bool confirmed,
    const std::vector<std::wstring>& userArguments,
    const std::function<bool(const std::vector<std::wstring>&)>& launch)
{
    if (!confirmed) {
        return SafeModeRestartOutcome::Cancelled;
    }
    const std::vector<std::wstring> arguments = BuildSafeModeRestartArguments(userArguments);
    if (!launch || !launch(arguments)) {
        return SafeModeRestartOutcome::LaunchFailed;
    }
    return SafeModeRestartOutcome::CloseCurrent;
}

std::wstring BuildWindowsCommandLine(
    std::wstring_view executable,
    const std::vector<std::wstring>& arguments)
{
    std::wstring commandLine;
    AppendQuotedArgument(commandLine, executable);
    for (const std::wstring& argument : arguments) {
        commandLine.push_back(L' ');
        AppendQuotedArgument(commandLine, argument);
    }
    return commandLine;
}

uint64_t NeuralPlaybackLifecycle::Begin()
{
    ++generation;
    state = NeuralPlaybackState::Acquiring;
    return generation;
}

bool NeuralPlaybackLifecycle::Accept(uint64_t candidateGeneration) const
{
    return candidateGeneration != 0 && candidateGeneration == generation &&
           state != NeuralPlaybackState::Idle;
}

bool NeuralPlaybackLifecycle::Transition(NeuralPlaybackState next)
{
    bool allowed = false;
    switch (state) {
    case NeuralPlaybackState::Idle:
        allowed = next == NeuralPlaybackState::Acquiring ||
                  next == NeuralPlaybackState::OriginalOnly;
        break;
    case NeuralPlaybackState::Acquiring:
        allowed = next == NeuralPlaybackState::Rendering || next == NeuralPlaybackState::Ready ||
                  next == NeuralPlaybackState::OriginalOnly || next == NeuralPlaybackState::Cancelling ||
                  next == NeuralPlaybackState::Failed || next == NeuralPlaybackState::Recovering;
        break;
    case NeuralPlaybackState::Rendering:
        allowed = next == NeuralPlaybackState::Validating || next == NeuralPlaybackState::Cancelling ||
                  next == NeuralPlaybackState::Failed || next == NeuralPlaybackState::Paused ||
                  next == NeuralPlaybackState::Recovering;
        break;
    case NeuralPlaybackState::Paused:
        allowed = next == NeuralPlaybackState::Rendering || next == NeuralPlaybackState::Cancelling ||
                  next == NeuralPlaybackState::Failed;
        break;
    case NeuralPlaybackState::Recovering:
        allowed = next == NeuralPlaybackState::Rendering || next == NeuralPlaybackState::Failed ||
                  next == NeuralPlaybackState::RetryExhausted || next == NeuralPlaybackState::Cancelling;
        break;
    case NeuralPlaybackState::Validating:
        allowed = next == NeuralPlaybackState::Ready || next == NeuralPlaybackState::OriginalOnly ||
                  next == NeuralPlaybackState::Cancelling || next == NeuralPlaybackState::Failed;
        break;
    case NeuralPlaybackState::Ready:
        allowed = next == NeuralPlaybackState::Acquiring || next == NeuralPlaybackState::OriginalOnly ||
                  next == NeuralPlaybackState::Idle;
        break;
    case NeuralPlaybackState::OriginalOnly:
        allowed = next == NeuralPlaybackState::Acquiring || next == NeuralPlaybackState::Idle;
        break;
    case NeuralPlaybackState::Cancelling:
        allowed = next == NeuralPlaybackState::OriginalOnly || next == NeuralPlaybackState::Idle ||
                  next == NeuralPlaybackState::Failed;
        break;
    case NeuralPlaybackState::Failed:
    case NeuralPlaybackState::RetryExhausted:
        allowed = next == NeuralPlaybackState::OriginalOnly || next == NeuralPlaybackState::Acquiring ||
                  next == NeuralPlaybackState::Idle;
        break;
    }
    if (allowed) state = next;
    return allowed;
}

const wchar_t* NeuralPlaybackStateName(NeuralPlaybackState state) noexcept
{
    switch (state) {
    case NeuralPlaybackState::Idle: return L"Idle";
    case NeuralPlaybackState::Acquiring: return L"Acquiring";
    case NeuralPlaybackState::Rendering: return L"Rendering";
    case NeuralPlaybackState::Validating: return L"Validating";
    case NeuralPlaybackState::Ready: return L"Ready";
    case NeuralPlaybackState::OriginalOnly: return L"OriginalOnly";
    case NeuralPlaybackState::Cancelling: return L"Cancelling";
    case NeuralPlaybackState::Failed: return L"Failed";
    case NeuralPlaybackState::Paused: return L"Paused";
    case NeuralPlaybackState::Recovering: return L"Recovering";
    case NeuralPlaybackState::RetryExhausted: return L"RetryExhausted";
    }
    return L"Unknown";
}

NeuralPlaybackState StateForFailure(NeuralRenderFailure failure) noexcept
{
    switch (failure) {
    case NeuralRenderFailure::RetryExhausted: return NeuralPlaybackState::RetryExhausted;
    case NeuralRenderFailure::Cancelled: return NeuralPlaybackState::OriginalOnly;
    default: return NeuralPlaybackState::Failed;
    }
}

NeuralPlaybackState StateForProgressPhase(NeuralRenderPhase phase, NeuralPlaybackState current) noexcept
{
    switch (phase) {
    case NeuralRenderPhase::Acquiring:
    case NeuralRenderPhase::Preflight: return NeuralPlaybackState::Acquiring;
    case NeuralRenderPhase::Decoding:
    case NeuralRenderPhase::NeuralRendering:
    case NeuralRenderPhase::Encoding: return NeuralPlaybackState::Rendering;
    case NeuralRenderPhase::Validating: return NeuralPlaybackState::Validating;
    case NeuralRenderPhase::Paused: return NeuralPlaybackState::Paused;
    case NeuralRenderPhase::Recovering: return NeuralPlaybackState::Recovering;
    case NeuralRenderPhase::CheckingCache:
    case NeuralRenderPhase::Ready: return current;
    }
    return current;
}

void NeuralPlaybackLifecycle::Invalidate()
{
    ++generation;
    state = NeuralPlaybackState::Idle;
}

NeuralOpenAction DecideNeuralOpen(bool runtimeComplete, bool safeMode, bool cacheValid)
{
    if (!runtimeComplete || safeMode) return NeuralOpenAction::OriginalOnly;
    return cacheValid ? NeuralOpenAction::UseCache : NeuralOpenAction::StartJob;
}

bool CanPublishNeuralCompletion(bool renderOk, bool probeOk, bool manifestValid)
{
    return renderOk && probeOk && manifestValid;
}

void ExecuteNeuralReplacementSequence(const std::function<void()>& requestStop,
                                      const std::function<void()>& joinWorker,
                                      const std::function<void()>& replace)
{
    if (requestStop) requestStop();
    if (joinWorker) joinWorker();
    if (replace) replace();
}
