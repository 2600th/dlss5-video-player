#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <iterator>

class Localizer {
public:
    struct LanguageEntry { std::wstring code; std::wstring name; };
    void Initialize() {
        m_base = ExeDirectory();
        m_code = ReadConfiguredLanguage();
        if (m_code.empty()) m_code = L"en-US";
        SetLanguage(m_code, false);
    }

    bool SetLanguage(const std::wstring& code, bool persist = true) {
        const std::wstring normalized = NormalizeCode(code);
        m_strings = EnglishDefaults();
        const auto langFile = m_base / L"languages" / (normalized + L".lang");
        LoadUtf8File(langFile, m_strings);
        m_code = normalized;
        if (persist) SaveConfiguredLanguage();
        return true;
    }

    std::wstring Get(const wchar_t* key) const {
        auto it = m_strings.find(key);
        if (it != m_strings.end()) return it->second;
        return key ? std::wstring(key) : std::wstring();
    }

    const std::wstring& Code() const { return m_code; }

    std::vector<LanguageEntry> AvailableLanguages() const {
        std::vector<LanguageEntry> out;
        out.push_back({L"en-US", L"English"});
        std::error_code ec;
        const auto dir = m_base / L"languages";
        if (std::filesystem::is_directory(dir, ec)) {
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (ec || !e.is_regular_file(ec) || e.path().extension() != L".lang") continue;
                std::wstring code = e.path().stem().wstring();
                if (code == L"en-US") continue;
                Map tmp; LoadUtf8File(e.path(), tmp);
                auto it = tmp.find(L"meta.name");
                std::wstring name = (it != tmp.end() && !it->second.empty()) ? it->second : code;
                out.push_back({code, name});
            }
        }
        std::sort(out.begin()+1, out.end(), [](const LanguageEntry& a, const LanguageEntry& b){ return a.name < b.name; });
        return out;
    }

