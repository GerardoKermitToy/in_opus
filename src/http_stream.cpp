#include "http_stream.hpp"
#include <algorithm>
#include <cwchar>
#include <vector>

namespace winamp_opus {
bool IsHttpUrl(const wchar_t* path) noexcept {
    return path && (_wcsnicmp(path, L"http://", 7) == 0 || _wcsnicmp(path, L"https://", 8) == 0);
}
std::wstring StreamUrl(const wchar_t* path) {
    std::wstring url(path ? path : L"");
    // Explicit routing hint compatible with the older plugin; never sent to server.
    if (url.size() >= 6 && _wcsicmp(url.c_str() + url.size() - 6, L">.opus") == 0)
        url.resize(url.size() - 6);
    return url;
}
HttpStream::~HttpStream() {
    Cancel();
    if (connection_) WinHttpCloseHandle(connection_);
    if (session_) WinHttpCloseHandle(session_);
}
void HttpStream::Cancel() noexcept {
    cancelled_.store(true);
    if (const auto handle = request_.exchange(nullptr)) WinHttpCloseHandle(handle);
}
std::wstring HttpStream::Header(const wchar_t* name) const {
    const auto request = request_.load();
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, nullptr, &bytes, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 65536) return {};
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, value.data(), &bytes, nullptr)) return {};
    return value.data();
}
bool HttpStream::Open(const wchar_t* path) {
    if (!IsHttpUrl(path)) return false;
    const auto url = StreamUrl(path);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.dwUserNameLength || parts.dwPasswordLength)
        return false; // Do not implicitly forward credentials to stations or redirects.
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) return false;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring resource(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (resource.empty()) resource = L"/";
    if (parts.dwExtraInfoLength) resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (const auto fragment = resource.find(L'#'); fragment != std::wstring::npos) resource.resize(fragment);
    session_ = WinHttpOpen(L"OggOpusPlayer/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) return false;
    if (!WinHttpSetTimeouts(session_, 5000, 5000, 5000, 3000)) return false;
    connection_ = WinHttpConnect(session_, host.c_str(), parts.nPort, 0);
    if (!connection_) return false;
    const auto request = WinHttpOpenRequest(connection_, L"GET", resource.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    request_.store(request);
    if (!request) return false;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon))) return false;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) return false;
    DWORD redirects = 5;
    WinHttpSetOption(request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects));
    // No ICY frames embedded in the Ogg byte stream, no contact details/referer.
    constexpr wchar_t headers[] = L"Icy-MetaData: 0\r\nAccept: audio/ogg, application/ogg, audio/opus, */*\r\n";
    if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) return false;
    DWORD status = 0, bytes = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, nullptr) || status != 200) return false;
    // If a station ignores Icy-MetaData: 0, fail instead of passing ICY bytes to libogg.
    const auto metaint = Header(L"icy-metaint");
    if (!metaint.empty() && wcstoul(metaint.c_str(), nullptr, 10) != 0) return false;
    station_.name = Header(L"icy-name");
    station_.description = Header(L"icy-description");
    station_.genre = Header(L"icy-genre");
    station_.website = Header(L"icy-url");
    station_.content_type = Header(L"Content-Type");
    const auto bitrate = Header(L"icy-br");
    const auto kbps = wcstoul(bitrate.c_str(), nullptr, 10);
    if (kbps <= 100000) station_.bitrate = static_cast<int>(kbps * 1000);
    deadline_ = GetTickCount64() + 10000;
    return true;
}
int HttpStream::Read(unsigned char* data, int bytes) noexcept {
    if (bytes <= 0) return 0;
    if (cancelled_.load() || (opening_ &&
        (GetTickCount64() > deadline_ || opening_bytes_ >= 1024 * 1024))) return -1;
    const auto request = request_.load();
    if (!request) return -1;
    DWORD read = 0;
    const int wanted = opening_ ? std::min(bytes, 1024 * 1024 - opening_bytes_) : bytes;
    if (!WinHttpReadData(request, data, static_cast<DWORD>(wanted), &read)) return -1;
    if (opening_) opening_bytes_ += static_cast<int>(read);
    return static_cast<int>(read);
}
} // namespace winamp_opus
