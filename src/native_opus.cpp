// SPDX-License-Identifier: GPL-3.0-only
#include "native_opus.hpp"
#include "gain_policy.hpp"
#include "opus_packet.hpp"
#include "ambisonics.hpp"
#include <ogg/ogg.h>
#include <opus_multistream.h>
#include <speex_resampler.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace winamp_opus {
namespace {
using opus_native::MaxPacketBytes;
constexpr std::size_t MaxBatchBytes = MaxPacketBytes + 65025;
constexpr unsigned MaxPages = 4 * 1024 * 1024, MaxPackets = 16 * 1024 * 1024;
constexpr unsigned MaxLinks = 256, MaxSkippedPages = 8192;
constexpr std::size_t MaxMetadataBytes = 8 * 1024 * 1024;
constexpr auto I64Max = (std::numeric_limits<std::int64_t>::max)();
std::uint32_t Le32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
int Le16(const unsigned char* p) noexcept { return p[0] | (p[1] << 8); }
int Milliseconds(std::int64_t frames) noexcept {
    return frames < 0 ? -1 : static_cast<int>((std::min)(static_cast<std::int64_t>(INT_MAX), frames / 48));
}
std::wstring Text(const unsigned char* data, std::size_t size) {
    // Convert only the bounded string, using the facade's UTF-8 policy.
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    return Utf8ToWide(bytes.c_str());
}
struct Packet {
    std::vector<unsigned char> data;
    std::int64_t granule = -1, bos_offset = 0;
    std::uint32_t serial = 0;
    int duration = 0;
    bool bos = false, eos = false, gap = false;
};

// CRC is checked with libogg; packet assembly is ours, so a run of 255 laces
// cannot grow libogg's internal buffers without a bound. Never resynchronize
// past CRC, framing, continuation, or truncation errors.
class Demux {
    std::shared_ptr<NativeInput> input_;
    std::deque<Packet> ready_;
    std::vector<unsigned char> partial_;
    std::uint32_t serial_ = 0, sequence_ = 0;
    std::int64_t bos_offset_ = 0;
    unsigned pages_ = 0, packets_ = 0;
    bool active_ = false, first_ = false, bounded_ = false, recovery_ = false;
    bool first_audio_ = false, audio_ = false, join_fragment_ = false;
    std::size_t join_bytes_ = 0;
    std::wstring error_;
    bool Fail(const wchar_t* text) { error_ = text; return false; }
    int Exact(unsigned char* out, int bytes, bool allow_eof = false) {
        int at = 0;
        while (at < bytes) {
            const int n = input_->Read(out + at, bytes - at);
            if (n < 0) { Fail(L"Ogg input read failed or was cancelled."); return -1; }
            if (!n) {
                if (!at && allow_eof) return 0;
                Fail(L"Truncated Ogg page."); return -1;
            }
            if (n > bytes - at) { Fail(L"Invalid input read size."); return -1; }
            at += n;
        }
        return 1;
    }
    int Page() {
        std::array<unsigned char, 282> header{};
        const auto offset = input_->Tell();
        const int read = Exact(header.data(), 27, true);
        if (read <= 0) {
            if (!read && (active_ || !partial_.empty())) { Fail(L"Ogg Opus ended without an EOS page."); return -1; }
            return read;
        }
        if ((bounded_ && ++pages_ > MaxPages) || std::memcmp(header.data(), "OggS", 4) || header[4] || (header[5] & ~7)) {
            Fail(L"Invalid Ogg page or indexing page limit exceeded."); return -1;
        }
        const int segments = header[26];
        if (Exact(header.data() + 27, segments) < 0) return -1;
        int body_size = 0;
        for (int i = 0; i < segments; ++i) body_size += header[27 + i];
        std::vector<unsigned char> body(static_cast<std::size_t>(body_size));
        if (Exact(body.data(), body_size) < 0) return -1;
        const auto checksum = Le32(header.data() + 22);
        ogg_page page{header.data(), 27 + segments, body.data(), body_size};
        ogg_page_checksum_set(&page);
        if (Le32(header.data() + 22) != checksum) { Fail(L"Ogg CRC mismatch."); return -1; }
        const auto serial = Le32(header.data() + 14);
        const bool opus_bos = (header[5] & 2) && !(header[5] & 1) && body_size >= 8 && !std::memcmp(body.data(), "OpusHead", 8);
        bool selected_bos = false;
        if (active_ && serial != serial_ && opus_bos) { Fail(L"Ambiguous concurrent Ogg Opus logical streams."); return -1; }
        if (!active_) {
            if (!opus_bos) return 2;
            active_ = first_ = true; audio_ = first_audio_ = false;
            serial_ = serial; sequence_ = 0; bos_offset_ = offset; selected_bos = true;
        }
        if (serial != serial_) return 2;
        const auto sequence = Le32(header.data() + 18);
        const bool initial_audio = first_audio_;
        first_audio_ = false;
        const bool gap = sequence != sequence_;
        const bool forward = static_cast<std::uint32_t>(sequence - sequence_) < UINT32_C(0x80000000);
        const bool continued = (header[5] & 1) != 0;
        const bool join = initial_audio && !input_->Seekable();
        const bool recover = gap && forward && recovery_ && audio_ && !initial_audio &&
            !continued && partial_.empty() && !join_fragment_ && !(header[5] & 6) &&
            segments && header[26 + segments] < 255;
        if (((header[5] & 2) != 0) != selected_bos || (gap && (!forward || (!join && !recover)))) {
            Fail(L"Ogg Opus page sequence mismatch (not an eligible interior loss)."); return -1;
        }
        if (gap && join) { partial_.clear(); join_fragment_ = continued; join_bytes_ = 0; }
        if (continued != (!partial_.empty() || join_fragment_)) {
            if (join && continued && partial_.empty()) { join_fragment_ = true; join_bytes_ = 0; }
            else { Fail(L"Ogg Opus continuation mismatch."); return -1; }
        }
        sequence_ = sequence + 1;
        const std::uint64_t gp = static_cast<std::uint64_t>(Le32(header.data() + 6)) |
            (static_cast<std::uint64_t>(Le32(header.data() + 10)) << 32);
        if ((gp > static_cast<std::uint64_t>(I64Max) && gp != UINT64_MAX) || (recover && gp == UINT64_MAX)) {
            Fail(L"Invalid or unanchored Ogg granule position."); return -1;
        }
        std::size_t at = 0;
        bool discarded = false, mark_gap = recover;
        for (int i = 0; i < segments; ++i) {
            const std::size_t count = header[27 + i];
            if (join_fragment_) {
                if (join_bytes_ > MaxPacketBytes - count) { Fail(L"Initial Ogg join fragment exceeds the packet limit."); return -1; }
                join_bytes_ += count; at += count;
                if (count < 255) { join_fragment_ = false; join_bytes_ = 0; discarded = true; }
                continue;
            }
            if (partial_.size() > MaxPacketBytes - count) { Fail(L"Ogg Opus packet exceeds the 1 MiB limit."); return -1; }
            partial_.insert(partial_.end(), body.begin() + at, body.begin() + at + count); at += count;
            if (count < 255) {
                if (bounded_ && ++packets_ > MaxPackets) { Fail(L"Ogg indexing packet limit exceeded."); return -1; }
                Packet packet; packet.data = std::move(partial_); partial_.clear();
                packet.serial = serial_; packet.bos_offset = bos_offset_; packet.bos = first_;
                packet.gap = mark_gap; mark_gap = false; first_ = false;
                ready_.push_back(std::move(packet));
            }
        }
        if ((header[5] & 4) && (!partial_.empty() || join_fragment_)) { Fail(L"Truncated packet on Ogg EOS page."); return -1; }
        if (ready_.empty()) {
            if ((gp != UINT64_MAX || (header[5] & 4)) && !discarded) { Fail(L"Ogg page has no complete packet for its granule/EOS."); return -1; }
            if (header[5] & 4) active_ = false;
        } else {
            ready_.back().granule = gp == UINT64_MAX ? -1 : static_cast<std::int64_t>(gp);
            if (header[5] & 4) { ready_.back().eos = true; active_ = false; }
        }
        return 1;
    }
public:
    Demux(std::shared_ptr<NativeInput> input, bool bounded, bool recovery)
        : input_(std::move(input)), bounded_(bounded), recovery_(recovery) {}
    const std::wstring& Error() const noexcept { return error_; }
    void AudioHeadersComplete() noexcept { audio_ = first_audio_ = true; }
    int Next(Packet& packet) {
        unsigned skipped = 0;
        while (ready_.empty()) {
            const int n = Page();
            if (n <= 0) return n;
            if (++skipped > MaxSkippedPages) { Fail(L"Too many Ogg pages without an Opus packet."); return -1; }
        }
        packet = std::move(ready_.front()); ready_.pop_front(); return 1;
    }
};

struct Logical {
    opus_native::Header header;
    TrackInfo tags;
    std::int64_t bos_offset = 0, base = -1, cursor = 0, frames48 = -1, audio_bytes = 0;
    std::uint32_t serial = 0;
    int rate = 48000, channels = 0;
    bool ended = false, qext = false;
};
struct Batch {
    std::vector<Packet> packets;
    std::int64_t decode_at = 0, limit = 0;
    int missing48 = 0, remaining48 = 0;
    std::size_t at = 0;
    bool redundancy_checked = false;
    int dred_available = 0, dred_end = 0;
};
bool GainTag(const std::string& text, int& result) noexcept {
    std::size_t at = 0; bool negative = false;
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) { negative = text[0] == '-'; ++at; }
    if (at == text.size()) return false;
    int value = 0;
    for (; at < text.size(); ++at) {
        const auto digit = static_cast<unsigned char>(text[at]);
        if (digit < '0' || digit > '9' || value > (32767 + (negative ? 1 : 0) - (digit - '0')) / 10) return false;
        value = value * 10 + digit - '0';
    }
    result = negative ? -value : value; return true;
}
bool Comments(const std::vector<unsigned char>& data, TrackInfo& info, std::size_t& metadata) {
    if (data.size() < 16 || std::memcmp(data.data(), "OpusTags", 8)) return false;
    std::size_t at = 8;
    auto length = [&](std::uint32_t& n) {
        if (at > data.size() || data.size() - at < 4) return false;
        n = Le32(data.data() + at); at += 4;
        return static_cast<std::size_t>(n) <= data.size() - at;
    };
    std::uint32_t n = 0;
    if (!length(n) || n > 65536) return false;
    info.vendor = Text(data.data() + at, n); at += n; metadata += n;
    if (data.size() - at < 4) return false;
    const auto count = Le32(data.data() + at); at += 4;
    if (count > 2048) return false;
    std::size_t budget = 256 * 1024;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!length(n)) return false;
        if (n <= 65536 && n <= budget) {
            const std::string comment(reinterpret_cast<const char*>(data.data() + at), n);
            const auto equals = comment.find('=');
            if (equals != std::string::npos && equals && equals <= 128) {
                auto key = comment.substr(0, equals);
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const auto value = comment.substr(equals + 1);
                if (key == "r128_track_gain" && !info.has_track_gain) info.has_track_gain = GainTag(value, info.track_gain_q8);
                if (key == "r128_album_gain" && !info.has_album_gain) info.has_album_gain = GainTag(value, info.album_gain_q8);
                if (key != "metadata_block_picture" && key != "coverart") {
                    auto& stored = info.tags[key]; const auto wide = Utf8ToWide(value.c_str());
                    if (!stored.empty() && !wide.empty()) stored += L"; ";
                    stored += wide;
                }
            }
            budget -= n; metadata += n;
        }
        at += n;
        if (metadata > MaxMetadataBytes) return false;
    }
    info.title = info.tags["title"]; info.artist = info.tags["artist"]; info.album = info.tags["album"];
    info.display_title = info.artist.empty() ? info.title : info.artist + L" - " + info.title;
    return metadata <= MaxMetadataBytes; // Trailing OpusTags application data is legal.
}
int Headers(Demux& demux, Logical& logical, std::size_t& metadata, std::wstring& error) {
    Packet head;
    const int n = demux.Next(head);
    if (n <= 0) { if (n < 0) error = demux.Error(); return n; }
    if (!head.bos || head.eos || head.gap || head.granule != 0 ||
        !opus_native::ParseHeader(head.data.data(), head.data.size(), logical.header)) {
        error = L"Invalid or unsupported OpusHead/mapping family."; return -1;
    }
    logical.serial = head.serial; logical.bos_offset = head.bos_offset;
    logical.tags.header_gain_q8 = logical.header.gain_q8;
    Packet tags;
    if (demux.Next(tags) != 1 || tags.bos || tags.eos || tags.gap || tags.serial != head.serial || tags.granule != 0 ||
        !Comments(tags.data, logical.tags, metadata)) {
        error = demux.Error().empty() ? L"Missing, oversized or malformed OpusTags." : demux.Error(); return -1;
    }
    demux.AudioHeadersComplete(); return 1;
}
bool AudioBatch(Demux& demux, Logical& logical, Batch& batch, const PlaybackSettings& settings, std::wstring& error) {
    batch = {};
    std::int64_t duration = 0, granule = -1;
    std::size_t bytes = 0;
    bool eos = false, gap = false;
    do {
        Packet packet;
        if (demux.Next(packet) != 1) { error = demux.Error().empty() ? L"Truncated Ogg Opus audio." : demux.Error(); return false; }
        bool qext = false;
        if (packet.bos || packet.serial != logical.serial ||
            !opus_native::InspectPacket(packet.data.data(), packet.data.size(), logical.header.streams, packet.duration, qext)) {
            error = L"Invalid Opus audio packet."; return false;
        }
        if (packet.gap) {
            if (!batch.packets.empty() || gap) { error = L"Ogg loss has no preceding complete granule anchor."; return false; }
            gap = true;
        }
        logical.qext = logical.qext || qext;
        bytes += packet.data.size(); duration += packet.duration;
        if (batch.packets.size() >= 255 || bytes > MaxBatchBytes) { error = L"Ogg Opus audio batch limit exceeded."; return false; }
        granule = packet.granule; eos = packet.eos;
        batch.packets.push_back(std::move(packet));
        if (eos && granule < 0) { error = L"Ogg EOS has no granule position."; return false; }
    } while (granule < 0);
    if (logical.base < 0) {
        if (gap || (!eos && granule < duration)) { error = L"Invalid initial Opus granule position."; return false; }
        logical.base = (std::max)(INT64_C(0), granule - duration); logical.cursor = logical.base;
        if (logical.base > I64Max - logical.header.preskip) { error = L"Opus granule overflow."; return false; }
    }
    if (logical.cursor > I64Max - duration) { error = L"Opus duration overflow."; return false; }
    auto expected = logical.cursor + duration;
    if (gap) {
        // The next non-EOS granule determines the EXACT missing duration. Never
        // guess a previous packet size or use an uncertain EOS trimming value.
        const auto missing = granule - expected;
        if (settings.recovery == RecoveryMode::Strict || eos || logical.cursor <= logical.base + logical.header.preskip ||
            missing <= 0 || missing % 120 || missing > static_cast<std::int64_t>(settings.recovery_limit_ms) * 48) {
            error = L"Ogg loss has an ambiguous, non-interior or excessive duration."; return false;
        }
        batch.missing48 = batch.remaining48 = static_cast<int>(missing); expected = granule;
    }
    if ((!eos && granule != expected) || (eos && (granule < logical.cursor || granule > expected ||
        granule < logical.base + logical.header.preskip))) {
        error = L"Discontinuous or invalid Opus granule position."; return false;
    }
    batch.decode_at = logical.cursor; batch.limit = granule; logical.cursor = expected;
    if (logical.audio_bytes > I64Max - static_cast<std::int64_t>(bytes)) { error = L"Opus byte-count overflow."; return false; }
    logical.audio_bytes += static_cast<std::int64_t>(bytes);
    if (eos) { logical.ended = true; logical.frames48 = granule - logical.base - logical.header.preskip; }
    return true;
}
int RenderChannels(const Logical& link, AmbisonicMode mode) noexcept {
    if (link.header.family < 2) return link.header.channels;
    switch (mode) {
    case AmbisonicMode::Quad: return 4;
    case AmbisonicMode::Surround51: return 6;
    case AmbisonicMode::Surround71: return 8;
    default: return 2;
    }
}
void Format(Logical& link, const PlaybackSettings& settings) {
    link.rate = settings.opus_hd && link.qext ? 96000 : 48000;
    link.channels = RenderChannels(link, settings.ambisonic);
}
// libopusfile's matrix, indexed in Opus/Vorbis order. SourceChannels is
// reordered separately; an ACN array is rendered, never speaker-downmixed.
constexpr float StereoMatrix[6][8][2] = {
    {{.5858f,0},{.4142f,.4142f},{0,.5858f}},
    {{.4226f,0},{0,.4226f},{.366f,.2114f},{.2114f,.336f}},
    {{.651f,0},{.46f,.46f},{0,.651f},{.5636f,.3254f},{.3254f,.5636f}},
    {{.529f,0},{.3741f,.3741f},{0,.529f},{.4582f,.2645f},{.2645f,.4582f},{.3741f,.3741f}},
    {{.4553f,0},{.322f,.322f},{0,.4553f},{.3943f,.2277f},{.2277f,.3943f},{.2788f,.2788f},{.322f,.322f}},
    {{.3886f,0},{.2748f,.2748f},{0,.3886f},{.3366f,.1943f},{.1943f,.3366f},{.3366f,.1943f},{.1943f,.3366f},{.2748f,.2748f}}
};
void Stereo(const float* source, float* output, int frames, int channels) noexcept {
    for (int i = 0; i < frames; ++i) {
        const float* in = source + static_cast<std::size_t>(i) * channels;
        if (channels == 1) { output[2 * i] = output[2 * i + 1] = in[0]; continue; }
        if (channels == 2) { output[2 * i] = in[0]; output[2 * i + 1] = in[1]; continue; }
        float left = 0, right = 0;
        for (int c = 0; c < channels; ++c) { left += StereoMatrix[channels - 3][c][0] * in[c]; right += StereoMatrix[channels - 3][c][1] * in[c]; }
        output[2 * i] = left; output[2 * i + 1] = right;
    }
}

