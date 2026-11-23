# Winamp Opus Input 1.5.0

A 32-bit input plugin for Ogg Opus files and HTTP/HTTPS radio in Winamp 5.9.

## Features

- `.opus` and `.opu`, plus `.ogg` and `.oga` containing Opus; Vorbis is not claimed.
- HTTP/HTTPS Ogg Opus radio with station information and chained-stream tags.
- Unicode paths and tags; standard Winamp metadata, safe local OpusTags editing and rating.
- Integer/IEEE-float extended-read PCM sessions with seeking and cancellation.
- Optional Winamp ReplayGain/proxy snapshots and independent language-pack support.
- Original 1–8 channels, stereo/mono/quad downmix, explicit DS/DS2 matrices and codec-order compatibility.
- Stable radio output; ICY titles, optional recording, bounded reconnect and configurable buffering.
- 8/16/24/32-bit integer PCM, optional 8–192 kHz SpeexDSP resampling.
- ReplayGain/R128 album, track, and shuffle-aware gain; separate file/radio preamps.
- Target LUFS, preferred/fallback gain, untagged preamp, tagged-peak policies and bitrate display.
- Opt-in custom rectangular/triangular/Gaussian dither and first-order/ATH noise shaping.
- Pause, local-file seeking, DSP, visualization, and native English settings dialogs.
- Actual QEXT/Opus HD at native 96 kHz, not inferred from an input-rate hint.
- Optional LACE/NoLACE speech enhancement, 48-kHz speech BWE and mono-compatible intensity stereo.
- Ambisonics mapping families 2/3 with stereo/headphone/quad/5.1/7.1 rendering.
- Explicit packet-extension comparison and bounded PLC/neural PLC/FEC/DRED recovery.
- Packet bandwidth, duration, pitch, entropy-state and recovery diagnostics.

Defaults remain **48 kHz / 16-bit, source channels, header gain only, dithering on**.
Custom processing, recording, reconnect, remote seeking and host ReplayGain inheritance
are opt-in. The technical unified pane is read-only; Winamp basic fields and the
local tag editor can write supported Opus comments. See [feature transfer and
conflict choices](docs/FEATURE_TRANSFER.md).

## Requirements

- Windows 10/11 with **32-bit Winamp 5.9.x**.
- **Microsoft Visual C++ Redistributable v14 (x86)** and Universal CRT
  (included with Windows 10/11). The x64 redistributable alone is not sufficient.

## Installation

1. Close Winamp completely.
2. Copy `dist/package-1.5.0/Plugins/in_opus.dll` to the `Plugins` folder beside `winamp.exe`.
3. Restart Winamp. The plugin appears under **Preferences → Plug-ins → Input**.

Alternatively, run the installer, which backs up an existing plugin:

```powershell
.\tools\Install-Plugin.ps1 -PluginPath .\dist\package-1.5.0\Plugins\in_opus.dll
```

Installing into Program Files normally requires administrator privileges.

## HTTP/HTTPS radio

