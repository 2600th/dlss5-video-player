#pragma once

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "AppMenu.h"
#include "Utf8Text.h"

// The player's keys, as one table: what WM_KEYDOWN runs, what every menu row
// prints in its key column, and therefore what the shortcut sheet lists. The
// defaults are the keys the player has always had. A plain-text file beside the
// ini, input.conf, changes them in the style of mpv's file of the same name:
//
//     # KEY        COMMAND
//     Ctrl+P       picture
//     F            ignore        # F no longer previews the frame
//
// A line binds one key to one command, replacing whatever that key did; the
// command keeps its other keys. `ignore` unbinds a key. A line the file gets
// wrong - an unknown command, a key with no name, a key the player keeps for
// itself - is reported in the log and skipped, never fatal: a typo costs that
// one line, not the keyboard. Tab, Enter, Esc and F10 stay what they are,
// because they are how the player is driven without a mouse, and Alt opens the
// menu bar. Punctuation is named by its US-layout key ([ is the key right of
// P), which is what the player has always matched.
namespace key_bindings {

// A key as pressed: a virtual key and its modifiers, or a character for the one
// shortcut that is a character on every layout but a key on none (?).
struct Chord {
    UINT key = 0;
    bool control = false;
    bool shift = false;
    wchar_t character = 0;

