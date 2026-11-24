# Verification — Winamp Opus Input 1.5.1 — 2026-11-24

## Version 1.5.1 build and regression checks

The Win32 Release and Debug configurations were rebuilt after the version bump.
The combined CTest suite passed **41/41 Release** (133.74 s) and **41/41 Debug**
(173.07 s). The language catalogue check passed with 266 stable strings.
The Release DLL reports FileVersion and ProductVersion **1.5.1**, and Winamp's
plugin description reports the same version.

`dist/Plugins/in_opus.dll` is byte-identical to the rebuilt Release DLL and
passed the dedicated unified editor regression after staging. Its SHA-256 is:

```text
dc2765f713dbc80539fc36306f173ca10b1d6d5ccae281841a7ff40b03ebb039  Plugins/in_opus.dll
```

The prior staged editor DLL was preserved under `build/staged-plugin-backups`.
The 1.5.1 DLL was not installed into Winamp. The isolated native Winamp test
below was performed before the version bump, on the editor build identified by
its original hash.

## Unified Advanced editor before the version bump (1.5.0)

The unified pane is now an independently authored native Advanced editor, not a
second technical summary or a launcher for another dialog. Winamp retains Basic
Info, Artwork, the path, tabs and OK/Cancel. An ordered OpusTags document retains
repeated fields, literal empty values and long/multiline Unicode text; only edited
field groups are rewritten. Opaque comments, embedded artwork, unrelated links
and encoded audio remain untouched. Stale editor snapshots are rejected. Host
rating/R128 conversions and the existing active-playback metadata overlay remain
separate from raw Advanced text editing.

All twelve Basic Info fields return supported-but-empty when unset. Format Info
starts with the historical Length/Average bitrate/File size/Channels/Sampling
rate/Serial number/Version/Vendor block, followed by known Opus details. Complete
source size is preserved through RIFF unwrapping; container bitrate uses the
48-kHz granule timeline even when actual QEXT decoding is 96 kHz. Header version,
pre-skip, stream/coupled counts and existing packet/recovery diagnostics are shown
only when available. Existing language IDs are retained with append-only entries.
The recognized host-owned Format Info Edit receives reversible scroll styles;
there is no replacement technical control.

An isolated **Winamp 5.9.2 Build 10042** runtime loaded the new in_opus.dll and
opened the actual native File Info window. All twelve standard controls were
enabled; all twelve were edited, Advanced reflected the changes, and editing the
title in Advanced immediately updated Basic Info. Native OK persisted the fields;
native Cancel discarded a later draft. Format Info mouse-wheel scrolling advanced
from first visible line **0 to 3**. Basic Info/Advanced/scrolled Format Info captures
were inspected. The isolated runtime was closed and the installed Vorbis input
DLL's SHA-256 was verified unchanged. No installed DLL or profile was a writable
test target.

Final combined MSVC Win32 runs passed **41/41 Release** (72.96 s) and
**41/41 Debug** (92.79 s), including the sibling Ogg tests and the enabled
real-file surround cases. The dedicated `unified_tag_editor` exercises duplicate
comments, literal empty values, multiline Unicode, a 200-character name and a
70,000-character value, host broadcasts, OK/Cancel, stale-write rejection,
artwork/opaque-comment and decoded-audio preservation, native gain/rating
conversion, no-op file identity and the unchanged host-control style. Its
unattended host is hidden; unexpected top-level dialogs fail rather than block.
The translation catalogue check passed with 266 stable IDs. Release retains
IPO/LTO and the dynamic CRT; the output is PE32/x86. `git diff --check` passes.

At the time, the editor DLL in `dist/Plugins/in_opus.dll` was byte-identical to
its 1.5.0 Release build and passed the editor regression directly after staging.
Its SHA-256 matched the DLL loaded in the isolated Winamp runtime:

