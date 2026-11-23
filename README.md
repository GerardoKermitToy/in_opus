# Winamp Opus Input

A 32-bit input plugin for local Ogg Opus playback in Winamp 5.9.

## Features

- `.opus`, plus `.ogg` and `.oga` containing Opus; Ogg Vorbis files are not claimed.
- Unicode paths, tags, duration, and bitrate information.
- 48 kHz / 16-bit PCM, with original 1–8 channels or stereo downmix.
- Pause, seeking, DSP, and visualization support.
- Native Windows dialogs with English labels.

HTTP/HTTPS playback and tag editing are not supported.

## Requirements

- Windows with **32-bit Winamp 5.9.x**.
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

## Output channels

Open the plugin's **Configure** dialog:

- **Source channels** (default): preserves mono and multichannel audio without
  downmixing and reorders samples to Windows PCM channel order.
- **Stereo**: produces two channels for any supported recording.

Restart playback after changing the setting. Unsupported channel layouts or
formats rejected by the output plugin fall back to stereo. Multichannel playback
requires a compatible output plugin, driver, and device.

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
**libopus 1.6.1**, **libopusfile 0.12**, and **libogg 1.3.6**.
See [cmake/Dependencies.cmake](cmake/Dependencies.cmake).
Optional experimental custom modes, DRED, OSCE, and Deep PLC are disabled because
this plugin performs ordinary local-file decoding and does not use those paths.
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
