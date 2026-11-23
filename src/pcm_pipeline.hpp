// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "opus_decoder.hpp"
#include "processing.hpp"
#include <speex_resampler.h>
#include <array>
#include <optional>

namespace winamp_opus {
// Bounded rendering of decoder-delivered Windows speakers (including already
// rendered Ambisonics); this stage never interprets raw ACN coefficients.
// Source/Stereo at the decoder rate, PCM16, and custom_quantizer=false retain
// libopusfile's integer soft-clipping/dither path. All buffers and custom
// quantizer histories are owned by this instance and allocated before Read.
class PcmPipeline final {
public:
    PcmPipeline(Decoder& decoder, PlaybackSettings settings) noexcept;
    ~PcmPipeline();
    PcmPipeline(const PcmPipeline&) = delete;
    PcmPipeline& operator=(const PcmPipeline&) = delete;
    bool Ready() const noexcept;
    int Channels() const noexcept { return channels_; }
    int Rate() const noexcept { return rate_; }
    int Read(void* pcm, int capacity_frames);
    // Interleaved final-channel float PCM, without integer quantization, dither,
    // shaping, or clipping to +/-1. Capacity is in frames, in the range 1..576.
    // ReadFloat does not advance either packer's random/feedback state.
    int ReadFloat(float* pcm, int capacity_frames);
    void ResetAfterSeek() noexcept;
    // Position is always in Opus's 48 kHz timeline, not output-rate frames.
    std::int64_t PositionFrames() const noexcept;
private:
    int Render(float* pcm, int capacity_frames);
    void MixBlock(float* target, const float* source, int frames) noexcept;
    bool RawCodecOrder() const noexcept;
    std::int64_t TargetFrames() const noexcept;
    double Random() noexcept;
    void Pack(void* pcm, const float* samples, int frames) noexcept;
    Decoder& decoder_;
    PlaybackSettings settings_;
    int source_rate_ = 0, source_channels_ = 0, rate_ = 0, channels_ = 0;
    std::optional<Quantizer> quantizer_;
    SpeexResamplerState* resampler_ = nullptr;
    std::array<float, 576 * kMaxChannels> input_{}, output_{};
    int input_frames_ = 0, input_offset_ = 0, tail_frames_ = 0, error_ = 0;
    bool source_eof_ = false;
    std::int64_t source_frames_ = 0, emitted_frames_ = 0, base_frame_ = 0;
    std::uint32_t rng_ = 0x9e3779b9;
};
} // namespace winamp_opus
