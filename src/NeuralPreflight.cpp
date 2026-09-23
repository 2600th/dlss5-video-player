#include "NeuralPreflight.h"

#include "NeuralCache.h"
#include "Utf8Text.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")

namespace {

// The twelve lock-pinned vendor modules first, then the worker, so the pinned
// set is this array's leading twelve entries and neither list can drift from
// the other.
constexpr std::array<std::wstring_view, 13> kLockedRuntimeFiles{
    L"nvngx_dlssnr.dll", L"nvngx_dlss.dll", L"dxgi.dll", L"renodx-dlss5.addon64", L"sl.common.dll",
    L"sl.dlss.dll", L"sl.dlss_g.dll", L"sl.dlss_nr.dll", L"sl.interposer.dll", L"sl.nis.dll", L"sl.pcl.dll",
    L"sl.reflex.dll", L"NeuralWorker.exe"};
constexpr size_t kLockPinnedRuntimeFileCount = 12;

// The NGX core registration the display driver writes; NGX resolves the same
// value through NGXGetPathUsingQAI, and on this machine it reads
// C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispsi.inf_amd64_<id>.
constexpr const wchar_t* kNgxCoreKey = L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore";

// The model store is over a gigabyte of weight blobs on this machine's driver
// (measure it at %ProgramData%\NVIDIA\NGX\models), and this digest is computed
// on the path that answers a cache lookup, so hashing every byte per request
// is not affordable. Files at or below this bound - every config, mapping,
// deny list and Streamline module, which is what decides *which* weights load
// - are hashed byte for byte; the blobs above it are identified by relative
// name, size and write time, the same identity Sha256FileCached already
// trusts for installation files. A refresh writes new version directories and
// new blob names, so it moves the listing either way.
constexpr uintmax_t kModelContentHashLimit = 1u << 20;

// Returns the text between `prefix` and the first `terminator` character
// after it, searched on the original (case-preserved) log.
std::string Between(std::string_view text, std::string_view prefix, std::string_view terminators)
{
    const size_t start = text.find(prefix);
    if (start == std::string_view::npos) return {};
    const size_t valueStart = start + prefix.size();
    const size_t end = text.find_first_of(terminators, valueStart);
    return std::string(text.substr(valueStart, end == std::string_view::npos ? std::string_view::npos : end - valueStart));
}

std::string Trim(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
    size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
    return value.substr(first);
}

std::string_view LineContaining(std::string_view text, size_t position)
{
    const size_t lineStart = text.rfind('\n', position);
    const size_t lineEnd = text.find('\n', position);
    const size_t begin = lineStart == std::string_view::npos ? 0 : lineStart + 1;
    return text.substr(begin, (lineEnd == std::string_view::npos ? text.size() : lineEnd) - begin);
}

std::string LowerAscii(std::string_view value)
{
    std::string result(value);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') character = char(character - 'A' + 'a');
    }
    return result;
}

// Lower-cased input only: every caller scans a LowerAscii copy of the log.
int HexNibble(char character) noexcept
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

std::wstring FileVersionText(const std::filesystem::path& path)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return {};
    std::vector<std::byte> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoBytes = 0;
    if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &infoBytes) || !info ||
        infoBytes < sizeof(VS_FIXEDFILEINFO)) return {};
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L'.' + std::to_wstring(LOWORD(info->dwFileVersionMS)) +
           L'.' + std::to_wstring(HIWORD(info->dwFileVersionLS)) + L'.' + std::to_wstring(LOWORD(info->dwFileVersionLS));
}

std::wstring LowerWide(std::wstring value)
{
    std::ranges::transform(value, value.begin(), towlower);
    return value;
}

