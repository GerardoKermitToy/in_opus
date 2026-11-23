#pragma once
#include <algorithm>
#include <string>

namespace winamp_opus {
enum class OutputMode { Stereo = 0, SourceChannels = 1, Mono = 2, Quad = 3,
    LegacyUnmapped = 4, Surround = 5, SurroundII = 6 };
enum class DitherPdf { Rectangular = 0, Triangular = 1, Gaussian = 2 };
enum class NoiseShape { None = 0, FirstOrder = 1, Ath = 2, AthLow = 3 };
enum class GainMode { Header = 0, Album = 1, Track = 2, Auto = 3, Raw = 4 };
enum class SpeechMode { Off = 0, Lace = 1, NoLace = 2 };
enum class AmbisonicMode { Stereo = 0, Headphones = 1, Quad = 2, Surround51 = 3, Surround71 = 4 };
enum class RecoveryMode { Strict = 0, Plc = 1, NeuralPlc = 2, Fec = 3, Dred = 4 };
struct PlaybackSettings {
    OutputMode output_mode = OutputMode::SourceChannels;
    int sample_rate = 48000;
    int bits = 16;
    int resample_quality = 5;
    GainMode gain_mode = GainMode::Header;
    int pre_gain_q8 = 0;
    int radio_gain_q8 = 0;
    int target_lufs = -23;
    bool dither = true;
    bool instant_bitrate = true;
    bool unified_info = true;
    bool opus_hd = true, opus_bwe = false;
    bool mono_compatible = false, ignore_extensions = false;
    SpeechMode speech = SpeechMode::Off;
    AmbisonicMode ambisonic = AmbisonicMode::Stereo;
    RecoveryMode recovery = RecoveryMode::Strict;
    int recovery_limit_ms = 120;
    // Default output remains compatible with earlier versions. New processing is opt-in.
    DitherPdf dither_pdf = DitherPdf::Triangular;
    NoiseShape noise_shape = NoiseShape::None;
    bool custom_quantizer = false;
    bool use_host_replaygain = false, use_host_proxy = true;
    bool preferred_gain_only = false, prevent_clipping = false;
    int peak_mode = 0, non_rg_q8 = 0;
    bool buffer_small_files = false;
    int timeout_ms = 5000, probe_kb = 1024;
    int reconnect_attempts = 0, reconnect_delay_ms = 1000;
    int prebuffer_kb = 0, stream_buffer_kb = 128, underrun_percent = 0;
    bool save_streams = false, interactive_auth = false, http_seek = false;
    int proxy_mode = 2;
    std::wstring proxy_url, save_directory;
};
inline PlaybackSettings ValidateSettings(PlaybackSettings s) noexcept {
    if (static_cast<unsigned>(s.output_mode) > 6) s.output_mode = OutputMode::SourceChannels;
    if (static_cast<unsigned>(s.dither_pdf) > 2) s.dither_pdf = DitherPdf::Triangular;
    if (static_cast<unsigned>(s.noise_shape) > 3) s.noise_shape = NoiseShape::None;
    // Zero selects the native rate (96 kHz only for actual enabled QEXT).
    if (s.sample_rate != 0 && (s.sample_rate < 8000 || s.sample_rate > 192000)) s.sample_rate = 48000;
    if (s.bits != 8 && s.bits != 16 && s.bits != 24 && s.bits != 32) s.bits = 16;
    s.resample_quality = std::clamp(s.resample_quality, 0, 10);
    if (static_cast<unsigned>(s.gain_mode) > 4) s.gain_mode = GainMode::Header;
    s.pre_gain_q8 = std::clamp(s.pre_gain_q8, -60 * 256, 30 * 256);
    s.radio_gain_q8 = std::clamp(s.radio_gain_q8, -60 * 256, 30 * 256);
    s.target_lufs = std::clamp(s.target_lufs, -40, 0);
    if (static_cast<unsigned>(s.speech) > 2) s.speech = SpeechMode::Off;
    if (static_cast<unsigned>(s.ambisonic) > 4) s.ambisonic = AmbisonicMode::Stereo;
    if (static_cast<unsigned>(s.recovery) > 4) s.recovery = RecoveryMode::Strict;
    s.recovery_limit_ms = std::clamp(s.recovery_limit_ms, 20, 1000);
    s.non_rg_q8 = std::clamp(s.non_rg_q8, -60 * 256, 30 * 256);
    s.peak_mode = std::clamp(s.peak_mode, 0, 3);
    s.timeout_ms = std::clamp(s.timeout_ms, 1000, 30000);
    s.probe_kb = std::clamp(s.probe_kb, 64, 4096);
    s.reconnect_attempts = std::clamp(s.reconnect_attempts, 0, 10);
    s.reconnect_delay_ms = std::clamp(s.reconnect_delay_ms, 250, 30000);
    s.stream_buffer_kb = std::clamp(s.stream_buffer_kb, 32, 4096);
    s.prebuffer_kb = std::clamp(s.prebuffer_kb, 0, std::min(s.stream_buffer_kb, s.probe_kb / 2));
    s.underrun_percent = std::clamp(s.underrun_percent, 0, 100);
    s.proxy_mode = std::clamp(s.proxy_mode, 0, 2);
    if (s.proxy_url.size() > 2048) s.proxy_url.clear();
    if (s.save_directory.size() > 32767) s.save_directory.clear();
    return s;
}
} // namespace winamp_opus
