#pragma once

#include <opusfile.h>
#include "channel_layout.hpp"
#include <cstdint>
#include <string>

namespace winamp_opus {
inline constexpr int kSampleRate = 48000;
inline constexpr int kBitsPerSample = 16;
enum class OutputMode { Stereo = 0, SourceChannels = 1 };
inline int BitrateKbps(int bitrate) noexcept {
    return bitrate > 0 ? static_cast<int>((static_cast<std::int64_t>(bitrate) + 500) / 1000) : 0;
}

struct TrackInfo {
    std::int64_t total_frames = 0;
    int length_ms = 0;
    int source_channels = 0;
    // 0 if links change layout or do not use standard speaker mappings.
    int native_channels = 0;
    int source_sample_rate = 0;
    int bitrate = 0;
    int links = 0;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring display_title;
};

class Decoder final {
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Local files only. All filesystem operations use wide Windows paths.
    bool Open(const wchar_t* path, int* error = nullptr);
    void Close() noexcept;
    // Choose before reading; SourceChannels returns false for unsafe layouts.
    bool SetOutputMode(OutputMode mode) noexcept;
    int OutputChannels() const noexcept { return output_channels_; }
    int ReadFrames(std::int16_t* pcm, int capacity_frames);
    bool SeekFrame(std::int64_t frame);
    bool SeekMs(int milliseconds);
    std::int64_t PositionFrames() const noexcept;
    const TrackInfo& Info() const noexcept { return info_; }

private:
    OggOpusFile* file_ = nullptr;
    TrackInfo info_;
    int pending_error_ = 0;
    bool forced_eof_ = false;
    bool native_output_ = false;
    int output_channels_ = kStereoChannels;
};

// Reads only the initial Ogg/Opus headers; never scans or decodes the whole file.
bool HasOpusContent(const wchar_t* path) noexcept;
std::wstring Utf8ToWide(const char* text);
const wchar_t* ErrorText(int code) noexcept;
} // namespace winamp_opus