class NativeReader final : public NativeOpus {
    std::shared_ptr<NativeInput> input_;
    PlaybackSettings settings_, gain_settings_;
    TrackInfo info_;
    std::unique_ptr<Demux> demux_;
    std::vector<Logical> known_;
    Logical logical_;
    Batch batch_;
    OpusMSDecoder* decoder_ = nullptr;
    OpusDREDDecoder* dred_decoder_ = nullptr;
    OpusDRED* dred_ = nullptr;
    SpeexResamplerState* resampler_ = nullptr;
    AmbisonicRenderer renderer_;
    std::vector<float> decoded_, projected_, rendered_, pending_, zeros_;
    std::size_t pending_at_ = 0, metadata_ = 0;
    std::int64_t bitrate_bytes_ = 0, bitrate_frames_ = 0, position48_ = 0, produced48_ = 0;
    int produced_sub_ = 0, position_sub_ = 0;
    int index_ = -1, rate_ = 48000, native_rate_ = 48000, channels_ = 2, decode_channels_ = 0, complexity_ = 0;
    int instant_bitrate_ = 0;
    OutputMode mode_ = OutputMode::Stereo;
    unsigned revision_ = 0;
    bool scanned_ = false, active_ = false, failed_ = false, eof_ = false, needed_ = false, shuffle_ = false;
    bool prior_silk_ = false, gain_dirty_ = true, note_dirty_ = true;
    float gain_ = 1;
    std::wstring published_recovery_note_;
    std::wstring error_, recovery_note_;