private:
    using Map = std::unordered_map<std::wstring, std::wstring>;

    static std::filesystem::path ExeDirectory() {
        wchar_t p[32768]{};
        DWORD n = GetModuleFileNameW(nullptr, p, static_cast<DWORD>(std::size(p)));
        if (!n || n >= std::size(p)) return std::filesystem::current_path();
        return std::filesystem::path(p).parent_path();
    }

    static std::wstring Utf8ToWide(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) {
            n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            if (n <= 0) return {};
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    static std::string WideToUtf8(const std::wstring& s) {
        if (s.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    static std::string TrimAscii(std::string s) {
        auto ws = [](unsigned char c){ return c==' ' || c=='\t' || c=='\r' || c=='\n'; };
        while (!s.empty() && ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && ws(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    }

    static void LoadUtf8File(const std::filesystem::path& p, Map& dst) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return;
        std::string line;
        bool first = true;
        while (std::getline(f, line)) {
            if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);
            first = false;
            line = TrimAscii(line);
            if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = TrimAscii(line.substr(0, eq));
            std::string v = TrimAscii(line.substr(eq + 1));
            for (size_t pos = 0; (pos = v.find("\\t", pos)) != std::string::npos; ) { v.replace(pos, 2, "\t"); ++pos; }
            if (!k.empty()) dst[Utf8ToWide(k)] = Utf8ToWide(v);
        }
    }

    static std::wstring NormalizeCode(std::wstring code) {
        if (code.empty() || code.size() > 32) return L"en-US";
        std::wstring lower = code;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c){ return static_cast<wchar_t>(towlower(c)); });
        if (lower == L"en" || lower == L"en-us" || lower == L"english") return L"en-US";
        if (lower == L"pt" || lower == L"pt-br" || lower == L"pt_br" || lower == L"portuguese" || lower == L"portugues") return L"pt-BR";
        for (wchar_t c : code) if (!(iswalnum(c) || c == L'-' || c == L'_')) return L"en-US";
        return code;
    }

    std::wstring ReadConfiguredLanguage() const {
        const auto ini = m_base / L"DLSSVideoPlayer.ini";
        wchar_t value[64]{};
        GetPrivateProfileStringW(L"General", L"Language", L"", value, static_cast<DWORD>(std::size(value)), ini.c_str());
        if (value[0]) return NormalizeCode(value);

        // Backward compatibility with V9/V10 flat "Language=..." files.
        std::ifstream f(ini, std::ios::binary);
        if (!f) return L"en-US";
        std::string line;
        while (std::getline(f, line)) {
            line = TrimAscii(line);
            if (line.rfind("Language=", 0) == 0 || line.rfind("language=", 0) == 0)
                return NormalizeCode(Utf8ToWide(TrimAscii(line.substr(line.find('=') + 1))));
        }
        return L"en-US";
    }

    void SaveConfiguredLanguage() const {
        const auto ini = m_base / L"DLSSVideoPlayer.ini";
        WritePrivateProfileStringW(L"General", L"Language", m_code.c_str(), ini.c_str());
    }

    static Map EnglishDefaults() {
        return {
            {L"app.title", L"DLSS Video Player"},
            {L"menu.file", L"File"}, {L"menu.open", L"Open video...\tCtrl+O"}, {L"menu.exit", L"Exit"},
            {L"menu.playback", L"Playback"}, {L"menu.playpause", L"Play / Pause\tSpace   (Overlay: Ctrl+Alt+Space)"}, {L"menu.stop", L"Stop"},
            {L"menu.back10", L"Back 10 s\tLeft"}, {L"menu.forward10", L"Forward 10 s\tRight"}, {L"menu.mute", L"Mute\tM"},
            {L"menu.video", L"Video"}, {L"menu.aspectfit", L"Original aspect ratio (Fit)"}, {L"menu.aspectfill", L"Fill without stretching (Crop)"},
            {L"menu.adjustments", L"Image adjustments...\tCtrl+E   (Overlay: Ctrl+Alt+C)"},
            {L"menu.final", L"Final image\t1"}, {L"menu.input", L"DLSS input\t2"}, {L"menu.mv", L"Motion vectors\t3"},
            {L"menu.depth", L"Depth\t4"}, {L"menu.mask", L"BiasCurrent mask\t5"}, {L"menu.fullscreen", L"Fullscreen\tF11"},
            {L"menu.dlss", L"DLSS"}, {L"menu.dlss_toggle", L"Enable DLSS\tD"}, {L"menu.rehook", L"Recreate NGX / re-hook DLSS 5\tF6"},
            {L"menu.depthmode", L"Estimated / flat depth proxy\tG"}, {L"menu.quality", L"Mode / quality"},
            {L"menu.quality_auto", L"Auto (realtime recommended)"},
            {L"menu.language", L"Language"}, {L"menu.lang_en", L"English"}, {L"menu.lang_pt", L"Portuguese (Brazil)"},
            {L"button.open", L"Open"}, {L"button.pause", L"Pause"}, {L"button.play", L"Play"}, {L"button.stop", L"Stop"},
            {L"button.mute", L"Mute"}, {L"button.sound", L"Sound"}, {L"button.aspect", L"Aspect"}, {L"button.crop", L"Crop"},
            {L"button.rehook", L"Re-hook"}, {L"button.color", L"Color"}, {L"button.full", L"Full"},
            {L"adjustments.title", L"Image adjustments"}, {L"adjustments.brightness", L"Brightness"}, {L"adjustments.contrast", L"Contrast"},
            {L"adjustments.saturation", L"Saturation"}, {L"adjustments.gamma", L"Gamma"}, {L"adjustments.temperature", L"Temperature"},
            {L"adjustments.tint", L"Tint"}, {L"adjustments.note", L"Adjustments are applied after DLSS to the final video image. Debug views remain unmodified."},
            {L"adjustments.reset", L"Reset"}, {L"adjustments.close", L"Close"},
            {L"idle.title", L"Drop a video here"}, {L"idle.subtitle", L"or open a file to start playback with DLSS"}, {L"idle.open", L"Open video..."},
            {L"dialog.title", L"Open video"}, {L"dialog.all_ffmpeg", L"All files (FFmpeg auto-detect)"}, {L"dialog.supported", L"Common video files"}, {L"dialog.all", L"All files"},
            {L"error.decode", L"Could not open or decode this video. See DLSSVideoPlayer.log for details."},
            {L"error.renderer", L"Could not initialize D3D12/NGX. See DLSSVideoPlayer.log for details."},
            {L"error.frame", L"The file opened, but no video frame could be decoded."},
            {L"error.seek", L"Could not seek to the requested position."},
            {L"status.muted", L"Muted"}, {L"status.volume", L"Vol"}, {L"status.seeking", L"Seeking..."}
        };
    }

    std::filesystem::path m_base;
    std::wstring m_code = L"en-US";
    Map m_strings = EnglishDefaults();
};
