// SPDX-License-Identifier: GPL-3.0-only
#include "gain_policy.hpp"
#include "opus_decoder.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string_view>

namespace winamp_opus {
namespace {
constexpr int kGainMin = -32768, kGainMax = 32767;

bool EqualKey(const std::string& key, std::string_view wanted) noexcept {
    if (key.size() != wanted.size()) return false;
    for (std::size_t i = 0; i < key.size(); ++i) {
        const auto c = static_cast<unsigned char>(key[i]);
        const auto lower = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
        if (lower != static_cast<unsigned char>(wanted[i])) return false;
    }
    return true;
}
struct Tag {
    const std::wstring* text = nullptr;
    bool duplicate = false;
    void Set(const std::wstring& value) noexcept {
        if (text) duplicate = true;
        else text = &value;
    }
};
struct GainTags {
    Tag track, album, track_peak, album_peak, peak;
    explicit GainTags(const TrackInfo& info) noexcept {
        // Metadata is already bounded by the readers. Visit it once, without
        // temporary key strings or locale-dependent wide/narrow conversions.
        for (const auto& item : info.tags) {
            if (EqualKey(item.first, "r128_track_gain")) track.Set(item.second);
            else if (EqualKey(item.first, "r128_album_gain")) album.Set(item.second);
            else if (EqualKey(item.first, "replaygain_track_peak")) track_peak.Set(item.second);
            else if (EqualKey(item.first, "replaygain_album_peak")) album_peak.Set(item.second);
            else if (EqualKey(item.first, "rg_peak")) peak.Set(item.second);
        }
    }
};
bool ParseR128(const std::wstring& text, int& result) noexcept {
    std::size_t at = 0;
    bool negative = false;
    if (!text.empty() && (text[0] == L'-' || text[0] == L'+')) {
        negative = text[0] == L'-';
        ++at;
    }
    if (at == text.size()) return false;
    const int maximum = negative ? 32768 : 32767;
    int value = 0;
    for (; at < text.size(); ++at) {
        const auto c = text[at];
        if (c < L'0' || c > L'9' || value > (maximum - (c - L'0')) / 10) return false;
        value = value * 10 + c - L'0';
    }
    result = negative ? -value : value;
    return true;
}
bool Gain(const Tag& tag, bool cached, int value, int& result) noexcept {
    if (tag.text) return !tag.duplicate && ParseR128(*tag.text, result);
    if (!cached || value < kGainMin || value > kGainMax) return false;
    result = value;
    return true;
}
double Peak(const Tag& tag) noexcept {
    if (!tag.text || tag.duplicate) return 0.;
    std::wstring_view wide(*tag.text);
    while (!wide.empty() && (wide.front() == L' ' || wide.front() == L'\t')) wide.remove_prefix(1);
    while (!wide.empty() && (wide.back() == L' ' || wide.back() == L'\t')) wide.remove_suffix(1);
    if (!wide.empty() && wide.front() == L'+') wide.remove_prefix(1);
    if (wide.empty() || wide.size() > 127 || wide.front() == L'-' || wide.front() == L'+') return 0.;
    std::array<char, 128> text{};
    for (std::size_t i = 0; i < wide.size(); ++i) {
        if (static_cast<unsigned int>(wide[i]) > 127) return 0.;
        text[i] = static_cast<char>(wide[i]);
    }
    double value = 0.;
    const auto parsed = std::from_chars(text.data(), text.data() + wide.size(), value, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + wide.size() &&
           std::isfinite(value) && value > 0. ? value : 0.;
}
int Bounded(double value) noexcept {
    return static_cast<int>(std::clamp(value, static_cast<double>(kGainMin), static_cast<double>(kGainMax)));
}
} // namespace

int OpusGainQ8(const TrackInfo& info, const PlaybackSettings& settings, bool shuffle) noexcept {
    auto mode = settings.gain_mode;
    if (static_cast<unsigned>(mode) > static_cast<unsigned>(GainMode::Raw)) mode = GainMode::Header;
    if (mode == GainMode::Auto) mode = shuffle ? GainMode::Track : GainMode::Album;
    const int preamp = std::clamp(info.radio ? settings.radio_gain_q8 : settings.pre_gain_q8, -60 * 256, 30 * 256);
    const int header = std::clamp(info.header_gain_q8, kGainMin, kGainMax);
    int total = preamp + (mode == GainMode::Raw ? 0 : header);
    if (mode != GainMode::Album && mode != GainMode::Track) return std::clamp(total, kGainMin, kGainMax);

    const GainTags tags(info);
    bool album = mode == GainMode::Album;
    int adjustment = 0;
    auto selected_gain = [&](bool use_album) noexcept {
        return use_album ? Gain(tags.album, info.has_album_gain, info.album_gain_q8, adjustment) :
                           Gain(tags.track, info.has_track_gain, info.track_gain_q8, adjustment);
    };
    bool tagged = selected_gain(album);
    if (!tagged && !settings.preferred_gain_only) {
        tagged = selected_gain(!album);
        if (tagged) album = !album;
    }
    total += tagged ? adjustment + (std::clamp(settings.target_lufs, -40, 0) + 23) * 256 :
                      std::clamp(settings.non_rg_q8, -60 * 256, 30 * 256);

    const int peak_mode = std::clamp(settings.peak_mode, 0, 3);
    if (peak_mode == 0 || (peak_mode == 1 && !settings.prevent_clipping))
        return std::clamp(total, kGainMin, kGainMax);
    double peak = Peak(album ? tags.album_peak : tags.track_peak);
    if (peak == 0. && !settings.preferred_gain_only) peak = Peak(album ? tags.track_peak : tags.album_peak);
    if (peak == 0.) peak = Peak(tags.peak);
    if (peak == 0.) return std::clamp(peak_mode == 3 ? header : total, kGainMin, kGainMax);

    // Log-domain comparison avoids overflow for any finite positive peak tag.
    // Never add the header a second time after deriving an absolute peak gain.
    const double ceiling = -20. * 256 * std::log10(peak);
    if (peak_mode == 2) return Bounded(std::floor(preamp + ceiling));
    const int wanted = peak_mode == 3 ? header : total;
    return ceiling < wanted ? Bounded(std::floor(ceiling)) : std::clamp(wanted, kGainMin, kGainMax);
}
double OpusGainMultiplier(const TrackInfo& info, const PlaybackSettings& settings, bool shuffle) noexcept {
    return std::pow(10., static_cast<double>(OpusGainQ8(info, settings, shuffle)) / (20. * 256));
}
} // namespace winamp_opus