```text
9dc335a0439ec31b022ed55941dc8bd08cff620edeec76213a502b5f47b42b9f  Plugins/in_opus.dll
```

The previous staged DLL was backed up under `build/staged-plugin-backups` before
replacement. No DLL was installed into Winamp. These results cover this Winamp
version and generated fixtures, not every skin/version or malformed input.

---

# Historical verification — 2026-11-23

## Final Opus-only feature-transfer verification

- Combined MSVC/VS 2022 Win32 build: **39/39 Release** (122.03 s)
  and **39/39 Debug** (147.91 s). This includes all 15 independent
  Ogg sibling checks and the enabled source/channel/real-file cases. No test was
  disabled to obtain these results.
- Final C/C++ compilation logs contain no compiler warnings/errors. Release
  retains IPO/LTO and dynamic /MD; Debug uses /MDd. The upstream libogg CMake
  deprecation notice remains unrelated to compiler diagnostics.
- New real-DLL checks cover local OpusTags setters/writes, per-link R128
  conversions, rating, Unicode/deletion/no-op saves, paused/backpressured editing,
  one changed-tag title IPC and zero no-op IPCs, native selected-link editing,
  and independent integer/float extended-read sessions including seek/kill/guards.
- Direct writer fixtures check packet/decoded-audio/artwork preservation, no-op
  file identity and refusal of malformed CRC/lifecycle, hardlinks and unsafe
  replacement. Only local sequential Ogg Opus is editable.
- New transport checks cover local caching/RIFF bounds, ICY stripping/title
  deduplication, exact encoded recording, opt-in probe replay, strict Range and
  representation validators, proxy policy, transient authentication and redirect
  isolation, bounded probing and cancellation of blocked Open/Read/Seek.
- Real-DLL streaming checks verify a single initial connection, bounded reconnect,
  exact frame counts, unchanged/album-only tags, no output reopen/flush on
  reconnect, and Stop cancellation during delay or blocked reopen. Unknown zero
  startup bitrate is a valid host value, not a title-update failure.
- Processing/gain checks cover 1–8 channels, mono/quad/DS/DS2/raw-order modes,
  integer formats/PDFs/shapes and ATH fallbacks, float purity, gain/header/R128/peak
  policy, native/libopusfile first-link and cross-link seek behavior, resampler
  lengths/tails, deterministic state and preservation of the ordinary integer
  decoder path. Custom quantization is opt-in.
- Host settings and language checks include redirected test registry, read-only
  Wasabi snapshots, partial/invalid service fallback, language module isolation,
  all settings bounds/persistence, original Advanced and More parent/child
  OK/Cancel transactions, and a verified 221-string translation catalogue.
- Existing HD/QEXT, OSCE/BWE, Ambisonics, PLC/FEC/DRED, exact FLAC sibling,
  multichannel, DSP/visualization and playlist/seek regressions remain enabled.

### Failures found and corrected during integration

An isolated transport test displayed the checked-STL `transposed pointer range`
assertion. The ICY terminator search used pointers to separate literal arrays;
it now uses one named array and a valid same-array range. Test processes keep
checks enabled but route CRT reports to stderr rather than blocking the user's
desktop on modal assertions.

The first combined run passed 36/39. New tests found path separator mismatches in
active tag-cache invalidation and optimizer-dependent ordering of two stateful
TPDF random draws. Paths are now canonicalized and the draws explicitly sequenced;
the byte-exact comparison remains enabled. The reconnect host test
also incorrectly rejected an unknown startup bitrate of zero; its ABI assertion
was corrected without disabling the title-notification assertions. Cross-link
absolute gain is refreshed before playback after a seek, not only after the
first decoded block. Integration timestamps were refreshed to prevent MSBuild
from linking stale objects after a changed transport signature.

## 1.5.0 package

