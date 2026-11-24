# Advanced Opus decoding — 1.5.1

## Defaults and scope

Existing PCM defaults stay at **48000 Hz, 16-bit, source channels, header gain,
dither enabled**. Optional speech processing, mono compatibility, packet-extension
suppression and loss recovery are off by default. Actual QEXT HD detection is on;
set the PCM rate to **0** to follow the native decoder rate, or choose a fixed PCM
rate explicitly. A fixed 48000-Hz output still downsamples a native 96000-Hz decode.

These features apply to Ogg Opus through the independent `in_opus.dll`, not MP4,
WebM, Matroska or raw RTP. Existing extension routing and HTTP security policy
remain Opus-only. Local text-tag editing and other codec-independent additions
are described in [feature transfer](FEATURE_TRANSFER.md).

Configure opens the existing playback dialog; **Advanced...** opens decoder
settings. Changes take effect at the next playback. Accepting Advanced does not
persist anything until the parent settings dialog is also accepted. Cancelling
either dialog leaves the corresponding settings unchanged.

## Decoder architecture and HD

The bounded native reader scans seekable Ogg input for link metadata, actual QEXT
packet-padding extensions and a safe fixed output format. The informational
`OpusHead` input rate is not evidence of HD. Ordinary local files without advanced
options retain the libopusfile decoding/quantization path after inspection.
Advanced files/options and forward-only HTTP use the native reader. Network input
uses the existing cancellable WinHTTP transport, not an extra download or a
second playback connection.

Ordinary Opus decodes at 48000 Hz. Enabled QEXT content decodes at **96000 Hz**.
Seekable mixed-rate chains choose a fixed maximum native rate; lower-rate links
are normalized before delivery. The output PCM resampler runs only when that
rate differs from the requested fixed output rate. Selecting 96000/192000 Hz for
ordinary Opus does not create QEXT or restore discarded ultrasonic information.

Ogg granules, pre-skip, duration, the public sample-position API and seeking stay
on the required **48000-Hz timeline**, independently of PCM output rate. Native
seeking rebuilds history from the target link's beginning, so long-link seeks
can be slow. Local scanning also adds startup I/O compared with libopusfile alone.
Forward-only playback must establish its fixed format at startup; late first HD
that would require increasing an established rate is an explicit error rather
than falsely advertised native HD.

## Speech and mono-compatible decoding

- **Off / LACE / NoLACE** select standard decoding or libopus's speech-enhancement
  profiles. In pinned libopus 1.6.1 LACE uses decoder complexity 6, NoLACE 7.
  These are not general-purpose music-quality levels. Eligible SILK speech changes;
  CELT music is not promised an improvement.
- **Speech BWE** enables blind bandwidth extension for eligible wideband speech at
  a 48000-Hz decode rate. It synthesizes high frequencies; it does not reconstruct
  the original ones. BWE is inapplicable to native 96000-Hz HD decoding.
- **Mono-compatible intensity stereo** disables decoder phase inversion. This can
  improve a later mono downmix but can reduce normal stereo quality. It is not a
  Dolby matrix encoder and does not force mono output.
- **Ignore packet extensions** suppresses padding extensions in each elementary
  decoder. It is a compatibility/comparison control, not an HD detector or a way
  to disable separately parsed DRED. HD suppression and extension suppression
  intentionally have different meanings.

The implementation applies CTLs to every elementary stream, including projection
streams. Embedded, checksum-pinned upstream model tables are compiled in. There
is no runtime model download, arbitrary external model loading, or execution of
Python/PyTorch/training scripts.

## Ambisonics

Ogg Opus mapping families **2** (ACN/SN3D) and **3** (projection/demixing) are
validated separately from ordinary Windows speaker layouts. Complete harmonic
orders and an optional non-diegetic stereo pair are supported. Rectangular family-3
matrices use the full signed-Q15, column-major demixing matrix without dropping
coded columns.

