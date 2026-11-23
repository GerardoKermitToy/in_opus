#include "pcm_pipeline.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace winamp_opus {
PcmPipeline::PcmPipeline(Decoder& decoder, PlaybackSettings settings) noexcept
    : decoder_(decoder), settings_(ValidateSettings(settings)) {
    if (settings_.sample_rate != kSampleRate) {
        int error = 0;
        resampler_ = speex_resampler_init(decoder_.OutputChannels(), kSampleRate,
            settings_.sample_rate, settings_.resample_quality, &error);
        if (resampler_ && error) { speex_resampler_destroy(resampler_); resampler_ = nullptr; }
    }
    ResetAfterSeek();
}
PcmPipeline::~PcmPipeline() { if (resampler_) speex_resampler_destroy(resampler_); }
void PcmPipeline::ResetAfterSeek() noexcept {
    input_frames_ = input_offset_ = tail_frames_ = error_ = 0;
    source_frames_ = emitted_frames_ = 0;
    source_eof_ = false;
    base_frame_ = decoder_.PositionFrames();
    rng_ = 0x9e3779b9;
    if (resampler_) {
        speex_resampler_reset_mem(resampler_);
        speex_resampler_skip_zeros(resampler_);
    }
}
std::int64_t PcmPipeline::PositionFrames() const noexcept {
    return resampler_ ? base_frame_ + emitted_frames_ * kSampleRate / settings_.sample_rate : decoder_.PositionFrames();
}
double PcmPipeline::Random() noexcept {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return static_cast<double>(rng_) / 4294967296.0;
}
void PcmPipeline::Pack(void* data, const float* samples, int count) noexcept {
    auto* out = static_cast<unsigned char*>(data);
    const int bytes = settings_.bits / 8;
    const double scale = std::ldexp(1.0, settings_.bits - 1);
    for (int i = 0; i < count; ++i) {
        double value = std::isfinite(samples[i]) ? static_cast<double>(samples[i]) * scale : 0;
        if (settings_.dither && samples[i] != 0.0f) value += Random() - Random();
        const auto sample = static_cast<std::int64_t>(std::llround(std::clamp(value, -scale, scale - 1)));
        std::uint32_t packed = static_cast<std::uint32_t>(sample);
        if (settings_.bits == 8) packed = static_cast<std::uint32_t>(sample + 128);
        for (int byte = 0; byte < bytes; ++byte) *out++ = static_cast<unsigned char>(packed >> (byte * 8));
    }
}
int PcmPipeline::Read(void* pcm, int capacity) {
    if (!pcm || capacity <= 0 || capacity > 576 || !Ready()) return OP_EINVAL;
    if (!resampler_ && settings_.bits == 16) return decoder_.ReadFrames(static_cast<std::int16_t*>(pcm), capacity);
    const int channels = decoder_.OutputChannels();
    if (!resampler_) {
        const int count = decoder_.ReadFloatFrames(output_.data(), capacity);
        if (count > 0) Pack(pcm, output_.data(), count * channels);
        return count;
    }
    int frames = 0;
    while (frames < capacity) {
        if (input_offset_ == input_frames_ && !source_eof_) {
            input_frames_ = decoder_.ReadFloatFrames(input_.data(), 576);
            input_offset_ = 0;
            if (input_frames_ > 0) source_frames_ += input_frames_;
            else {
                if (input_frames_ < 0) error_ = input_frames_;
                input_frames_ = 0;
                source_eof_ = true;
                tail_frames_ = speex_resampler_get_input_latency(resampler_) + 2;
            }
        }
        const auto target = source_frames_ * settings_.sample_rate / kSampleRate;
        if (source_eof_ && (emitted_frames_ + frames >= target || tail_frames_ == 0)) break;
        spx_uint32_t in = static_cast<spx_uint32_t>(source_eof_ ? std::min(tail_frames_, 576) : input_frames_ - input_offset_);
        spx_uint32_t out = static_cast<spx_uint32_t>(capacity - frames);
        if (source_eof_) out = static_cast<spx_uint32_t>(std::min<std::int64_t>(out, target - emitted_frames_ - frames));
        const int result = speex_resampler_process_interleaved_float(resampler_,
            source_eof_ ? nullptr : input_.data() + input_offset_ * channels, &in,
            output_.data() + frames * channels, &out);
        if (result != RESAMPLER_ERR_SUCCESS || (in == 0 && out == 0)) {
            error_ = OP_EFAULT;
            source_eof_ = true;
            tail_frames_ = 0;
            break;
        }
        if (source_eof_) tail_frames_ -= static_cast<int>(in);
        else input_offset_ += static_cast<int>(in);
        frames += static_cast<int>(out);
    }
    emitted_frames_ += frames;
    if (frames > 0) Pack(pcm, output_.data(), frames * channels);
    return frames ? frames : std::exchange(error_, 0);
}
} // namespace winamp_opus