// The NGX core directory the driver registers, or nothing when this machine
// has no NVIDIA display driver installed.
std::filesystem::path RegisteredNgxCoreDirectory()
{
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kNgxCoreKey, L"FullPath", RRF_RT_REG_SZ, nullptr, nullptr,
                     &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kNgxCoreKey, L"FullPath", RRF_RT_REG_SZ, nullptr,
                     value.data(), &bytes) != ERROR_SUCCESS) return {};
    if (const size_t terminator = value.find(L'\0'); terminator != std::wstring::npos)
        value.resize(terminator);
    if (value.empty()) return {};
    return std::filesystem::path(value);
}

std::filesystem::path NgxModelStoreDirectory()
{
    PWSTR programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)) || !programData)
        return {};
    std::filesystem::path models = std::filesystem::path(programData) / L"NVIDIA" / L"NGX" / L"models";
    CoTaskMemFree(programData);
    return models;
}

struct ModelStoreFile {
    std::wstring relative;
    uintmax_t size{};
    int64_t writeTime{};
    std::string digest;  // empty above kModelContentHashLimit or when unreadable
    bool unreadable{};   // size, write time or (within the bound) content missing
    // Written inside the quiet period before the read, and how much of it is
    // left. See NeuralModelStore::recentlyWrittenFiles.
    bool recent{};
    std::chrono::milliseconds settlesIn{};
};

// Which NGX features the neural pass evaluates, as the model store names them.
//
// The store is organised by feature - models/<feature>/versions/<n>/files/...
// under %ProgramData%\NVIDIA\NGX - and nvngx_config.txt and the server config
// (config/versions/<n>/files/nvngx_server_config.txt) carry one [<feature>]
// section each. Listed on the development machine (2026-09-23, 185 files):
// dlss, dlss_override, dlssd, dlssg, dlisp,
// config, and sl_<plugin>_0 / sl_<plugin>_override_0 for common, sdk, dlss,
// dlss_d, dlss_g, nis, reflex, pcl, nvperf and deepdvc; the server config
// adds sections for dlisr, dlslowmo, dlinpainting, dlvsr, vsr, truehdr,
// nvbroadcast*, nvbcast* and rise*. There is no NR (dlssnr) directory: the
// locked runtime evaluates feature 18 out of neural-runtime\nvngx_dlssnr.dll,
// which runtimeDigest already covers.
//
// The walk took all of it, so the NVIDIA App refreshing Ray Reconstruction or
// Frame Generation weights - 463 MB and 51 MB of blobs here - changed every
// render key and orphaned the whole cache. What the pass does evaluate is the
// player's own DLAA (feature 1, SuperSampling: dlss, and dlss_override, which
// is how the NVIDIA App substitutes those weights), feature 18 (a future
// dlssnr OTA directory, and the Streamline NR plugin) and the Streamline core
// the add-on loads (sl_common, sl_sdk, sl_dlss). So this is a deny list of
// the features that are certainly not in the pass - denoising, frame
// generation, sharpening, latency, vibrance, profiling, and the non-game
// features - and everything else stays in: a feature this list has not heard
// of is kept, because leaving a pixel-relevant file out of the key serves a
// stale render, while keeping an irrelevant one only costs a re-render.
bool OutsideNeuralPass(std::wstring_view feature)
{
    static constexpr std::array<std::wstring_view, 9> kFeatures{
        L"dlssd", L"dlssg", L"dlisp", L"dlisr", L"dlslowmo", L"dlinpainting", L"dlvsr", L"vsr",
        L"truehdr"};
    static constexpr std::array<std::wstring_view, 10> kFamilies{
        L"sl_dlss_d_", L"sl_dlss_g_", L"sl_nis_", L"sl_reflex_", L"sl_pcl_", L"sl_nvperf_",
        L"sl_deepdvc_", L"nvbroadcast", L"nvbcast", L"rise"};
    return std::ranges::find(kFeatures, feature) != kFeatures.end() ||
           std::ranges::any_of(kFamilies, [&](std::wstring_view family) {
               return feature.starts_with(family);
           });
}