`dist/package-1.5.0` is staged from the final Release build and archived as
`dist/winamp-opus-1.5.0-win32.zip`. Both staged DLLs are byte-compared with their
build outputs; they are PE32/x86 with ASLR/NX and the Release dynamic CRT.
`SHA256SUMS.txt` records the DLLs and an adjacent archive checksum is generated.
Previous packages/archives are retained unchanged.

```text
a7d30b5d989813e4e248705cfde4908513982a31c76731817e5e2a037b0b3c22  Plugins/in_opus.dll
d8c4abda5087abca8ba6e5604dc47d44dede9a04a3217a06019462a65ab1f883  Plugins/in_vorbis.dll
```

## Not performed / limits

No installed Winamp DLL was replaced, no plugin/language pack was installed,
and no commit, push or public release was made. Mock-host success does not prove
real Winamp playlist appearance, physical output/hardware behavior or a complete
TLS/proxy/authentication deployment matrix. Actual installed-host Wasabi/language
integration and third-party conversion consumers still need real-host testing.

The format remains Ogg Opus only. Gain-tag edits take effect on next playback;
current host tags update without altering decoder history. Remote seeking needs
validated finite Range access with a stable validator and can incur native index/
BOS-seek I/O. Stable output normalization wins over gratuitous output reopens.
Analytic binaural, bounded loss-recovery and tagged-sample-peak limitations remain
as documented. See [feature transfer](docs/FEATURE_TRANSFER.md).

---

## Historical 1.4.0 verification (unchanged results)

# Verification — Winamp Opus Input 1.4.0 — 2026-11-23

## Build and regression results

The combined Win32/x86 Visual Studio 2022 build includes **30 CTest checks**
(the 15 Opus checks and all 15 independent Ogg checks). The initial periodic
notification fix passed 30/30 Release and Debug; the seek-specific correction
extends both DLL notification regressions rather than disabling existing checks.
Existing decoder, DLL, channel-order, surround, PCM/resampler, network, gain,
metadata, dialog and dynamic-CRT tests remain enabled. Both source repositories
are intentionally changed by the seek and playback-work cleanup.

Release uses the existing IPO/LTO and dynamic `/MD` CRT; Debug uses `/MDd`.
Final compiler logs are checked for compiler warnings/errors. Upstream libogg's
CMake minimum-version deprecation warning remains. Historical development runs
passed 28/28 before notification regressions were added, and 29/29 after the first
Opus-only periodic-update fix. Final seek-cleanup results are recorded separately
below rather than treating those earlier binaries as the current package.

## New verified behavior

`native_codecs` uses deterministic encoder-generated signals and malformed
variants, not external copyrighted recordings:

- Actual QEXT detection and native 96000-Hz decoding. A 96000-Hz input-rate hint
  without QEXT stays at 48000 Hz. Generated 30000-Hz content had measured amplitude
  **0.219999**, distinguishing HD from ordinary 48-kHz decode plus upsampling.
- Exact pre-skip, granule offsets, EOS trimming, mixed 48/96-kHz chain lengths,
  Windows speaker output, stereo downmix and arbitrary-frame seeks on the 48-kHz
  timeline. Failed seeks preserve the original transport cursor and decoder state.
- LACE, NoLACE, BWE, phase-inversion and extension-suppression controls have actual
  PCM-differential tests. Recorded speech mean absolute differences against
  ordinary decoding were **0.00694871 LACE**, **0.0356574 NoLACE** and
  **0.000729129 BWE**. These are activation checks, not perceptual-quality scores.
- Ambisonics families 2/3, analytic headphones, stereo/quad/5.1/7.1, silent LFE,
  non-diegetic stereo and rectangular C×K Q15 matrices including the final coded
  column when K differs from C. Sequential/seeked renderer history agrees.
- Header/R128/track/album/auto/raw gain and separate radio preamp are applied once,
  including the first returned block of a new logical link.
