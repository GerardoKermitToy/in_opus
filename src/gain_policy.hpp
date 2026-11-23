// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace winamp_opus {
struct TrackInfo;
struct PlaybackSettings;

// Final TOTAL codec gain, including the Opus header exactly once (except Raw),
// in signed Q7.8 dB, clamped to libopusfile/OPUS_SET_GAIN's [-32768,32767].
// Apply using op_set_gain_offset(file, OP_ABSOLUTE_GAIN, result), or apply the
// corresponding multiplier once in a decoder configured without header gain.
// Do not also apply this policy in PcmPipeline. Preserve op_read/op_read_stereo
// for the legacy integer path: those own libopusfile's soft clipping and dither.
//
// R128 uses the -23 LUFS reference; target_lufs=-18 adds +5 dB ONLY when an
// R128 tag is selected. Auto prefers track in shuffle, album otherwise. Invalid
// or missing preferred gain falls back to the other tag unless preferred-only;
// if neither is usable, non_rg_q8 replaces the missing tag/target adjustment.
// Raw/Header ignore R128, peak modes, and non_rg_q8, but still use the selected
// local/radio preamp. R128 tags must be complete signed-16-bit decimal integers.
// Tag text takes precedence over cached fields, so malformed/duplicate text
// cannot silently become a valid gain via a more permissive backend parser.
//
// Optional positive finite replaygain_{track,album}_peak (then rg_peak) tags
// describe unamplified decoded sample peaks, not measured/oversampled true peaks.
// Peak modes are 0=RG gain, 1=RG gain with prevent_clipping's optional cap,
// 2=absolute preamp/peak normalization (compensating header/RG), 3=header-only
// with a peak cap, ignoring preamp/RG. A missing peak leaves RG gain unchanged
// in mode 2, header-only in mode 3. Peak-derived Q8 values round DOWN so codec
// gain granularity cannot undo a cap, subject to the codec's minimum -128 dB.
// No audio scanning, matrix-peak prediction, allocation or mutable global state.
int OpusGainQ8(const TrackInfo& info, const PlaybackSettings& settings, bool shuffle) noexcept;
// Exactly the same quantized policy as OpusGainQ8, not a higher-precision policy.
double OpusGainMultiplier(const TrackInfo& info, const PlaybackSettings& settings, bool shuffle) noexcept;
} // namespace winamp_opus
