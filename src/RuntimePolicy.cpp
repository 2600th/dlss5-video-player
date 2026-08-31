#include "RuntimePolicy.h"

#include <cwctype>
#include <string>

#include <dxgi1_6.h>

namespace {

constexpr uint32_t kNvidiaVendorId = 0x10DE;
constexpr std::wstring_view kSafeModeArgument = L"--safe-mode";
constexpr std::wstring_view kBootstrapMarkerArgument = L"--addon-bootstrap-restarted";

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
    if (ContainsCaseInsensitive(description, L"GeForce RTX 40")) {
        return GpuGeneration::Rtx40Ada;
    }
    if (ContainsCaseInsensitive(description, L"GeForce RTX 50")) {
        return GpuGeneration::Rtx50Blackwell;
    }
    return GpuGeneration::OtherNvidia;
}

bool NeuralAddonDesired(GpuGeneration gpu, bool safeMode)
{
    return !safeMode && (gpu == GpuGeneration::Rtx40Ada || gpu == GpuGeneration::Rtx50Blackwell);
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
        adapter->Release();
        if (FAILED(descriptionResult) || (adapterDescription.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0) {
            continue;
        }

        factory->Release();
        return {ClassifyGpu(adapterDescription.VendorId, adapterDescription.Description), adapterDescription.Description};
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
