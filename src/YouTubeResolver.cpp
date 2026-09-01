#include "YouTubeResolver.h"

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <limits>
#include <string>

namespace {

constexpr size_t kMaximumInputCharacters = 2048;
constexpr size_t kMaximumOutputBytes = 16 * 1024;
constexpr size_t kMaximumDetailCharacters = 4 * 1024;

struct CrackedUrl {
    INTERNET_SCHEME scheme{0};
    std::wstring host;
    std::wstring path;
    std::wstring extra;
    bool hasUserInfo{false};
};

bool equals_case_insensitive(std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size()) return false;
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool has_dot_bound_suffix(std::wstring_view host, std::wstring_view suffix)
{
    if (equals_case_insensitive(host, suffix)) return true;
    if (host.size() <= suffix.size()) return false;
    const size_t suffixOffset = host.size() - suffix.size();
    return host[suffixOffset - 1] == L'.' &&
           equals_case_insensitive(host.substr(suffixOffset), suffix);
}

bool crack_url(std::wstring_view value, CrackedUrl& result)
{
    if (value.empty() || value.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
        return false;
    }

    std::wstring host(value.size() + 1, L'\0');
    std::wstring path(value.size() + 1, L'\0');
    std::wstring extra(value.size() + 1, L'\0');
    std::wstring user(value.size() + 1, L'\0');
    std::wstring password(value.size() + 1, L'\0');

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = host.data();
    components.dwHostNameLength = static_cast<DWORD>(host.size());
    components.lpszUrlPath = path.data();
    components.dwUrlPathLength = static_cast<DWORD>(path.size());
    components.lpszExtraInfo = extra.data();
    components.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    components.lpszUserName = user.data();
    components.dwUserNameLength = static_cast<DWORD>(user.size());
    components.lpszPassword = password.data();
    components.dwPasswordLength = static_cast<DWORD>(password.size());

    if (!WinHttpCrackUrl(value.data(), static_cast<DWORD>(value.size()), 0, &components)) {
        return false;
    }

    host.resize(components.dwHostNameLength);
    path.resize(components.dwUrlPathLength);
    extra.resize(components.dwExtraInfoLength);
    result.scheme = components.nScheme;
    result.host = std::move(host);
    result.path = std::move(path);
    result.extra = std::move(extra);
    result.hasUserInfo = components.dwUserNameLength != 0 || components.dwPasswordLength != 0;
    return true;
}

bool is_video_id(std::wstring_view value)
{
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](wchar_t character) {
        return (character >= L'a' && character <= L'z') ||
               (character >= L'A' && character <= L'Z') ||
               (character >= L'0' && character <= L'9') ||
               character == L'-' || character == L'_';
    });
}

std::wstring_view query_part(std::wstring_view extra)
{
    if (extra.empty() || extra.front() != L'?') return {};
    extra.remove_prefix(1);
    const size_t fragment = extra.find(L'#');
    if (fragment != std::wstring_view::npos) extra = extra.substr(0, fragment);
    return extra;
}

bool query_has_video_id(std::wstring_view query)
{
    while (!query.empty()) {
        const size_t separator = query.find(L'&');
        const std::wstring_view item = query.substr(0, separator);
        const size_t equals = item.find(L'=');
        if (equals != std::wstring_view::npos && item.substr(0, equals) == L"v" &&
            is_video_id(item.substr(equals + 1))) {
            return true;
        }
        if (separator == std::wstring_view::npos) break;
        query.remove_prefix(separator + 1);
    }
    return false;
}

bool has_forbidden_input_character(std::wstring_view value)
{
    return std::any_of(value.begin(), value.end(), [](wchar_t character) {
        return character <= 0x1f || character == 0x7f || character == L'\'' ||
               character == L'"' || character == L'\\' || std::iswspace(character) != 0;
    });
}

std::wstring utf8_to_wide(std::string_view value)
{
    if (value.empty() || value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring converted(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), converted.data(), count) != count) {
        return {};
    }
    return converted;
}

bool contains_case_insensitive(std::string_view value, std::string_view needle)
{
    return std::search(value.begin(), value.end(), needle.begin(), needle.end(),
                       [](char left, char right) {
                           return std::tolower(static_cast<unsigned char>(left)) ==
                                  std::tolower(static_cast<unsigned char>(right));
                       }) != value.end();
}