Targets are stereo speakers, approximate binaural headphones, quad, Windows-order
5.1 and 7.1. LFE is intentionally silent; non-diegetic stereo routes to front
left/right. The headphone renderer is an **analytic approximation** using head
shadow and interaural delay, not measured/personalized HRTF, head tracking or a
certified proprietary renderer. Mixing can require preamp headroom.

Source-channel mode uses a safe fixed layout for a local chain; stereo mode and
forward-only radio keep a fixed two-channel output. Output-plugin rejection of a
multichannel layout falls back to stereo. Actual device/output-plugin support
for 96 kHz and surround must be checked in the user's host.

## Bounded recovery, not automatic TCP repair

Strict mode is the default. Optional modes are basic PLC, neural PLC, FEC with PLC
fallback and DRED with FEC/PLC fallback. They apply only to provable **interior**
loss at complete packet/page boundaries, when a subsequent CRC-valid, non-EOS
Ogg page and granule establish the missing duration unambiguously. A per-gap
limit of **20–1000 ms** bounds work; default 120 ms.

- CRC failures, invalid headers/matrices, invalid packet envelopes and truncated
  input are still errors. Initial Icecast join recovery is a separate narrow
  startup policy, not an excuse to conceal later corruption.
- PLC synthesizes replacement audio, not original lost samples. Neural PLC is
  codec/mode dependent, not a guarantee for every music packet.
- FEC requires suitable in-band redundancy in a following packet. DRED requires
  actual DRED redundancy; neither can invent redundancy absent from the source.
- Redundancy reconstruction is restricted to supported identity-mapped elementary
  mono/stereo streams; uncovered sub-packet intervals use bounded PLC. Actual
  DRED/FEC coverage is applied only where present, with separate frame counters
  rather than relabelling PLC as redundancy reconstruction.
- Ogg packet durations must remain valid 2.5-ms multiples. Unknown, excessive or
  ambiguous gaps are rejected, including uncertain beginning/end loss.

HTTP/TCP retransmits transport packets. These modes do not fix a stalled server,
themselves add reconnect/authentication, or make ordinary intact files sound better.
Separate opt-in transport features provide reconnect/authentication; their policy
is independent of decoder packet-loss recovery.

## Diagnostics and safety

File information reports the effective decoder policy, mapping family, native
decode rate, last packet bandwidth/duration, voiced pitch when available, entropy
final range and separate PLC/FEC/DRED frame counts on the 48-kHz timeline. Entropy
state is a testing diagnostic, not a perceptual-quality score. Packet diagnostics
can change during playback; the host-facing snapshot is periodically refreshed.

The full output format is reported to Winamp once when playback starts. Later
instantaneous-bitrate updates change only the displayed kbps, preserving other
fields with the ABI's `-1` sentinel and skipping unchanged values. They do not
request a title/playlist refresh. A seek only changes position and flushes queued
PCM; seeking within the same logical link does not reparse or republish its tags.
A different link with identical tags also does not refresh the playlist. Actual
changes to tags (including fields used by Winamp title formatting) still notify
the host, independently of bitrate and technical diagnostics. Disabling the
bitrate display is not required.

Playback snapshots copy tag maps only on metadata events, not on the bitrate
clock. Packet diagnostics retain their bounded scalar refresh. Native gain and
decoder-description strings are cached until their inputs change. Visualization
conversion runs only when a visualization callback is present. Paused and
already-notified EOF workers wait on control/stop events rather than polling;
seek, resume and stop wake them. Output backpressure/drain checks and exact native
seek preroll remain necessary and are not skipped.

Packets, page batches, metadata and local indexing are bounded. Ogg CRC, selected
serial/sequence/continuation, header dimensions and granules are checked before
untrusted data drive decoding. Continuous radio has no session-total page/packet
lifetime counter. No Winamp installation or settings are modified by building or
running isolated test hosts.
