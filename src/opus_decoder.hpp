#pragma once

#include <opusfile.h>
#include "channel_layout.hpp"
#include "playback_settings.hpp"
#include "http_stream.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>

namespace winamp_opus {
class NativeOpus;
class NativeInput;
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
    int links = 0, current_link = 0;
    std::uint32_t serial = 0;
    int header_gain_q8 = 0;
    int track_gain_q8 = 0, album_gain_q8 = 0;
    bool has_track_gain = false, has_album_gain = false;
    bool seekable = true, radio = false;
    std::wstring title, artist, album, display_title, vendor;
    std::map<std::string, std::wstring> tags;
    StationInfo station;
    int decoded_rate = 48000, output_rate = 48000, mapping_family = 0;
    int bandwidth_hz = 0, packet_duration_us = 0, pitch48 = 0;
    std::uint32_t final_range = 0;
    bool qext = false;
    std::uint64_t concealed_frames48 = 0, fec_frames48 = 0, dred_frames48 = 0;
    std::wstring decoder_note;
};

class Decoder final {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Local UTF-16 paths or forward-only HTTP(S) Ogg Opus streams.
    bool Open(const wchar_t* path, int* error = nullptr, const PlaybackSettings& settings = {});
    void Close() noexcept;
    void Cancel() noexcept;
    bool SetOutputMode(OutputMode mode) noexcept;
    void SetGain(const PlaybackSettings& settings, bool shuffle) noexcept;
    int OutputChannels() const noexcept { return native_ ? NativeChannels() : output_channels_; }
    int SampleRate() const noexcept;
    bool NativeActive() const noexcept { return native_ != nullptr; }
    int ReadFrames(std::int16_t* pcm, int capacity_frames);
    int ReadFloatFrames(float* pcm, int capacity_frames);
    int InstantBitrate() noexcept;
    unsigned MetadataRevision() const noexcept;
    bool SeekFrame(std::int64_t frame);
    bool SeekMs(int milliseconds);
    std::int64_t PositionFrames() const noexcept;
    const TrackInfo& Info() const noexcept;
    const std::wstring& LastError() const noexcept { return last_error_; }

private:
    template<typename Sample> int ReadSamples(Sample* pcm, int capacity_frames);
    void RefreshTags();
    void RefreshNativeInfo();
    int NativeChannels() const noexcept;
    static int DecodeCallback(void*, OpusMSDecoder*, void*, const ogg_packet*, int, int, int, int);
    std::unique_ptr<NativeOpus> native_;
    std::shared_ptr<NativeInput> native_input_;
    PlaybackSettings settings_;
    std::wstring last_error_;
    OggOpusFile* file_ = nullptr;
    std::shared_ptr<HttpStream> http_;
    mutable std::mutex transport_mutex_;
    std::atomic<bool> cancelled_{false};
    TrackInfo info_;
    std::wstring fallback_title_;
    int pending_error_ = 0;
    bool forced_eof_ = false;
    bool native_output_ = false, shuffle_ = false;
    int output_channels_ = kStereoChannels;
    int metadata_link_ = -1;
    unsigned metadata_revision_ = 0, native_revision_ = 0, station_revision_ = 0;
};

// Bounded content detection. HTTP probes never claim MP3/Vorbis streams.
bool HasOpusContent(const wchar_t* path) noexcept;
std::wstring Utf8ToWide(const char* text);
const wchar_t* ErrorText(int code) noexcept;
} // namespace winamp_opus