// The two selector files hold a section per feature, so a refresh of any
// feature rewrote them - the same orphaning through the back door. Their
// sections for features outside the pass are dropped before hashing; every
// other line, the ones before the first section included, is kept byte for
// byte.
bool SectionedSelector(std::wstring_view lowerName)
{
    return lowerName == L"nvngx_config.txt" || lowerName == L"nvngx_server_config.txt";
}

std::string KeepNeuralPassSections(std::string_view text)
{
    std::string kept;
    bool keep = true;
    while (!text.empty()) {
        const size_t newline = text.find('\n');
        const std::string_view line =
            text.substr(0, newline == std::string_view::npos ? text.size() : newline + 1);
        text.remove_prefix(line.size());
        std::string_view trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' ||
                                    trimmed.back() == ' ' || trimmed.back() == '\t'))
            trimmed.remove_suffix(1);
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            std::wstring section;
            for (const char character : trimmed.substr(1, trimmed.size() - 2))
                section.push_back(static_cast<wchar_t>(std::towlower(static_cast<unsigned char>(character))));
            keep = !OutsideNeuralPass(section);
        }
        if (keep) kept.append(line);
    }
    return kept;
}

std::optional<std::string> HashNeuralPassSelector(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return std::nullopt;
    return Sha256Bytes(KeepNeuralPassSections(bytes));
}

// One root's listing, or nothing when the root cannot be walked whole: a
// partial listing would digest as though the files it missed did not exist.
std::optional<std::vector<ModelStoreFile>> CollectModelRoot(const NeuralModelRoot& root,
                                                            std::stop_token stop,
                                                            std::chrono::milliseconds quietPeriod)
{
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory(root.directory, error) || error) return std::nullopt;
    const std::wstring prefix = LowerWide(std::wstring(root.namePrefix));
    std::vector<ModelStoreFile> files;
    const auto consider = [&](const fs::directory_entry& entry) {
        std::error_code local;
        if (!entry.is_regular_file(local) || local) return;
        if (!prefix.empty() && !LowerWide(entry.path().filename().wstring()).starts_with(prefix)) return;
        ModelStoreFile file;
        file.relative = LowerWide(entry.path().lexically_relative(root.directory).generic_wstring());
        const uintmax_t size = entry.file_size(local);
        const bool sized = !local;
        if (sized) file.size = size;
        else file.unreadable = true;
        local.clear();
        const auto written = entry.last_write_time(local);
        if (!local) {
            file.writeTime = written.time_since_epoch().count();
            // Read the clock per file: hashing the ones before it takes time,
            // and a write that lands meanwhile is the one this is looking for.
            // A time more than a second ahead of the clock is not a write in
            // progress but a skewed stamp, which would otherwise hold the
            // store unsettled for good.
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::filesystem::file_time_type::clock::now() - written);
            if (age > -std::chrono::seconds(1) && age < quietPeriod) {
                file.recent = true;
                file.settlesIn = quietPeriod - std::max(age, std::chrono::milliseconds{0});
            }
        } else {
            file.unreadable = true;
        }
        if (sized && size <= kModelContentHashLimit) {
            // Uncached on purpose. Sha256FileCached keys on (path, size, write
            // time), and Windows write times move in ~15 ms ticks, so a small
            // selector file replaced in place at the same size inside one tick
            // reuses the previous digest. That is tolerable for installation
            // files, which this process does not edit, and not tolerable for a
            // term of the render identity: a stale digest here serves a render
            // built against weights that are no longer the ones on disk. Only
            // files at or below the bound reach this, so the re-read is small.
            const bool selector =
                SectionedSelector(LowerWide(entry.path().filename().wstring()));
            if (const auto digest = selector ? HashNeuralPassSelector(entry.path())
                                             : Sha256File(entry.path(), stop))
                file.digest = *digest;
            else file.unreadable = true;
        }
        files.push_back(std::move(file));
    };
    constexpr auto options = fs::directory_options::skip_permission_denied;
    if (root.recursive) {
        for (fs::recursive_directory_iterator iterator(root.directory, options, error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (stop.stop_requested()) return std::nullopt;
            // A feature directory the pass never evaluates is not walked at
            // all (see OutsideNeuralPass).
            std::error_code local;
            if (iterator.depth() == 0 && iterator->is_directory(local) && !local &&
                OutsideNeuralPass(LowerWide(iterator->path().filename().wstring()))) {
                iterator.disable_recursion_pending();
                continue;
            }
            consider(*iterator);
        }
    } else {
        for (fs::directory_iterator iterator(root.directory, options, error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (stop.stop_requested()) return std::nullopt;
            consider(*iterator);
        }
    }
    if (error) return std::nullopt;
    std::ranges::sort(files, {}, &ModelStoreFile::relative);
    return files;
}

} // namespace