    friend bool operator==(const Chord&, const Chord&) = default;
};

// The names a file uses for commands. Stable: a name, once shipped, keeps its
// meaning, whatever the menu calls the row.
struct Command {
    std::wstring_view name;
    UINT id;
};

inline constexpr Command kCommands[] = {
    // File
    {L"open", app_menu::IDM_OPEN}, {L"open-youtube", app_menu::IDM_OPEN_YOUTUBE},
    {L"export", app_menu::IDM_EXPORT_STAGES}, {L"save-rendered", app_menu::IDM_EXPORT_CACHED_VIDEO},
    {L"save-comparison", app_menu::IDM_SAVE_COMPARISON_IMAGE}, {L"show-last-export", app_menu::IDM_SHOW_FRAMEGEN_OUTPUT},
    {L"exit", app_menu::IDM_EXIT},
    // Playback
    {L"play-pause", app_menu::IDM_PLAY}, {L"stop", app_menu::IDM_STOP},
    {L"seek-back", app_menu::IDM_BACK10}, {L"seek-forward", app_menu::IDM_FWD10},
    {L"step-frame", app_menu::IDM_STEP_FRAME}, {L"goto-timecode", app_menu::IDM_GOTO_TIMECODE},
    {L"mute", app_menu::IDM_MUTE}, {L"audio-passthrough", app_menu::IDM_AUDIO_PASSTHROUGH},
    {L"subtitles-off", app_menu::IDM_SUBTITLE_OFF}, {L"subtitles-next", app_menu::IDM_SUBTITLE_NEXT},
    {L"subtitles-load", app_menu::IDM_SUBTITLE_LOAD}, {L"subtitles-earlier", app_menu::IDM_SUBTITLE_EARLIER},
    {L"subtitles-later", app_menu::IDM_SUBTITLE_LATER}, {L"subtitles-reset-delay", app_menu::IDM_SUBTITLE_DELAY_RESET},
    {L"mark-in", app_menu::IDM_MARK_IN}, {L"mark-out", app_menu::IDM_MARK_OUT}, {L"clear-marks", app_menu::IDM_CLEAR_MARKS},
    {L"next-file", app_menu::IDM_NEXT_FILE}, {L"previous-file", app_menu::IDM_PREVIOUS_FILE},
    {L"resume-toggle", app_menu::IDM_RESUME_POSITION},
    // View
    {L"aspect-toggle", app_menu::IDM_ASPECT_TOGGLE}, {L"aspect-fit", app_menu::IDM_ASPECT_FIT},
    {L"aspect-fill", app_menu::IDM_ASPECT_FILL}, {L"aspect-pixels", app_menu::IDM_ASPECT_ONE_TO_ONE},
    {L"fullscreen", app_menu::IDM_FULLSCREEN}, {L"viewing-adjustments", app_menu::IDM_VIDEO_ADJUSTMENTS},
    {L"debug-final", app_menu::IDM_VIEW_FINAL}, {L"debug-input", app_menu::IDM_VIEW_INPUT},
    {L"debug-motion", app_menu::IDM_VIEW_MV}, {L"debug-depth", app_menu::IDM_VIEW_DEPTH},
    // Compare
    {L"compare-dlss5", app_menu::IDM_COMPARE_NEURAL}, {L"compare-original", app_menu::IDM_COMPARE_ORIGINAL},
    {L"compare-vsr", app_menu::IDM_COMPARE_VSR}, {L"compare-split", app_menu::IDM_COMPARE_SPLIT},
    {L"compare-wipe", app_menu::IDM_COMPARE_WIPE}, {L"compare-difference", app_menu::IDM_COMPARE_DIFFERENCE},
    {L"compare-side-by-side", app_menu::IDM_COMPARE_SIDE_BY_SIDE}, {L"compare-quad", app_menu::IDM_COMPARE_QUAD},
    {L"compare-next", app_menu::IDM_COMPARE_NEXT_MODE}, {L"compare-previous", app_menu::IDM_COMPARE_PREVIOUS_MODE},
    {L"mix-less", app_menu::IDM_COMPARE_BLEND_LESS}, {L"mix-more", app_menu::IDM_COMPARE_BLEND_MORE},
    {L"swap", app_menu::IDM_COMPARE_SWAP}, {L"zoom-in", app_menu::IDM_COMPARE_ZOOM},
    {L"zoom-out", app_menu::IDM_COMPARE_ZOOM_OUT}, {L"zoom-fit", app_menu::IDM_COMPARE_ZOOM_FIT},
    {L"loupe", app_menu::IDM_COMPARE_LOUPE}, {L"compare-against-vsr", app_menu::IDM_COMPARE_AGAINST_VSR},
    {L"difference-less", app_menu::IDM_COMPARE_DIFFERENCE_LESS}, {L"difference-more", app_menu::IDM_COMPARE_DIFFERENCE_MORE},
    {L"difference-luma", app_menu::IDM_COMPARE_DIFFERENCE_LUMA}, {L"mask-load", app_menu::IDM_COMPARE_MASK_LOAD},
    {L"mask-invert", app_menu::IDM_COMPARE_MASK_INVERT}, {L"mask-clear", app_menu::IDM_COMPARE_MASK_CLEAR},
    {L"compare-hdr-at-sdr", app_menu::IDM_COMPARE_HDR_AT_SDR},
    // Enhance
    {L"dlss5", app_menu::IDM_NEURAL_RENDERING}, {L"picture", app_menu::IDM_NEURAL_SETTINGS},
    {L"pause-render", app_menu::IDM_PAUSE_NEURAL_RENDER}, {L"preview-frame", app_menu::IDM_PREVIEW_FRAME},
    {L"preview-clip", app_menu::IDM_PREVIEW_CLIP}, {L"rerender", app_menu::IDM_RERENDER_HERE},
    {L"render-range", app_menu::IDM_RENDER_RANGE}, {L"render-whole", app_menu::IDM_RENDER_WHOLE},
    {L"upscaler-vsr", app_menu::IDM_RTX_VSR_UPSCALING}, {L"upscaler-dlss-sr", app_menu::IDM_DLSS_UPSCALING},
    {L"upscaler-none", app_menu::IDM_UPSCALER_NONE}, {L"generate-frames", app_menu::IDM_FRAME_GENERATION},
    // Tools
    {L"cache-encoding", app_menu::IDM_ENCODER_SETTINGS}, {L"clear-cache", app_menu::IDM_CLEAR_NEURAL_CACHE},
    {L"render-report", app_menu::IDM_RENDER_REPORT}, {L"open-receipt", app_menu::IDM_OPEN_RENDER_RECEIPT},
    {L"safe-mode", app_menu::IDM_ADVANCED_SAFE_MODE}, {L"rehook", app_menu::IDM_REHOOK},
    // Help
    {L"shortcuts", app_menu::IDM_KEYBOARD_SHORTCUTS}, {L"key-bindings", app_menu::IDM_HELP_KEY_BINDINGS},
    {L"user-guide", app_menu::IDM_HELP_USER_GUIDE}, {L"log-folder", app_menu::IDM_HELP_LOG_FOLDER},
    {L"check-updates", app_menu::IDM_CHECK_FOR_UPDATES}, {L"about", app_menu::IDM_HELP_ABOUT},
};

// The keys the player has always had, in the order a menu row prints them
// ("? / F1", "Shift+I / Shift+O").
struct DefaultBinding {
    std::wstring_view key;
    std::wstring_view command;
};
inline constexpr DefaultBinding kDefaults[] = {
    {L"Ctrl+O", L"open"}, {L"Ctrl+L", L"open-youtube"}, {L"Ctrl+S", L"export"}, {L"Ctrl+Shift+S", L"save-comparison"},
    {L"Space", L"play-pause"}, {L"S", L"stop"}, {L"Left", L"seek-back"}, {L"Right", L"seek-forward"},
    {L".", L"step-frame"}, {L"Ctrl+G", L"goto-timecode"}, {L"M", L"mute"},
    {L"V", L"subtitles-next"}, {L"H", L"subtitles-earlier"}, {L"J", L"subtitles-later"},
    {L"I", L"mark-in"}, {L"O", L"mark-out"}, {L"Shift+I", L"clear-marks"}, {L"Shift+O", L"clear-marks"},
    // New in 0.30.0, MPC-HC's and PotPlayer's keys for the folder's next and
    // previous file; nothing had them.
    {L"PgDn", L"next-file"}, {L"PgUp", L"previous-file"},
    {L"A", L"aspect-toggle"}, {L"F11", L"fullscreen"}, {L"Ctrl+E", L"viewing-adjustments"},
    {L"1", L"debug-final"}, {L"2", L"debug-input"}, {L"3", L"debug-motion"}, {L"4", L"debug-depth"},
    {L"R", L"compare-vsr"}, {L"C", L"compare-next"}, {L"Shift+C", L"compare-previous"},
    {L"[", L"mix-less"}, {L"]", L"mix-more"}, {L"X", L"swap"},
    {L"Z", L"zoom-in"}, {L"Shift+Z", L"zoom-out"}, {L"L", L"loupe"}, {L"Shift+R", L"compare-against-vsr"},
    {L"Shift+[", L"difference-less"}, {L"Shift+]", L"difference-more"},
    // P is new with the Enhance menu: Space was printed on this row and on
    // Play / Pause, and only one row can own a key.
    {L"D", L"dlss5"}, {L"Ctrl+N", L"picture"}, {L"P", L"pause-render"},
    {L"F", L"preview-frame"}, {L"Shift+F", L"preview-clip"}, {L"Ctrl+R", L"render-range"},
    {L"F6", L"rehook"}, {L"?", L"shortcuts"}, {L"F1", L"shortcuts"},
};

inline std::optional<UINT> CommandId(std::wstring_view name)
{
    for (const Command& command : kCommands)
        if (command.name == name) return command.id;
    return std::nullopt;
}

inline std::wstring_view CommandName(UINT id)
{
    for (const Command& command : kCommands)
        if (command.id == id) return command.name;
    return {};
}

namespace detail {

struct NamedKey {
    std::wstring_view name;
    UINT key;
};
// The display name first where a key has two ("PgUp" before "PageUp").
inline constexpr NamedKey kNamedKeys[] = {
    {L"Space", VK_SPACE}, {L"Left", VK_LEFT}, {L"Right", VK_RIGHT}, {L"Up", VK_UP}, {L"Down", VK_DOWN},
    {L"Home", VK_HOME}, {L"End", VK_END}, {L"PgUp", VK_PRIOR}, {L"PageUp", VK_PRIOR},
    {L"PgDn", VK_NEXT}, {L"PageDown", VK_NEXT}, {L"Ins", VK_INSERT}, {L"Insert", VK_INSERT},
    {L"Del", VK_DELETE}, {L"Delete", VK_DELETE}, {L"Backspace", VK_BACK},
    {L"[", VK_OEM_4}, {L"]", VK_OEM_6}, {L".", VK_OEM_PERIOD}, {L",", VK_OEM_COMMA}, {L"-", VK_OEM_MINUS},
    {L"=", VK_OEM_PLUS}, {L";", VK_OEM_1}, {L"'", VK_OEM_7}, {L"/", VK_OEM_2}, {L"\\", VK_OEM_5}, {L"`", VK_OEM_3},
};

inline bool SameName(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
        return std::towlower(x) == std::towlower(y);
    });
}

} // namespace detail

