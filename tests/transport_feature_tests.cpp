// SPDX-License-Identifier: GPL-3.0-only
#include <winsock2.h>
#include <ws2tcpip.h>
#include "fixtures.hpp"
#include "http_stream.hpp"
#include <opusfile.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <crtdbg.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

// Counts this test thread's C++ allocations, not WinHTTP's internal OS heaps.
// The transport must allocate on metadata changes, not on every read/title.
namespace { thread_local bool counting_allocations = false; thread_local unsigned allocation_count = 0; }
void* operator new(std::size_t size) {
    if (counting_allocations) ++allocation_count;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace winamp_opus;
namespace {
using Bytes = std::vector<unsigned char>;
Bytes Load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Check(file.good(), "open fixture bytes");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Save(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(file.good(), "save transport fixture");
}
void Le32(Bytes& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
void Chunk(Bytes& out, const char* name, const Bytes& bytes) {
    out.insert(out.end(), name, name + 4); Le32(out, static_cast<std::uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
    if (bytes.size() & 1) out.push_back(0);
}
Bytes Wave(const Bytes& opus) {
    Bytes body{'W','A','V','E'};
    Chunk(body, "JUNK", {1, 2, 3}); Chunk(body, "fmt ", Bytes(16));
    Chunk(body, "data", opus); Chunk(body, "TAIL", Bytes(17, 0xe7));
    Bytes out{'R','I','F','F'}; Le32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end()); return out;
}
Bytes Drain(InputStream& stream, bool expect_error = false) {
    Bytes out;
    std::array<unsigned char, 7003> data{};
    for (;;) {
        const auto got = stream.Read(data.data(), static_cast<int>(data.size()));
        if (got <= 0) { Check(expect_error ? got < 0 : got == 0, "EOF/error distinction"); break; }
        Check(got <= static_cast<int>(data.size()), "bounded transport read");
        out.insert(out.end(), data.begin(), data.begin() + got);
    }
    return out;
}
void Decode(const Bytes& bytes, std::int64_t frames) {
    int error = 0;
    auto* file = op_open_memory(bytes.data(), bytes.size(), &error);
    Check(file && !error && op_channel_count(file, -1) == 2, "stripped transport is real stereo Ogg Opus");
    std::array<float, 576 * 2> pcm{};
    std::int64_t decoded = 0;
    for (;;) {
        const auto got = op_read_float(file, pcm.data(), static_cast<int>(pcm.size()), nullptr);
        if (got <= 0) { op_free(file); Check(got == 0 && decoded == frames, "real Opus decoded frame count"); break; }
        decoded += got;
    }
}
Bytes Icy(const Bytes& opus, int interval = 1024) {
    const std::string title = "StreamTitle='Loopback \xd0\x9e\xd0\xbf\xd1\x83\xd1\x81';";
    const auto units = (title.size() + 15) / 16;
    Bytes wire;
    for (std::size_t at = 0; at < opus.size();) {
        const auto count = std::min<std::size_t>(interval, opus.size() - at);
        wire.insert(wire.end(), opus.begin() + at, opus.begin() + at + count); at += count;
        if (count == static_cast<std::size_t>(interval)) {
            wire.push_back(static_cast<unsigned char>(units));
            wire.insert(wire.end(), title.begin(), title.end());
            wire.insert(wire.end(), units * 16 - title.size(), 0);
        }
    }
    return wire;
}
std::string RequestHeader(const std::string& request, const std::string& name) {
    const auto at = request.find("\r\n" + name + ": ");
    if (at == std::string::npos) return {};
    const auto first = at + name.size() + 4;
    return request.substr(first, request.find("\r\n", first) - first);
}
class Server final {
public:
    explicit Server(Bytes opus) : opus_(std::move(opus)), icy_(Icy(opus_)) {
        WSADATA data{}; Check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); Check(listener_ != INVALID_SOCKET, "loopback socket");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind transport fixture");
        int bytes = sizeof(address);
        Check(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &bytes) == 0 && listen(listener_, 16) == 0, "listen transport fixture");
        port_ = ntohs(address.sin_port);
        acceptor_ = std::thread([this] { Accept(); });
    }
    ~Server() {
        stop_ = true;
        if (acceptor_.joinable()) acceptor_.join();
        closesocket(listener_);
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
        WSACleanup();
    }
    std::wstring Url(const wchar_t* path) const { return L"http://127.0.0.1:" + std::to_wstring(port_) + path; }
    std::wstring Proxy() const { return L"127.0.0.1:" + std::to_wstring(port_); }
    unsigned Requests() const noexcept { return requests_.load(); }
    unsigned RangeRequests() const noexcept { return ranges_.load(); }
    bool SawIfRange() const noexcept { return saw_if_range_.load(); }
    bool SawDateRange() const noexcept { return saw_date_range_.load(); }
    bool LeakedAuth() const noexcept { return leaked_auth_.load(); }
    void CrossRedirect(const std::wstring& url) {
        for (const auto ch : url) { Check(ch < 128, "ASCII loopback redirect URL"); cross_url_.push_back(static_cast<char>(ch)); }
        // Set before requests; tests never write it concurrently with workers.
    }
private:
    static bool Send(SOCKET socket, const void* bytes, std::size_t length) noexcept {
        auto* data = static_cast<const char*>(bytes);
        while (length) {
            const auto wanted = static_cast<int>(std::min<std::size_t>(length, 4093));
            const auto got = send(socket, data, wanted, 0);
            if (got <= 0) return false;
            data += got; length -= got;
        }
        return true;
    }
    static bool Headers(SOCKET socket, const std::string& fields, int status = 200) {
        const auto text = "HTTP/1.1 " + std::to_string(status) + " Fixture\r\nConnection: close\r\n" + fields + "\r\n";
        return Send(socket, text.data(), text.size());
    }
    void WaitClosed(SOCKET socket) noexcept {
        for (unsigned ticks = 0; !stop_ && ticks < 300; ++ticks) {
            fd_set ready; FD_ZERO(&ready); FD_SET(socket, &ready); timeval timeout{0, 50000};
            if (select(0, &ready, nullptr, nullptr, &timeout) > 0) {
                char byte{}; if (recv(socket, &byte, 1, MSG_PEEK) <= 0) break;
            }
        }
    }
    void Accept() noexcept {
        while (!stop_) {
            fd_set ready; FD_ZERO(&ready); FD_SET(listener_, &ready); timeval timeout{0, 50000};
            if (select(0, &ready, nullptr, nullptr, &timeout) <= 0) continue;
            const auto client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            workers_.emplace_back([this, client] {
                try { Serve(client); } catch (...) { failed_ = true; }
                shutdown(client, SD_BOTH); closesocket(client);
            });
        }
    }
    void Serve(SOCKET client) {
        DWORD timeout = 1500;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request;
        std::array<char, 2048> buffer{};
        while (request.size() < 32768 && request.find("\r\n\r\n") == std::string::npos) {
            const auto got = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (got <= 0) return;
            request.append(buffer.data(), got);
        }
        ++requests_;
        const auto space = request.find(' '), end = request.find(' ', space + 1);
        std::string path = request.substr(space + 1, end - space - 1);
        if (path.compare(0, 7, "http://") == 0) {
            const auto slash = path.find('/', 7); path = slash == std::string::npos ? "/" : path.substr(slash);
        }
        if (const auto query = path.find('?'); query != std::string::npos) path.resize(query);
        const auto authorization = RequestHeader(request, "Authorization");
        const bool authenticated = path.find("digest") != std::string::npos ?
            authorization.find("Digest ") == 0 && authorization.find("fixture-user") != std::string::npos :
            authorization == "Basic Zml4dHVyZS11c2VyOmZpeHR1cmUtc2VjcmV0";
        if (path == "/check-no-auth" && !authorization.empty()) leaked_auth_ = true;
        if (path.find("/auth-") == 0 && !authenticated) {
            Headers(client, path.find("digest") != std::string::npos ?
                "WWW-Authenticate: Digest realm=\"fixture\", nonce=\"fixture-nonce\", qop=\"auth\", algorithm=MD5\r\nContent-Length: 0\r\n" :
                "WWW-Authenticate: Basic realm=\"fixture\"\r\nContent-Length: 0\r\n", 401);
            return;
        }
        if (path == "/auth-cross") { Headers(client, "Location: " + cross_url_ + "\r\nContent-Length: 0\r\n", 302); return; }
        if (path == "/nested/redirect") { Headers(client, "Location: ../file?redirected=1\r\nContent-Length: 0\r\n", 302); return; }
        if (path == "/credential-redirect") { Headers(client, "Location: http://user:secret@127.0.0.1/file\r\nContent-Length: 0\r\n", 302); return; }
        if (path == "/redirect-loop") { Headers(client, "Location: /redirect-loop\r\nContent-Length: 0\r\n", 302); return; }
        if (path == "/stall-open") { WaitClosed(client); return; }
        if (path == "/stall-body") { Headers(client, "Content-Type: audio/ogg\r\n"); WaitClosed(client); return; }
        if (path == "/bad-icy") { Headers(client, "icy-metaint: 12oops\r\nContent-Length: 0\r\n"); return; }
        if (path == "/huge-icy") { Headers(client, "icy-metaint: 16777217\r\nContent-Length: 0\r\n"); return; }
        if (path == "/bad-length") { Headers(client, "Content-Length: -2\r\n"); return; }
        if (path == "/encoded") { Headers(client, "Content-Encoding: gzip\r\nContent-Length: 0\r\n"); return; }
        if (path == "/icy" || path == "/icy-cut") {
            const Bytes cut(1024, 'x');
            auto body = path == "/icy" ? icy_ : cut;
            if (path == "/icy-cut") { body.push_back(2); body.push_back('x'); }
            Headers(client, "Content-Type: audio/ogg\r\nicy-name: ICY fixture\r\nicy-metaint: 1024\r\nContent-Length: " + std::to_string(body.size()) + "\r\n");
            Send(client, body.data(), body.size()); return;
        }
        const auto range = RequestHeader(request, "Range");
        if (!range.empty()) {
            ++ranges_;
            if (path == "/slow-range") { WaitClosed(client); return; }
            std::int64_t first = 0;
            try { first = std::stoll(range.substr(6)); } catch (...) { Headers(client, "Content-Length: 0\r\n", 416); return; }
            if (RequestHeader(request, "If-Range") == "\"fixture-v1\"") saw_if_range_ = true;
            if (RequestHeader(request, "If-Range") == "Mon, 02 Nov 2026 00:00:00 GMT") saw_date_range_ = true;
            if (first < 0 || first >= static_cast<std::int64_t>(opus_.size())) { Headers(client, "Content-Length: 0\r\n", 416); return; }
            const auto count = opus_.size() - static_cast<std::size_t>(first);
            auto content = "bytes " + std::to_string(first) + "-" + std::to_string(opus_.size() - 1) + "/" + std::to_string(opus_.size());
            if (path == "/wrong-first") content = "bytes " + std::to_string(first + 1) + "-" + std::to_string(opus_.size() - 1) + "/" + std::to_string(opus_.size());
            if (path == "/wrong-total") content += "0";
            if (path == "/range-garbage") content += " garbage";
            if (path == "/unknown-total") content = "bytes " + std::to_string(first) + "-" + std::to_string(opus_.size() - 1) + "/*";
            const auto validator = path == "/changed" ? "\"fixture-v2\"" : "\"fixture-v1\"";
            const auto etag = path == "/last-modified" ? "ETag: W/\"weak\"\r\nLast-Modified: Mon, 02 Nov 2026 00:00:00 GMT\r\n" :
                "ETag: " + std::string(validator) + "\r\n";
            Headers(client, "Content-Type: audio/ogg\r\n" + etag + "Content-Range: " + content + "\r\nContent-Length: " +
                std::to_string(path == "/wrong-length" ? count + 1 : count) + "\r\n", path == "/ignored-range" ? 200 : 206);
            Send(client, opus_.data() + first, count); return;
        }
        std::string fields = "Content-Type: audio/ogg\r\nicy-name: Loopback Opus\r\nicy-br: 128\r\n";
        if (path != "/plain" && path != "/auth-basic" && path != "/auth-digest" && path != "/check-no-auth") {
            fields += "Accept-Ranges: bytes\r\nContent-Length: " + std::to_string(opus_.size() + (path == "/truncated" ? 13 : 0)) + "\r\n";
        if (path != "/no-validator") fields += path == "/last-modified" ? "ETag: W/\"weak\"\r\nLast-Modified: Mon, 02 Nov 2026 00:00:00 GMT\r\n" : "ETag: \"fixture-v1\"\r\n";
        }
        Headers(client, fields); Send(client, opus_.data(), opus_.size());
    }
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    Bytes opus_, icy_;
    std::string cross_url_;
    std::atomic<bool> stop_{false}, failed_{false}, saw_if_range_{false}, saw_date_range_{false}, leaked_auth_{false};
    std::atomic<unsigned> requests_{0}, ranges_{0};
    std::thread acceptor_;
    std::vector<std::thread> workers_;
};
void CheckLocal(const std::filesystem::path& directory, const Bytes& opus) {
    const auto unicode = directory / L"Unicode Опус 音.opus";
    Save(unicode, opus);
    for (bool cache : {false, true}) {
        PlaybackSettings settings; settings.buffer_small_files = cache;
        InputStream stream; Check(stream.Open(unicode.c_str(), settings), "Unicode local Open");
        Check(stream.Seekable() && !stream.Radio() && stream.Size() == static_cast<std::int64_t>(opus.size()), "local flags/size");
        Check(Drain(stream) == opus, "local cached/uncached exact bytes");
        Check(stream.Seek(0, SEEK_SET) && stream.Seek(12, SEEK_CUR) && stream.Tell() == 12, "local SET/CUR seeks");
        Check(!stream.Seek(INT64_MIN, SEEK_CUR) && !stream.Seek(INT64_MAX, SEEK_CUR) && !stream.Seek(-1, SEEK_SET) &&
            !stream.Seek(0, 192) && stream.Tell() == 12, "local invalid/overflow seeks leave position");
        Check(stream.Seek(-5, SEEK_END), "local END seek");
        std::array<unsigned char, 64> tail{};
        Check(stream.Read(tail.data(), 64) == 5 && std::equal(tail.begin(), tail.begin() + 5, opus.end() - 5), "exact local upper bound");
        Check(stream.Read(tail.data(), 64) == 0 && stream.Read(nullptr, 0) == 0, "local clean EOF");
    }
    const auto historical = directory / L"old\xff5cname.opus";
    Save(historical, opus);
    InputStream pipe; Check(pipe.Open((directory / L"old|name.opus").c_str()) && Drain(pipe) == opus, "historical pipe fallback");
    const auto wave = directory / L"wrapped.wav"; Save(wave, Wave(opus));
    for (bool cache : {false, true}) {
        PlaybackSettings settings; settings.buffer_small_files = cache;
        InputStream stream; Check(stream.Open(wave.c_str(), settings) && stream.Size() == static_cast<std::int64_t>(opus.size()), "RIFF logical data bounds");
        Check(Drain(stream) == opus && stream.Seek(0, SEEK_SET) && stream.Tell() == 0, "RIFF excludes pad/trailing chunks and seeks to data");
    }
    auto malformed = Wave(opus); malformed[4] = malformed[5] = malformed[6] = malformed[7] = 255;
    const auto broken = directory / L"broken.wav"; Save(broken, malformed);
    InputStream bad; Check(!bad.Open(broken.c_str()) && !bad.Station().error.empty(), "reject truncated RIFF length");
    const auto empty = directory / L"empty.opus"; Save(empty, {});
    PlaybackSettings cached; cached.buffer_small_files = true;
    InputStream zero; Check(zero.Open(empty.c_str(), cached) && zero.Size() == 0 && zero.Seek(0, SEEK_END), "cached empty input");
    std::array<unsigned char, 32> data{}; Check(zero.Read(data.data(), 32) == 0, "empty EOF");
    // Exercise the inclusive 64 MiB cache cap without relying on private state.
    // Cached bytes remain unchanged on an in-place edit; over-cap bytes do not.
    for (const std::int64_t size : {64LL * 1024 * 1024, 64LL * 1024 * 1024 + 1}) {
        const auto path = directory / (size & 1 ? L"over-cap.bin" : L"at-cap.bin");
        { std::ofstream file(path, std::ios::binary); file.put('A'); file.seekp(size - 1); file.put('Z'); Check(file.good(), "cache boundary fixture"); }
        InputStream stream; Check(stream.Open(path.c_str(), cached), "cache boundary Open");
        { std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary); file.put('B'); Check(file.good(), "edit cache boundary fixture"); }
        Check(stream.Read(data.data(), 1) == 1 && data[0] == (size & 1 ? 'B' : 'A'), "inclusive 64 MiB cache bound");
    }
    // Playback's opened file identity survives atomic tag-writer replacement.
    const auto stable = directory / L"stable.opus", replacement = directory / L"replacement.opus";
    Save(stable, opus); auto edited = opus; edited[0] ^= 1; Save(replacement, edited);
    InputStream old; Check(old.Open(stable.c_str()), "open stable audio handle");
    Check(ReplaceFileW(stable.c_str(), replacement.c_str(), nullptr, 0, nullptr, nullptr) != FALSE, "atomic replace while local stream open");
    Check(Drain(old) == opus, "active stream retains old file after replacement");
    InputStream fresh; Check(fresh.Open(stable.c_str()) && Drain(fresh) == edited, "fresh stream sees replacement");
    InputStream cancelled; cancelled.Cancel(); Check(!cancelled.Open(unicode.c_str()) && !cancelled.Station().error.empty(), "pre-Open local cancellation preserved");
}
void CheckRange(Server& server, const Bytes& opus) {
    PlaybackSettings settings; settings.proxy_mode = 0; settings.http_seek = true;
    InputStream stream; Check(stream.Open(server.Url(L"/file").c_str(), settings), "known-length HTTP Open");
    Check(stream.Seekable() && !stream.Radio() && stream.Size() == static_cast<std::int64_t>(opus.size()), "HTTP seek flags/size");
    stream.FinishProbe();
    Check(stream.Seek(123, SEEK_SET) && stream.Tell() == 123, "HTTP Range SET");
    std::array<unsigned char, 31> bytes{};
    Check(stream.Read(bytes.data(), static_cast<int>(bytes.size())) == static_cast<int>(bytes.size()) &&
        std::equal(bytes.begin(), bytes.end(), opus.begin() + 123), "HTTP Range exact bytes");
    Check(stream.Seek(-7, SEEK_END) && Drain(stream) == Bytes(opus.end() - 7, opus.end()), "HTTP Range END and upper bound");
    const auto requests = server.RangeRequests();
    Check(stream.Seek(0, SEEK_END) && stream.Read(bytes.data(), 31) == 0 && server.RangeRequests() == requests, "virtual exact HTTP EOF without invalid Range");
    Check(!stream.Seek(1, SEEK_END) && !stream.Seek(INT64_MIN, SEEK_CUR), "HTTP invalid seek bounds");
    Check(stream.Seek(0, SEEK_SET) && Drain(stream) == opus && server.SawIfRange(), "HTTP If-Range identity and complete restart");
    InputStream unvalidated; Check(unvalidated.Open(server.Url(L"/no-validator").c_str(), settings), "unvalidated range fixture Open");
    Check(!unvalidated.Seekable() && unvalidated.Radio() && !unvalidated.Seek(1, SEEK_SET), "Range requires a stable representation validator");
    unvalidated.FinishProbe(); Check(Drain(unvalidated) == opus, "unvalidated file remains forward-only");
    InputStream modified; Check(modified.Open(server.Url(L"/last-modified").c_str(), settings), "Last-Modified fixture Open");
    modified.FinishProbe(); Check(modified.Seek(55, SEEK_SET) && server.SawDateRange(), "Last-Modified fallback ignores weak ETag");
    for (const auto* path : {L"/wrong-first", L"/wrong-total", L"/range-garbage", L"/unknown-total", L"/wrong-length", L"/changed", L"/ignored-range"}) {
        InputStream invalid; Check(invalid.Open(server.Url(path).c_str(), settings), "invalid range fixture Open");
        invalid.FinishProbe(); Check(!invalid.Seek(7, SEEK_SET) && invalid.Tell() == 0 && !invalid.Station().error.empty(), "strict HTTP range rejection");
        Check(Drain(invalid) == opus, "failed Range retains prior response without mixing bytes");
    }
}
HWND PromptWindow(DWORD thread) {
    struct Result { HWND window = nullptr; } result;
    EnumThreadWindows(thread, [](HWND window, LPARAM raw) -> BOOL {
        wchar_t name[128]{}; GetWindowTextW(window, name, 128);
        if (!wcscmp(name, L"Opus Stream Authentication") && GetDlgItem(window, 100) && GetDlgItem(window, 101)) {
            reinterpret_cast<Result*>(raw)->window = window; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result.window;
}
void Authenticate(InputStream& stream, const std::wstring& url, bool cancel) {
    PlaybackSettings settings; settings.proxy_mode = 0; settings.interactive_auth = true; settings.timeout_ms = 10000;
    std::atomic<DWORD> worker{0};
    auto pending = std::async(std::launch::async, [&] { worker = GetCurrentThreadId(); return stream.Open(url.c_str(), settings); });
    HWND dialog = nullptr;
    const auto deadline = GetTickCount64() + 3000;
    while (!dialog && GetTickCount64() < deadline && pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        dialog = PromptWindow(worker.load()); Sleep(10);
    }
    if (!dialog) { stream.Cancel(); pending.wait(); Check(false, "cancellable native authentication prompt appeared"); }
    if (cancel) stream.Cancel();
    else {
        SetDlgItemTextW(dialog, 100, L"fixture-user"); SetDlgItemTextW(dialog, 101, L"fixture-secret");
        if (const auto consent = GetDlgItem(dialog, 102)) SendMessageW(consent, BM_CLICK, 0, 0);
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    }
    Check(pending.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "authentication completion/cancellation is bounded");
    Check(pending.get() != cancel, "authentication result");
}
void CheckCancellation(Server& server) {
    PlaybackSettings settings; settings.proxy_mode = 0; settings.timeout_ms = 10000;
    for (const bool prebuffer : {false, true}) {
        InputStream stream;
        if (prebuffer) settings.prebuffer_kb = 16;
        auto pending = std::async(std::launch::async, [&] { return stream.Open(server.Url(prebuffer ? L"/stall-body" : L"/stall-open").c_str(), settings); });
        Sleep(100); stream.Cancel();
        Check(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready && !pending.get(), "Cancel interrupts Open/headers/prebuffer");
    }
    settings.prebuffer_kb = 0;
    InputStream body; Check(body.Open(server.Url(L"/stall-body").c_str(), settings), "stalled Read fixture Open"); body.FinishProbe();
    auto reading = std::async(std::launch::async, [&] { std::array<unsigned char, 512> data{}; return body.Read(data.data(), 512); });
    Sleep(100); body.Cancel();
    Check(reading.wait_for(std::chrono::seconds(2)) == std::future_status::ready && reading.get() < 0 && !body.Station().error.empty(), "Cancel interrupts Read after probe");
    settings.http_seek = true;
    InputStream seeking; Check(seeking.Open(server.Url(L"/slow-range").c_str(), settings), "stalled Range fixture Open"); seeking.FinishProbe();
    auto range = std::async(std::launch::async, [&] { return seeking.Seek(100, SEEK_SET); });
    Sleep(100); seeking.Cancel();
    Check(range.wait_for(std::chrono::seconds(2)) == std::future_status::ready && !range.get(), "Cancel interrupts pending Range");
    InputStream prior; prior.Cancel(); const auto requests = server.Requests();
    Check(!prior.Open(server.Url(L"/file").c_str(), settings) && server.Requests() == requests, "pre-Open HTTP cancellation preserved");
}
void CheckProbe(Server& server, const Bytes& opus) {
    PlaybackSettings settings; settings.proxy_mode = 0; settings.probe_kb = 64; settings.stream_buffer_kb = 32;
    InputStream capped; Check(capped.Open(server.Url(L"/plain").c_str(), settings), "probe budget fixture Open");
    const auto bytes = Drain(capped, true);
    Check(bytes.size() <= 64 * 1024 && bytes.size() < opus.size() && !capped.Station().error.empty(), "configured probe byte bound");
    InputStream unlimited; Check(unlimited.Open(server.Url(L"/plain").c_str(), settings), "post-probe fixture Open"); unlimited.FinishProbe();
    Check(Drain(unlimited) == opus, "FinishProbe removes byte budget");
    settings.timeout_ms = 1000;
    const auto start = GetTickCount64();
    InputStream timed; Check(!timed.Open(server.Url(L"/stall-open").c_str(), settings) && GetTickCount64() - start < 3000 &&
        !timed.Station().error.empty(), "absolute probe deadline bounds blocked Open");
    InputStream buffered; Check(buffered.Open(server.Url(L"/plain").c_str(), settings), "idle probe fixture Open");
    Sleep(1150); std::array<unsigned char, 16> data{};
    Check(buffered.Read(data.data(), 16) < 0, "probe timeout cannot be bypassed by buffered data");
}
void CheckRecording(Server& server, const Bytes& opus, const std::filesystem::path& directory, std::int64_t frames) {
    PlaybackSettings settings; settings.proxy_mode = 0; settings.save_streams = true; settings.save_directory = directory.wstring();
    settings.prebuffer_kb = 16; settings.stream_buffer_kb = 32; settings.underrun_percent = 50;
    std::wstring destination;
    {
        InputStream stream; Check(stream.Open(server.Url(L"/icy").c_str(), settings, nullptr, true) && stream.Radio() && !stream.Seekable() && stream.Size() == -1, "ICY Open and encoded size unknown");
        Check(stream.Station().recording_path.empty(), "no recording before content validation");
        std::array<unsigned char, 1150> probe{};
        Bytes prefix;
        while (prefix.size() < 3000) {
            const auto got = stream.Read(probe.data(), static_cast<int>(probe.size())); Check(got > 0, "encoded ICY probe");
            prefix.insert(prefix.end(), probe.begin(), probe.begin() + got);
        }
        Check(stream.Station().title == L"Loopback Опус", "ICY StreamTitle UTF-8");
        Check(stream.RewindProbe() && stream.Tell() == 0, "nonseekable prefix replay");
        stream.FinishProbe(); destination = stream.Station().recording_path;
        Check(!destination.empty() && stream.Station().recording_error.empty(), "opt-in recording starts only after probe");
        const auto revision = stream.StationRevision();
        Bytes decoded;
        for (;;) {
            allocation_count = 0; counting_allocations = true;
            const int got = stream.Read(probe.data(), static_cast<int>(probe.size()));
            counting_allocations = false;
            Check(allocation_count == 0, "ordinary audio reads/repeated ICY title allocate nothing");
            Check(stream.StationRevision() == revision, "unchanged title/audio reads do not publish station events");
            Check(got >= 0, "recorded ICY Read"); if (!got) break;
            decoded.insert(decoded.end(), probe.begin(), probe.begin() + got);
        }
        Check(decoded == opus && stream.Tell() == static_cast<std::int64_t>(opus.size()), "ICY stripping and prefix replay exact bytes/position");
        Decode(decoded, frames);
        stream.FinishProbe(); Check(stream.Station().recording_path == destination, "FinishProbe is idempotent");
    }
    Check(Load(destination) == opus, "recording includes probe once, no ICY or network replay duplicates");
    // Recording failures are independent metadata events, not audio failures.
    settings.save_directory = (directory / L"missing-directory").wstring();
    InputStream unavailable; Check(unavailable.Open(server.Url(L"/plain").c_str(), settings), "bad recording directory fixture Open");
    const auto revision = unavailable.StationRevision(); unavailable.FinishProbe();
    Check(!unavailable.Station().recording_error.empty() && unavailable.StationRevision() != revision &&
        unavailable.Station().error.empty() && Drain(unavailable) == opus, "recording error publishes once without breaking playback");
}
void CheckProxy(Server& server, const Bytes& opus) {
    PlaybackSettings settings; settings.proxy_mode = 2; settings.proxy_url = server.Proxy();
    InputStream named; Check(named.Open(L"http://transport-fixture.invalid:18081/plain", settings), "manual endpoint proxy Open");
    named.FinishProbe(); Check(Drain(named) == opus, "manual proxy transports encoded Opus");
    settings.proxy_url = L"http://" + server.Proxy();
    InputStream scheme; Check(scheme.Open(L"http://transport-fixture.invalid:18081/plain", settings), "http scheme proxy normalization");
    scheme.FinishProbe(); Check(Drain(scheme) == opus, "normalized proxy exact bytes");
    settings.proxy_mode = 1; settings.proxy_url = L"127.0.0.1:1";
    InputStream only80; Check(only80.Open(server.Url(L"/plain").c_str(), settings), "port-80-only proxy bypass on custom destination port");
    only80.FinishProbe(); Check(Drain(only80) == opus, "port policy direct body");
    settings.proxy_mode = 0;
    InputStream direct; Check(direct.Open(server.Url(L"/plain").c_str(), settings), "explicit no-proxy policy");
    direct.FinishProbe(); Check(Drain(direct) == opus, "no proxy exact body");
    for (const auto* endpoint : {L"user:secret@host:80", L"host:80;other:80", L"host:80/path", L"host:0", L"host:65536", L"host\r\nBad: x"}) {
        settings.proxy_url = endpoint;
        InputStream invalid; Check(!invalid.Open(server.Url(L"/plain").c_str(), settings), "reject credential/list/control/malformed proxy");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    for (const int report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
#endif
    try {
        Check(argc == 2, "usage: transport_feature_tests DATA_DIR");
        const std::filesystem::path directory(argv[1]); std::filesystem::create_directories(directory);
        const auto fixture = directory / L"generated.opus";
        constexpr std::int64_t frames = 6 * 48000;
        MakeOpus(fixture, 2, static_cast<int>(frames), 23031);
        const auto opus = Load(fixture); Check(opus.size() > 64 * 1024, "generated Opus exceeds minimum probe cap"); Decode(opus, frames);
        std::cerr << "transport: local/cache/RIFF\n";
        CheckLocal(directory, opus);
        std::cerr << "transport: HTTP basics\n";
        Server server(opus), other(opus); server.CrossRedirect(other.Url(L"/check-no-auth"));
        PlaybackSettings settings; settings.proxy_mode = 0;
        HttpStream compatible; Check(compatible.Open(server.Url(L"/plain").c_str()), "legacy HttpStream.Open(url) API");
        compatible.FinishProbe(); Check(Drain(compatible) == opus, "legacy byte reads unchanged");
        InputStream redirected; Check(redirected.Open(server.Url(L"/nested/redirect").c_str(), settings), "relative HTTP redirect");
        redirected.FinishProbe(); Check(Drain(redirected) == opus, "redirected Opus body");
        for (const auto* path : {L"/bad-icy", L"/huge-icy", L"/bad-length", L"/encoded", L"/credential-redirect", L"/redirect-loop", L"/auth-basic"}) {
            InputStream invalid; std::wstring error;
            Check(!invalid.Open(server.Url(path).c_str(), settings, &error) && !error.empty() && error == invalid.Station().error, "unsafe/malformed HTTP fails with Station error");
        }
        InputStream userinfo; Check(!userinfo.Open(L"http://user:secret@127.0.0.1/plain", settings), "reject URL credentials");
        InputStream truncated; Check(truncated.Open(server.Url(L"/truncated").c_str(), settings), "truncated body fixture Open");
        truncated.FinishProbe(); Drain(truncated, true); Check(!truncated.Station().error.empty(), "truncation is error not clean EOF");
        InputStream icy_cut; Check(icy_cut.Open(server.Url(L"/icy-cut").c_str(), settings), "truncated ICY fixture Open"); icy_cut.FinishProbe();
        Drain(icy_cut, true); Check(!icy_cut.Station().error.empty(), "truncated ICY metadata is error");
        std::cerr << "transport: Range\n"; CheckRange(server, opus);
        std::cerr << "transport: recording\n"; CheckRecording(server, opus, directory, frames);
        std::cerr << "transport: cancellation\n"; CheckCancellation(server);
        std::cerr << "transport: probe bounds\n"; CheckProbe(server, opus);
        std::cerr << "transport: proxy policy\n"; CheckProxy(server, opus);
        std::cerr << "transport: authentication\n";
        InputStream basic; Authenticate(basic, server.Url(L"/auth-basic"), false); basic.FinishProbe(); Check(Drain(basic) == opus, "transient Basic authentication");
        InputStream digest; Authenticate(digest, server.Url(L"/auth-digest"), false); digest.FinishProbe(); Check(Drain(digest) == opus, "transient Digest authentication");
        InputStream cross; Authenticate(cross, server.Url(L"/auth-cross"), false); cross.FinishProbe();
        Check(Drain(cross) == opus && !other.LeakedAuth(), "credentials never blindly forwarded across host/port redirect");
        InputStream auth_cancel; Authenticate(auth_cancel, server.Url(L"/auth-basic"), true);
        std::cout << "Transport: Unicode/local identity/cache/RIFF, ICY real Opus, strict Range/identity, recording/replay, bounded probe, proxy, auth and cancellation passed.\n";
        return 0;
    } catch (const std::exception& error) {
        counting_allocations = false;
        std::cerr << error.what() << '\n'; return 1;
    }
}