std::span<const std::wstring_view> LockedRuntimeFileNames()
{
    return kLockedRuntimeFiles;
}

std::span<const std::wstring_view> LockPinnedRuntimeFileNames()
{
    return std::span<const std::wstring_view>(kLockedRuntimeFiles).first(kLockPinnedRuntimeFileCount);
}

const char* NeuralModelStoreSourceName(NeuralModelStoreSource source) noexcept
{
    const size_t index = static_cast<size_t>(source);
    // Every entry is a string literal, so data() is null-terminated.
    return index < kNeuralModelStoreSourceNames.size() ? kNeuralModelStoreSourceNames[index].data()
                                                       : "driverVersion";
}

std::vector<NeuralModelRoot> RegisteredNeuralModelRoots()
{
    std::vector<NeuralModelRoot> roots;
    // The driver store holds the whole display driver, so only the NGX modules
    // beside the registered core are listed; the model store is ours to walk.
    if (std::filesystem::path core = RegisteredNgxCoreDirectory(); !core.empty())
        roots.push_back(NeuralModelRoot{std::move(core), false, L"nvngx"});
    if (std::filesystem::path models = NgxModelStoreDirectory(); !models.empty())
        roots.push_back(NeuralModelRoot{std::move(models), true, {}});
    return roots;
}

NeuralModelStore DigestNeuralModelStore(std::span<const NeuralModelRoot> roots,
                                        std::wstring_view driverVersion,
                                        std::stop_token stop,
                                        std::chrono::milliseconds quietPeriod)
{
    NeuralModelStore store;
    // Canonical form 2: form 1 listed every feature, and the size and write
    // time of files it had already hashed. The root spelling is part of it: a
    // driver update lands a new nv_dispsi.inf_<id> directory, so the name
    // alone retires the renders made against the old one even before its
    // contents are compared. The no-root fallback below keeps form 1, which
    // this change did not alter.
    std::string canonical = "model-store=2\n";
    for (const NeuralModelRoot& root : roots) {
        const std::wstring spelling = LowerWide(root.directory.generic_wstring());
        const auto files = CollectModelRoot(root, stop, quietPeriod);
        canonical += "root=" + utf8_text::FromWide(spelling);
        if (!files) {
            // An unreadable root still belongs in the digest: a machine that
            // grows one later must not answer with the key it used without it.
            canonical += "|unavailable\n";
            // A root that is not there reads the same way every time; one that
            // is there and could not be walked may read whole next time.
            std::error_code existsError;
            if (std::filesystem::exists(root.directory, existsError) || existsError)
                ++store.unavailableRoots;
            if (!store.fallbackDetail.empty()) store.fallbackDetail += L"; ";
            store.fallbackDetail += spelling + L" could not be enumerated";
            continue;
        }
        canonical += '\n';
        store.enumeratedRoots.push_back(spelling);
        for (const ModelStoreFile& file : *files) {
            // A hashed file is its content and nothing else. The NVIDIA App
            // rewrites the config files in place with unchanged bytes - their
            // write times on the reference machine are the time of its last
            // check - and each rewrite moved every render key. Size and write
            // time identify only the blobs too large to hash.
            canonical += utf8_text::FromWide(file.relative);
            canonical.push_back('\0');
            if (!file.digest.empty()) {
                canonical += file.digest;
            } else {
                canonical += std::to_string(file.size);
                canonical.push_back('\0');
                canonical += std::to_string(file.writeTime);
                canonical += '\0';
                canonical += '-';
            }
            canonical.push_back('\n');
            ++store.files;
            if (!file.digest.empty()) ++store.contentHashedFiles;
            if (file.unreadable) ++store.unreadableFiles;
            if (file.recent) {
                ++store.recentlyWrittenFiles;
                store.settlesIn = std::max(store.settlesIn, file.settlesIn);
            }
            store.bytes += file.size;
        }
    }
    if (store.enumeratedRoots.empty()) {
        // The cheap fallback, and the only one available: with no root to read,
        // the driver version is all that still moves when the weights do.
        store.source = NeuralModelStoreSource::DriverVersion;
        canonical = "model-store=1\ndriver=" + utf8_text::FromWide(driverVersion) + "\n";
        if (store.fallbackDetail.empty())
            store.fallbackDetail = L"No NGX model root is registered on this machine.";
    } else {
        store.source = NeuralModelStoreSource::ModelContents;
    }
    store.digest = Sha256Bytes(canonical).value_or(std::string{});
    return store;
}

