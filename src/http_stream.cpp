// SPDX-License-Identifier: GPL-3.0-only
#include "http_stream.hpp"
#include "credentials.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <cwchar>
#include <limits>

namespace winamp_opus {
namespace {
constexpr std::int64_t kSmallFileLimit = 64LL * 1024 * 1024;
constexpr DWORD kHeaderLimit = 65536;
constexpr wchar_t kHeaders[] = L"Icy-MetaData: 1\r\nAccept-Encoding: identity\r\nAccept: audio/ogg, application/ogg, audio/opus, */*\r\n";
void CloseRequest(std::atomic<HINTERNET>& slot) noexcept {
    if (const auto request = slot.exchange(nullptr)) WinHttpCloseHandle(request);
}
bool Controls(const std::wstring& text) noexcept {
    for (const auto c : text) if (c < L' ' || c == 127) return true;
    return false;
}
std::wstring Trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t");
    return value.substr(first, last - first + 1);
}
bool Unsigned(const wchar_t*& p, std::uint64_t& value) noexcept {
    if (*p < L'0' || *p > L'9') return false;
    value = 0;
    do {
        const auto digit = static_cast<unsigned>(*p++ - L'0');
        if (value > (static_cast<std::uint64_t>(INT64_MAX) - digit) / 10) return false;
        value = value * 10 + digit;
    } while (*p >= L'0' && *p <= L'9');
    return true;
}
bool Number(const std::wstring& text, std::int64_t& value) noexcept {
    const auto* p = text.c_str();
    std::uint64_t number = 0;
    if (!Unsigned(p, number) || *p) return false;
    value = static_cast<std::int64_t>(number);
    return true;
}
bool ContentRange(const std::wstring& value, std::int64_t offset, std::int64_t size) noexcept {
    if (value.size() < 6 || _wcsnicmp(value.c_str(), L"bytes ", 6)) return false;
    const auto* p = value.c_str() + 6;
    std::uint64_t first = 0, last = 0, total = 0;
    if (!Unsigned(p, first) || *p++ != L'-' || !Unsigned(p, last) || *p++ != L'/' ||
        !Unsigned(p, total) || *p) return false;
    // An open-ended request must describe the complete remaining representation.
    return first == static_cast<std::uint64_t>(offset) && total == static_cast<std::uint64_t>(size) &&
        first <= last && last < total && last == total - 1;
}
bool Status(HINTERNET request, DWORD& status) noexcept {
    DWORD bytes = sizeof(status);
    return WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, nullptr) != FALSE;
}
std::uint32_t Le32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
struct Url {
    std::wstring host, resource, origin;
    INTERNET_PORT port = 0;
    bool secure = false;
};
bool ParseUrl(const std::wstring& url, Url& out) {
    if (url.empty() || url.size() > 32768 || Controls(url) || url.find(L'\\') != std::wstring::npos) return false;
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.dwUserNameLength || parts.dwPasswordLength ||
        !parts.dwHostNameLength || !parts.nPort ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) return false;
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    out.port = parts.nPort;
    out.resource.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (out.resource.empty()) out.resource = L"/";
    if (parts.dwExtraInfoLength) out.resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (const auto hash = out.resource.find(L'#'); hash != std::wstring::npos) out.resource.resize(hash);
    out.origin = out.secure ? L"https://" : L"http://";
    if (out.host.find(L':') != std::wstring::npos && out.host.front() != L'[') out.origin += L"[" + out.host + L"]";
    else out.origin += out.host;
    if (out.port != (out.secure ? 443 : 80)) out.origin += L":" + std::to_wstring(out.port);
    return !out.resource.empty() && out.resource.front() == L'/';
}
bool ProxyEndpoint(const std::wstring& configured, std::wstring& endpoint) {
    if (configured.empty()) { endpoint.clear(); return true; }
    if (Controls(configured) || configured.find_first_of(L" \t@;=?#") != std::wstring::npos) return false;
    std::wstring address = configured;
    if (!_wcsnicmp(address.c_str(), L"http://", 7)) {
        address.erase(0, 7);
        if (!address.empty() && address.back() == L'/') address.pop_back();
    }
    if (address.empty() || address.find(L'/') != std::wstring::npos || address.find(L'\\') != std::wstring::npos) return false;
    // Single endpoint only: no WinHTTP per-scheme lists or embedded passwords.
    const auto colon = address.front() == L'[' ? address.find(L':', address.find(L']')) : address.find(L':');
    if (colon != std::wstring::npos) {
        std::int64_t port = 0;
        if (!Number(address.substr(colon + 1), port) || port < 1 || port > 65535) return false;
    }
    Url parsed;
    if (!ParseUrl(L"http://" + address, parsed) || parsed.resource != L"/" || parsed.secure) return false;
    endpoint = parsed.host;
    if (endpoint.find(L':') != std::wstring::npos && endpoint.front() != L'[') endpoint = L"[" + endpoint + L"]";
    endpoint += L":" + std::to_wstring(parsed.port);
    return true;
}
std::wstring RedirectUrl(const Url& old, const std::wstring& location) {
    if (location.empty() || Controls(location) || location.find(L'\\') != std::wstring::npos) return {};
    if (IsHttpUrl(location.c_str())) return location;
    const std::wstring scheme = old.secure ? L"https:" : L"http:";
    if (location.compare(0, 2, L"//") == 0) return scheme + location;
    if (const auto colon = location.find(L':'); colon != std::wstring::npos &&
        colon < location.find_first_of(L"/?#")) return {}; // Not a relative HTTP reference.
    std::wstring resource;
    if (location.front() == L'/') resource = location;
    else if (location.front() == L'#') resource = old.resource + location;
    else {
        const auto query = old.resource.find(L'?');
        resource = old.resource.substr(0, query);
        if (location.front() == L'?') resource += location;
        else { resource.resize(resource.find_last_of(L'/') + 1); resource += location; }
    }
    // Normalize dot segments only in the path, never in query text.
    const auto suffix_at = resource.find_first_of(L"?#");
    const auto suffix = suffix_at == std::wstring::npos ? std::wstring{} : resource.substr(suffix_at);
    const auto path = resource.substr(0, suffix_at);
    std::wstring normalized = L"/";
    std::vector<std::wstring> segments;
    for (std::size_t at = 1; at <= path.size();) {
        const auto slash = path.find(L'/', at);
        const auto part = path.substr(at, slash == std::wstring::npos ? slash : slash - at);
        if (part == L"..") { if (!segments.empty()) segments.pop_back(); }
        else if (part != L".") segments.push_back(part);
        if (slash == std::wstring::npos) break;
        at = slash + 1;
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i) normalized += L'/';
        normalized += segments[i];
    }
    if ((path.size() >= 2 && path.compare(path.size() - 2, 2, L"/.") == 0) ||
        (path.size() >= 3 && path.compare(path.size() - 3, 3, L"/..") == 0)) {
        if (normalized.back() != L'/') normalized += L'/';
    }
    return old.origin + normalized + suffix;
}
}
bool IsHttpUrl(const wchar_t* path) noexcept {
    return path && (!_wcsnicmp(path, L"http://", 7) || !_wcsnicmp(path, L"https://", 8));
}
std::wstring StreamUrl(const wchar_t* path) {
    std::wstring url(path ? path : L"");
    // Opus-only routing hint; never sent to a server or used on local paths.
    if (url.size() >= 6 && !_wcsicmp(url.c_str() + url.size() - 6, L">.opus")) url.resize(url.size() - 6);
    return url;
}
InputStream::~InputStream() {
    Cancel();
    CloseResources();
    if (const auto event = probe_stop_.exchange(nullptr)) CloseHandle(event);
}
void InputStream::Cancel() noexcept {
    cancelled_.store(true);
    if (const auto event = probe_stop_.load()) SetEvent(event);
    CloseRequest(pending_);
    CloseRequest(request_);
    if (const auto file = file_.load(); file != INVALID_HANDLE_VALUE) CancelIoEx(file, nullptr);
}
void InputStream::CloseNetwork() noexcept {
    CloseRequest(pending_);
    CloseRequest(request_);
    if (connection_) { WinHttpCloseHandle(connection_); connection_ = nullptr; }
    if (session_) { WinHttpCloseHandle(session_); session_ = nullptr; }
}
void InputStream::CloseResources() noexcept {
    StopProbeTimer();
    CloseNetwork();
    if (const auto file = file_.exchange(INVALID_HANDLE_VALUE); file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (recording_ != INVALID_HANDLE_VALUE) { CloseHandle(recording_); recording_ = INVALID_HANDLE_VALUE; }
    opened_ = false;
}
void InputStream::SetError(const wchar_t* text, DWORD code) noexcept {
    try {
        std::wstring message(text);
        if (code) message += L" (Windows error " + std::to_wstring(code) + L").";
        if (station_.error != message) { station_.error = std::move(message); Changed(); }
    } catch (...) { }
}
void InputStream::SetRecordingError(const wchar_t* text, DWORD code) noexcept {
    try {
        std::wstring message(text);
        if (code) message += L" (Windows error " + std::to_wstring(code) + L").";
        if (station_.recording_error != message) { station_.recording_error = std::move(message); Changed(); }
    } catch (...) { }
}
bool InputStream::Interrupted() noexcept {
    if (cancelled_) { SetError(L"The input operation was cancelled."); return true; }
    if (probe_expired_ || (http_ && opening_ && GetTickCount64() >= deadline_)) {
        SetError(L"The HTTP content probe timed out."); return true;
    }
    return false;
}
bool InputStream::StartProbeTimer() {
    deadline_ = GetTickCount64() + static_cast<ULONGLONG>(settings_.timeout_ms);
    const auto stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) { SetError(L"Cannot create the HTTP probe timer", GetLastError()); return false; }
    probe_stop_.store(stop);
    probe_active_.store(true);
    // A watchdog bounds DNS/proxy resolution and all phases of synchronous
    // WinHTTP, not just the next ReadData. It never mutates StationInfo.
    probe_timer_ = std::thread([this, stop] {
        const auto now = GetTickCount64();
        const auto delay = static_cast<DWORD>(now < deadline_ ? deadline_ - now : 0);
        if (WaitForSingleObject(stop, delay) == WAIT_TIMEOUT && probe_active_.exchange(false)) {
            probe_expired_.store(true);
            CloseRequest(pending_);
            CloseRequest(request_);
        }
    });
    if (cancelled_) SetEvent(stop);
    return true;
}
void InputStream::StopProbeTimer() noexcept {
    probe_active_.store(false);
    if (const auto stop = probe_stop_.load()) SetEvent(stop);
    if (probe_timer_.joinable()) { try { probe_timer_.join(); } catch (...) { } }
}
std::wstring InputStream::Header(HINTERNET request, const wchar_t* name) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, nullptr, &bytes, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > kHeaderLimit || bytes < sizeof(wchar_t)) return {};
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, value.data(), &bytes, nullptr)) return {};
    return Trim(value.data());
}
bool InputStream::Open(const wchar_t* path, const PlaybackSettings& settings, std::wstring* error, bool replay_probe) {
    bool success = false;
    try {
        if (attempted_) SetError(L"An input stream can only be opened once.");
        else {
            attempted_ = true;
            settings_ = ValidateSettings(settings);
            replay_enabled_ = replay_probe;
            capture_probe_ = replay_probe || settings_.save_streams;
            probe_limit_ = settings_.probe_kb * 1024;
            if (cancelled_) SetError(L"The input operation was cancelled.");
            else if (!path || !*path) SetError(L"No file or URL was supplied.");
            else {
                http_ = IsHttpUrl(path);
                success = http_ ? OpenHttp(path) : OpenLocal(path);
                if (success && Interrupted()) success = false;
                opened_ = success;
            }
        }
    } catch (...) { SetError(L"Cannot allocate input transport resources."); }
    if (!success && !opened_) CloseResources();
    if (error) *error = success ? std::wstring{} : station_.error;
    return success;
}
bool InputStream::ReadFileAt(std::int64_t offset, void* data, DWORD bytes) noexcept {
    if (Interrupted()) return false;
    LARGE_INTEGER next{}; next.QuadPart = offset;
    const auto file = file_.load();
    DWORD got = 0;
    if (!SetFilePointerEx(file, next, nullptr, FILE_BEGIN) ||
        !ReadFile(file, data, bytes, &got, nullptr) || got != bytes) {
        if (!Interrupted()) SetError(L"Cannot read the audio file", GetLastError());
        return false;
    }
    return !Interrupted();
}
bool InputStream::OpenLocal(const wchar_t* path) {
    auto file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE && wcschr(path, L'|')) {
        if (Interrupted()) return false;
        std::wstring historical(path);
        std::replace(historical.begin(), historical.end(), L'|', L'\xff5c');
        file = CreateFileW(historical.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) { SetError(L"Cannot open the audio file for reading", GetLastError()); return false; }
    file_.store(file);
    if (Interrupted()) return false;
    LARGE_INTEGER physical{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &physical) || physical.QuadPart < 0) {
        SetError(L"Cannot determine the audio file size."); return false;
    }
    size_ = physical.QuadPart;
    unsigned char header[12]{};
    if (size_ >= 12) {
        if (!ReadFileAt(0, header, sizeof(header))) return false;
        if (!std::memcmp(header, "RIFF", 4) && !std::memcmp(header + 8, "WAVE", 4)) {
            const auto riff_size = static_cast<std::int64_t>(Le32(header + 4)) + 8;
            if (riff_size < 12 || riff_size > size_) { SetError(L"The WAVE wrapper is truncated."); return false; }
            bool found = false;
            std::int64_t at = 12;
            for (unsigned chunks = 0; chunks < 65536 && at <= riff_size - 8 && at < kSmallFileLimit; ++chunks) {
                unsigned char chunk[8]{};
                if (!ReadFileAt(at, chunk, sizeof(chunk))) return false;
                const auto length = static_cast<std::int64_t>(Le32(chunk + 4));
                if (length + (length & 1) > riff_size - at - 8) break;
                if (!std::memcmp(chunk, "data", 4)) {
                    base_offset_ = at + 8; size_ = length; found = true; break;
                }
                at += 8 + length + (length & 1);
            }
            if (!found) { SetError(L"The WAVE wrapper has no valid bounded Ogg data chunk."); return false; }
        }
    }
    LARGE_INTEGER start{}; start.QuadPart = base_offset_;
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) { SetError(L"Cannot seek to the audio data."); return false; }
    if (settings_.buffer_small_files && size_ <= kSmallFileLimit) {
        memory_.resize(static_cast<std::size_t>(size_));
        std::size_t at = 0;
        while (at < memory_.size()) {
            if (Interrupted()) return false;
            const auto wanted = static_cast<DWORD>(std::min<std::size_t>(65536, memory_.size() - at));
            DWORD got = 0;
            if (!ReadFile(file, memory_.data() + at, wanted, &got, nullptr) || got != wanted) {
                if (!Interrupted()) SetError(L"Cannot buffer the complete audio file.");
                return false;
            }
            at += got;
        }
        cached_ = true;
        CloseHandle(file_.exchange(INVALID_HANDLE_VALUE));
    }
    return !Interrupted();
}
bool InputStream::ConfigureRequest(HINTERNET request) noexcept {
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    // Redirects are validated explicitly. Never enable TLS certificate bypass,
    // automatic content decompression, ambient Windows logon or cookie state.
    return WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon)) &&
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
}
bool InputStream::Request(HINTERNET request, const std::wstring& headers,
    const std::wstring& server, bool secure) {
    if (!ConfigureRequest(request)) {
        if (!Interrupted()) SetError(L"Cannot apply safe HTTP request policy", GetLastError());
        return false;
    }
    for (unsigned attempt = 0; attempt <= 3; ++attempt) {
        if (Interrupted()) return false;
        if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1),
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
            if (!Interrupted()) SetError(L"The HTTP request failed", GetLastError());
            return false;
        }
        if (Interrupted()) return false;
        DWORD status = 0;
        if (!Status(request, status)) { SetError(L"Cannot read the HTTP response status", GetLastError()); return false; }
        if (status != 401 && status != 407) return true;
        if (!settings_.interactive_auth || attempt == 3) { SetError(L"HTTP authentication is required or was rejected."); return false; }
        DWORD supported = 0, first = 0, target = 0;
        if (!WinHttpQueryAuthSchemes(request, &supported, &first, &target)) {
            SetError(L"Cannot read the HTTP authentication challenge."); return false;
        }
        const bool proxy = status == 407;
        const DWORD scheme = supported & WINHTTP_AUTH_SCHEME_DIGEST ? WINHTTP_AUTH_SCHEME_DIGEST :
            supported & WINHTTP_AUTH_SCHEME_BASIC ? WINHTTP_AUTH_SCHEME_BASIC : 0;
        if (!scheme) { SetError(L"The HTTP authentication scheme is not supported."); return false; }
        std::wstring user, password;
        struct Secret { std::wstring& value; ~Secret() { ClearPassword(value); } } secret{password};
        const auto authority = proxy ? (settings_.proxy_url.empty() ? L"Windows system proxy" : settings_.proxy_url) : server;
        if (!AskCredentials(nullptr, proxy, authority, cancelled_, probe_expired_, user, password,
                scheme == WINHTTP_AUTH_SCHEME_BASIC && (proxy || !secure))) {
            if (!Interrupted()) SetError(L"HTTP authentication was cancelled.");
            return false;
        }
        if (Interrupted()) return false;
        const auto applied = WinHttpSetCredentials(request, proxy ? WINHTTP_AUTH_TARGET_PROXY : WINHTTP_AUTH_TARGET_SERVER,
            scheme, user.c_str(), password.c_str(), nullptr);
        ClearPassword(password);
        if (!applied) { if (!Interrupted()) SetError(L"Cannot apply HTTP credentials", GetLastError()); return false; }
    }
    return false;
}
bool InputStream::OpenHttp(const wchar_t* path) {
    Url target;
    if (!ParseUrl(StreamUrl(path), target)) { SetError(L"Invalid HTTP URL. URL credentials are not permitted."); return false; }
    // Normalize a credential-free single host[:port] or http://host[:port]
    // endpoint. Empty selects Windows system/PAC settings.
    std::wstring proxy_endpoint;
    if (!ProxyEndpoint(settings_.proxy_url, proxy_endpoint)) {
        SetError(L"Invalid proxy endpoint. Lists, credentials and URL paths are not permitted."); return false;
    }
    settings_.proxy_url = std::move(proxy_endpoint);
    if (!StartProbeTimer() || Interrupted()) return false;
    for (unsigned redirects = 0; redirects <= 5; ++redirects) {
        if (Interrupted()) return false;
        // proxy_mode: 0 direct, 1 proxy only destination port 80, 2 all ports.
        // An empty proxy URL selects Windows system/PAC policy. Host settings
        // (use_host_proxy) are resolved by the caller, not by the transport.
        const bool proxy = settings_.proxy_mode == 2 || (settings_.proxy_mode == 1 && target.port == 80);
        const DWORD access = !proxy ? WINHTTP_ACCESS_TYPE_NO_PROXY : settings_.proxy_url.empty() ?
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        session_ = WinHttpOpen(L"OggOpusPlayer/2.0", access,
            proxy && !settings_.proxy_url.empty() ? settings_.proxy_url.c_str() : WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_ || !WinHttpSetTimeouts(session_, settings_.timeout_ms, settings_.timeout_ms,
                settings_.timeout_ms, settings_.timeout_ms)) {
            if (!Interrupted()) SetError(L"Cannot initialize Windows HTTP", GetLastError()); return false;
        }
        connection_ = WinHttpConnect(session_, target.host.c_str(), target.port, 0);
        if (!connection_) { if (!Interrupted()) SetError(L"Cannot create the HTTP connection", GetLastError()); return false; }
        request_flags_ = target.secure ? WINHTTP_FLAG_SECURE : 0;
        const auto request = WinHttpOpenRequest(connection_, L"GET", target.resource.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, request_flags_);
        request_.store(request);
        if (!request || Interrupted()) {
            CloseRequest(request_);
            if (!Interrupted()) SetError(L"Cannot create the HTTP request", GetLastError()); return false;
        }
        if (!Request(request, kHeaders, target.host, target.secure)) return false;
        DWORD status = 0;
        if (!Status(request, status)) { if (!Interrupted()) SetError(L"Cannot read HTTP status."); return false; }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (redirects == 5) { SetError(L"Too many HTTP redirects."); return false; }
            Url next;
            if (!ParseUrl(RedirectUrl(target, Header(request, L"Location")), next)) {
                SetError(L"The HTTP redirect is invalid or contains URL credentials."); return false;
            }
            if (target.secure && !next.secure) { SetError(L"HTTPS to HTTP redirects are not permitted."); return false; }
            // Closing every request/session also discards transient credentials;
            // they are never blindly forwarded even across same-host redirects.
            CloseNetwork();
            target = std::move(next);
            continue;
        }
        if (status != 200) { SetError((L"HTTP server returned status " + std::to_wstring(status) + L".").c_str()); return false; }
        const auto encoding = Header(request, L"Content-Encoding");
        if (!encoding.empty() && _wcsicmp(encoding.c_str(), L"identity")) {
            SetError(L"The HTTP server returned a non-identity content encoding."); return false;
        }
        const auto interval = Header(request, L"icy-metaint");
        std::int64_t metaint = 0;
        if (!interval.empty() && (!Number(interval, metaint) || metaint > 16 * 1024 * 1024)) {
            SetError(L"Invalid ICY metadata interval."); return false;
        }
        icy_interval_ = icy_left_ = static_cast<int>(metaint);
        const auto length = Header(request, L"Content-Length");
        if (!length.empty() && !Number(length, response_left_)) { SetError(L"Invalid HTTP content length."); return false; }
        size_ = icy_interval_ ? -1 : response_left_;
        etag_ = Header(request, L"ETag");
        if (etag_.size() < 2 || etag_.front() != L'"' || etag_.back() != L'"' || Controls(etag_)) etag_.clear();
        last_modified_ = Header(request, L"Last-Modified");
        SYSTEMTIME date{};
        if (Controls(last_modified_) || !WinHttpTimeToSystemTime(last_modified_.c_str(), &date)) last_modified_.clear();
        // A matching size alone cannot prevent mixing same-sized revisions.
        // Enable Range only with a stable If-Range representation validator.
        http_seekable_ = settings_.http_seek && size_ >= 0 && !icy_interval_ &&
            (!etag_.empty() || !last_modified_.empty()) &&
            !_wcsicmp(Header(request, L"Accept-Ranges").c_str(), L"bytes");
        resource_ = target.resource; host_ = target.host;
        station_.name = Header(request, L"icy-name");
        station_.description = Header(request, L"icy-description");
        station_.genre = Header(request, L"icy-genre");
        station_.website = Header(request, L"icy-url");
        station_.content_type = Header(request, L"Content-Type");
        std::int64_t bitrate = 0;
        if (Number(Header(request, L"icy-br"), bitrate) && bitrate <= 100000) station_.bitrate = static_cast<int>(bitrate * 1000);
        Changed();
        if (Interrupted()) return false;
        network_buffer_.resize(static_cast<std::size_t>(settings_.stream_buffer_kb) * 1024);
        if (!http_seekable_ && capture_probe_) prefix_.reserve(static_cast<std::size_t>(probe_limit_));
        const int wanted = settings_.prebuffer_kb * 1024;
        while (network_used_ < wanted) {
            const int got = NetworkRead(network_buffer_.data() + network_used_, wanted - network_used_);
            if (got < 0) return false;
            if (!got) break;
            network_used_ += got;
        }
        return !Interrupted();
    }
    return false;
}
int InputStream::NetworkRead(unsigned char* data, int bytes) noexcept {
    if (bytes <= 0) return 0;
    if (Interrupted()) return -1;
    if (network_failed_) return -1;
    if (network_eof_ || response_left_ == 0) return 0;
    int wanted = bytes;
    if (response_left_ >= 0) wanted = static_cast<int>(std::min<std::int64_t>(wanted, response_left_));
    if (opening_) {
        if (probe_bytes_ >= probe_limit_) { SetError(L"The HTTP content probe exceeded its byte limit."); network_failed_ = true; return -1; }
        wanted = std::min(wanted, probe_limit_ - probe_bytes_);
    }
    DWORD got = 0;
    const auto request = request_.load();
    if (!request || !WinHttpReadData(request, data, static_cast<DWORD>(wanted), &got)) {
        if (!Interrupted()) SetError(L"Cannot read the HTTP body", GetLastError());
        network_failed_ = true; return -1;
    }
    if (Interrupted()) { network_failed_ = true; return -1; }
    if (!got) {
        network_eof_ = true;
        if (response_left_ > 0) { SetError(L"The HTTP response ended before its declared length."); network_failed_ = true; return -1; }
        return 0;
    }
    if (opening_) probe_bytes_ += static_cast<int>(got);
    if (response_left_ >= 0) response_left_ -= got;
    return static_cast<int>(got);
}
int InputStream::RawRead(unsigned char* data, int bytes) noexcept {
    if (Interrupted()) return -1;
    if (!http_) {
        const auto wanted = static_cast<DWORD>(std::min<std::int64_t>(bytes, size_ - position_));
        if (!wanted) return 0;
        if (cached_) { std::memcpy(data, memory_.data() + static_cast<std::size_t>(position_), wanted); return static_cast<int>(wanted); }
        DWORD got = 0;
        if (!ReadFile(file_.load(), data, wanted, &got, nullptr) || !got) {
            if (!Interrupted()) SetError(L"The audio file could not be read completely", GetLastError()); return -1;
        }
        return Interrupted() ? -1 : static_cast<int>(got);
    }
    if (network_at_ == network_used_) {
        network_at_ = network_used_ = 0;
        const int capacity = static_cast<int>(network_buffer_.size());
        const int target = opening_ ? 1 : std::max(1, capacity * settings_.underrun_percent / 100);
        do {
            const int got = NetworkRead(network_buffer_.data() + network_used_, capacity - network_used_);
            if (got <= 0) { if (!network_used_) return got; break; }
            network_used_ += got;
        } while (network_used_ < target);
    }
    const int got = std::min(bytes, network_used_ - network_at_);
    if (got) { std::memcpy(data, network_buffer_.data() + network_at_, got); network_at_ += got; }
    return got;
}
bool InputStream::Exact(unsigned char* data, int bytes) noexcept {
    while (bytes > 0) {
        const int got = RawRead(data, bytes);
        if (got <= 0) { if (!Interrupted() && !network_failed_) SetError(L"The ICY metadata block is truncated."); network_failed_ = true; return false; }
        data += got; bytes -= got;
    }
    return true;
}
void InputStream::UpdateTitle(const unsigned char* bytes, int length) noexcept {
    constexpr char key[] = "StreamTitle='";
    const auto* text = reinterpret_cast<const char*>(bytes);
    const auto* end = text + length;
    const auto* start = std::search(text, end, key, key + sizeof(key) - 1);
    if (start == end) return;
    start += sizeof(key) - 1;
    constexpr char terminator[] = "';";
    const auto* finish = std::search(start, end, terminator, terminator + sizeof(terminator) - 1);
    if (finish == end) return;
    std::array<wchar_t, 4081> title{};
    const int count = static_cast<int>(finish - start);
    int converted = count ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start, count, title.data(), static_cast<int>(title.size())) : 0;
    if (count && !converted) converted = MultiByteToWideChar(CP_ACP, 0, start, count, title.data(), static_cast<int>(title.size()));
    if (count && !converted) return;
    for (int i = 0; i < converted; ++i) if (title[i] < L' ' || title[i] == 127) title[i] = L' ';
    if (station_.title.size() == static_cast<std::size_t>(converted) &&
        std::equal(title.data(), title.data() + converted, station_.title.begin())) return;
    try { station_.title.assign(title.data(), static_cast<std::size_t>(converted)); Changed(); }
    catch (...) { SetError(L"Cannot store the ICY stream title."); }
}
int InputStream::Read(unsigned char* data, int bytes) noexcept {
    if (bytes <= 0) return 0;
    if (Interrupted()) return -1;
    if (!opened_ || !data) { SetError(L"Invalid input read request."); return -1; }
    if (replaying_) {
        const auto got = static_cast<int>(std::min<std::size_t>(bytes, prefix_.size() - replay_at_));
        if (got) {
            std::memcpy(data, prefix_.data() + replay_at_, got); replay_at_ += got; position_ += got;
            return got; // Already captured/recorded on the original pass.
        }
        replaying_ = false;
        if (!opening_) {
            prefix_.clear(); prefix_.shrink_to_fit();
            capture_probe_ = false; replay_enabled_ = false;
        }
    }
    int got = 0;
    if (http_ && icy_interval_) {
        if (!icy_left_) {
            unsigned char units = 0;
            if (!Exact(&units, 1)) return -1;
            std::array<unsigned char, 4080> metadata{};
            if (units && !Exact(metadata.data(), units * 16)) return -1;
            if (units) UpdateTitle(metadata.data(), units * 16);
            icy_left_ = icy_interval_;
        }
        got = RawRead(data, std::min(bytes, icy_left_));
        if (got > 0) icy_left_ -= got;
    } else got = RawRead(data, bytes);
    if (got > 0) {
        if (position_ > INT64_MAX - got) { SetError(L"The encoded stream position overflowed."); return -1; }
        if (opening_ && http_ && !http_seekable_ && capture_probe_) {
            if (prefix_.size() + static_cast<std::size_t>(got) > static_cast<std::size_t>(probe_limit_)) {
                SetError(L"The encoded probe prefix exceeded its byte limit."); return -1;
            }
            // Capacity was reserved once at Open; normal reads never allocate.
            prefix_.insert(prefix_.end(), data, data + got);
        }
        position_ += got;
        if (!opening_) Record(data, got);
    }
    return got;
}
bool InputStream::Seek(std::int64_t offset, int origin) noexcept {
    if (Interrupted() || !Seekable()) return false;
    const std::int64_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? position_ : origin == SEEK_END ? size_ : -1;
    if (base < 0 || offset < -base || offset > size_ - base) return false;
    const auto next = base + offset;
    if (next == position_) return true;
    if (http_seekable_) {
        if (next == size_) { position_ = next; network_at_ = network_used_ = 0; network_eof_ = true; response_left_ = 0; return true; }
        try { return Range(next); } catch (...) { SetError(L"Cannot allocate the HTTP range request."); return false; }
    }
    if (!cached_) {
        LARGE_INTEGER target{}; target.QuadPart = base_offset_ + next;
        if (!SetFilePointerEx(file_.load(), target, nullptr, FILE_BEGIN)) {
            if (!Interrupted()) SetError(L"Cannot seek in the audio file", GetLastError()); return false;
        }
    }
    if (Interrupted()) return false;
    position_ = next;
    return true;
}
bool InputStream::Range(std::int64_t offset) {
    if (Interrupted()) return false;
    const auto candidate = WinHttpOpenRequest(connection_, L"GET", resource_.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, request_flags_);
    pending_.store(candidate);
    struct Pending {
        std::atomic<HINTERNET>& slot;
        ~Pending() { CloseRequest(slot); }
    } cleanup{pending_};
    if (!candidate || Interrupted()) { if (!Interrupted()) SetError(L"Cannot create the HTTP range request."); return false; }
    auto headers = L"Icy-MetaData: 0\r\nAccept-Encoding: identity\r\nRange: bytes=" + std::to_wstring(offset) + L"-\r\n";
    if (!etag_.empty()) headers += L"If-Range: " + etag_ + L"\r\n";
    else if (!last_modified_.empty()) headers += L"If-Range: " + last_modified_ + L"\r\n";
    if (!Request(candidate, headers, host_, request_flags_ != 0)) return false;
    DWORD status = 0;
    if (!Status(candidate, status) || status != 206) {
        if (!Interrupted()) SetError(L"The remote file changed or the server rejected the byte range."); return false;
    }
    if (!ContentRange(Header(candidate, L"Content-Range"), offset, size_)) { SetError(L"Invalid HTTP Content-Range response."); return false; }
    const auto length = Header(candidate, L"Content-Length");
    std::int64_t body_size = size_ - offset;
    if (!length.empty() && (!Number(length, body_size) || body_size != size_ - offset)) {
        SetError(L"The HTTP range length does not match Content-Range."); return false;
    }
    const auto metaint = Header(candidate, L"icy-metaint");
    std::int64_t interval = 0;
    if (!metaint.empty() && (!Number(metaint, interval) || interval)) { SetError(L"ICY metadata is not allowed in HTTP byte ranges."); return false; }
    const auto encoding = Header(candidate, L"Content-Encoding");
    if (!encoding.empty() && _wcsicmp(encoding.c_str(), L"identity")) { SetError(L"The range response has a different content encoding."); return false; }
    // If-Range prevents 206 after a changed representation. Also validate the
    // returned validator rather than trusting a broken server that ignores it.
    if ((!etag_.empty() && Header(candidate, L"ETag") != etag_) ||
        (etag_.empty() && !last_modified_.empty() && Header(candidate, L"Last-Modified") != last_modified_)) {
        SetError(L"The remote file identity changed during seeking."); return false;
    }
    if (Interrupted()) return false;
    const auto replacement = pending_.exchange(nullptr);
    if (!replacement) { Interrupted(); return false; }
    const auto old = request_.exchange(replacement);
    if (old) WinHttpCloseHandle(old);
    if (Interrupted()) { CloseRequest(request_); return false; }
    network_at_ = network_used_ = 0;
    network_eof_ = network_failed_ = false;
    response_left_ = size_ - offset;
    position_ = offset;
    replaying_ = false; prefix_.clear();
    SetError(L"");
    return true;
}
bool InputStream::RewindProbe() noexcept {
    if (Interrupted() || !opened_) return false;
    if (Seekable()) return Seek(0, SEEK_SET);
    if (!http_ || !replay_enabled_ || position_ != static_cast<std::int64_t>(prefix_.size())) return false;
    replay_at_ = 0; position_ = 0; replaying_ = true;
    return true;
}
void InputStream::Record(const unsigned char* data, int bytes) noexcept {
    if (recording_ == INVALID_HANDLE_VALUE || bytes <= 0) return;
    DWORD wrote = 0;
    if (!WriteFile(recording_, data, static_cast<DWORD>(bytes), &wrote, nullptr) || wrote != static_cast<DWORD>(bytes)) {
        const auto code = GetLastError();
        CloseHandle(recording_); recording_ = INVALID_HANDLE_VALUE;
        SetRecordingError(L"Recording stopped: the destination could not be written", code);
    }
}
void InputStream::FinishProbe() noexcept {
    if (!opening_) return;
    StopProbeTimer();
    if (Interrupted()) return;
    opening_ = false;
    struct ReleasePrefix {
        std::vector<unsigned char>* bytes;
        ~ReleasePrefix() { if (bytes) { bytes->clear(); bytes->shrink_to_fit(); } }
    } release{replay_enabled_ ? nullptr : &prefix_};
    if (!replay_enabled_) capture_probe_ = false;
    if (!Radio() || !settings_.save_streams) return;
    try {
        const auto& directory = settings_.save_directory;
        const auto attributes = directory.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            SetRecordingError(L"The configured recording directory is unavailable."); return;
        }
        SYSTEMTIME now{}; GetLocalTime(&now);
        static std::atomic<unsigned> serial{0};
        std::wstring path = directory;
        if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
        wchar_t name[160]{};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            swprintf_s(name, L"Opus radio %04u-%02u-%02u %02u-%02u-%02u %llu-%u.opus",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                GetTickCount64(), serial.fetch_add(1));
            const auto destination = path + name;
            recording_ = CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (recording_ != INVALID_HANDLE_VALUE) {
                station_.recording_path = destination; Changed();
                // Encoded bytes returned during probing, already stripped of
                // ICY. Future prefix replay bypasses Record() entirely.
                Record(prefix_.data(), static_cast<int>(prefix_.size()));
                return;
            }
            if (GetLastError() != ERROR_FILE_EXISTS) break;
        }
        SetRecordingError(L"Cannot create the stream recording", GetLastError());
    } catch (...) {
        if (recording_ != INVALID_HANDLE_VALUE) { CloseHandle(recording_); recording_ = INVALID_HANDLE_VALUE; }
        SetRecordingError(L"Cannot allocate the stream recording path.");
    }
}
} // namespace winamp_opus
