#pragma once

#include <opusfile.h>
#include "channel_layout.hpp"
#include "playback_settings.hpp"
#include "http_stream.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace winamp_opus {
inline constexpr int kSampleRate = 48000;
inline constexpr int kBitsPerSample = 16;
inline int BitrateKbps(int bitrate) noexcept {
    return bitrate > 0 ? static_cast<int>((static_cast<std::int64_t>(bitrate) + 500) / 1000) : 0;
}

struct TrackInfo {
    std::int64_t total_frames = 0;
    int length_ms = 0;
    int source_channels = 0;
    int native_channels = 0;
    int source_sample_rate = 0;
    int bitrate = 0;
    int links = 0;
    int header_gain_q8 = 0;
    int track_gain_q8 = 0, album_gain_q8 = 0;
    bool has_track_gain = false, has_album_gain = false;
    bool seekable = true, radio = false;
    std::wstring title, artist, album, display_title, vendor;
    std::map<std::string, std::wstring> tags;
    StationInfo station;
};

class Decoder final {
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Local UTF-16 paths or forward-only HTTP(S) Ogg Opus streams.
    bool Open(const wchar_t* path, int* error = nullptr);
    void Close() noexcept;
    void Cancel() noexcept;
    bool SetOutputMode(OutputMode mode) noexcept;
    void SetGain(const PlaybackSettings& settings, bool shuffle) noexcept;
    int OutputChannels() const noexcept { return output_channels_; }
    int ReadFrames(std::int16_t* pcm, int capacity_frames);
    int ReadFloatFrames(float* pcm, int capacity_frames);
    int InstantBitrate() noexcept;
    unsigned MetadataRevision() const noexcept { return metadata_revision_; }
    bool SeekFrame(std::int64_t frame);
    bool SeekMs(int milliseconds);
    std::int64_t PositionFrames() const noexcept;
    const TrackInfo& Info() const noexcept { return info_; }

private:
    template<typename Sample> int ReadSamples(Sample* pcm, int capacity_frames);
    void RefreshTags();
    OggOpusFile* file_ = nullptr;
    std::unique_ptr<HttpStream> http_;
    TrackInfo info_;
    std::wstring fallback_title_;
    int pending_error_ = 0;
    bool forced_eof_ = false;
    bool native_output_ = false;
    int output_channels_ = kStereoChannels;
    opus_uint32 metadata_serial_ = 0;
    unsigned metadata_revision_ = 0;
};

// Bounded content detection. HTTP probes never claim MP3/Vorbis streams.
bool HasOpusContent(const wchar_t* path) noexcept;
std::wstring Utf8ToWide(const char* text);
const wchar_t* ErrorText(int code) noexcept;
} // namespace winamp_opus
