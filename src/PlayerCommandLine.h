#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

// A player launch's own arguments: what is left of the command line once
// ParseRuntimeArguments has taken the runtime's (and kept --safe-mode, which
// it has already read) and `--render` has been ruled out (RenderCommandLine.h).
// ParseArgs in main.cpp reads the process command line; everything it decides
// is decided here.
namespace player_command_line {

struct Parsed {
    // The output box a neural render is fitted into; `--output WxH`.
    uint32_t maxWidth = 3840, maxHeight = 2160;
    bool outputExplicit = false;
    // The file or URL to open: the last argument that is not an option.
    std::wstring file;
    // Non-empty when the command line is refused; the text is shown as is.
    std::wstring error;
};

inline Parsed Parse(const std::vector<std::wstring>& arguments)
{
    Parsed parsed;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"--safe-mode") {
            continue;
        } else if (argument == L"--output" && index + 1 < arguments.size()) {
            // A value without an `x` is taken and ignored; either side below
            // 64, or not a number, is 64.
            const std::wstring value = arguments[++index];
            auto cross = value.find(L'x');
            if (cross == std::wstring::npos) cross = value.find(L'X');
            if (cross != std::wstring::npos) {
                parsed.maxWidth = static_cast<uint32_t>(std::max(64, _wtoi(value.substr(0, cross).c_str())));
                parsed.maxHeight = static_cast<uint32_t>(std::max(64, _wtoi(value.substr(cross + 1).c_str())));
                parsed.outputExplicit = true;
            }
        } else if (argument == L"--quality") {
            parsed.error = L"The legacy --quality option was removed. Neural rendering preserves source resolution; "
                           L"choose Super Resolution output in the DLSS menu.";
            return parsed;
        } else if (!argument.empty() && argument[0] != L'-') {
            parsed.file = argument;
        }
    }
    return parsed;
}

} // namespace player_command_line
