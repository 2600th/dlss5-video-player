#pragma once

#include "NvencDirectPolicy.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

// The hand-off file between the direct NVENC session and FFmpeg's muxer.
//
// The session produces what hevc_nvenc hands the Matroska muxer inside the
// encoder child: Annex B parameter sets from nvEncGetSequenceParams, and Annex B
// pictures in coding order, each with the index of the frame it shows. A raw
// .hevc stream cannot carry that across: with the preset's B-frames the picture
// order is not the frame order, and FFmpeg's raw HEVC demuxer knows no
// timestamps at all - measured, remuxing one refused every packet with "Can't
// write packet with unknown timestamp". So the session writes this minimal
// Matroska instead - one track, the parameter sets as CodecPrivate in their
// Annex B form, each picture a SimpleBlock at its millisecond timestamp - and
// RemuxVideoStream stream-copies it into the cache file. The muxer then sees
// exactly what it sees inside the child, extradata and packets alike (it
// converts Annex B to length-prefixed NAL units and builds hvcC from the Annex B
// extradata the same way for both), so the file comes out laid out, indexed and
// tagged by FFmpeg as before. Nothing here is ever read by anything but that
// remux.
namespace nvenc_direct::mkv {

// EBML's variable-length size: the shortest encoding whose value bits are not
// all ones, which is the "unknown size" marker at every length.
inline void AppendSize(std::vector<uint8_t>& out, uint64_t size)
{
    int length = 1;
    while (length < 8 && size >= (uint64_t{1} << (7 * length)) - 1u) ++length;
    const uint64_t marked = size | (uint64_t{1} << (7 * length));
    for (int index = length - 1; index >= 0; --index) out.push_back(uint8_t(marked >> (8 * index)));
}

// Element IDs keep their length marker, so they are written as they are spelled.
inline void AppendId(std::vector<uint8_t>& out, uint32_t id)
{
    int bytes = id > 0xFFFFFFu ? 4 : id > 0xFFFFu ? 3 : id > 0xFFu ? 2 : 1;
    for (int index = bytes - 1; index >= 0; --index) out.push_back(uint8_t(id >> (8 * index)));
}

inline void AppendBinary(std::vector<uint8_t>& out, uint32_t id, std::span<const uint8_t> payload)
{
    AppendId(out, id);
    AppendSize(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

inline void AppendUnsigned(std::vector<uint8_t>& out, uint32_t id, uint64_t value)
{
    uint8_t bytes[8];
    int length = 0;
    do {
        bytes[7 - length++] = uint8_t(value);
        value >>= 8;
    } while (value && length < 8);
    AppendBinary(out, id, std::span<const uint8_t>(bytes + 8 - length, size_t(length)));
}

inline void AppendString(std::vector<uint8_t>& out, uint32_t id, std::string_view text)
{
    AppendBinary(out, id, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

inline void AppendMaster(std::vector<uint8_t>& out, uint32_t id, const std::vector<uint8_t>& children)
{
    AppendBinary(out, id, children);
}

namespace id {
inline constexpr uint32_t kEbml = 0x1A45DFA3, kEbmlVersion = 0x4286, kEbmlReadVersion = 0x42F7,
    kEbmlMaxIdLength = 0x42F2, kEbmlMaxSizeLength = 0x42F3, kDocType = 0x4282, kDocTypeVersion = 0x4287,
    kDocTypeReadVersion = 0x4285, kSegment = 0x18538067, kInfo = 0x1549A966, kTimestampScale = 0x2AD7B1,
    kMuxingApp = 0x4D80, kWritingApp = 0x5741, kTracks = 0x1654AE6B, kTrackEntry = 0xAE,
    kTrackNumber = 0xD7, kTrackUid = 0x73C5, kTrackType = 0x83, kFlagLacing = 0x9C, kCodecId = 0x86,
    kCodecPrivate = 0x63A2, kDefaultDuration = 0x23E383, kVideo = 0xE0, kPixelWidth = 0xB0,
    kPixelHeight = 0xBA, kColour = 0x55B0, kMatrixCoefficients = 0x55B1, kRange = 0x55B9,
    kTransferCharacteristics = 0x55BA, kPrimaries = 0x55BB, kCluster = 0x1F43B675, kTimestamp = 0xE7,
    kSimpleBlock = 0xA3;
}

// Everything up to and including the Tracks element, with the Segment's size
// left as an eight-byte field the writer fills in once the file is complete.
// `segmentSizeOffset` receives where that field starts.
inline std::vector<uint8_t> Header(uint32_t width, uint32_t height, Rational rate,
                                   std::span<const uint8_t> parameterSets, size_t& segmentSizeOffset)
{
    std::vector<uint8_t> out, ebml, info, track, video, colour, entry;
    AppendUnsigned(ebml, id::kEbmlVersion, 1);
    AppendUnsigned(ebml, id::kEbmlReadVersion, 1);
    AppendUnsigned(ebml, id::kEbmlMaxIdLength, 4);
    AppendUnsigned(ebml, id::kEbmlMaxSizeLength, 8);
    AppendString(ebml, id::kDocType, "matroska");
    AppendUnsigned(ebml, id::kDocTypeVersion, 4);
    AppendUnsigned(ebml, id::kDocTypeReadVersion, 2);
    AppendMaster(out, id::kEbml, ebml);
    AppendId(out, id::kSegment);
    segmentSizeOffset = out.size();
    out.insert(out.end(), {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    AppendUnsigned(info, id::kTimestampScale, 1'000'000);
    AppendString(info, id::kMuxingApp, "DLSSVideoPlayer");
    AppendString(info, id::kWritingApp, "DLSSVideoPlayer NVENC direct");
    AppendMaster(out, id::kInfo, info);
    // BT.709 limited range, the same four properties the encoder child stamps
    // with setparams and states in the VUI.
    AppendUnsigned(colour, id::kMatrixCoefficients, 1);
    AppendUnsigned(colour, id::kRange, 1);
    AppendUnsigned(colour, id::kTransferCharacteristics, 1);
    AppendUnsigned(colour, id::kPrimaries, 1);
    AppendUnsigned(video, id::kPixelWidth, width);
    AppendUnsigned(video, id::kPixelHeight, height);
    AppendMaster(video, id::kColour, colour);
    AppendUnsigned(entry, id::kTrackNumber, 1);
    AppendUnsigned(entry, id::kTrackUid, 1);
    AppendUnsigned(entry, id::kTrackType, 1);
    AppendUnsigned(entry, id::kFlagLacing, 0);
    AppendString(entry, id::kCodecId, "V_MPEGH/ISO/HEVC");
    AppendBinary(entry, id::kCodecPrivate, parameterSets);
    if (const uint64_t duration = DefaultDurationNanoseconds(rate)) AppendUnsigned(entry, id::kDefaultDuration, duration);
    AppendMaster(entry, id::kVideo, video);
    AppendMaster(track, id::kTrackEntry, entry);
    AppendMaster(out, id::kTracks, track);
    return out;
}

// One SimpleBlock of track 1: its timestamp relative to the cluster's, and the
// keyframe flag, which the cache file's index (Cues) is built from.
inline void AppendSimpleBlock(std::vector<uint8_t>& out, std::span<const uint8_t> picture, int16_t relative,
                              bool keyframe)
{
    AppendId(out, id::kSimpleBlock);
    AppendSize(out, picture.size() + 4u);
    out.push_back(0x81);
    out.push_back(uint8_t(uint16_t(relative) >> 8));
    out.push_back(uint8_t(uint16_t(relative)));
    out.push_back(keyframe ? 0x80 : 0x00);
    out.insert(out.end(), picture.begin(), picture.end());
}

class Writer {
public:
    Writer() = default;
    ~Writer() { Abandon(); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    bool Open(const std::filesystem::path& path, uint32_t width, uint32_t height, Rational rate,
              std::span<const uint8_t> parameterSets)
    {
        Abandon();
        if (_wfopen_s(&file_, path.c_str(), L"wb") != 0 || !file_) { file_ = nullptr; return false; }
        path_ = path;
        const std::vector<uint8_t> header = Header(width, height, rate, parameterSets, segmentSizeOffset_);
        segmentStart_ = segmentSizeOffset_ + 8u;
        written_ = header.size();
        ok_ = Put(header);
        return ok_;
    }

    // Pictures arrive in coding order. A cluster opens at every keyframe, and
    // before a timestamp would leave the SimpleBlock's signed 16-bit range.
    bool Write(std::span<const uint8_t> picture, int64_t milliseconds, bool keyframe)
    {
        if (!file_ || !ok_) return false;
        const bool fits = clusterOpen_ && milliseconds - clusterTime_ > kMinRelative &&
                          milliseconds - clusterTime_ < kMaxRelative && cluster_.size() < kClusterBytes;
        if (!fits || (keyframe && clusterBlocks_)) {
            if (!FlushCluster()) return false;
            clusterOpen_ = true;
            clusterTime_ = milliseconds;
        }
        AppendSimpleBlock(cluster_, picture, int16_t(milliseconds - clusterTime_), keyframe);
        ++clusterBlocks_;
        return true;
    }

    // Writes the last cluster and the Segment's size.
    bool Close()
    {
        if (!file_) return false;
        bool ok = ok_ && FlushCluster();
        const uint64_t segmentBytes = written_ - segmentStart_;
        uint8_t size[8];
        for (int index = 0; index < 8; ++index) size[index] = uint8_t(segmentBytes >> (8 * (7 - index)));
        size[0] |= 0x01;
        ok = ok && segmentBytes < (uint64_t{1} << 56) - 1u &&
             _fseeki64(file_, int64_t(segmentSizeOffset_), SEEK_SET) == 0 && fwrite(size, 1, 8, file_) == 8;
        ok = fclose(file_) == 0 && ok;
        file_ = nullptr;
        return ok;
    }

    // Closes without finishing and deletes what was written.
    void Abandon()
    {
        if (file_) {
            fclose(file_);
            file_ = nullptr;
        }
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
            path_.clear();
        }
        cluster_.clear();
        clusterOpen_ = false;
        clusterBlocks_ = 0;
    }

    // Keeps the finished file; the next Abandon no longer deletes it.
    void Release() { path_.clear(); }

private:
    static constexpr int64_t kMinRelative = std::numeric_limits<int16_t>::min() + 1000;
    static constexpr int64_t kMaxRelative = std::numeric_limits<int16_t>::max() - 1000;
    static constexpr size_t kClusterBytes = 32u * 1024u * 1024u;

    bool Put(std::span<const uint8_t> bytes)
    {
        return fwrite(bytes.data(), 1, bytes.size(), file_) == bytes.size();
    }

    bool FlushCluster()
    {
        if (!clusterBlocks_) {
            cluster_.clear();
            return true;
        }
        std::vector<uint8_t> element, timestamp;
        AppendUnsigned(timestamp, id::kTimestamp, uint64_t(clusterTime_));
        AppendId(element, id::kCluster);
        AppendSize(element, timestamp.size() + cluster_.size());
        element.insert(element.end(), timestamp.begin(), timestamp.end());
        ok_ = ok_ && Put(element) && Put(cluster_);
        written_ += element.size() + cluster_.size();
        cluster_.clear();
        clusterBlocks_ = 0;
        return ok_;
    }

    FILE* file_{};
    std::filesystem::path path_;
    std::vector<uint8_t> cluster_;
    size_t segmentSizeOffset_{};
    uint64_t segmentStart_{};
    uint64_t written_{};
    int64_t clusterTime_{};
    uint32_t clusterBlocks_{};
    bool clusterOpen_{};
    bool ok_{};
};

} // namespace nvenc_direct::mkv
