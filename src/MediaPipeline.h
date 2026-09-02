#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

enum class EncoderKind {
    HevcNvenc,
    H264Software,
};

enum class EncodeError {
    None,
    InvalidSpecification,
    HelperMissing,
    StartFailed,
    WriteFailed,
    FinishFailed,
    Cancelled,
    InvalidFrame,
};

enum class MaterializeError {
    None,
    InvalidRequest,
    HelperMissing,
    StartFailed,
    ProcessFailed,
    Cancelled,
};

struct MaterializeRequest {
    std::wstring videoUrl;
    std::wstring audioUrl;
    std::filesystem::path output;
};

struct EncoderSpec {
    uint32_t width{};
    uint32_t height{};
    double fps{};
    EncoderKind kind{EncoderKind::HevcNvenc};
};

struct MaterializeResult {
    bool ok{};
    MaterializeError error{MaterializeError::None};
    std::wstring detail;
};

struct ProbeResult {
    bool ok{};
    uint32_t width{};
    uint32_t height{};
    uint64_t frameCount{};
    int64_t duration100ns{};
    bool decodedFinalFrame{};
    std::wstring detail;
};

std::vector<std::wstring> BuildMaterializeArguments(const MaterializeRequest& request);
std::vector<std::wstring> BuildEncoderArguments(const EncoderSpec& spec,
                                                const std::filesystem::path& output);
size_t ExpectedBgraFrameBytes(const EncoderSpec& spec);
bool ShouldRetryWithSoftware(EncoderKind attempted, EncodeError error);

class MediaMaterializer {
public:
    explicit MediaMaterializer(std::filesystem::path helperDirectory = {});
    MaterializeResult Run(const MaterializeRequest& request, std::stop_token stop);

private:
    std::filesystem::path helperDirectory_;
};

class RawVideoEncoder {
public:
    explicit RawVideoEncoder(std::filesystem::path helperDirectory = {});
    ~RawVideoEncoder();
    RawVideoEncoder(const RawVideoEncoder&) = delete;
    RawVideoEncoder& operator=(const RawVideoEncoder&) = delete;

    EncodeError Start(const EncoderSpec& spec, const std::filesystem::path& output);
    EncodeError WriteFrame(std::span<const uint8_t> bgra, std::stop_token stop = {});
    EncodeError Finish(std::stop_token stop = {});
    void Cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class MediaProbeMode { FullValidation, CachedMetadata };

ProbeResult ProbeMedia(const std::filesystem::path& helperDirectory,
                       const std::filesystem::path& media,
                       std::stop_token stop,
                       MediaProbeMode mode = MediaProbeMode::FullValidation);