- Strict and bounded recovery policies use granule-proven interior page loss only.
  FEC reconstructed **960 frames** using signalled redundancy. DRED reconstructed
  **720 frames**, with only the uncovered **240 frames** assigned to PLC. Counters
  partition the exact missing interval rather than relabelling PLC as DRED.
  DRED→FEC fallback and neural/basic PLC differences are also tested.
- CRC, invalid headers/matrices, continuation errors, unanchored timestamps,
  excessive/uncertain loss and truncation remain rejected even in recovery modes.
- Output canaries, finite PCM, bounded packet parsing, forward-only late-HD errors,
  initial Icecast joins and single-connection loopback HTTP. A 270-link live chain
  verifies that radio has no cumulative indexed-link lifetime cap.
- Public Decoder/PcmPipeline tests cover native rate 0 and explicit 48000-Hz output,
  exact resampler tails, positions and seeking through native mixed-rate chains.

`advanced_plugin` loads the actual `in_opus.dll` into an isolated mock Winamp host:

- Native HD96k and explicit 48-kHz resampling.
- Ambisonics 5.1 and stereo fallback after output-plugin rejection.
- Exact output frame counts, 24-bit audio, 16-bit visualization, DSP negotiation,
  EOF and packet-diagnostics metadata exports.

`advanced_settings` loads the actual DLL with a per-process redirected HKCU:

- Safe defaults, validation and persistence of every advanced preset.
- Nested Advanced OK/Cancel plus parent OK/Cancel transaction boundaries.
- Main and advanced control bounds. Both native dialog captures were visually
  inspected after the timing adjustment; no control clipping was observed.
- No reads/writes of the user's real plugin settings.

## Playlist redraw regression

