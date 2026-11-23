# Ogg Opus feature transfer — 1.5.0

This release adapts codec-independent facilities from the independent GPL-only
`in_vorbis` project. It remains an **Ogg Opus** input: it does not register or decode
Vorbis, FLAC, Speex, MP4, WebM, Matroska or RTP. No additional codec dependency is
introduced. The existing libopusfile/native-QEXT, OSCE/BWE, Ambisonics and bounded
PLC/FEC/DRED implementations remain in place.

## Conflict choices and defaults

Existing 48 kHz / 16-bit / source-channel / header-gain defaults remain unchanged.
The existing libopusfile integer path (including its own soft clipping/dither) is
retained when no resampling, custom mixing or explicit custom quantization is
requested. New quantization never dithers an already-quantized integer block.

A stable output format is preferred over repeatedly closing and opening the
output plugin at chained-link boundaries. Mixed native 48/96-kHz chains retain
the existing normalization. Layout-changing chains fall back to stereo at the
codec boundary; explicit mono/quad/matrix processing can then operate on that
stable speaker signal. Obsolete nonstandard six-channel header reinterpretation
is not added to a spec-compliant Opus decoder. Raw codec order is an explicit
compatibility output, never the default and never applied to raw Ambisonics
coefficients or already-rendered families 2/3.

Network recording, reconnect, HTTP Range access, interactive credentials, local
memory caching, custom quantization and host ReplayGain inheritance are opt-in.
System/host proxy use remains enabled by default. All new options persist in this
plugin's per-user settings; inherited Winamp snapshots are never persisted back.

## OpusTags and metadata

Local Ogg Opus text comments may be changed through Winamp's metadata setter/write
exports or the file-information tag editor. The editor can target one sequential
logical link or all links. Empty values delete a field. Unspecified comments and
embedded pictures are preserved; artwork editing is not implemented. Audio
packets are not reencoded.

The conservative writer validates page CRC, packet/header lifecycle and safe
sequential framing. It writes a temporary file beside the original, checks for
concurrent source changes and uses atomic replacement. Unsafe multiplexing,
corruption, reparse-point source files and hard-link aliases are rejected. It
never broadens format claims to Vorbis just because the shared editor originated
in a multi-codec plugin. Same-value edits do not rewrite the file.

Rating and metadata aliases are exposed to Winamp. ReplayGain setter values map
to signed R128 Q8 values with the required OpusHead and -18/-23 LUFS reference
conversion; gains are not scanned or invented. Setter/write staging belongs to
the calling thread, preventing unrelated host threads from replacing each
other's pending edits.

A confirmed playing-file edit invalidates the host cache and overlays updated
link tags without seeking, flushing or reopening audio. The existing open handle
continues decoding the original encoded stream. Gain-tag changes take effect on
the next playback, avoiding an unexpected mid-track level change. A changed tag
may affect Winamp ATF, so actual tag/title changes notify Winamp; a no-op does not.

## Extended read

The standard ANSI/Unicode extended-read exports provide independent integer PCM
sessions (8/16/24/32-bit) and an explicit 32-bit IEEE-float entry point. Requested
mono/stereo/quad, output rate, seeking and the host kill flag are supported. Each
session keeps a stable format and independent decoder/resampler/quantizer state.
Float reads do not pass through integer packing, dithering or noise shaping.
Ordinary output-plugin negotiation is still **integer**, not float.

The interface is a decoder API, not an encoder. Conversion destinations depend
on the calling host and its encoders. No playback callbacks/title notifications
are needed for decode-only sessions.

## Gain and Winamp configuration

Optional read-only Wasabi configuration inheritance is resolved on the host/UI
thread at playback startup, with independent ReplayGain/proxy switches. Missing,
invalid or old services leave local fallbacks in place. No periodic host polling
or mid-track preference synchronization is added.

Both native and libopusfile decoding use one gain policy: header/track/album/auto/
raw, R128 target, preferred-only or fallback tags, untagged preamp and optional
peak modes. The final gain includes OpusHead exactly once. Header gain is retained
when the host disables ReplayGain; disabling RG does not mean Raw mode.

