#pragma once

#include "VideoDecoder.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

enum class NetworkCommitKind {
    InitialOpen,
    Seek,
    QualityReload,
};

struct NetworkRenderConfiguration {
    uint32_t sourceWidth{};
    uint32_t sourceHeight{};
    uint32_t decodeWidth{};
    uint32_t decodeHeight{};
    uint32_t inputWidth{};
    uint32_t inputHeight{};
    uint32_t outputWidth{};
    uint32_t outputHeight{};
    uint32_t guideWidth{};
    uint32_t guideHeight{};
    int quality{};

    bool operator==(const NetworkRenderConfiguration&) const = default;
};

inline bool NetworkConfigurationMatchesExceptInput(const NetworkRenderConfiguration& left,
                                                   const NetworkRenderConfiguration& right)
{
    auto a=left,b=right;a.inputWidth=b.inputWidth=0;a.inputHeight=b.inputHeight=0;return a==b;
}

inline bool NetworkPreparedGeometryIsValid(const NetworkRenderConfiguration& configuration,
                                           uint32_t frameWidth,
                                           uint32_t frameHeight,
                                           size_t frameBytes)
{
    if (!configuration.sourceWidth || !configuration.sourceHeight ||
        !configuration.decodeWidth || !configuration.decodeHeight ||
        !configuration.inputWidth || !configuration.inputHeight ||
        !configuration.outputWidth || !configuration.outputHeight ||
        !configuration.guideWidth || !configuration.guideHeight ||
        frameWidth != configuration.decodeWidth ||
        frameHeight != configuration.decodeHeight) {
        return false;
    }
    constexpr size_t bytesPerPixel = 4;
    if (frameWidth > std::numeric_limits<size_t>::max() / bytesPerPixel ||
        frameHeight > std::numeric_limits<size_t>::max() /
                          (static_cast<size_t>(frameWidth) * bytesPerPixel)) {
        return false;
    }
    return frameBytes == static_cast<size_t>(frameWidth) * frameHeight * bytesPerPixel;
}

template<class Candidate, class Factory, class Validator, class Committer>
bool ExecuteNetworkCandidateTransaction(Factory&& factory,
                                        Validator&& validate,
                                        Committer&& commit)
{
    std::unique_ptr<Candidate> candidate = std::forward<Factory>(factory)();
    if (!candidate || !std::forward<Validator>(validate)(*candidate)) return false;
    std::forward<Committer>(commit)(std::move(candidate));
    return true;
}

template<class StopOld, class InstallPrepared, class ActivatePrepared>
void CommitPreparedAudioHandoff(StopOld&& stopOld,
                                InstallPrepared&& installPrepared,
                                ActivatePrepared&& activatePrepared)
{
    std::forward<InstallPrepared>(installPrepared)();
    std::forward<StopOld>(stopOld)();
    std::forward<ActivatePrepared>(activatePrepared)();
}

enum class NetworkReadPosition {
    BeforeRender,
    AfterRender,
};

enum class NetworkReadAction {
    UseFrame,
    Wait,
    StopClean,
    StopError,
    StopCancelled,
};

struct NetworkReadDecision {
    NetworkReadAction action{NetworkReadAction::Wait};
    bool notify{false};
    std::wstring_view messageKey{};
};

class NetworkReadState {
public:
    NetworkReadDecision Resolve(VideoReadResult result, NetworkReadPosition) {
        switch (result) {
        case VideoReadResult::FrameReady:
            return {NetworkReadAction::UseFrame, false, {}};
        case VideoReadResult::NotReady:
            return {NetworkReadAction::Wait, false, {}};
        case VideoReadResult::EndOfStream:
            return {NetworkReadAction::StopClean, false, {}};
        case VideoReadResult::Cancelled:
            return {NetworkReadAction::StopCancelled, false, {}};
        case VideoReadResult::Stalled:
            return Error(L"youtube.error.media_stalled");
        case VideoReadResult::Error:
        default:
            return Error(L"youtube.error.ffmpeg");
        }
    }

    void Reset() { m_terminalReported = false; }

private:
    NetworkReadDecision Error(std::wstring_view key) {
        const bool notify = !m_terminalReported;
        m_terminalReported = true;
        return {NetworkReadAction::StopError, notify, key};
    }

    bool m_terminalReported{false};
};