bool NeuralModelStoreSettled(const NeuralModelStore& store)
{
    return store.digest.size() == 64 && store.unavailableRoots == 0 && store.unreadableFiles == 0 &&
           store.recentlyWrittenFiles == 0;
}

bool NeuralModelStoresAgree(const NeuralModelStore& first, const NeuralModelStore& second)
{
    return NeuralModelStoreSettled(first) && NeuralModelStoreSettled(second) && first.digest == second.digest;
}

// A read that catches NGX mid-rewrite is not an error to report but a moment to
// wait out: the rewrite ends within a second or two, and the digest the render
// is keyed under must be the store's, not the moment's. Measured on the
// development machine: the player's own NGX initialisation truncated
// config/versions/2/files/nvngx_server_config.txt 0.7 s before its render key
// was digested, and the key carried a69cdc79... - exactly the store with that
// one file empty - instead of 59792fa7... A render made under it could never
// be looked up again, and the next start's eviction deleted it.
NeuralModelStore DigestSettledNeuralModelStore(std::span<const NeuralModelRoot> roots,
                                               std::wstring_view driverVersion,
                                               std::stop_token stop,
                                               std::chrono::milliseconds quietPeriod,
                                               std::chrono::milliseconds patience)
{
    const auto deadline = std::chrono::steady_clock::now() + patience;
    for (;;) {
        NeuralModelStore store = DigestNeuralModelStore(roots, driverVersion, stop, quietPeriod);
        if (store.recentlyWrittenFiles == 0 || stop.stop_requested()) return store;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return store;
        // A little past the youngest file's quiet period, so the next read
        // does not land on its last millisecond; never past the deadline.
        auto wait = std::min<std::chrono::steady_clock::duration>(
            store.settlesIn + std::chrono::milliseconds(50), deadline - now);
        while (wait > std::chrono::steady_clock::duration::zero() && !stop.stop_requested()) {
            const auto slice = std::min<std::chrono::steady_clock::duration>(wait, std::chrono::milliseconds(50));
            std::this_thread::sleep_for(slice);
            wait -= slice;
        }
        if (stop.stop_requested()) return store;
    }
}

NeuralModelStore ResolveNeuralModelStore(std::wstring_view driverVersion, std::stop_token stop,
                                         std::chrono::milliseconds patience)
{
    const std::vector<NeuralModelRoot> roots = RegisteredNeuralModelRoots();
    return DigestSettledNeuralModelStore(roots, driverVersion, std::move(stop), kModelStoreQuietPeriod,
                                         patience);
}