    bool Fail(const wchar_t* error) { error_ = error; return false; }
    void DestroyDecoder() noexcept {
        if (decoder_) opus_multistream_decoder_destroy(decoder_);
        if (dred_decoder_) opus_dred_decoder_destroy(dred_decoder_);
        if (dred_) opus_dred_free(dred_);
        if (resampler_) speex_resampler_destroy(resampler_);
        decoder_ = nullptr; dred_decoder_ = nullptr; dred_ = nullptr; resampler_ = nullptr;
    }
    OpusDecoder* Elementary(int stream = 0) const noexcept {
        OpusDecoder* state = nullptr;
        if (!decoder_ || opus_multistream_decoder_ctl(decoder_, OPUS_MULTISTREAM_GET_DECODER_STATE(stream, &state)) != OPUS_OK) return nullptr;
        return state;
    }
    bool Configure() {
        DestroyDecoder();
        const auto& h = logical_.header;
        // Apply family 3's CxK signed-Q15 matrix ourselves. Decode ALL K
        // columns of rectangular matrices, including the K>C case.
        decode_channels_ = h.family == 3 ? h.streams + h.coupled : h.channels;
        std::array<unsigned char, 255> identity{};
        for (int i = 0; i < decode_channels_; ++i) identity[i] = static_cast<unsigned char>(i);
        int error = OPUS_OK;
        decoder_ = opus_multistream_decoder_create(native_rate_, decode_channels_, h.streams, h.coupled,
            h.family == 3 ? identity.data() : h.mapping.data(), &error);
        if (!decoder_ || error != OPUS_OK) return Fail(L"Cannot create native Opus decoder (96 kHz requires QEXT support).");
        complexity_ = settings_.speech == SpeechMode::NoLace ? 7 : settings_.speech == SpeechMode::Lace ? 6 :
            settings_.opus_bwe && native_rate_ == 48000 ? 4 : 0;
        if (settings_.recovery == RecoveryMode::NeuralPlc || settings_.recovery == RecoveryMode::Fec || settings_.recovery == RecoveryMode::Dred) complexity_ = (std::max)(complexity_, 5);
        for (int i = 0; i < h.streams; ++i) {
            OpusDecoder* state = Elementary(i);
            if (!state || opus_decoder_ctl(state, OPUS_SET_GAIN(0)) != OPUS_OK ||
                opus_decoder_ctl(state, OPUS_SET_COMPLEXITY(complexity_)) != OPUS_OK ||
                opus_decoder_ctl(state, OPUS_SET_PHASE_INVERSION_DISABLED(settings_.mono_compatible ? 1 : 0)) != OPUS_OK ||
                opus_decoder_ctl(state, OPUS_SET_IGNORE_EXTENSIONS(settings_.ignore_extensions ? 1 : 0)) != OPUS_OK)
                return Fail(L"Cannot configure native Opus elementary decoder controls.");
            // These controls are NOT forwarded by the multistream ctl switch.
            const bool enabled = settings_.opus_bwe && native_rate_ == 48000;
            const int bwe = opus_decoder_ctl(state, OPUS_SET_OSCE_BWE(enabled ? 1 : 0));
            if (bwe != OPUS_OK && (enabled || bwe != OPUS_UNIMPLEMENTED)) return Fail(L"Cannot configure Opus blind bandwidth extension.");
        }
        if (settings_.recovery == RecoveryMode::Dred) {
            dred_decoder_ = opus_dred_decoder_create(&error);
            if (dred_decoder_ && error == OPUS_OK) dred_ = opus_dred_alloc(&error);
            if (!dred_decoder_ || !dred_ || error != OPUS_OK) recovery_note_ = L"DRED unavailable: bounded PLC fallback";
        }
        if (h.family >= 2) {
            const auto render_mode = mode_ == OutputMode::Stereo && settings_.ambisonic != AmbisonicMode::Headphones ?
                AmbisonicMode::Stereo : settings_.ambisonic;
            if (!renderer_.Init(h.channels, native_rate_, render_mode)) return Fail(L"Cannot initialize ACN/SN3D renderer.");
        }
        if (native_rate_ != rate_) {
            resampler_ = speex_resampler_init(static_cast<spx_uint32_t>(channels_), static_cast<spx_uint32_t>(native_rate_),
                static_cast<spx_uint32_t>(rate_), settings_.resample_quality, &error);
            if (!resampler_ || error || speex_resampler_skip_zeros(resampler_) != RESAMPLER_ERR_SUCCESS)
                return Fail(L"Cannot normalize chained Opus sample rates.");
        }
        produced48_ = 0; produced_sub_ = 0; prior_silk_ = false;
        return true;
    }
    bool Scan() {
        Demux scan(input_, true, settings_.recovery != RecoveryMode::Strict);
        std::size_t metadata = 0;
        std::int64_t total = 0, bytes = 0;
        bool identified = false;
        for (;;) {
            Logical link;
            const int n = Headers(scan, link, metadata, error_);
            if (n < 0) return false;
            if (!n) break;
            if (known_.size() >= MaxLinks) return Fail(L"Too many indexed chained Opus links.");
            Batch batch;
            do {
                if (!AudioBatch(scan, link, batch, settings_, error_)) return false;
                if (!identified) { input_->ProbeComplete(); identified = true; }
            } while (!link.ended);
            Format(link, settings_);
            if (total > I64Max - link.frames48 || bytes > I64Max - link.audio_bytes) return Fail(L"Chained Opus duration/size overflow.");
            total += link.frames48; bytes += link.audio_bytes; rate_ = (std::max)(rate_, link.rate);
            needed_ = needed_ || link.qext || link.header.family >= 2;
            known_.push_back(std::move(link));
        }
        if (known_.empty()) return Fail(L"No Ogg Opus logical stream.");
        info_.native_channels = known_.front().channels;
        for (const auto& link : known_) if (link.channels != info_.native_channels) info_.native_channels = 0;
        info_.total_frames = total; info_.length_ms = Milliseconds(total); info_.links = static_cast<int>(known_.size());
        if (total > 0) info_.bitrate = static_cast<int>((std::min)(static_cast<long double>(INT_MAX), static_cast<long double>(bytes) * 8 * 48000 / total));
        scanned_ = true; return true;
    }
    void Publish(bool tags) {
        const auto& tags_info = logical_.tags;
        const int link_index = scanned_ ? index_ : info_.links - 1;
        if (tags && (!revision_ || info_.current_link != link_index || info_.serial != logical_.serial ||
            info_.tags != tags_info.tags || info_.vendor != tags_info.vendor ||
            info_.source_channels != logical_.header.channels ||
            info_.source_sample_rate != static_cast<int>((std::min)(static_cast<std::uint32_t>(INT_MAX), logical_.header.input_rate)) ||
            info_.header_gain_q8 != logical_.header.gain_q8 || info_.mapping_family != logical_.header.family ||
            info_.has_track_gain != tags_info.has_track_gain || info_.has_album_gain != tags_info.has_album_gain ||
            info_.track_gain_q8 != tags_info.track_gain_q8 || info_.album_gain_q8 != tags_info.album_gain_q8)) {
            info_.current_link = link_index; info_.serial = logical_.serial;
            info_.source_channels = logical_.header.channels;
            info_.source_sample_rate = static_cast<int>((std::min)(static_cast<std::uint32_t>(INT_MAX), logical_.header.input_rate));
            info_.header_gain_q8 = logical_.header.gain_q8;
            info_.track_gain_q8 = logical_.tags.track_gain_q8; info_.album_gain_q8 = logical_.tags.album_gain_q8;
            info_.has_track_gain = logical_.tags.has_track_gain; info_.has_album_gain = logical_.tags.has_album_gain;
            info_.tags = logical_.tags.tags; info_.title = logical_.tags.title; info_.artist = logical_.tags.artist;
            info_.album = logical_.tags.album; info_.display_title = logical_.tags.display_title; info_.vendor = logical_.tags.vendor;
            ++revision_; gain_dirty_ = true;
        }
        const bool qext = scanned_ ? known_[index_].qext : logical_.qext;
        const bool note_changed = tags || note_dirty_ || info_.qext != qext || published_recovery_note_ != recovery_note_;
        info_.decoded_rate = native_rate_; info_.mapping_family = logical_.header.family;
        info_.qext = qext;
        if (!note_changed) return;
        note_dirty_ = false; published_recovery_note_ = recovery_note_;
        auto& note = info_.decoder_note;
        note = native_rate_ == 96000 ? L"Native QEXT 96000 Hz" : info_.qext ? L"QEXT, native 48000 Hz (HD disabled)" : L"Native Opus 48000 Hz";
        if (resampler_) note += L"; normalized to 96000 Hz for chained output";
        if (logical_.header.family >= 2) {
            note += L"; ambisonic family " + std::to_wstring(logical_.header.family);
            if (logical_.header.family == 3) note += logical_.header.channels == decode_channels_ ? L", Q15 demix" : L", rectangular Q15 demix";
            if (settings_.ambisonic == AmbisonicMode::Headphones) note += L", analytic approximate binaural (not measured HRTF)";
            else if (mode_ == OutputMode::Stereo) note += L", stereo";
            else note += L", Windows " + std::to_wstring(channels_) + L" channels (silent LFE)";
        }
        note += settings_.speech == SpeechMode::Lace ? L"; OSCE LACE (complexity 6)" :
            settings_.speech == SpeechMode::NoLace ? L"; OSCE NoLACE (complexity 7)" : L"; OSCE speech off";
        if (settings_.opus_bwe) note += native_rate_ == 48000 ? L"; OSCE BWE enabled" : L"; OSCE BWE inapplicable at 96k";
        if (settings_.mono_compatible) note += L"; phase inversion disabled";
        if (settings_.ignore_extensions) note += L"; extensions ignored";
        if (!recovery_note_.empty()) note += L"; " + recovery_note_;
    }
    int StartLink() {
        logical_ = {}; metadata_ = 0; recovery_note_.clear();
        const int n = Headers(*demux_, logical_, metadata_, error_);
        if (n <= 0) { if (!n) eof_ = true; return n; }
        if (scanned_) {
            ++index_;
            if (index_ >= static_cast<int>(known_.size())) { Fail(L"Ogg Opus link index changed."); return -1; }
        } else if (info_.links < INT_MAX) ++info_.links; // Live state never grows with link count.
        if (!AudioBatch(*demux_, logical_, batch_, settings_, error_)) return -1;
        if (scanned_) {
            const auto& expected = known_[index_];
            const auto& h = logical_.header; const auto& e = expected.header;
            if (logical_.bos_offset != expected.bos_offset || logical_.serial != expected.serial || h.channels != e.channels ||
                h.family != e.family || h.streams != e.streams || h.coupled != e.coupled || h.preskip != e.preskip ||
                h.gain_q8 != e.gain_q8 || h.mapping != e.mapping || h.matrix != e.matrix) {
                Fail(L"Ogg Opus changed since its link index was read."); return -1;
            }
            native_rate_ = expected.rate;
        } else {
            Format(logical_, settings_); native_rate_ = logical_.rate;
            if (!revision_) rate_ = native_rate_;
            if (native_rate_ > rate_) { Fail(L"Live QEXT would change the established native output rate."); return -1; }
        }
        if (!Configure()) return -1;
        active_ = true; Publish(true); return 1;
    }
    bool EligibleRedundancy() const noexcept {
        const auto& h = logical_.header;
        if (h.family == 3 || h.streams != 1 || h.channels != 1 + h.coupled || native_rate_ != 48000) return false;
        for (int c = 0; c < h.channels; ++c) if (h.mapping[c] != c) return false;
        return true;
    }
    bool Recover(int& duration) {
        const auto& next = batch_.packets.front();
        if (!batch_.redundancy_checked) {
            batch_.redundancy_checked = true;
            if (settings_.recovery == RecoveryMode::Dred && EligibleRedundancy() && dred_decoder_ && dred_) {
                const int n = opus_dred_parse(dred_decoder_, dred_, next.data.data(), static_cast<opus_int32>(next.data.size()),
                    batch_.missing48, 48000, &batch_.dred_end, 0);
                if (n > 0) batch_.dred_available = n;
            }
        }
        const bool eligible = EligibleRedundancy();
        const int fec_duration = opus_packet_get_samples_per_frame(next.data.data(), 48000);
        const bool can_fec = (settings_.recovery == RecoveryMode::Fec || settings_.recovery == RecoveryMode::Dred) &&
            eligible && prior_silk_ && !(next.data[0] & 128) &&
            opus_packet_has_lbrr(next.data.data(), static_cast<opus_int32>(next.data.size())) > 0;
        duration = (std::min)(960, batch_.remaining48);
        const bool have_dred = settings_.recovery == RecoveryMode::Dred && eligible && dred_ &&
            batch_.dred_available > batch_.dred_end;
        // DRED coverage can start partway through a missing packet. Conceal
        // only the uncovered prefix, then use the actual covered interval.
        // All these bounds are multiples of the Opus 2.5-ms time quantum.
        if (have_dred && batch_.remaining48 > batch_.dred_available)
            duration = (std::min)(duration, batch_.remaining48 - batch_.dred_available);
        else if (have_dred && batch_.remaining48 > batch_.dred_end)
            duration = (std::min)(duration, batch_.remaining48 - batch_.dred_end);
        const bool dred = have_dred && batch_.dred_available >= batch_.remaining48 &&
            batch_.remaining48 - duration >= batch_.dred_end;
        // Prefer covered DRED; otherwise leave the final signalled SILK frame
        // for FEC and conceal only the proven earlier duration.
        if (!dred && can_fec && batch_.remaining48 > fec_duration)
            duration = (std::min)(duration, batch_.remaining48 - fec_duration);
        const bool fec = !dred && can_fec && batch_.remaining48 == fec_duration;
        if (fec) duration = fec_duration;
        if (duration <= 0 || duration % 120) return Fail(L"Invalid redundancy coverage alignment.");
        const int frames = duration * (native_rate_ / 48000);
        decoded_.resize(static_cast<std::size_t>(frames) * decode_channels_);
        int n = 0;
        if (fec) {
            n = opus_decode_float(Elementary(), next.data.data(), static_cast<opus_int32>(next.data.size()), decoded_.data(), frames, 1);
            recovery_note_ = L"FEC: decoded signalled SILK LBRR for a granule-proven interior loss";
        } else if (dred) {
            n = opus_decoder_dred_decode_float(Elementary(), dred_, batch_.remaining48, decoded_.data(), frames);
            recovery_note_ = L"DRED: decoded available redundancy for a granule-proven interior loss";
        } else {
            const bool neural = settings_.recovery == RecoveryMode::NeuralPlc || settings_.recovery == RecoveryMode::Fec || settings_.recovery == RecoveryMode::Dred;
            const int plc_complexity = neural ? (std::max)(complexity_, 5) : (std::min)(complexity_, 4);
            for (int i = 0; i < logical_.header.streams; ++i)
                if (opus_decoder_ctl(Elementary(i), OPUS_SET_COMPLEXITY(plc_complexity)) != OPUS_OK) return Fail(L"Cannot configure bounded PLC.");
            n = opus_multistream_decode_float(decoder_, nullptr, 0, decoded_.data(), frames, 0);
            for (int i = 0; i < logical_.header.streams; ++i)
                if (opus_decoder_ctl(Elementary(i), OPUS_SET_COMPLEXITY(complexity_)) != OPUS_OK) return Fail(L"Cannot restore Opus decoder controls.");
            recovery_note_ = neural ? L"Neural PLC for a granule-proven interior loss" : L"PLC for a granule-proven interior loss";
            if (settings_.recovery == RecoveryMode::Fec) recovery_note_ += eligible ? L"; FEC absent/inapplicable: bounded PLC fallback" : L"; FEC ineligible mapping/rate: bounded PLC fallback";
            if (settings_.recovery == RecoveryMode::Dred) recovery_note_ += eligible ? L"; DRED absent/outside coverage: bounded PLC fallback" : L"; DRED ineligible mapping/rate: bounded PLC fallback";
        }
        if (n != frames) return Fail(L"Opus loss recovery failed or returned the wrong duration.");
        if (fec) info_.fec_frames48 += static_cast<unsigned>(duration);
        else if (dred) info_.dred_frames48 += static_cast<unsigned>(duration);
        else info_.concealed_frames48 += static_cast<unsigned>(duration);
        batch_.remaining48 -= duration; return true;
    }
    void Diagnostics(const Packet* packet, int duration) {
        info_.packet_duration_us = duration * 1000 / 48;
        info_.pitch48 = 0; info_.final_range = 0; info_.bandwidth_hz = 0;
        for (int i = 0; i < logical_.header.streams; ++i) {
            auto* state = Elementary(i); opus_int32 pitch = 0; opus_uint32 range = 0; int bandwidth = 0;
            if (opus_decoder_ctl(state, OPUS_GET_PITCH(&pitch)) == OPUS_OK) info_.pitch48 = (std::max)(info_.pitch48, static_cast<int>(pitch));
            if (opus_decoder_ctl(state, OPUS_GET_FINAL_RANGE(&range)) == OPUS_OK) info_.final_range ^= range;
            if (opus_decoder_ctl(state, OPUS_GET_BANDWIDTH(&bandwidth)) == OPUS_OK) {
                const int hz = bandwidth == OPUS_BANDWIDTH_NARROWBAND ? 4000 : bandwidth == OPUS_BANDWIDTH_MEDIUMBAND ? 6000 :
                    bandwidth == OPUS_BANDWIDTH_WIDEBAND ? 8000 : bandwidth == OPUS_BANDWIDTH_SUPERWIDEBAND ? 12000 :
                    bandwidth == OPUS_BANDWIDTH_FULLBAND ? 20000 : 0;
                info_.bandwidth_hz = (std::max)(info_.bandwidth_hz, hz);
            }
        }
        if (packet) prior_silk_ = !(packet->data[0] & 128);
        Publish(false);
    }
    void Produced(int frames) noexcept {
        const int ratio = rate_ / 48000;
        produced48_ += (produced_sub_ + frames) / ratio; produced_sub_ = (produced_sub_ + frames) % ratio;
    }
    bool Normalize(const float* source, int frames) {
        pending_.clear(); pending_at_ = 0;
        if (!resampler_) { pending_.assign(source, source + static_cast<std::size_t>(frames) * channels_); Produced(frames); return true; }
        pending_.resize(static_cast<std::size_t>(frames * 2 + 16) * channels_);
        spx_uint32_t in = static_cast<spx_uint32_t>(frames), out = static_cast<spx_uint32_t>(frames * 2 + 16);
        if (speex_resampler_process_interleaved_float(resampler_, source, &in, pending_.data(), &out) != RESAMPLER_ERR_SUCCESS || in != static_cast<spx_uint32_t>(frames))
            return Fail(L"Chained Opus rate normalization failed.");
        pending_.resize(static_cast<std::size_t>(out) * channels_); Produced(static_cast<int>(out)); return true;
    }
    int FlushNormalizer() {
        if (!resampler_ || logical_.frames48 < 0) return 0;
        const auto remaining48 = logical_.frames48 - produced48_;
        if (remaining48 < 0 || (!remaining48 && produced_sub_)) { Fail(L"Rate normalization exceeded the exact link length."); return -1; }
        if (!remaining48) return 0;
        const int capacity = static_cast<int>((std::min)(remaining48, INT64_C(5760))) * 2 - produced_sub_;
        const int latency = speex_resampler_get_input_latency(resampler_) + 16;
        zeros_.assign(static_cast<std::size_t>(latency) * channels_, 0.f);
        pending_.resize(static_cast<std::size_t>(capacity) * channels_); pending_at_ = 0;
        spx_uint32_t in = static_cast<spx_uint32_t>(latency), out = static_cast<spx_uint32_t>(capacity);
        if (speex_resampler_process_interleaved_float(resampler_, zeros_.data(), &in, pending_.data(), &out) != RESAMPLER_ERR_SUCCESS || !out) {
            Fail(L"Cannot flush chained Opus rate normalization."); return -1;
        }
        pending_.resize(static_cast<std::size_t>(out) * channels_); Produced(static_cast<int>(out)); return 1;
    }
    int Fill() {
        for (;;) {
            if (!active_) { const int n = StartLink(); if (n <= 0) return n; }
            if (!batch_.remaining48 && batch_.at == batch_.packets.size()) {
                if (logical_.ended) {
                    const int n = FlushNormalizer(); if (n) return n;
                    active_ = false; continue;
                }
                if (!AudioBatch(*demux_, logical_, batch_, settings_, error_)) return -1;
                if (!scanned_ && settings_.opus_hd && logical_.qext && native_rate_ != 96000) {
                    Fail(L"QEXT first appeared after the initial audio page on a forward-only stream."); return -1;
                }
            }
            Packet* packet = nullptr;
            int duration = 0;
            if (batch_.remaining48) { if (!Recover(duration)) return -1; }
            else {
                packet = &batch_.packets[batch_.at++]; duration = packet->duration;
                const int frames = duration * (native_rate_ / 48000);
                decoded_.resize(static_cast<std::size_t>(frames) * decode_channels_);
                const int n = opus_multistream_decode_float(decoder_, packet->data.data(), static_cast<opus_int32>(packet->data.size()), decoded_.data(), frames, 0);
                if (n != frames) { Fail(L"Opus packet decode failed or returned the wrong duration."); return -1; }
                if (bitrate_frames_ >= 48000) bitrate_bytes_ = bitrate_frames_ = 0;
                bitrate_bytes_ += static_cast<std::int64_t>(packet->data.size()); bitrate_frames_ += duration;
            }
            if (std::any_of(decoded_.begin(), decoded_.end(), [](float value) { return !std::isfinite(value); })) { Fail(L"Opus decoder returned non-finite PCM."); return -1; }
            Diagnostics(packet, duration);
            const int ratio = native_rate_ / 48000, frames = duration * ratio;
            const float* source = decoded_.data();
            if (logical_.header.family == 3) {
                const int rows = logical_.header.channels;
                projected_.assign(static_cast<std::size_t>(frames) * rows, 0.f);
                for (int column = 0; column < decode_channels_; ++column) for (int row = 0; row < rows; ++row) {
                    const auto at = static_cast<std::size_t>(column * rows + row) * 2;
                    const int word = Le16(logical_.header.matrix.data() + at);
                    const float weight = (word >= 32768 ? word - 65536 : word) / 32768.f;
                    if (!weight) continue;
                    for (int frame = 0; frame < frames; ++frame) projected_[static_cast<std::size_t>(frame) * rows + row] +=
                        weight * decoded_[static_cast<std::size_t>(frame) * decode_channels_ + column];
                }
                source = projected_.data();
            }
            if (logical_.header.family >= 2) {
                rendered_.resize(static_cast<std::size_t>(frames) * channels_); renderer_.Render(source, rendered_.data(), frames); source = rendered_.data();
            } else if (mode_ == OutputMode::Stereo) {
                rendered_.resize(static_cast<std::size_t>(frames) * 2); Stereo(source, rendered_.data(), frames, logical_.header.channels); source = rendered_.data();
            } else {
                ReorderToWindows(decoded_.data(), frames, channels_); source = decoded_.data();
            }
            if (std::any_of(source, source + static_cast<std::size_t>(frames) * channels_, [](float value) { return !std::isfinite(value); })) {
                Fail(L"Opus matrix/rendering returned non-finite PCM."); return -1;
            }
            const auto begin = batch_.decode_at, end = begin + duration; batch_.decode_at = end;
            const auto first = (std::max)(begin, logical_.base + logical_.header.preskip), last = (std::min)(end, batch_.limit);
            if (last <= first) continue;
            const auto offset = static_cast<std::size_t>((first - begin) * ratio) * channels_;
            const int playable = static_cast<int>((last - first) * ratio);
            if (!Normalize(source + offset, playable)) return -1;
            if (!pending_.empty()) return 1;
        }
    }
    float Gain() const noexcept {
        return static_cast<float>(OpusGainMultiplier(info_, gain_settings_, shuffle_));
    }
    void SwapRuntime(NativeReader& other) noexcept {
        using std::swap;
        swap(info_, other.info_); swap(demux_, other.demux_); swap(logical_, other.logical_); swap(batch_, other.batch_);
        swap(decoder_, other.decoder_); swap(dred_decoder_, other.dred_decoder_); swap(dred_, other.dred_); swap(resampler_, other.resampler_);
        swap(renderer_, other.renderer_); swap(decoded_, other.decoded_); swap(projected_, other.projected_); swap(rendered_, other.rendered_);
        swap(pending_, other.pending_); swap(zeros_, other.zeros_); swap(pending_at_, other.pending_at_); swap(metadata_, other.metadata_);
        swap(bitrate_bytes_, other.bitrate_bytes_); swap(bitrate_frames_, other.bitrate_frames_); swap(position48_, other.position48_);
        swap(position_sub_, other.position_sub_); swap(produced48_, other.produced48_); swap(produced_sub_, other.produced_sub_);
        swap(index_, other.index_); swap(native_rate_, other.native_rate_); swap(channels_, other.channels_); swap(decode_channels_, other.decode_channels_);
        swap(complexity_, other.complexity_); swap(instant_bitrate_, other.instant_bitrate_); swap(mode_, other.mode_); swap(revision_, other.revision_);
        swap(active_, other.active_); swap(failed_, other.failed_); swap(eof_, other.eof_); swap(prior_silk_, other.prior_silk_);
        swap(error_, other.error_); swap(recovery_note_, other.recovery_note_);
        swap(gain_, other.gain_); swap(gain_dirty_, other.gain_dirty_); swap(note_dirty_, other.note_dirty_);
        swap(published_recovery_note_, other.published_recovery_note_);
    }
    bool SeekWithMode(std::int64_t target, OutputMode mode) {
        if (!scanned_ || !info_.seekable || target < 0 || target > info_.total_frames) return false;
        if (mode == OutputMode::SourceChannels && !info_.native_channels) return false;
        const auto saved = input_->Tell();
        if (saved < 0) return false;
        try {
            NativeReader next(settings_);
            next.input_ = input_; next.known_ = known_; next.scanned_ = true; next.rate_ = rate_; next.needed_ = needed_;
            next.info_ = info_; next.gain_settings_ = gain_settings_; next.shuffle_ = shuffle_; next.mode_ = mode;
            next.channels_ = mode == OutputMode::Stereo ? 2 : info_.native_channels; next.revision_ = revision_;
            auto local = target; std::size_t index = 0; std::int64_t base = 0;
            while (index + 1 < known_.size() && local >= known_[index].frames48) { local -= known_[index].frames48; base += known_[index].frames48; ++index; }
            if (!input_->Seek(known_[index].bos_offset, SEEK_SET)) {
                if (!input_->Seek(saved, SEEK_SET)) { failed_ = true; Fail(L"Failed seek could not restore the input cursor."); }
                return false;
            }
            next.demux_ = std::make_unique<Demux>(input_, false, settings_.recovery != RecoveryMode::Strict);
            next.index_ = static_cast<int>(index) - 1; next.position48_ = base;
            if (next.StartLink() != 1) throw std::runtime_error("seek link");
            std::array<float, 5760 * kMaxChannels> scratch{};
            while (next.position48_ < target || next.position_sub_) {
                const auto left = target - next.position48_;
                const int capacity = static_cast<int>((std::min)(left, INT64_C(2880))) * (rate_ / 48000) - next.position_sub_;
                if (capacity <= 0 || next.Read(scratch.data(), capacity) <= 0) throw std::runtime_error("seek preroll");
            }
            if (target == info_.total_frames) { next.eof_ = true; next.pending_.clear(); next.pending_at_ = 0; }
            SwapRuntime(next); return true;
        } catch (...) {
            // Old queued packets and decoder remain untouched. Restore the
            // transport before permitting that old state to read again.
            if (!input_->Seek(saved, SEEK_SET)) { failed_ = true; error_ = L"Failed seek could not restore the input cursor."; }
            return false;
        }
    }
public:
    explicit NativeReader(const PlaybackSettings& settings) : settings_(ValidateSettings(settings)), gain_settings_(settings_) {}
    ~NativeReader() override { DestroyDecoder(); }
    bool Open(std::shared_ptr<NativeInput> input, std::wstring& error) {
        input_ = std::move(input);
        if (!input_) { error = L"No native Opus input."; return false; }
        info_.seekable = input_->Seekable(); info_.radio = !info_.seekable;
        info_.total_frames = info_.length_ms = -1;
        needed_ = info_.radio || settings_.opus_bwe || settings_.speech != SpeechMode::Off || settings_.ignore_extensions ||
            settings_.mono_compatible || settings_.recovery != RecoveryMode::Strict;
        const auto beginning = input_->Tell();
        if (info_.seekable && (beginning < 0 || !Scan() || !input_->Seek(beginning, SEEK_SET))) {
            error = error_.empty() ? L"Cannot index/rewind Ogg Opus." : error_; return false;
        }
        demux_ = std::make_unique<Demux>(input_, false, settings_.recovery != RecoveryMode::Strict);
        if (StartLink() != 1) { error = error_.empty() ? L"No Ogg Opus audio." : error_; return false; }
        return true;
    }
    int Read(float* output, int capacity) override {
        if (!output || capacity <= 0 || capacity > 5760) return OP_EINVAL;
        if (failed_) return OP_EBADLINK;
        try {
            if (pending_at_ >= pending_.size()) {
                pending_.clear(); pending_at_ = 0;
                if (eof_) return 0;
                const int n = Fill();
                if (n <= 0) { if (n < 0) failed_ = true; return n < 0 ? OP_EBADLINK : 0; }
            }
            const int count = (std::min)(capacity, static_cast<int>((pending_.size() - pending_at_) / channels_));
            if (gain_dirty_) { gain_ = Gain(); gain_dirty_ = false; }
            if (gain_ == 1) std::copy_n(pending_.data() + pending_at_, count * channels_, output);
            else for (int i = 0; i < count * channels_; ++i) output[i] = pending_[pending_at_ + i] * gain_;
            pending_at_ += static_cast<std::size_t>(count) * channels_;
            const int ratio = rate_ / 48000, advance = (position_sub_ + count) / ratio;
            if (position48_ > I64Max - advance) { failed_ = true; Fail(L"Opus playback position overflow."); return OP_EBADTIMESTAMP; }
            position48_ += advance; position_sub_ = (position_sub_ + count) % ratio;
            return count;
        } catch (...) { failed_ = true; Fail(L"Cannot allocate native Opus decoding buffers."); return OP_EFAULT; }
    }
    bool SeekFrame(std::int64_t frames) override { return SeekWithMode(frames, mode_); }
    std::int64_t PositionFrames() const noexcept override { return position48_; }
    int SampleRate() const noexcept override { return rate_; }
    int OutputChannels() const noexcept override { return channels_; }
    bool SetOutputMode(OutputMode mode) override {
        if (mode != OutputMode::Stereo && mode != OutputMode::SourceChannels) return false;
        if (mode == OutputMode::SourceChannels && !info_.native_channels) return false;
        if (mode == mode_) return true;
        return SeekWithMode(position48_, mode);
    }
    void SetGain(const PlaybackSettings& settings, bool shuffle) override {
        gain_settings_.gain_mode=settings.gain_mode; gain_settings_.pre_gain_q8=settings.pre_gain_q8;
        gain_settings_.radio_gain_q8=settings.radio_gain_q8; gain_settings_.target_lufs=settings.target_lufs;
        gain_settings_.preferred_gain_only=settings.preferred_gain_only;
        gain_settings_.prevent_clipping=settings.prevent_clipping; gain_settings_.peak_mode=settings.peak_mode;
        gain_settings_.non_rg_q8=settings.non_rg_q8;
        shuffle_=shuffle; gain_dirty_=true;
    }
    const TrackInfo& Info() const noexcept override { return info_; }
    unsigned MetadataRevision() const noexcept override { return revision_; }
    int InstantBitrate() noexcept override {
        if (bitrate_frames_ > 0) {
            instant_bitrate_ = static_cast<int>((std::min)(static_cast<std::int64_t>(INT_MAX), bitrate_bytes_ * 8 * 48000 / bitrate_frames_));
            bitrate_bytes_ = bitrate_frames_ = 0;
        }
        return instant_bitrate_ ? instant_bitrate_ : info_.bitrate;
    }
    bool Needed() const noexcept override { return needed_; }
    const std::wstring& Error() const noexcept override { return error_; }
};
} // namespace

std::unique_ptr<NativeOpus> OpenNativeOpus(std::shared_ptr<NativeInput> input, const PlaybackSettings& settings, std::wstring& error) {
    try {
        auto reader = std::make_unique<NativeReader>(settings);
        if (!reader->Open(std::move(input), error)) return {};
        return reader;
    } catch (...) { error = L"Cannot allocate or initialize native Ogg Opus decoder."; return {}; }
}
bool IsNativeOpusHead(const unsigned char* data, std::size_t bytes) noexcept {
    opus_native::Header header;
    return opus_native::ParseHeader(data, bytes, header);
}
} // namespace winamp_opus
