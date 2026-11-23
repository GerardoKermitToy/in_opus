// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "playback_settings.hpp"
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace winamp_opus {
bool IsHttpUrl(const wchar_t* path) noexcept;
std::wstring StreamUrl(const wchar_t* path);
struct StationInfo {
    std::wstring name, description, genre, website, content_type;
    int bitrate = 0;
    std::wstring title, recording_path, recording_error, error;
};
// Single-reader byte transport. Only Cancel() may run concurrently with Open,
// Read or Seek; destroy the object after that operation has returned. Station()
// is read on the reader thread, then copied/published only when its revision
// changes. Normal audio reads do not allocate or publish station events.
//
// Read: >0 encoded bytes, 0 clean EOF, -1 error/cancellation (Station().error).
// A broken connection is NEVER silently resumed: a playback owner must reopen
// and re-probe/recreate its decoder when applying its reconnect policy.
// FinishProbe() means the caller has validated Ogg Opus, not merely HTTP/MIME.
class InputStream final {
public:
    InputStream() = default;
    ~InputStream();
    InputStream(const InputStream&) = delete;
    InputStream& operator=(const InputStream&) = delete;
    bool Open(const wchar_t* path, const PlaybackSettings& settings = {}, std::wstring* error = nullptr,
        bool replay_probe = false);
    int Read(unsigned char* data, int bytes) noexcept;
    bool Seek(std::int64_t offset, int origin) noexcept;
    std::int64_t Tell() const noexcept { return position_; }
    std::int64_t Size() const noexcept { return size_; }
    bool Seekable() const noexcept { return opened_ && (!http_ || http_seekable_); }
    bool Radio() const noexcept { return opened_ && http_ && !http_seekable_; }
    void Cancel() noexcept;
    void FinishProbe() noexcept;
    // Pass replay_probe=true to Open when sniffing before creating a decoder.
    // Replays only the bytes already returned during probing, without refetching
    // or recording twice. Capture is otherwise enabled only for recording.
    // Call before consuming fresh post-probe bytes; seekable inputs seek to 0.
    bool RewindProbe() noexcept;
    const StationInfo& Station() const noexcept { return station_; }
    unsigned StationRevision() const noexcept { return revision_.load(std::memory_order_acquire); }
private:
    bool OpenLocal(const wchar_t* path);
    bool OpenHttp(const wchar_t* path);
    bool Range(std::int64_t offset);
    bool Request(HINTERNET request, const std::wstring& headers, const std::wstring& server, bool secure);
    bool ConfigureRequest(HINTERNET request) noexcept;
    static std::wstring Header(HINTERNET request, const wchar_t* name);
    int RawRead(unsigned char* data, int bytes) noexcept;
    int NetworkRead(unsigned char* data, int bytes) noexcept;
    bool Exact(unsigned char* data, int bytes) noexcept;
    bool ReadFileAt(std::int64_t offset, void* data, DWORD bytes) noexcept;
    bool Interrupted() noexcept;
    void SetError(const wchar_t* text, DWORD windows_error = 0) noexcept;
    void SetRecordingError(const wchar_t* text, DWORD windows_error = 0) noexcept;
    void UpdateTitle(const unsigned char* data, int bytes) noexcept;
    void Record(const unsigned char* data, int bytes) noexcept;
    bool StartProbeTimer();
    void StopProbeTimer() noexcept;
    void CloseNetwork() noexcept;
    void CloseResources() noexcept;
    void Changed() noexcept { revision_.fetch_add(1, std::memory_order_release); }

    std::atomic<HANDLE> file_{INVALID_HANDLE_VALUE};
    HANDLE recording_ = INVALID_HANDLE_VALUE;
    HINTERNET session_ = nullptr, connection_ = nullptr;
    std::atomic<HINTERNET> request_{nullptr}, pending_{nullptr};
    std::atomic<bool> cancelled_{false}, probe_expired_{false}, probe_active_{false};
    std::atomic<HANDLE> probe_stop_{nullptr};
    std::thread probe_timer_;
    std::atomic<unsigned> revision_{0};
    PlaybackSettings settings_;
    StationInfo station_;
    std::vector<unsigned char> memory_, prefix_, network_buffer_;
    std::int64_t position_ = 0, size_ = -1, base_offset_ = 0, response_left_ = -1;
    std::size_t replay_at_ = 0;
    int network_at_ = 0, network_used_ = 0, probe_bytes_ = 0, probe_limit_ = 0;
    int icy_interval_ = 0, icy_left_ = 0;
    ULONGLONG deadline_ = 0;
    bool attempted_ = false, opened_ = false, http_ = false, cached_ = false, capture_probe_ = false;
    bool opening_ = true, replaying_ = false, replay_enabled_ = false, http_seekable_ = false;
    bool network_eof_ = false, network_failed_ = false;
    std::wstring resource_, host_, etag_, last_modified_;
    DWORD request_flags_ = 0;
};
using HttpStream = InputStream;
} // namespace winamp_opus