// "Ctrl+Shift+S", "Space", "[", "?": a key's name, or why it cannot be bound.
struct ParsedChord {
    std::optional<Chord> chord;
    std::wstring error;
};

inline ParsedChord ParseChord(std::wstring_view text)
{
    ParsedChord parsed;
    Chord chord;
    // Modifiers are the parts before the last '+', so "Shift+=" and "Ctrl+-" work
    // and a lone "+" is not a modifier.
    while (text.size() > 1) {
        const size_t plus = text.find(L'+', 1);
        if (plus == std::wstring_view::npos) break;
        const std::wstring_view modifier = text.substr(0, plus);
        if (detail::SameName(modifier, L"Ctrl") || detail::SameName(modifier, L"Control")) chord.control = true;
        else if (detail::SameName(modifier, L"Shift")) chord.shift = true;
        else if (detail::SameName(modifier, L"Alt")) { parsed.error = L"Alt opens the menu bar and cannot be bound"; return parsed; }
        else { parsed.error = L"'" + std::wstring(modifier) + L"' is not a modifier (Ctrl or Shift)"; return parsed; }
        text.remove_prefix(plus + 1);
    }
    for (const std::wstring_view reserved : {L"Tab", L"Enter", L"Esc", L"Escape", L"F10"})
        if (detail::SameName(text, reserved)) { parsed.error = std::wstring(text) + L" is kept for driving the player from the keyboard"; return parsed; }
    if (text == L"?") {
        if (chord.control || chord.shift) { parsed.error = L"? is a character; it takes no modifier"; return parsed; }
        chord.character = L'?';
        parsed.chord = chord;
        return parsed;
    }
    if (text.size() == 1 && std::iswalnum(text[0]) && text[0] < 0x80) {
        chord.key = static_cast<UINT>(std::towupper(text[0]));
        parsed.chord = chord;
        return parsed;
    }
    if (text.size() >= 2 && (text[0] == L'F' || text[0] == L'f')) {
        const std::wstring_view digits = text.substr(1);
        if (std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }) && digits.size() <= 2) {
            const int number = std::stoi(std::wstring(digits));
            if (number >= 1 && number <= 24) {
                chord.key = static_cast<UINT>(VK_F1 + number - 1);
                parsed.chord = chord;
                return parsed;
            }
        }
    }
    for (const detail::NamedKey& named : detail::kNamedKeys) {
        if (detail::SameName(text, named.name)) {
            chord.key = named.key;
            parsed.chord = chord;
            return parsed;
        }
    }
    parsed.error = L"'" + std::wstring(text) + L"' is not a key this file can name";
    return parsed;
}