NeuralRuntimeBanner ParseNeuralRuntimeBanner(std::string_view reshadeLog)
{
    NeuralRuntimeBanner banner;
    banner.reshadeVersion = Between(reshadeLog, "ReShade version '", "'");
    const size_t addon = reshadeLog.find("Registered add-on \"DLSS 5 Neural Rendering\" v");
    if (addon != std::string_view::npos) {
        const std::string_view line = LineContaining(reshadeLog, addon);
        banner.addonVersion = Between(line, "\" v", " ");
        banner.addonApiVersion = Trim(Between(line, "ReShade API version ", ".\r\n"));
    }
    const size_t renodx = reshadeLog.find("RenoDX DLSS5 Generic v");
    if (renodx != std::string_view::npos) {
        const std::string_view line = LineContaining(reshadeLog, renodx);
        banner.renodxVersion = Between(line, "RenoDX DLSS5 Generic v", " ");
        banner.renodxBuild = Between(line, "(build ", ")");
    }
    const size_t dlssnr = reshadeLog.find("DLSSNR ");
    if (dlssnr != std::string_view::npos) {
        const std::string_view line = LineContaining(reshadeLog, dlssnr);
        const std::string value = Between(line, "DLSSNR ", " ");
        if (!value.empty() && value[0] >= '0' && value[0] <= '9') banner.dlssnrRuntime = value;
    }
    const size_t active = reshadeLog.find("active settings: ");
    if (active != std::string_view::npos) {
        const std::string_view line = LineContaining(reshadeLog, active);
        banner.activeSettings = Trim(Between(line, "active settings: ", "\r\n"));
    }
    return banner;
}

std::vector<Feature18Observation> CollectFeature18Observations(std::string_view reshadeLogSegment)
{
    constexpr std::array<std::string_view, 7> markers{
        "feature 18 created", "feature 18 create failed", "feature 18 evaluation succeeded",
        "feature 18 evaluation failed", "feature 18 evaluate raised an exception", "nr skipped:",
        "nr declined an evaluate:"};
    std::vector<Feature18Observation> observations;
    const std::string lower = LowerAscii(reshadeLogSegment);
    size_t lineStart = 0;
    while (lineStart < lower.size()) {
        size_t lineEnd = lower.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = lower.size();
        const std::string_view line(lower.data() + lineStart, lineEnd - lineStart);
        for (const std::string_view marker : markers) {
            if (line.find(marker) == std::string_view::npos) continue;
            Feature18Observation observation;
            observation.line = Trim(std::string(reshadeLogSegment.substr(lineStart, lineEnd - lineStart)));
            observation.failure = marker != "feature 18 created" && marker != "feature 18 evaluation succeeded";
            observations.push_back(std::move(observation));
            break;
        }
        lineStart = lineEnd + 1;
    }
    return observations;
}

std::optional<uint32_t> ParseFeature18CreateResult(std::span<const Feature18Observation> observations)
{
    constexpr std::string_view marker = "feature 18 create failed with 0x";
    std::optional<uint32_t> result;
    for (const Feature18Observation& observation : observations) {
        const std::string line = LowerAscii(observation.line);
        for (size_t at = line.find(marker); at != std::string::npos; at = line.find(marker, at + marker.size())) {
            const size_t start = at + marker.size();
            size_t end = start;
            while (end < line.size() && HexNibble(line[end]) >= 0) ++end;
            // A 32-bit result and nothing else: an empty or over-long run is
            // some other number that happens to follow the marker.
            if (end == start || end - start > 8) continue;
            uint32_t value = 0;
            for (size_t index = start; index < end; ++index) {
                value = (value << 4) | static_cast<uint32_t>(HexNibble(line[index]));
            }
            result = value;
        }
    }
    return result;
}

