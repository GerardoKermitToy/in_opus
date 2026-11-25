# Winamp Opus Input 1.5.1

A 32-bit input plugin for Ogg Opus files and HTTP/HTTPS radio in Winamp 5.9.

## Features

- Ogg Opus in `.opus`, `.opu`, `.ogg` and `.oga` files; Vorbis is not supported.
- HTTP/HTTPS radio with station tags, ICY titles and optional recording/reconnect.
- Unicode paths and tags; safe local tag editing without reencoding audio.
- Multichannel playback and downmixing, 8/16/24/32-bit integer PCM and optional
  8–192 kHz resampling.
- ReplayGain/R128, preamps, configurable dithering and noise shaping.
- DSP, visualization, decode-only PCM sessions and language-pack support.
- Advanced decoding: QEXT/Opus HD at 96 kHz, speech enhancement and Ambisonics.

Defaults: **48 kHz / 16-bit, source channels, header gain only, dithering on**.
Change playback options under **Preferences → Plug-ins → Input → Configure**;
restart playback to apply them. Multichannel output requires a compatible output
plugin, driver and device.

## Requirements

- Windows 10/11 with **32-bit Winamp 5.9.x**.
- **Microsoft Visual C++ Redistributable v14 (x86)**. The x64 package alone is
  not sufficient.

## Installation

1. Close Winamp completely.
2. Copy `dist/Plugins/in_opus.dll` to the `Plugins` folder beside `winamp.exe`.
3. Restart Winamp. The plugin appears under **Preferences → Plug-ins → Input**.

Alternatively, use the installer, which backs up an existing plugin:

```powershell
.\tools\Install-Plugin.ps1 -PluginPath .\dist\Plugins\in_opus.dll
```

Installing into Program Files normally requires administrator privileges.

## HTTP/HTTPS radio

Open a direct Ogg Opus stream URL with **Open URL** (Ctrl+L), not a station website
or playlist. The [Xiph Opus directory](https://dir.xiph.org/codecs/Opus) lists
example stations.

If another input plugin handles the URL (common with `.ogg`), append `>.opus`;
the hint is removed before connecting. Live radio is non-seekable and uses stereo
output. Alt+3 shows available station information and tags. M3U/PLS playlist
parsing is not provided.

## Build

Use Visual Studio 2022 with **Desktop development with C++**, a Windows SDK and
**CMake 3.25+**. Run from the repository root:

```powershell
.\tools\Build.ps1
```

This builds Release for **Win32** and stages the DLL, documentation and licenses
under `dist`; it does not install into Winamp. Tests are neither built nor run by
default. To include them:

```powershell
.\tools\Build.ps1 -RunTests
```

The script also accepts `-Configuration Debug` and `-Preset vs2026-win32`
(the latter requires Visual Studio 2026 and a CMake version supporting it).
Distribute **Release builds only**. Dependencies are downloaded from pinned
sources on the first configure; see [cmake/Dependencies.cmake](cmake/Dependencies.cmake).

## Documentation

- [Metadata, gain, networking and output options](docs/FEATURE_TRANSFER.md)
- [Advanced decoding and limits](docs/ADVANCED_DECODING.md)
- [Language packs](docs/language-pack/README.md)

## License

Copyright (c) 2026 Winamp Opus Input contributors.

Licensed under the [GNU General Public License, version 3 only](LICENSE)
(`GPL-3.0-only`). Third-party libraries retain their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `dist/licenses/`.
