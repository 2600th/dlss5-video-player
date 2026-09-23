#pragma once
#include <d3d12.h>
#include <dxgiformat.h>
#include <cstdint>
#include <string>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include "NgxSession.h"

// What the runtime answered when asked for DLSS Frame Generation on this
// machine. Every field is measured: nothing here is inferred from the GPU
// name, the driver version or the presence of nvngx_dlssg.dll.
struct DLSSGCapability {
    bool available = false;          // feature created (the caller releases it: ReleaseProbedFeature)
    uint32_t multiFrameCountMax = 0; // generated frames per source pair the runtime admits (1 => 2x)
    bool hagsEnabled = false;        // HwSchMode == 2
    NVSDK_NGX_Result createResult = NVSDK_NGX_Result_Fail;
    std::wstring detail;             // human-readable reason when !available
};

// Direct NGX DLSS-G (Frame Generation) host.
//
// Frame Generation is normally reached through Streamline, whose sl.dlss_g.dll
// owns the swapchain and drives presentation and pacing. This player holds its
// own swapchain and calls NGX directly, so whether the raw
// NVSDK_NGX_D3D12_CreateFeature path admits NVSDK_NGX_Feature_FrameGeneration
// at all was an open question that only hardware could answer, and whether a
// feature it admits actually produces an intermediate image is a second one:
// NVSDK_NGX_Result_Success with an untouched output texture is a negative
// result, not a working feature. So there are two entry points here, in the
// order the two measurements had to be made. Probe creates the feature and
// records the NVSDK_NGX_Result, answering admission; the caller releases that
// feature once the create work has retired. Initialize creates the same
// feature and keeps it, so Evaluate can run on it and a caller can read the
// interpolated frame back.
//
// What Probe measured on 2026-09-17 (RTX 5090, driver 616.64, Windows 11 26200,
// no Streamline module loaded): the create returns NVSDK_NGX_Result_Success
// at 1920x1080 B8G8R8A8_UNORM and DLSSG.MultiFrameCountMax reads 5 - but only
// with nvngx_dlssg.dll resolvable beside the executable. Without it the same
// create answers 0xbad0000b and the capability block reports
// FrameGeneration.Available=0 with FeatureInitResult=0xbad00004, even though
// NGX locates and logs the driver-store fallback snippet. So raw-NGX DLSS-G
// admission is real on this machine and it is conditional on shipping the
// snippet, exactly as DLSS-SR ships nvngx_dlss.dll.
//
// What Evaluate measured on the same machine, same day, through
// tests/DlssgEvaluateSmoke.cpp - a synthetic pair whose only content is a
// 200x200 square translated exactly +200 px between two 1920x1080 frames, so
// the true intermediate frame has one correct answer: the raw evaluate path
// returns Success and writes a real intermediate frame. All 2073600 output
// pixels were written over a sentinel fill, and the square landed with its
// horizontal centroid at 812.7 against 699.5 in frame A and 899.5 in frame B -
// between the two inputs, differing from both (mean channel difference 5.4 and
// 4.4 of 255), soft-edged over a 736..894 span, and 13.2 px past the 799.5
// midpoint toward the newer frame. Production is therefore real, not just
// admission.
//
// It is not produced from the motion buffer. The same pair was evaluated three
// times - motion written in backbuffer pixels, motion written in normalized
// screen units, and motion deliberately zeroed, which claims nothing moved
// while the colour pair shows a 200 px jump - and the output was the same all
// three times (centroid 812.73 / 812.73 / 812.79, mean channel difference
// between runs 0.00 and 0.01 of 255). Whatever this build interpolates from,
// the tagged DLSSG.MVecs are not it, so nothing may claim that a better motion
// estimate buys a better generated frame until a runtime is measured that
// reads them.
class DLSSGBackend {
public:
    ~DLSSGBackend();

    // Creates the FrameGeneration feature at the given backbuffer geometry to
    // answer whether the runtime admits it. The create records work on `cmd`
    // that references the feature, so the feature is NOT released here: it is
    // held apart from the one Initialize keeps (Evaluate never sees it) until
    // the caller has submitted `cmd`, waited for it to retire and called
    // ReleaseProbedFeature - or Shutdown, which releases it too. Releasing it
    // here, as Probe did, freed the feature before the GPU ran its create.
    DLSSGCapability Probe(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                          uint32_t width, uint32_t height, DXGI_FORMAT backbufferFormat);
    // Releases the feature Probe created and answers with the release's own
    // result, Success when there is none. Only after the command list Probe
    // recorded into has retired; for one that never did, Abandon forgets it.
    NVSDK_NGX_Result ReleaseProbedFeature();