inline std::wstring ChordText(const Chord& chord)
{
    if (chord.character) return std::wstring(1, chord.character);
    std::wstring text;
    if (chord.control) text += L"Ctrl+";
    if (chord.shift) text += L"Shift+";
    if ((chord.key >= 'A' && chord.key <= 'Z') || (chord.key >= '0' && chord.key <= '9')) return text + wchar_t(chord.key);
    if (chord.key >= VK_F1 && chord.key <= VK_F24) return text + L"F" + std::to_wstring(chord.key - VK_F1 + 1);
    for (const detail::NamedKey& named : detail::kNamedKeys)
        if (named.key == chord.key) return text + std::wstring(named.name);
    return text + L"?";
}

struct Binding {
    Chord chord;
    UINT command;
};

class Table {
public:
    // The command a key runs, if it runs one.
    std::optional<UINT> Lookup(const Chord& chord) const
    {
        for (const Binding& binding : bindings_)
            if (binding.chord == chord) return binding.command;
        return std::nullopt;
    }
    std::optional<UINT> LookupKey(UINT key, bool control, bool shift) const { return Lookup(Chord{key, control, shift, 0}); }

    // What a menu row prints: every key of the command, in the table's order.
    std::wstring KeysFor(UINT command) const
    {
        std::wstring keys;
        for (const Binding& binding : bindings_) {
            if (binding.command != command) continue;
            if (!keys.empty()) keys += L" / ";
            keys += ChordText(binding.chord);
        }
        return keys;
    }

    // Binds `chord` to `command` in place of whatever it ran; nullopt unbinds it.
    void Bind(const Chord& chord, std::optional<UINT> command)
    {
        const auto existing = std::find_if(bindings_.begin(), bindings_.end(),
                                           [&](const Binding& binding) { return binding.chord == chord; });
        if (!command) {
            if (existing != bindings_.end()) bindings_.erase(existing);
        } else if (existing != bindings_.end()) {
            existing->command = *command;
        } else {
            bindings_.push_back(Binding{chord, *command});
        }
    }

    std::span<const Binding> Bindings() const { return bindings_; }

private:
    std::vector<Binding> bindings_;
};