const char* NeuralPreflightCauseName(NeuralPreflightCause cause) noexcept
{
    const size_t index = static_cast<size_t>(cause);
    // Every entry is a string literal, so data() is null-terminated.
    return index < kNeuralPreflightCauseNames.size() ? kNeuralPreflightCauseNames[index].data() : "none";
}

NeuralPreflightDiagnosis DiagnoseNeuralPreflight(const DetectedGpu& gpu,
                                                 std::span<const Feature18Observation> observations,
                                                 bool created,
                                                 bool evidenceValid,
                                                 std::wstring_view probeFailure)
{
    constexpr uint32_t kFeatureNotSupported = 0xbad00001u;  // NVSDK_NGX_Result_FAIL_FeatureNotSupported
    constexpr uint32_t kPlatformError = 0xbad00002u;        // NVSDK_NGX_Result_FAIL_PlatformError
    constexpr uint32_t kOutOfGpuMemory = 0xbad0000du;       // NVSDK_NGX_Result_FAIL_OutOfGPUMemory

    NeuralPreflightDiagnosis diagnosis;
    diagnosis.ngxResult = ParseFeature18CreateResult(observations);
    // Nothing refused the feature in the end: a code from a create the runtime
    // went on to satisfy is history, not a verdict.
    if (created && evidenceValid && probeFailure.empty()) return diagnosis;

    // ClassifyNeuralDriver, with the parsed number kept for the message.
    const std::optional<NvidiaDriverVersion> driver = ParseNvidiaDriverVersion(gpu.driverVersion);
    const bool belowFloor = driver && *driver < kNeuralDriverFloor;
    const std::wstring recommended = FormatNvidiaDriverVersion(kNeuralDriverRecommended);
    const auto blameDriver = [&] {
        diagnosis.cause = NeuralPreflightCause::DriverBelowFloor;
        diagnosis.detail = L"NVIDIA driver " + FormatNvidiaDriverVersion(*driver) + L" is below the " +
                           FormatNvidiaDriverVersion(kNeuralDriverFloor) +
                           L" minimum for neural rendering. Update to " + recommended + L" or newer, then try again.";
    };

    if (diagnosis.ngxResult == kFeatureNotSupported) {
        diagnosis.cause = NeuralPreflightCause::ArchitectureUnsupported;
        diagnosis.detail = L"The neural runtime refused feature 18 with " + HexResultTextWide(kFeatureNotSupported) +
                           L" (feature not supported): this GPU architecture is not served by the neural runtime, "
                           L"so neural rendering is unavailable on it.";
    } else if (diagnosis.ngxResult == kOutOfGpuMemory) {
        diagnosis.cause = NeuralPreflightCause::OutOfVideoMemory;
        diagnosis.detail = L"The neural runtime ran out of GPU memory creating feature 18 (" +
                           HexResultTextWide(kOutOfGpuMemory) +
                           L"). Choose a lower source resolution or close other GPU applications, then try again.";
    } else if (diagnosis.ngxResult == kPlatformError) {
        // The driver's NGX core, not the runtime, declined to service the
        // feature. An out-of-date driver is the cause that can be acted on.
        if (belowFloor) {
            blameDriver();
        } else {
            diagnosis.cause = NeuralPreflightCause::PlatformRefusal;
            diagnosis.detail = L"The neural runtime refused feature 18 with " + HexResultTextWide(kPlatformError) +
                               L" (platform error). Update the NVIDIA driver to " + recommended +
                               L" or newer and close other DLSS injectors or overlays, then try again.";
        }
    } else if (belowFloor) {
        blameDriver();
    } else if (!probeFailure.empty()) {
        diagnosis.cause = NeuralPreflightCause::ProbeFailed;
        diagnosis.detail = probeFailure;
    } else {
        // The early return above leaves only an incomplete evidence chain.
        diagnosis.cause = NeuralPreflightCause::EvidenceIncomplete;
        diagnosis.detail = L"The neural runtime did not arm the inline interception contract for feature 18. "
                           L"Close other DLSS injectors or overlays and try again; if it repeats, reinstall the "
                           L"neural runtime.";
    }
    return diagnosis;
}

