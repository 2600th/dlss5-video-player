#pragma once

#include <string>
#include <string_view>

// One argument of a Windows command line, always quoted, with the escaping
// CommandLineToArgvW and the CRT undo: a quote inside becomes \", and the
// backslashes before a quote - or before the closing one - are doubled. The
// player built its ffmpeg command lines with a bare "+path+", which a file name
// cannot break today (NTFS refuses a quote) but a path ending in a backslash, or
// a URL, could.
inline std::wstring QuoteCommandArgument(std::wstring_view argument)
{
    std::wstring quoted(1, L'"');
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') { ++backslashes; continue; }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}
