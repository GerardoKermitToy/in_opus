// SPDX-License-Identifier: GPL-3.0-only
#include "gain_policy.hpp"
#include "opus_decoder.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace winamp_opus;
namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Expect(const TrackInfo& info, const PlaybackSettings& settings, int expected, bool shuffle = false) {
    Check(OpusGainQ8(info, settings, shuffle) == expected, "final total Q8 policy differs");
    Check(OpusGainMultiplier(info, settings, shuffle) == std::pow(10., expected / (20. * 256)),
          "native and libopusfile helpers use identical quantized gain");
}
void DefaultsAndReferences() {
    TrackInfo info;
    PlaybackSettings s;
    Expect(info, s, 0);
    info.header_gain_q8 = 384;
    Expect(info, s, 384);
    s.pre_gain_q8 = 256;
    Expect(info, s, 640);
    s.radio_gain_q8 = -512;
    info.radio = true;
    Expect(info, s, -128);
    s.gain_mode = GainMode::Raw;
    Expect(info, s, -512);
    info.radio = false;
    Expect(info, s, 256);

    info.header_gain_q8 = 256;
    info.has_track_gain = info.has_album_gain = true;
    info.track_gain_q8 = -512;
    info.album_gain_q8 = -1024;
    s.pre_gain_q8 = 0;
    s.gain_mode = GainMode::Track;
    Expect(info, s, -256);
    s.gain_mode = GainMode::Album;
    Expect(info, s, -768);
    s.gain_mode = GainMode::Auto;
    Expect(info, s, -768, false);
    Expect(info, s, -256, true);
    s.target_lufs = -18;
    Expect(info, s, 512, false);
    Expect(info, s, 1024, true);
    s.gain_mode = GainMode::Header;
    Expect(info, s, 256);
    s.gain_mode = GainMode::Raw;
    Expect(info, s, 0);
    info.tags["replaygain_track_gain"] = L"+60 dB";
    s.gain_mode = GainMode::Track;
    Expect(info, s, 1024); // Opus R128, not obsolete ReplayGain gain tags.
}
void SelectionAndFallback() {
    TrackInfo info;
    info.header_gain_q8 = 256;
    info.tags["r128_track_gain"] = L"-512";
    PlaybackSettings s;
    s.gain_mode = GainMode::Album;
    s.non_rg_q8 = -6 * 256;
    s.target_lufs = -18;
    Expect(info, s, 4 * 256);
    s.preferred_gain_only = true;
    Expect(info, s, -5 * 256); // Missing tag: no +5 dB target adjustment.
    s.preferred_gain_only = false;
    info.tags.erase("r128_track_gain");
    info.tags["R128_ALBUM_GAIN"] = L"-1024";
    s.gain_mode = GainMode::Track;
    Expect(info, s, 2 * 256);
    s.preferred_gain_only = true;
    Expect(info, s, -5 * 256);
    info.tags["r128_track_gain"] = L"0";
    Expect(info, s, 6 * 256); // A present zero is not a missing gain.
    s.gain_mode = GainMode::Auto;
    Expect(info, s, 2 * 256, false);
    Expect(info, s, 6 * 256, true);
    s.gain_mode = GainMode::Header;
    Expect(info, s, 256);
}
void StrictTags() {
    TrackInfo info;
    PlaybackSettings s;
    s.gain_mode = GainMode::Track;
    s.preferred_gain_only = true;
    s.non_rg_q8 = -777;
    // Text is authoritative even if a backend's cached parser accepted it.
    info.has_track_gain = true;
    info.track_gain_q8 = 1234;
    const std::vector<std::wstring> invalid{
        L"", L"+", L"-", L" 1", L"1 ", L"1\t", L"1\n", L"1.0", L"1e2", L"+1 dB",
        L"0x10", L"--1", L"+-1", L"32768", L"-32769", L"9999999999999999999999999999",
        L"1; 2", L"nan", L"inf", L"１２", std::wstring(L"1\0junk", 6)};
    for (const auto& text : invalid) {
        info.tags["r128_track_gain"] = text;
        Expect(info, s, -777);
    }
    for (const auto& item : std::vector<std::pair<std::wstring, int>>{
        {L"0", 0}, {L"+0", 0}, {L"-0", 0}, {L"+123", 123}, {L"-123", -123},
        {L"000123", 123}, {L"32767", 32767}, {L"-32768", -32768}}) {
        info.tags["r128_track_gain"] = item.first;
        Expect(info, s, item.second);
    }
    info.tags.clear();
    Expect(info, s, 1234); // Allows callers which only retain validated fields.
    info.track_gain_q8 = 32768;
    Expect(info, s, -777);
    info.tags["r128_track_gain"] = L"1";
    info.tags["R128_TRACK_GAIN"] = L"2";
    Expect(info, s, -777); // Ambiguous case-variant duplicate keys are invalid.
}
void Peaks() {
    TrackInfo info;
    info.header_gain_q8 = 6 * 256;
    info.tags["r128_track_gain"] = L"-256";
    info.tags["replaygain_track_peak"] = L"2";
    PlaybackSettings s;
    s.gain_mode = GainMode::Track;
    s.pre_gain_q8 = 3 * 256;
    const int cap = static_cast<int>(std::floor(-20. * 256 * std::log10(2.)));
    Expect(info, s, 8 * 256);
    s.peak_mode = 1;
    Expect(info, s, 8 * 256); // Explicit prevent-clipping switch is required.
    s.prevent_clipping = true;
    Expect(info, s, cap);
    const double limited = OpusGainMultiplier(info, s, false);
    Check(limited * 2. <= 1., "Q8 peak cap must not round into clipping");
    Check(std::pow(10., (cap + 1) / (20. * 256)) * 2. > 1., "cap is the largest safe Q8 code");
    s.peak_mode = 2;
    Expect(info, s, cap + 3 * 256); // Absolute preamp/peak, not another header gain.
    s.peak_mode = 3;
    Expect(info, s, cap);
    info.tags["replaygain_track_peak"] = L"0.25";
    Expect(info, s, 6 * 256); // No-clip-only never boosts beyond the header.
    info.tags.erase("replaygain_track_peak");
    Expect(info, s, 6 * 256); // No peak: still header-only, no preamp/R128.
    s.peak_mode = 2;
    Expect(info, s, 8 * 256); // No peak: normal loudness gain is retained.

    // Peak selection follows the actually selected gain, including fallback.
    info.tags["replaygain_track_peak"] = L"2";
    info.tags["replaygain_album_peak"] = L"0.25";
    s.pre_gain_q8 = 0;
    s.gain_mode = GainMode::Album; // Album absent; falls back to track.
    Expect(info, s, cap);
    s.preferred_gain_only = true;
    Expect(info, s, static_cast<int>(std::floor(20. * 256 * std::log10(4.))));
    info.tags.erase("replaygain_album_peak");
    Expect(info, s, 6 * 256); // Preferred-only also disables opposite peak fallback.
    info.tags["rg_peak"] = L"+2.0e0";
    Expect(info, s, cap); // Generic legacy sample peak remains a usable fallback.
    s.gain_mode = GainMode::Header;
    Expect(info, s, 6 * 256);
    s.gain_mode = GainMode::Raw;
    Expect(info, s, 0);
}
void PeakValidationAndBounds() {
    TrackInfo info;
    PlaybackSettings s;
    s.gain_mode = GainMode::Track;
    s.preferred_gain_only = true;
    s.peak_mode = 2;
    info.tags["r128_track_gain"] = L"512";
    const std::vector<std::wstring> invalid{
        L"", L"0", L"-1", L"nan", L"NaN", L"inf", L"+inf", L"1e9999", L"1e-9999",
        L"0x1p2", L"2 dB", L"2; 3", L"2x", L"++2", L"2\n", std::wstring(L"2\0x", 3)};
    for (const auto& text : invalid) {
        info.tags["replaygain_track_peak"] = text;
        Expect(info, s, 512);
    }
    info.tags["replaygain_track_peak"] = L" \t+2.0e0\t ";
    Expect(info, s, static_cast<int>(std::floor(-20. * 256 * std::log10(2.))));
    info.tags["replaygain_track_peak"] = L"1e300";
    Expect(info, s, -32768);
    info.tags["replaygain_track_peak"] = L"1e-300";
    Expect(info, s, 32767);

    info.tags.clear();
    info.header_gain_q8 = 32767;
    s = {};
    s.pre_gain_q8 = 30 * 256;
    Expect(info, s, 32767);
    info.header_gain_q8 = -32768;
    s.pre_gain_q8 = -60 * 256;
    Expect(info, s, -32768);
    // The noexcept policy validates scalar inputs without copying strings.
    info.header_gain_q8 = (std::numeric_limits<int>::max)();
    s.pre_gain_q8 = (std::numeric_limits<int>::min)();
    Expect(info, s, 32767 - 60 * 256);
    s.gain_mode = static_cast<GainMode>(999);
    Expect(info, s, 32767 - 60 * 256);
}
} // namespace
int main() {
    try {
        DefaultsAndReferences();
        SelectionAndFallback();
        StrictTags();
        Peaks();
        PeakValidationAndBounds();
        std::cout << "Gain policy: default/header/raw, R128 references, shuffle/fallback, strict tags, optional peaks, Q8 caps passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