bool diagnostic_token_is_sensitive(std::string_view token)
{
    return contains_case_insensitive(token, "://") ||
           contains_case_insensitive(token, "www.") ||
           contains_case_insensitive(token, "token") ||
           contains_case_insensitive(token, "bearer") ||
           contains_case_insensitive(token, "authorization") ||
           contains_case_insensitive(token, "signature") ||
           contains_case_insensitive(token, "sig=");
}

std::wstring sanitize_detail(std::string_view bytes)
{
    std::wstring detail;
    detail.reserve(std::min(bytes.size(), kMaximumDetailCharacters));
    size_t offset = 0;
    while (offset < bytes.size() && detail.size() < kMaximumDetailCharacters) {
        while (offset < bytes.size() &&
               std::isspace(static_cast<unsigned char>(bytes[offset])) != 0) {
            ++offset;
        }
        if (offset == bytes.size()) break;

        const size_t start = offset;
        while (offset < bytes.size() &&
               std::isspace(static_cast<unsigned char>(bytes[offset])) == 0) {
            ++offset;
        }
        const std::string_view token = bytes.substr(start, offset - start);
        if (!detail.empty()) detail.push_back(L' ');
        if (diagnostic_token_is_sensitive(token)) {
            constexpr std::wstring_view replacement = L"[redacted]";
            detail.append(replacement.substr(
                0, std::min(replacement.size(), kMaximumDetailCharacters - detail.size())));
            continue;
        }

        for (const unsigned char character : token) {
            if (detail.size() == kMaximumDetailCharacters) break;
            detail.push_back(character >= 0x20 && character <= 0x7e &&
                                     character != '\'' && character != '"'
                                 ? static_cast<wchar_t>(character)
                                 : L'?');
        }
    }

    if (detail.empty()) detail = L"Resolver helper failed.";
    return detail;
}

ResolveResult invalid_output()
{
    ResolveResult result;
    result.error = ResolveError::InvalidOutput;
    result.detail = L"Resolver returned invalid output.";
    return result;
}

} // namespace

bool IsSupportedYouTubeUrl(std::wstring_view value)
{
    if (value.empty() || value.size() > kMaximumInputCharacters ||
        has_forbidden_input_character(value)) {
        return false;
    }

    CrackedUrl url;
    if (!crack_url(value, url) || url.scheme != INTERNET_SCHEME_HTTPS ||
        url.hasUserInfo || !has_dot_bound_suffix(url.host, L"youtube.com") &&
                                !equals_case_insensitive(url.host, L"youtu.be")) {
        return false;
    }

    if (equals_case_insensitive(url.host, L"youtu.be")) {
        if (url.path.size() <= 1 || url.path.front() != L'/') return false;
        return is_video_id(std::wstring_view(url.path).substr(1));
    }

    if (url.path == L"/watch") return query_has_video_id(query_part(url.extra));
    constexpr std::wstring_view shortsPrefix = L"/shorts/";
    if (url.path.starts_with(shortsPrefix)) {
        return is_video_id(std::wstring_view(url.path).substr(shortsPrefix.size()));
    }
    return false;
}

ResolveResult ParseResolverOutput(std::string_view stdoutBytes, DWORD exitCode)
{
    if (exitCode != 0) {
        ResolveResult result;
        result.error = ResolveError::ExtractionFailed;
        result.detail = sanitize_detail(stdoutBytes);
        return result;
    }

    while (!stdoutBytes.empty() &&
           (stdoutBytes.front() == '\r' || stdoutBytes.front() == '\n')) {
        stdoutBytes.remove_prefix(1);
    }
    while (!stdoutBytes.empty() &&
           (stdoutBytes.back() == '\r' || stdoutBytes.back() == '\n')) {
        stdoutBytes.remove_suffix(1);
    }
    if (stdoutBytes.empty() || stdoutBytes.size() > kMaximumOutputBytes ||
        stdoutBytes.find_first_of("\r\n") != std::string_view::npos) {
        return invalid_output();
    }

    std::wstring mediaUrl = utf8_to_wide(stdoutBytes);
    if (mediaUrl.empty() || has_forbidden_input_character(mediaUrl)) return invalid_output();

    CrackedUrl url;
    if (!crack_url(mediaUrl, url) || url.scheme != INTERNET_SCHEME_HTTPS ||
        url.hasUserInfo || !has_dot_bound_suffix(url.host, L"googlevideo.com")) {
        return invalid_output();
    }

    ResolveResult result;
    result.ok = true;
    result.mediaUrl = std::move(mediaUrl);
    return result;
}
