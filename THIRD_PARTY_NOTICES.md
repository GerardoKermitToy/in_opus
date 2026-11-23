# Third-party notices

The plugin's original source is licensed under GPL-3.0-only.
The third-party libraries below retain their original licenses.

This plugin statically links libraries from the Xiph.Org Foundation and contributors.
Their copyright notices, conditions, and disclaimers must accompany redistributed binaries.
CMake copies each upstream `COPYING` file into the distribution's `licenses/` directory.

| Component | Version | License | Source |
|---|---|---|---|
| libogg | 1.3.6 | BSD-style | https://github.com/xiph/ogg/releases/tag/v1.3.6 |
| libopus | 1.6.1 | BSD-style; see upstream COPYING for patent grant information | https://opus-codec.org/ and https://github.com/xiph/opus/tree/v1.6.1 |
| libopusfile | 0.12 | BSD-style | https://github.com/xiph/opusfile/releases/tag/v0.12 |

## Reproducible source archives

- libogg official release archive: `https://github.com/xiph/ogg/releases/download/v1.3.6/libogg-1.3.6.tar.xz`
  - SHA-256: `5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061`
- libopus source snapshot from upstream tag `v1.6.1`, commit `22244de5a79bd1d6d623c32e72bf1954b56235be`:
  `https://codeload.github.com/xiph/opus/tar.gz/22244de5a79bd1d6d623c32e72bf1954b56235be`
  - SHA-256: `3267aa969b2b26542417f57819a5bc25d5079355300307dd2dddc7c704aed418`
  - This is the upstream Git snapshot, not the differently packaged Xiph release tarball.
- libopusfile official release archive: `https://github.com/xiph/opusfile/releases/download/v0.12/opusfile-0.12.tar.gz`
  - SHA-256: `118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b`

TLS certificate verification and SHA-256 verification are enabled for downloads.

## Winamp ABI reference

The plugin declares only the minimal public ABI needed to interoperate with Winamp 5.x.
It does not link Winamp code or require the full SDK to build.
The input version used is the original Unicode-filename ABI (`IN_UNICODE | 0x100`),
which retains an ANSI description and a `void Init()` callback.

Reference headers:

- https://github.com/alexfreud/winamp/blob/community/Src/Winamp/IN2.H
- https://github.com/alexfreud/winamp/blob/community/Src/Winamp/OUT.H

Winamp is a trademark of its respective owner. This is an independent plugin, not an official Winamp release.
