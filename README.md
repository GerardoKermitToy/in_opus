# Winamp Opus Input

A 32-bit input plugin for Ogg Opus files and HTTP/HTTPS radio in Winamp 5.9.

## Features

- `.opus` and `.opu`, plus `.ogg` and `.oga` containing Opus; Vorbis is not claimed.
- HTTP/HTTPS Ogg Opus radio with station information and chained-stream tags.
- Unicode paths and tags; standard Winamp metadata fields and a read-only unified info dialog.
- Original 1–8 channels or stereo downmix; live radio uses stereo for changing layouts.
- 8/16/24/32-bit integer PCM, optional 8–192 kHz SpeexDSP resampling.
- ReplayGain/R128 album, track, and shuffle-aware gain; separate file/radio preamps.
- Configurable dithering, target LUFS, and instantaneous/average bitrate display.
- Pause, local-file seeking, DSP, visualization, and native English settings dialogs.

Defaults remain **48 kHz / 16-bit, source channels, header gain only, dithering on**.
Tag editing is not supported; information dialogs are read-only.

## Requirements

- Windows 10/11 with **32-bit Winamp 5.9.x**.
- **Microsoft Visual C++ Redistributable v14 (x86)** and Universal CRT
  (included with Windows 10/11). The x64 redistributable alone is not sufficient.

## Installation

1. Close Winamp completely.
2. Copy `dist/Plugins/in_opus.dll` to the `Plugins` folder beside `winamp.exe`.
3. Restart Winamp. The plugin appears under **Preferences → Plug-ins → Input**.

Alternatively, run the installer, which backs up an existing plugin:

```powershell
.\tools\Install-Plugin.ps1
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
Network reads have timeouts and are cancelled on Stop. Authentication, inserted
ICY metadata blocks, playlist parsing, and automatic reconnection are not provided.

## Output settings

Open the plugin's **Configure** dialog:

- **Source channels** (default): preserves mono and multichannel audio without
  downmixing and reorders samples to Windows PCM channel order.
- **Stereo**: produces two channels for any supported recording.

Restart playback after changing the setting. Unsupported channel layouts or
formats rejected by the output plugin fall back to stereo. Multichannel playback
requires a compatible output plugin, driver, and device.

The same dialog selects PCM resolution/rate, resampler quality (0–10), R128 gain
mode, preamps, target loudness, dithering, and the information dialog. At 48 kHz
resampling is bypassed. Higher output rates do not restore ultrasonic content:
standard Opus decodes at 48 kHz. 32-bit output is integer PCM, not floating-point.
For gain modes using R128 tags, -23 LUFS preserves their native reference; -18 LUFS
adds 5 dB. Header-only mode does not apply R128 tag gain. Positive gain may clip.
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
For Visual Studio 2026, use `vs2026-win32`, `release-vs2026`, and
`build/vs2026-win32` instead, with a CMake version supporting that generator.

Dependencies are downloaded and built from pinned sources:
**libopus 1.6.1**, **libopusfile 0.12**, **libogg 1.3.6**, and the **SpeexDSP 1.2.1** resampler.
See [cmake/Dependencies.cmake](cmake/Dependencies.cmake).
Optional experimental custom modes, DRED, OSCE, and Deep PLC are disabled because
this plugin uses standard Opus decoding and does not invoke those optional APIs.
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
