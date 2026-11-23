#pragma once
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <string>

namespace winamp_opus {
bool IsHttpUrl(const wchar_t* path) noexcept;
std::wstring StreamUrl(const wchar_t* path);
struct StationInfo {
    std::wstring name, description, genre, website, content_type;
    int bitrate = 0;
};
// A forward-only HTTP body, never an on-disk download. Windows validates TLS
// certificates and handles proxies/redirects; ICY metadata insertion is disabled.
class HttpStream final {
public:
    ~HttpStream();
    bool Open(const wchar_t* url);
    int Read(unsigned char* data, int bytes) noexcept;
    void Cancel() noexcept;
    void FinishProbe() noexcept { opening_ = false; }
    const StationInfo& Station() const noexcept { return station_; }
private:
    std::wstring Header(const wchar_t* name) const;
    HINTERNET session_ = nullptr, connection_ = nullptr;
    std::atomic<HINTERNET> request_{nullptr};
    std::atomic<bool> cancelled_{false};
    StationInfo station_;
    ULONGLONG deadline_ = 0;
    int opening_bytes_ = 0;
    bool opening_ = true;
};
} // namespace winamp_opus
