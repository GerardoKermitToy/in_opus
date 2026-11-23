// SPDX-License-Identifier: GPL-3.0-only
#include "pcm_pipeline.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace winamp_opus {
namespace {
// Windows slot -> family-1 codec slot. Inverting this table restores Opus order;
// it is NEVER applicable to a rendered Ambisonic speaker layout.
constexpr int kWindowsFromCodec[8][8]{
    {0}, {0, 1}, {0, 2, 1}, {0, 1, 2, 3}, {0, 2, 1, 3, 4},
    {0, 2, 1, 5, 3, 4}, {0, 2, 1, 6, 5, 3, 4}, {0, 2, 1, 7, 5, 6, 3, 4}};
}

PcmPipeline::PcmPipeline(Decoder& decoder, PlaybackSettings settings) noexcept
    : decoder_(decoder), settings_(ValidateSettings(std::move(settings))),
      source_rate_(decoder.SampleRate()), source_channels_(decoder.OutputChannels()),
      rate_(settings_.sample_rate ? settings_.sample_rate : source_rate_),
      channels_(OutputChannels(settings_.output_mode, source_channels_)) {
    if (settings_.custom_quantizer) quantizer_.emplace(settings_, channels_, rate_);
    if (channels_ && source_rate_ > 0 && rate_ != source_rate_) {
        int error = 0;
        // Mix first: fewer filter states for downmixes, without changing the
        // linear matrix or letting integer quantization into the float path.
        resampler_ = speex_resampler_init(channels_, source_rate_, rate_,
            settings_.resample_quality, &error);
        if (resampler_ && error) { speex_resampler_destroy(resampler_); resampler_ = nullptr; }
    }
    ResetAfterSeek();
}
PcmPipeline::~PcmPipeline() { if (resampler_) speex_resampler_destroy(resampler_); }
bool PcmPipeline::Ready() const noexcept {
    return channels_ > 0 && source_rate_ > 0 && rate_ > 0 &&
           (rate_ == source_rate_ || resampler_ != nullptr);
}
void PcmPipeline::ResetAfterSeek() noexcept {
    input_frames_ = input_offset_ = tail_frames_ = error_ = 0;
    source_frames_ = emitted_frames_ = 0;
    source_eof_ = false;
    base_frame_ = decoder_.PositionFrames();
    rng_ = 0x9e3779b9;
    if (quantizer_) quantizer_->Reset();
    if (resampler_) {
        speex_resampler_reset_mem(resampler_);
        speex_resampler_skip_zeros(resampler_);
    }
}
std::int64_t PcmPipeline::PositionFrames() const noexcept {
    if (!resampler_) return decoder_.PositionFrames();
    return base_frame_ + emitted_frames_ / rate_ * kSampleRate +
           emitted_frames_ % rate_ * kSampleRate / rate_;
}
bool PcmPipeline::RawCodecOrder() const noexcept {
    const auto& info = decoder_.Info();
    return settings_.output_mode == OutputMode::LegacyUnmapped &&
           info.mapping_family == 1 && info.source_channels == source_channels_;
}
void PcmPipeline::MixBlock(float* target, const float* source, int frames) noexcept {
    if (RawCodecOrder()) {
        for (int f = 0; f < frames; ++f) {
            std::array<float, kMaxChannels> frame{};
            std::copy_n(source + f * source_channels_, source_channels_, frame.begin());
            for (int c = 0; c < source_channels_; ++c)
                target[f * channels_ + kWindowsFromCodec[source_channels_ - 1][c]] = frame[c];
        }
        return;
    }
    const auto mode = settings_.output_mode;
    const bool identity = channels_ == source_channels_ &&
        (mode == OutputMode::SourceChannels || mode == OutputMode::LegacyUnmapped ||
         (mode == OutputMode::Stereo && source_channels_ == 2) ||
         (mode == OutputMode::Mono && source_channels_ == 1) ||
         (mode == OutputMode::Quad && source_channels_ <= 4));
    if (identity) {
        if (target != source) std::copy_n(source, frames * channels_, target);
        return;
    }
    // MixFrame permits aliasing. Expand mono->stereo backwards so the next
    // source frame is not overwritten; all other matrices compact forwards.
    if (target == source && channels_ > source_channels_) {
        for (int f = frames - 1; f >= 0; --f)
            MixFrame(source + f * source_channels_, source_channels_,
                     target + f * channels_, channels_, mode, 1.);
    } else {
        for (int f = 0; f < frames; ++f)
            MixFrame(source + f * source_channels_, source_channels_,
                     target + f * channels_, channels_, mode, 1.);
    }
}
std::int64_t PcmPipeline::TargetFrames() const noexcept {
    return source_frames_ / source_rate_ * rate_ +
           source_frames_ % source_rate_ * rate_ / source_rate_;
}
double PcmPipeline::Random() noexcept {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return static_cast<double>(rng_) / 4294967296.0;
}
void PcmPipeline::Pack(void* data, const float* samples, int frames) noexcept {
    if (settings_.custom_quantizer) {
        quantizer_->Pack(data, samples, frames);
        return;
    }
    // Compatibility packer: retain the old TPDF stream, zero suppression and
    // ties-away-from-zero rounding unless the custom quantizer is selected.
    auto* out = static_cast<unsigned char*>(data);
    const int bytes = settings_.bits / 8;
    const double scale = std::ldexp(1.0, settings_.bits - 1);
    for (int i = 0; i < frames * channels_; ++i) {
        double value = std::isfinite(samples[i]) ? static_cast<double>(samples[i]) * scale : 0;
        if (settings_.dither && samples[i] != 0.0f) {
            // Sequence the two stateful draws: operand evaluation order must
            // not change the compatibility stream between Debug/Release/LTO.
            const double first = Random();
            const double second = Random();
            value += first - second;
        }
        const auto sample = static_cast<std::int64_t>(std::llround(std::clamp(value, -scale, scale - 1)));
        std::uint32_t packed = static_cast<std::uint32_t>(sample);
        if (settings_.bits == 8) packed = static_cast<std::uint32_t>(sample + 128);
        for (int byte = 0; byte < bytes; ++byte) *out++ = static_cast<unsigned char>(packed >> (byte * 8));
    }
}
int PcmPipeline::Read(void* pcm, int capacity) {
    if (!pcm || capacity <= 0 || capacity > 576 || !Ready()) return OP_EINVAL;
    if (decoder_.OutputChannels() != source_channels_ || decoder_.SampleRate() != source_rate_) return OP_EBADLINK;
    if (!resampler_ && settings_.bits == 16 && !settings_.custom_quantizer &&
        !decoder_.NativeActive() && channels_ == source_channels_ &&
        (settings_.output_mode == OutputMode::SourceChannels || settings_.output_mode == OutputMode::Stereo))
        return decoder_.ReadFrames(static_cast<std::int16_t*>(pcm), capacity);
    const int frames = Render(output_.data(), capacity);
    if (frames > 0) Pack(pcm, output_.data(), frames);
    return frames;
}
int PcmPipeline::ReadFloat(float* pcm, int capacity) {
    if (!pcm || capacity <= 0 || capacity > 576 || !Ready()) return OP_EINVAL;
    return Render(pcm, capacity);
}
int PcmPipeline::Render(float* pcm, int capacity) {
    // Output format is fixed for the life of a pipeline. The decoder negotiates
    // stable source speakers or stable stereo before this object is created.
    if (decoder_.OutputChannels() != source_channels_ || decoder_.SampleRate() != source_rate_) return OP_EBADLINK;
    if (!resampler_) {
        float* decoded = channels_ == source_channels_ ? pcm : input_.data();
        const int count = decoder_.ReadFloatFrames(decoded, capacity);
        if (count > capacity || decoder_.OutputChannels() != source_channels_ ||
            decoder_.SampleRate() != source_rate_) return OP_EBADLINK;
        if (count > 0) MixBlock(pcm, decoded, count);
        return count;
    }
    int frames = 0;
    while (frames < capacity) {
        if (input_offset_ == input_frames_ && !source_eof_) {
            input_frames_ = decoder_.ReadFloatFrames(input_.data(), 576);
            input_offset_ = 0;
            if (input_frames_ > 576 || decoder_.OutputChannels() != source_channels_ ||
                decoder_.SampleRate() != source_rate_) input_frames_ = OP_EBADLINK;
            if (input_frames_ > 0) {
                source_frames_ += input_frames_;
                MixBlock(input_.data(), input_.data(), input_frames_);
            } else {
                if (input_frames_ < 0) error_ = input_frames_;
                input_frames_ = 0;
                source_eof_ = true;
                tail_frames_ = speex_resampler_get_input_latency(resampler_) + 2;
            }
        }
        const auto target = TargetFrames();
        if (source_eof_ && (emitted_frames_ + frames >= target || tail_frames_ == 0)) break;
        spx_uint32_t in = static_cast<spx_uint32_t>(source_eof_ ? std::min(tail_frames_, 576) : input_frames_ - input_offset_);
        spx_uint32_t out = static_cast<spx_uint32_t>(capacity - frames);
        if (source_eof_) out = static_cast<spx_uint32_t>(std::min<std::int64_t>(out, target - emitted_frames_ - frames));
        const int result = speex_resampler_process_interleaved_float(resampler_,
            source_eof_ ? nullptr : input_.data() + input_offset_ * channels_, &in,
            pcm + frames * channels_, &out);
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
    return frames ? frames : std::exchange(error_, 0);
}
} // namespace winamp_opus