std::vector<RuntimeModuleReceipt> DescribeRuntimeModules(const std::filesystem::path& directory,
                                                         std::span<const std::wstring_view> names)
{
    std::vector<RuntimeModuleReceipt> modules;
    modules.reserve(names.size());
    for (const std::wstring_view name : names) {
        RuntimeModuleReceipt module;
        module.name = std::wstring(name);
        const std::filesystem::path path = directory / name;
        std::error_code error;
        module.present = std::filesystem::is_regular_file(path, error) && !error;
        if (module.present) {
            module.sizeBytes = std::filesystem::file_size(path, error);
            if (error) module.sizeBytes = 0;
            module.fileVersion = FileVersionText(path);
            if (const auto digest = Sha256FileCached(path)) module.sha256 = *digest;
        }
        modules.push_back(std::move(module));
    }
    return modules;
}

std::string JsonEscapeWide(std::wstring_view text)
{
    return JsonEscape(utf8_text::FromWide(text));
}

std::string ReportedFeature18ObservationsJson(std::span<const Feature18Observation> observations)
{
    const size_t count = observations.size();
    const bool trimmed = count > kReportedObservationsPerEnd * 2;
    std::string json = "\"observations\":[";
    bool first = true;
    for (size_t index = 0; index < count; ++index) {
        if (trimmed && index == kReportedObservationsPerEnd) index = count - kReportedObservationsPerEnd;
        const Feature18Observation& observation = observations[index];
        std::string_view line = observation.line;
        std::string cut;
        if (line.size() > kReportedObservationLineBytes) {
            size_t end = kReportedObservationLineBytes;
            while (end && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80) --end;
            cut.assign(line.substr(0, end));
            cut += "...";
            line = cut;
        }
        if (!first) json += ',';
        first = false;
        json += "{\"failure\":" + std::string(observation.failure ? "true" : "false") + ",\"line\":\"" +
                JsonEscape(line) + "\"}";
    }
    json += "]";
    if (trimmed) json += ",\"observationsOmitted\":" + std::to_string(count - kReportedObservationsPerEnd * 2);
    return json;
}

std::string NeuralModelStoreJson(const NeuralModelStore& store)
{
    std::string json = "{\"source\":\"";
    json += NeuralModelStoreSourceName(store.source);
    json += "\",\"digest\":\"" + JsonEscape(store.digest) +
            "\",\"files\":" + std::to_string(store.files) +
            ",\"contentHashedFiles\":" + std::to_string(store.contentHashedFiles) +
            ",\"bytes\":" + std::to_string(store.bytes) + ",\"roots\":[";
    for (size_t index = 0; index < store.enumeratedRoots.size(); ++index) {
        if (index) json += ',';
        json += "\"" + JsonEscapeWide(store.enumeratedRoots[index]) + "\"";
    }
    json += "],\"fallback\":\"" + JsonEscapeWide(store.fallbackDetail) + "\",\"settled\":" +
            std::string(NeuralModelStoreSettled(store) ? "true" : "false") + "}";
    return json;
}

std::string BuildPreflightFailureJson(std::wstring_view detail)
{
    // The diagnosis is what the parent reads its message from
    // (ScanReceiptDiagnosis). Without one, a helper that refused to start -
    // an add-on the runtime lock does not name, a configuration it could not
    // enable - reached the user as "did not arm feature 18" and the reason
    // stayed in the helper's log.
    const std::string escaped = JsonEscapeWide(detail);
    return "{\"schema\":3,\"ok\":false,\"diagnosis\":{\"cause\":\"" +
           std::string(NeuralPreflightCauseName(NeuralPreflightCause::ProbeFailed)) + "\",\"detail\":\"" +
           escaped + "\"},\"error\":\"" + escaped + "\"}";
}