The original `C:\Temp\in_vorbis\wa2.cpp` (lines 192–208) publishes the full
output format only when opening/reopening output. Later bitrate updates preserve
the sample rate and channels with `-1` and skip an unchanged displayed bitrate.
This source was inspected for ABI behavior only; its implementation was not copied.
The community Winamp [`in_flac/DecodeThread.cpp`](https://github.com/manfromafar/winamp/blob/community/Src/Plugins/Input/in_flac/DecodeThread.cpp)
and [`main.cpp`](https://github.com/manfromafar/winamp/blob/community/Src/Plugins/Input/in_flac/main.cpp)
were also compared: their seek path queues the decoder seek and flushes output,
without posting a playlist-title update. A preloaded playlist does not require
refreshing merely because the current audio position changed.

The Opus playback loop had been resending bitrate, sample rate and channels every
250 ms. Winamp treats a `SetInfo` call containing both bitrate and sample rate as
a title-refresh request, and a supplied channel count also reinitializes its EQ
state. The fix retains the initial full-format call and sends only changed kbps
with `SetInfo(kbps, -1, -1, 1)` afterwards. Actual chained metadata/title change
notifications remain separate and enabled.

A new `playback_notifications` DLL regression test reproduced the failure before
the fix (`periodic bitrate updates must not resend sample rate/channels and
trigger playlist title redraws`). It uses paced generated CBR and changing-bitrate
chained fixtures, both libopusfile and native backends, and enabled/disabled
instantaneous bitrate. The host records every SetInfo and title notification.
The seek-specific regression reproduced **three title IPCs for three same-link
seeks** on the original libopusfile path. Successful seeks no longer invalidate
unchanged tags; the destination link index is retained to avoid reparsing them.
Native reconstruction also preserves the metadata revision when its contents are
unchanged. Host title IPC is filtered by actual display-title/tag changes, so a
format/vendor/decoder diagnostic is not a playlist change. Any tag may participate
in Winamp's title formatting; an album-only change still notifies the host.

Expanded `playback_notifications` coverage includes forward/backward seeks,
coalesced seek bursts, unchanged-tag chains, title-changing chains, album-only
changes, paused seek/resume, EOF seek/restart, no polling after EOF, and stop wakeup.
Both libopusfile and native decoding paths are exercised. The Ogg regression also
checks 44.1/48-kHz output reopening: rate/channels and bitrate are reported
separately so the format update cannot implicitly request a playlist refresh.

Native gain is cached until its inputs change, unity gain is copied directly,
and decoder-description strings are not rebuilt per packet. Host snapshots copy
tag maps only at metadata events, and visualizer conversion is skipped when
neither consumer exists. Paused/already-notified EOF workers wait on stop/control
events. Exact native preroll, validation, audio processing and real metadata
changes remain enabled. Real Winamp playlist appearance still requires a manual
check; no installed plugin was replaced.

## Real radio

The build-tree Release decoder played the original RadioSEGA URL
`http://icecast.radiosega.net:8006/rs-opus.ogg`: **9288 frames at 48000 Hz**, two
source channels, forward-only stereo output. No installed Winamp DLL was involved.

## Release binary

`build/vs2022-win32/bin/Release/in_opus.dll` is version **1.4.0**, PE32/x86, with
ASLR/NX and the Release Microsoft DLL CRT. The dynamic-CRT tests passed for both
configurations. The earlier periodic-notification fix remains staged at
`dist/package-1.4.0-playlist-fix`; the previous `dist/package-1.4.0` package and
older distribution files and archives were not overwritten. The later seek
correction uses a separate package recorded below.

Historical SHA-256 of the earlier periodic-notification DLL:

```text
146f6cca2f53ed29bc05289cd1197f418b6e625fb5db58ef5e50f7dde47ba02d  Plugins/in_opus.dll
```

## Final seek-cleanup verification

- Combined Winamp build: **30/30 Release** (105.75 s),
  **30/30 Debug** (123.83 s).
- Independent Ogg build: **15/15 Release** (43.77 s),
  **15/15 Debug** (50.80 s).
- Compiler logs have no C/C++ warnings/errors; dynamic-CRT, decoder, DLL,
  resampler, surround, exact FLAC, radio, dialogs and notification checks pass.
- On both DLL paths, repeated same-link seeks and identical-tag chains produce
  **zero title IPCs**. Actual title and album-only tag changes still notify the
  host. Native rate/channel reopening never combines a new rate with bitrate.
- Pause/seek/resume, EOF seek/restart, no output polling after EOF and stop wakeup
  pass. No installed Winamp was changed; real playlist appearance was not tested.

The final package is `dist/package-1.4.0-seek-fix`. Previous packages and
archives are retained. Both staged DLLs are byte-compared with this repository's
final Release build. SHA-256 (build-specific; separate build trees may differ):

```text
3dd2801cc1e61f2fa5b7f7ebf340864a2999b39d19893143a8804459eb777294  Plugins/in_opus.dll
d8c4abda5087abca8ba6e5604dc47d44dede9a04a3217a06019462a65ab1f883  Plugins/in_vorbis.dll
```

## Deliberate limits / not performed

- Headphone rendering is analytic approximation, not measured/personalized HRTF
  or head tracking. Surround LFE is intentionally silent.
- Recovery is not TCP repair, reconnect support or recovery of arbitrary damaged
  files. FEC/DRED eligibility is limited to appropriate single elementary
  identity-mapped mono/stereo streams; unavailable redundancy uses bounded PLC.
- Local indexing adds startup I/O; native seeks decode from link BOS and can be
  slow for long files. Forward-only output rate/layout must remain stable.
- Native mode uses the existing TPDF PCM packer; the unmodified ordinary local
  path retains libopusfile's original 16-bit dither/soft-clipping behavior.
- No plugin was installed, no existing installed DLL was replaced, and no commit,
  push or public release was made. The existing dirty working-tree changes were
  retained. Real Winamp/output-plugin/hardware 96-kHz and surround compatibility,
  a full TLS/proxy matrix and a large external fuzz corpus remain unverified.

See [advanced decoding](docs/ADVANCED_DECODING.md) for effective settings and bounds.