    // Geometry and format the feature is created for; backbuffer-resolution inputs.
    // Unlike Probe the feature is kept, so the create work this records on `cmd`
    // has to be submitted and retired before the first Evaluate.
    bool Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                    uint32_t width, uint32_t height, DXGI_FORMAT backbufferFormat);

    // Generates intermediate frame `multiFrameIndex` (1-based, <= multiFrameCount)
    // between the PREVIOUS evaluated backbuffer and `backbuffer`.
    // `motion` points from the current frame to the previous one, at backbuffer
    // resolution. `reset` breaks temporal continuity (first frame, scene cut).
    //
    // `backbufferFrameId` is DLSSG.BackbufferFrameID: the runtime's optional
    // declaration of frame progression, documented in nvsdk_ngx_defs_dlssg.h as
    // a counter that increments by one per fully rendered backbuffer frame and
    // keeps incrementing while generation is off. A caller that presents real
    // frames has one already; an offline caller has to state it, because the
    // alternative is a runtime deriving progression from nothing. 0 leaves the
    // key unset, which is what the capability probe wants - it creates a
    // feature and never evaluates. Every index generated between the same pair
    // carries the id of the newer frame of that pair: the pair is one rendered
    // frame's worth of progression however many frames are generated inside it.
    bool Evaluate(ID3D12GraphicsCommandList* cmd,
                  ID3D12Resource* backbuffer,
                  ID3D12Resource* motion,
                  ID3D12Resource* depth,
                  ID3D12Resource* outputInterpolated,
                  uint32_t multiFrameCount,
                  uint32_t multiFrameIndex,
                  bool reset,
                  uint64_t backbufferFrameId = 0);
    uint64_t EvaluationCount() const { return m_evaluations; }
    bool FeatureCreated() const { return m_handle != nullptr; }
    // DLSSG.MultiFrameCountMax as the runtime reported it to Initialize: the
    // most frames it will generate between one source pair, so the largest
    // multiplier is one more. 0 until Initialize has run, which is the same
    // "no answer" every other measured field here uses.
    uint32_t MultiFrameCountMax() const { return m_multiFrameCountMax; }

    // Units the motion-vector texture handed to Evaluate is written in. DLSS-G
    // takes no pixel-space option: DLSSG.MvecScale{X,Y} exist to normalize the
    // buffer into [-1,1], so the units are declared here and the scale is
    // derived from them rather than left to a caller to get consistent.
    // BackbufferPixels is the default because that is what this project's
    // motion producers emit - TemporalGuides writes "current -> previous,
    // already in DLSS input pixels" and the DLSS-SR path consumes it with
    // MV_Scale 1.0 - so the pixel buffer is the one that already exists.
    // NormalizedScreen exists because whether the runtime applies MvecScale at
    // all on the raw path could not be read out of the headers, and handing it
    // the same displacement under both conventions was the way to find out.
    // The answer measured on 2026-09-17 was that it cannot be told from the
    // output at all: both conventions, and a zeroed buffer, produced the same
    // interpolated frame. The declaration is still made correctly here, because
    // the option that is silently wrong the day a runtime does start reading
    // DLSSG.MVecs is the one that costs a debugging session.
    enum class MotionVectorUnits { BackbufferPixels, NormalizedScreen };
    void SetMotionVectorUnits(MotionVectorUnits units) { m_mvecUnits = units; }

    void Shutdown();
    // For a GPU that never retired the work this feature was recorded into:
    // forgets the feature, its parameters and the NGX session lease WITHOUT
    // releasing any of them, because releasing them under work that may still
    // be executing is the one thing worse than leaking them. Logged.
    void Abandon();

    // Whether the runtime admitted the feature: set by a Probe whose create
    // succeeded (and cleared again if its release is refused), and by an
    // Initialize that is holding a live feature. False until one of them succeeds and false again after
    // Shutdown, so no caller may read admission out of a call that did not run.
    bool Available() const { return m_available; }
    // True once NGX itself came up on the probe device and handed over a
    // parameter block, so a caller can tell a runtime refusal - which is a
    // verdict - from NGX never initializing, which is not one.
    bool SessionEstablished() const { return m_sessionLeaseAcquired && m_params != nullptr; }
    NVSDK_NGX_Result LastResult() const { return m_lastResult; }

private:
    bool AcquireSession(ID3D12Device* device);
    NVSDK_NGX_Result CreateFeature(ID3D12GraphicsCommandList* cmd,
                                   uint32_t width, uint32_t height, DXGI_FORMAT backbufferFormat);
    NVSDK_NGX_DLSSG_Opt_Eval_Params VideoEvalConstants(uint32_t multiFrameCount, uint32_t multiFrameIndex,
                                                       bool reset) const;

    ID3D12Device* m_device = nullptr;
    const void* m_sessionKey = nullptr;
    NVSDK_NGX_Parameter* m_params = nullptr;
    NVSDK_NGX_Handle* m_handle = nullptr;
    NVSDK_NGX_Handle* m_probeHandle = nullptr;  // Probe's, until ReleaseProbedFeature
    uint32_t m_width = 0, m_height = 0;
    DXGI_FORMAT m_backbufferFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t m_multiFrameCountMax = 0;
    uint64_t m_evaluations = 0;
    MotionVectorUnits m_mvecUnits = MotionVectorUnits::BackbufferPixels;
    NVSDK_NGX_Result m_lastResult = NVSDK_NGX_Result_Fail;
    bool m_sessionLeaseAcquired = false;
    bool m_available = false;
};
