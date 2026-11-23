#pragma once
#include "opus_decoder.hpp"
#include <speex_resampler.h>
#include <array>

namespace winamp_opus {
// Converts only when requested. 48 kHz/16-bit retains libopusfile's original
// integer path and noise-shaped dithering. All storage is allocated before Play.
class PcmPipeline final {
public:
    PcmPipeline(Decoder& decoder, PlaybackSettings settings) noexcept;
    ~PcmPipeline();
    PcmPipeline(const PcmPipeline&) = delete;
    PcmPipeline& operator=(const PcmPipeline&) = delete;
    bool Ready() const noexcept { return settings_.sample_rate == kSampleRate || resampler_; }
    int Read(void* pcm, int capacity_frames);
    void ResetAfterSeek() noexcept;
    std::int64_t PositionFrames() const noexcept;
private:
    double Random() noexcept;
    void Pack(void* pcm, const float* samples, int count) noexcept;
    Decoder& decoder_;
    PlaybackSettings settings_;
    SpeexResamplerState* resampler_ = nullptr;
    std::array<float, 576 * kMaxChannels> input_{}, output_{};
    int input_frames_ = 0, input_offset_ = 0, tail_frames_ = 0, error_ = 0;
    bool source_eof_ = false;
    std::int64_t source_frames_ = 0, emitted_frames_ = 0, base_frame_ = 0;
    std::uint32_t rng_ = 0x9e3779b9;
};
} // namespace winamp_opus
