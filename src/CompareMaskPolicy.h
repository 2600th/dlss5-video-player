#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The spatial mask on the Mix (P2.6): a grey image stretched over the frame, white
// where DLSS 5 is shown at the Mix and black where the original is, so a face can be
// protected from the model while the rest of the frame keeps it. It is composited in
// the player's own presentation pass - NGX's mask inputs are inert on this runtime -
// and it is presentation-only, like the Mix itself. What happens to the image before
// it reaches the GPU lives here: the size it is kept at, the feather, and how the
// player remembers which mask belongs to which source.
namespace compare_mask {

struct Gray {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // row-major, tightly packed
};

// A mask is stretched over the frame, so detail finer than the frame's own is wasted
// VRAM and upload time. Anything larger than this on either side is box-averaged down
// by the smallest whole factor that fits.
inline constexpr uint32_t kMaxSide = 4096;

inline Gray Shrink(const Gray& source, uint32_t maxSide = kMaxSide)
{
    if (!source.width || !source.height || source.pixels.size() < size_t(source.width) * source.height) return {};
    const uint32_t largest = std::max(source.width, source.height);
    const uint32_t factor = (largest + maxSide - 1) / maxSide;
    if (factor <= 1) return source;
    Gray out;
    out.width = std::max(1u, source.width / factor);
    out.height = std::max(1u, source.height / factor);
    out.pixels.resize(size_t(out.width) * out.height);
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            uint32_t sum = 0, count = 0;
            for (uint32_t dy = 0; dy < factor && y * factor + dy < source.height; ++dy)
                for (uint32_t dx = 0; dx < factor && x * factor + dx < source.width; ++dx) {
                    sum += source.pixels[size_t(y * factor + dy) * source.width + x * factor + dx];
                    ++count;
                }
            out.pixels[size_t(y) * out.width + x] = uint8_t((sum + count / 2) / std::max(1u, count));
        }
    }
    return out;
}

// One running-sum box pass of half-width `radius` along a line of `count` samples
// `stride` apart, edges replicated. O(1) per sample whatever the radius.
inline void BoxLine(uint8_t* line, size_t count, size_t stride, int radius, std::vector<uint32_t>& scratch)
{
    if (radius <= 0 || count < 2) return;
    scratch.resize(count);
    for (size_t i = 0; i < count; ++i) scratch[i] = line[i * stride];
    const auto at = [&](long long index) {
        return scratch[size_t(std::clamp<long long>(index, 0, (long long)count - 1))];
    };
    const uint32_t width = uint32_t(2 * radius + 1);
    uint32_t sum = 0;
    for (long long k = -radius; k <= radius; ++k) sum += at(k);
    for (size_t i = 0; i < count; ++i) {
        line[i * stride] = uint8_t((sum + width / 2) / width);
        sum += at((long long)i + radius + 1);
        sum -= at((long long)i - radius);
    }
}

// The feather: three box passes each way, the usual cheap stand-in for a Gaussian,
// with half-widths that add up to `radius` so the edge softens over about that many
// pixels of the MASK. Callers convert from frame pixels with MaskRadius.
inline void Feather(Gray& mask, int radius)
{
    if (radius <= 0 || !mask.width || !mask.height) return;
    const int pass = std::max(1, (radius + 1) / 3);
    std::vector<uint32_t> scratch;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (uint32_t y = 0; y < mask.height; ++y)
            BoxLine(mask.pixels.data() + size_t(y) * mask.width, mask.width, 1, pass, scratch);
        for (uint32_t x = 0; x < mask.width; ++x)
            BoxLine(mask.pixels.data() + x, mask.height, mask.width, pass, scratch);
    }
}

// The feather is chosen in pixels of the frame on screen - the render's output - and
// the mask is stretched over that, so its radius in mask pixels scales by their ratio.
inline int MaskRadius(int framePixels, uint32_t maskWidth, uint32_t frameWidth)
{
    if (framePixels <= 0 || !maskWidth) return 0;
    if (!frameWidth) return framePixels;
    return std::max(1, int(double(framePixels) * double(maskWidth) / double(frameWidth) + 0.5));
}

// The feather choices, in frame pixels. Off first; 8 is where a new mask starts, soft
// enough that a hand-painted edge does not read as a cut-out.
inline constexpr std::array<int, 6> kFeathers{0, 4, 8, 16, 32, 64};
inline constexpr int kDefaultFeather = 8;

// Which mask a source had, remembered in [ComparisonMasks] under a key per source:
// "seq|feather|invert|path". seq orders the entries so the oldest is the one dropped
// when there are more than kMaxRemembered - the section would otherwise grow by one
// line per video anyone ever masked.
inline constexpr size_t kMaxRemembered = 32;

struct Record {
    uint64_t sequence = 0;
    int feather = kDefaultFeather;
    bool invert = false;
    std::wstring path;

    friend bool operator==(const Record&, const Record&) = default;
};

inline std::wstring Format(const Record& record)
{
    return std::to_wstring(record.sequence) + L"|" + std::to_wstring(record.feather) + L"|" +
           (record.invert ? L"1" : L"0") + L"|" + record.path;
}

inline std::optional<Record> Parse(std::wstring_view text)
{
    std::array<std::wstring_view, 3> fields{};
    for (auto& field : fields) {
        const size_t bar = text.find(L'|');
        if (bar == std::wstring_view::npos) return std::nullopt;
        field = text.substr(0, bar);
        text.remove_prefix(bar + 1);
    }
    if (text.empty() || fields[0].empty() || fields[1].empty() || (fields[2] != L"0" && fields[2] != L"1"))
        return std::nullopt;
    Record record;
    for (const wchar_t c : fields[0]) {
        if (c < L'0' || c > L'9') return std::nullopt;
        record.sequence = record.sequence * 10u + uint64_t(c - L'0');
    }
    int feather = 0;
    for (const wchar_t c : fields[1]) {
        if (c < L'0' || c > L'9' || feather > 100000) return std::nullopt;
        feather = feather * 10 + int(c - L'0');
    }
    // Snapped onto the offered choices, so a hand edit cannot ask for a 10k blur.
    record.feather = kFeathers.front();
    for (const int choice : kFeathers)
        if (choice <= feather) record.feather = choice;
    record.invert = fields[2] == L"1";
    record.path.assign(text);
    return record;
}

// The keys to delete so at most kMaxRemembered remain: the lowest sequence numbers.
inline std::vector<std::wstring> Evict(const std::map<std::wstring, Record>& records, size_t keep = kMaxRemembered)
{
    std::vector<std::pair<uint64_t, std::wstring>> order;
    for (const auto& [key, record] : records) order.emplace_back(record.sequence, key);
    std::sort(order.begin(), order.end());
    std::vector<std::wstring> evicted;
    for (size_t index = 0; index + keep < order.size(); ++index) evicted.push_back(order[index].second);
    return evicted;
}

// An INI key for a source: 16 hex digits of FNV-1a over its identity, lowercased
// first because Windows paths are case-insensitive.
inline std::wstring SourceKey(std::wstring_view identity)
{
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : identity) {
        if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
        hash ^= uint64_t(uint16_t(c));
        hash *= 1099511628211ull;
    }
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring key(16, L'0');
    for (int index = 15; index >= 0; --index, hash >>= 4) key[size_t(index)] = digits[hash & 15u];
    return key;
}

} // namespace compare_mask