Open a direct Ogg Opus stream URL with Winamp's **Open URL** command (Ctrl+L).
The [Xiph Opus directory](https://dir.xiph.org/codecs/Opus) lists example stations;
use a direct stream endpoint, not a station website or an M3U download.
Extensionless URLs are checked for Opus content. If another input plugin takes
priority, append `>.opus` to the stream URL; the hint is removed before connecting.
Winamp loads its bundled Vorbis input first; it claims HTTP URLs ending in `.ogg`
without checking the codec. For Opus stations using that suffix, use the hint:

```text
http://icecast.radiosega.net:8006/rs-opus.ogg>.opus
```

Radio is forward-only: duration is unknown, seeking is disabled, and output is
stereo so chained streams may change source channels safely. Alt+3 shows available
station headers and Opus tags without opening another connection. HTTPS uses
Windows certificate verification; redirects cannot downgrade HTTPS to HTTP.
Network reads and reconnect waits are cancelled on Stop. More settings controls
timeouts, probe/buffer limits, ICY titles, transient Basic/Digest authentication,
proxy policy, recording and bounded reconnect. Finite non-ICY HTTP files can use
opt-in seeking when byte ranges and a stable representation validator are supplied.
Live radio stays non-seekable. M3U/PLS playlist parsing is not provided.

## Output settings

Open the plugin's **Configure** dialog:

- **Source channels** (default): preserves mono and multichannel audio without
  downmixing and reorders samples to Windows PCM channel order.
- **Stereo**: produces two channels for any supported recording.
- **Mono / Quad / DS / DS2**: explicit speaker processing; quad preserves fewer
  source channels when no multichannel downmix is needed. Matrix sums need headroom.
- **Unmapped codec order**: compatibility-only family-1 speaker order, not raw
  Ambisonics coefficients or a recommended Windows layout.

Restart playback after changing the setting. Unsupported channel layouts or
formats rejected by the output plugin fall back to stereo. Multichannel playback
requires a compatible output plugin, driver, and device.

The same dialog selects PCM resolution/rate, resampler quality (0–10), R128 gain
mode, preamps, target loudness, dithering, and the information dialog. Rate **0**
selects native output (48 kHz ordinarily, 96 kHz for enabled QEXT). Resampling is
bypassed when the selected rate equals the decoder rate. Higher output rates do
not manufacture HD content: standard Opus still decodes at 48 kHz.
32-bit output is integer PCM, not floating-point.
For gain modes using R128 tags, -23 LUFS preserves their native reference; -18 LUFS
adds 5 dB. Header-only mode does not apply R128 tag gain. Optional tagged-peak
policies do not replace a true-peak limiter; matrix/DSP processing may still clip.
Older DSP/output plugins may require the default 16-bit format.

## Build

Use Visual Studio 2022 with **Desktop development with C++**, a Windows SDK,
and CMake 3.24 or newer. Run from the repository root:

```powershell
cmake --preset vs2022-win32
```

```powershell
cmake --build --preset release
```

Package the DLL, documentation, and licenses into `dist`:

```powershell
cmake --install build/vs2022-win32 --config Release --component Runtime
```

This does not install the plugin into Winamp. Build only for **Win32**, not x64.
If the independent GPL Ogg-input project is present at `../in_vorbis`, CMake also
builds `in_vorbis.dll` using the same audio-library targets. Its source remains in
that separate repository. Set `WINAMP_OPUS_BUILD_OGG_INPUT=OFF` to build only Opus,
or set `WINAMP_OPUS_OGG_INPUT_SOURCE` to use another Ogg-input source directory.
This is a new implementation, not the WCL-licensed Winamp Vorbis source.
For Visual Studio 2026, use `vs2026-win32`, `release-vs2026`, and
`build/vs2026-win32` instead, with a CMake version supporting that generator.

Dependencies are downloaded and built from pinned sources:
**libopus 1.6.1**, **libopusfile 0.12**, **libogg 1.3.6**, and the **SpeexDSP 1.2.1** resampler.
See [cmake/Dependencies.cmake](cmake/Dependencies.cmake).
QEXT, OSCE/BWE, neural PLC and DRED support are compiled with pinned embedded
weight tables. Speech enhancement, BWE and loss recovery remain off by default;
standard playback does not opt into them. Nonstandard custom CELT modes stay off.
See [advanced decoding](docs/ADVANCED_DECODING.md) for effective policies and limits.
Audio libraries are linked statically; the plugin and dependencies use the
same dynamic CRT (`/MD` for Release, `/MDd` for Debug). Distribute Release builds only.

Optional tests:

```powershell
ctest --preset release
```

## License

Copyright (c) 2026 Winamp Opus Input contributors.

Licensed under the [GNU General Public License, version 3 only](LICENSE)
(`GPL-3.0-only`). Third-party libraries retain their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `dist/licenses/`.