inline const Table& Defaults()
{
    static const Table table = [] {
        Table defaults;
        for (const DefaultBinding& binding : kDefaults) defaults.Bind(*ParseChord(binding.key).chord, CommandId(binding.command));
        return defaults;
    }();
    return table;
}

struct Loaded {
    Table table;
    // "line 4: 'Ctrl+Q' - 'quit' is not a command", in file order. Logged.
    std::vector<std::wstring> problems;
};

// The defaults with a file's lines applied in order, so a later line for the
// same key wins, as in mpv.
inline Loaded Parse(std::string_view utf8)
{
    Loaded loaded{Defaults(), {}};
    if (utf8.size() >= 3 && utf8.substr(0, 3) == "\xEF\xBB\xBF") utf8.remove_prefix(3);
    std::vector<Chord> seen;
    size_t number = 0;
    while (!utf8.empty()) {
        ++number;
        const size_t end = utf8.find('\n');
        const std::string_view raw = utf8.substr(0, end);
        utf8 = end == std::string_view::npos ? std::string_view{} : utf8.substr(end + 1);
        const auto problem = [&](std::wstring_view what) {
            loaded.problems.push_back(L"line " + std::to_wstring(number) + L": " + std::wstring(what));
        };
        const std::optional<std::wstring> wide = utf8_text::ToWideStrict(raw);
        if (!wide) { problem(L"not UTF-8"); continue; }
        std::vector<std::wstring_view> words;
        const std::wstring_view line = *wide;
        for (size_t at = 0; at < line.size();) {
            while (at < line.size() && std::iswspace(line[at])) ++at;
            if (at >= line.size()) break;
            size_t stop = at;
            while (stop < line.size() && !std::iswspace(line[stop])) ++stop;
            words.push_back(line.substr(at, stop - at));
            at = stop;
        }
        // A comment is a word that starts with # once the key has been read, or
        // the whole line; a lone "#" key is not one this file can name anyway.
        if (words.empty() || words.front().front() == L'#') continue;
        if (const auto comment = std::find_if(words.begin() + 1, words.end(), [](std::wstring_view word) { return word.front() == L'#'; });
            comment != words.end()) words.erase(comment, words.end());
        if (words.size() != 2) { problem(L"a line is KEY COMMAND"); continue; }
        const ParsedChord chord = ParseChord(words[0]);
        if (!chord.chord) { problem(chord.error); continue; }
        std::optional<UINT> command;
        if (words[1] != L"ignore") {
            command = CommandId(words[1]);
            if (!command) { problem(L"'" + std::wstring(words[1]) + L"' is not a command"); continue; }
        }
        if (std::find(seen.begin(), seen.end(), *chord.chord) != seen.end())
            problem(ChordText(*chord.chord) + L" is bound twice; the last line is used");
        seen.push_back(*chord.chord);
        loaded.table.Bind(*chord.chord, command);
    }
    return loaded;
}

// What "Help > Edit key bindings" writes when there is no file yet: every
// default and every command, commented out, so the file starts as the keys
// the player already has and a line is changed by removing its #.
inline std::string TemplateText()
{
    std::wstring text =
        L"# DLSS 5 Video Player keys. One per line: KEY COMMAND, as in mpv's input.conf.\r\n"
        L"# A line replaces what its key did; \"KEY ignore\" unbinds a key. Saved changes\r\n"
        L"# apply when you go back to the player. Keys: A-Z, 0-9, F1-F24, Space, Left,\r\n"
        L"# Right, Up, Down, Home, End, PgUp, PgDn, Ins, Del, Backspace, [ ] . , - = ; ' / \\ `\r\n"
        L"# and ?, after Ctrl+ and/or Shift+. Tab, Enter, Esc, F10 and Alt are kept.\r\n"
        L"#\r\n# The defaults:\r\n";
    for (const DefaultBinding& binding : kDefaults) {
        std::wstring key(binding.key);
        key.resize(std::max<size_t>(key.size() + 1, 16), L' ');
        text += L"# " + key + std::wstring(binding.command) + L"\r\n";
    }
    text += L"#\r\n# Commands with no key of their own:\r\n";
    for (const Command& command : kCommands) {
        const bool bound = std::any_of(std::begin(kDefaults), std::end(kDefaults),
                                       [&](const DefaultBinding& binding) { return binding.command == command.name; });
        if (!bound) text += L"#                 " + std::wstring(command.name) + L"\r\n";
    }
    return utf8_text::FromWide(text);
}

} // namespace key_bindings