Peak modes use supplied sample-peak tags: normal gain, gain with peak protection,
peak normalization, or clipping prevention only. Missing peak data do not trigger
an extra scan. This is not true-peak limiting or a loudness analyzer. Downmixes,
DSP, output plugins or hardware may create additional peaks outside this claim.

## Transport, radio and remote files

The shared input transport covers local Unicode files and HTTP/HTTPS. Optional
local caching is bounded to small files (64 MiB), and historical RIFF/WAVE data
wrappers are bounded to their audio chunk. Content still has to be Ogg Opus.

Network options include timeout/probe limits, buffering/prebuffer thresholds,
proxy policy, transient Basic/Digest credentials, ICY metadata stripping and
StreamTitle. TLS certificate validation remains enabled; HTTPS downgrade and URL-
embedded credentials are rejected. Credentials are neither persisted nor blindly
forwarded across redirects. Basic credentials should only be used over HTTPS.

Opt-in stream recording saves incoming encoded Ogg bytes, not decoded PCM, with
ICY removed. It does not add an Opus encoder or recover unavailable source/header
bytes. Recording errors are visible technical information, not playlist-title
changes. Each new connection may produce its own recorded stream file.

Bounded reconnect opens a new Ogg source after live EOF/error, preserving the
already-open output rate, channel count and resolution. It does not repair TCP,
restore missing audio or promise gapless reconnection. Attempts and delays are
bounded and Stop interrupts the wait or active open/read. Unchanged reconnect
metadata do not notify the playlist. The configured retry budget is per playback
session and is not reset indefinitely by a stream that repeatedly ends.

Opt-in HTTP seeking requires a finite, non-ICY representation and validated
Range responses; live radio remains forward-only. If the server supplies a strong
ETag or valid Last-Modified validator and honors byte ranges, seekable access
preserves representation identity. Without such a validator it remains
forward-only, even when the length is known. Seekable advanced Opus still indexes
encoded links and native seek rebuilds decoder history from BOS, so remote files
can involve substantial I/O. This is not a promise of low-latency seek on every
server. URL metadata requests do not open an extra stream connection.

## Speaker processing and quantization

Additional explicit output modes are mono, quad, raw codec order and DS/DS2
amplitude matrices. Standard Windows ordering remains the normal output. DS/DS2
are not certified Dolby or phase-quadrature encoders. Unnormalized matrix sums
can clip; provide preamp headroom. Ambisonics is rendered first into speakers;
ordinary channel matrices never treat ACN coefficients as Windows speakers.

Explicit custom quantization offers rectangular/triangular/Gaussian dither and
none/first-order/ATH/low-ATH noise shaping. ATH coefficients are analytically
derived, not historical proprietary coefficient tables. ATH supports 22050,
32000, 37800, 44100 and 48000 Hz; low-ATH supports 44100/48000 Hz. Other rates use
first-order shaping. Dither off also disables shaping. Histories are independent
per channel/session and reset deterministically on seek. No allocation or locking
is required per PCM frame.

## Interface and language packs

The existing basic/advanced settings retain their stable control IDs. Additional
processing/gain/network controls use a separate settings dialog. Nested changes
are transactional: parent Cancel discards them and only parent OK persists.

A separate language-module GUID and stable append-only string IDs support
per-string English fallback. Language data translate UI text, not user tags,
paths, station titles, numeric values or foreign dialog layouts. The technical
unified pane remains read-only but no longer disables the host's editable basic
fields. Existing language packs generally need a module specifically for this
independent plugin.

## Playlist and work invariants

- Full SetInfo only at playback startup; changed kbps use `-1,-1` sentinels.
- Seeking, output position, diagnostics, recording errors and identical tags do
  not request a playlist-title refresh.
- Tag snapshots are copied at metadata events, not on the bitrate clock.
- No visualization conversion without a consumer; no idle pause/EOF polling.
- Gain and quantization state are reused until their actual inputs change.
- Disabled options add no recording, reconnect, full-file cache or peak scan.

See `VERIFICATION.md` for the actual builds/tests run and unverified real-host
cases. Unit and mock-host tests do not prove compatibility with every Winamp,
proxy, authentication deployment, language pack or physical output device.
