#pragma once
#include <algorithm>

namespace winamp_opus {
enum class OutputMode { Stereo = 0, SourceChannels = 1 };
enum class GainMode { Header = 0, Album = 1, Track = 2, Auto = 3, Raw = 4 };
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
};
inline PlaybackSettings ValidateSettings(PlaybackSettings s) noexcept {
    if (s.output_mode != OutputMode::Stereo) s.output_mode = OutputMode::SourceChannels;
    if (s.sample_rate < 8000 || s.sample_rate > 192000) s.sample_rate = 48000;
    if (s.bits != 8 && s.bits != 16 && s.bits != 24 && s.bits != 32) s.bits = 16;
    s.resample_quality = std::clamp(s.resample_quality, 0, 10);
    if (static_cast<unsigned>(s.gain_mode) > 4) s.gain_mode = GainMode::Header;
    s.pre_gain_q8 = std::clamp(s.pre_gain_q8, -60 * 256, 30 * 256);
    s.radio_gain_q8 = std::clamp(s.radio_gain_q8, -60 * 256, 30 * 256);
    s.target_lufs = std::clamp(s.target_lufs, -40, 0);
    return s;
}
} // namespace winamp_opus
