#include "opus_decoder.hpp"
#include "native_opus.hpp"
#include "gain_policy.hpp"
#include <ogg/ogg.h>
#include <opus_multistream.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace winamp_opus {
namespace {
int Read(void* stream, unsigned char* data, int bytes) {
    return static_cast<InputStream*>(stream)->Read(data, bytes);
}
int Seek(void* stream, opus_int64 offset, int origin) {
    return static_cast<InputStream*>(stream)->Seek(offset, origin) ? 0 : -1;
}
opus_int64 Tell(void* stream) { return static_cast<InputStream*>(stream)->Tell(); }
constexpr OpusFileCallbacks kCallbacks{Read, Seek, Tell, nullptr};
class NativeTransport final : public NativeInput {
    InputStream* stream_; // Decoder owns the stream and destroys it after its readers.
public:
    explicit NativeTransport(InputStream* stream) : stream_(stream) {}
    int Read(unsigned char* data, int bytes) noexcept override { return stream_->Read(data, bytes); }
    bool Seek(std::int64_t offset, int origin) noexcept override { return stream_->Seek(offset, origin); }
    std::int64_t Tell() const noexcept override { return stream_->Tell(); }
    bool Seekable() const noexcept override { return stream_->Seekable(); }
    void Cancel() noexcept override { stream_->Cancel(); }
    void ProbeComplete() noexcept override { stream_->FinishProbe(); }
};
// Bounded identification probe. It validates Ogg CRC and assembles only the
// first packet; an informational input-rate hint is never proof of QEXT.
bool AdvancedHead(const unsigned char* data, std::size_t bytes) {
    std::vector<unsigned char> packet;
    std::uint32_t serial = 0, sequence = 0;
    for (std::size_t at = 0; at + 27 <= bytes;) {
        const auto* h = data + at;
        if (std::memcmp(h, "OggS", 4) || h[4] || (h[5] & ~7)) return false;
        const auto segments = h[26];
        if (at + 27 + segments > bytes) return false;
        std::size_t body_size = 0;
        for (int i = 0; i < segments; ++i) body_size += h[27+i];
        const auto body = at + 27 + segments;
        if (body + body_size > bytes) return false;
        std::array<unsigned char, 282> header{};
        std::copy_n(h, 27 + segments, header.data());
        ogg_page page{header.data(), 27 + segments, const_cast<unsigned char*>(data + body), static_cast<long>(body_size)};
        const auto expected_crc = std::array<unsigned char,4>{h[22], h[23], h[24], h[25]};
        ogg_page_checksum_set(&page);
        if (!std::equal(expected_crc.begin(), expected_crc.end(), header.begin() + 22)) return false;
        auto le32 = [](const unsigned char* p) { return static_cast<std::uint32_t>(p[0]) |
            (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
            (static_cast<std::uint32_t>(p[3]) << 24); };
        if (!at) { if (h[5] != 2) return false; serial = le32(h+14); }
        else if (h[5] != 1 || le32(h+14) != serial) return false;
        if (le32(h+18) != sequence++) return false;
        std::size_t cursor = body;
        for (int i = 0; i < segments; ++i) {
            const auto n = h[27+i];
            if (packet.size() + n > 65025) return false;
            packet.insert(packet.end(), data + cursor, data + cursor + n); cursor += n;
            if (n < 255) return IsNativeOpusHead(packet.data(), packet.size());
        }
        at = body + body_size;
    }
    return false;
}
}

bool HasOpusContent(const wchar_t* path) noexcept {
    if (!path || !*path) return false;
    try {
        InputStream stream;
        // Identification never records, authenticates interactively, buffers an entire
        // file, or reconnects. Playback resolves its own explicit preferences later.
        if (!stream.Open(path)) return false;
        std::array<unsigned char, 65536> header{};
        std::size_t used = 0;
        while (used < header.size()) {
            const int requested = static_cast<int>(std::min<std::size_t>(512, header.size() - used));
            const int count = stream.Read(header.data() + used, requested);
            if (count < 0 || count > requested) return false;
            used += count;
            const int result = op_test(nullptr, header.data(), used);
            if (result == 0 || AdvancedHead(header.data(), used)) return true;
            if (!count || (result != OP_FALSE && result != OP_EBADHEADER && result != OP_EIMPL)) return false;
        }
    } catch (...) { }
    return false;
}
std::wstring Utf8ToWide(const char* text) {
    if (!text || !*text) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), size);
    result.pop_back(); return result;
}
Decoder::Decoder() = default;
Decoder::~Decoder() { Close(); }
int Decoder::SampleRate() const noexcept { return native_ ? native_->SampleRate() : kSampleRate; }
int Decoder::NativeChannels() const noexcept { return native_->OutputChannels(); }
const TrackInfo& Decoder::Info() const noexcept { return info_; }
unsigned Decoder::MetadataRevision() const noexcept { return metadata_revision_; }
void Decoder::RefreshNativeInfo() {
    const auto& source = native_->Info();
    const bool metadata_changed = native_revision_ != native_->MetadataRevision();
    const bool station_changed = http_ && station_revision_ != http_->StationRevision();
    if (metadata_changed || station_changed) {
        if (metadata_changed) { info_ = source; native_revision_ = native_->MetadataRevision(); }
        if (http_) {
            info_.station = http_->Station(); station_revision_ = http_->StationRevision();
            if (!info_.bitrate) info_.bitrate = info_.station.bitrate;
        }
        // ICY supplies an override, not a mutation of OpusTags. Clearing it restores
        // the codec title; errors in recording are technical metadata only.
        info_.display_title = source.display_title.empty() ? fallback_title_ : source.display_title;
        if (!info_.station.title.empty()) info_.display_title = info_.station.title;
        ++metadata_revision_;
    }
    info_.file_size = http_ ? http_->SourceSize() : -1;
    info_.bandwidth_hz = source.bandwidth_hz; info_.packet_duration_us = source.packet_duration_us;
    info_.pitch48 = source.pitch48; info_.final_range = source.final_range;
    info_.concealed_frames48 = source.concealed_frames48; info_.fec_frames48 = source.fec_frames48;
    info_.dred_frames48 = source.dred_frames48;
    info_.decoded_rate = source.decoded_rate; info_.output_rate = native_->SampleRate(); info_.qext = source.qext;
    if (info_.decoder_note != source.decoder_note) info_.decoder_note = source.decoder_note;
}
int Decoder::DecodeCallback(void* context, OpusMSDecoder* decoder, void* pcm, const ogg_packet* packet,
                           int frames, int channels, int format, int) {
    auto& self = *static_cast<Decoder*>(context);
    if (!packet || !pcm || frames <= 0 || channels < 1 || channels > kMaxChannels) return OPUS_BAD_ARG;
    // An absolute gain policy must be refreshed BEFORE the first packet of a new
    // logical link, not after that packet has already been rendered.
    if (op_current_link(self.file_) != self.metadata_link_) {
        self.RefreshTags();
        self.SetGain(self.settings_, self.shuffle_);
    }
    const int n = format == OP_DEC_FORMAT_FLOAT ? opus_multistream_decode_float(decoder, packet->packet,
        packet->bytes, static_cast<float*>(pcm), frames, 0) : opus_multistream_decode(decoder, packet->packet,
        packet->bytes, static_cast<opus_int16*>(pcm), frames, 0);
    if (n < 0) return n;
    if (n != frames) return OPUS_INVALID_PACKET;
    auto& info = self.info_;
    info.packet_duration_us = static_cast<int>(static_cast<std::int64_t>(frames) * 1000000 / 48000);
    const int band = opus_packet_get_bandwidth(packet->packet);
    info.bandwidth_hz = band == OPUS_BANDWIDTH_NARROWBAND ? 4000 : band == OPUS_BANDWIDTH_MEDIUMBAND ? 6000 :
        band == OPUS_BANDWIDTH_WIDEBAND ? 8000 : band == OPUS_BANDWIDTH_SUPERWIDEBAND ? 12000 :
        band == OPUS_BANDWIDTH_FULLBAND ? 20000 : 0;
    opus_multistream_decoder_ctl(decoder, OPUS_GET_FINAL_RANGE(&info.final_range));
    OpusDecoder* elementary = nullptr;
    if (opus_multistream_decoder_ctl(decoder, OPUS_MULTISTREAM_GET_DECODER_STATE(0, &elementary)) == OPUS_OK && elementary)
        opus_decoder_ctl(elementary, OPUS_GET_PITCH(&info.pitch48));
    return 0;
}
void Decoder::RefreshTags() {
    const int link = op_current_link(file_);
    if (metadata_revision_ && link == metadata_link_) return;
    const auto* tags = op_tags(file_, -1);
    const auto* head = op_head(file_, -1);
    std::map<std::string, std::wstring> parsed;
    int budget = 256 * 1024;
    if (tags) for (int i = 0; i < tags->comments && i < 512; ++i) {
        const int length = tags->comment_lengths[i];
        if (length <= 0 || length > 65536 || length > budget) continue;
        std::string tag(tags->user_comments[i], length);
        const auto equals = tag.find('=');
        if (equals == std::string::npos || equals == 0 || equals > 128) continue;
        auto key = tag.substr(0, equals);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == "metadata_block_picture" || key == "coverart") continue;
        auto value = Utf8ToWide(tag.c_str() + equals + 1);
        auto& stored = parsed[key];
        if (!stored.empty() && !value.empty()) stored += L"; ";
        stored += value; budget -= length;
    }
    // The link index, unlike an Ogg serial number, uniquely identifies a link
    // within this file. A position change inside it cannot change its tags.
    metadata_link_ = link;
    info_.current_link = link;
    info_.serial = static_cast<std::uint32_t>(op_serialno(file_, link));
    info_.format_version = head ? head->version : -1;
    info_.pre_skip48 = head ? static_cast<int>(head->pre_skip) : -1;
    info_.stream_count = head ? head->stream_count : 0;
    info_.coupled_count = head ? head->coupled_count : 0;
    const int header_gain = head ? head->output_gain : 0;
    const int source_channels = head ? head->channel_count : 0;
    const int source_rate = head ? static_cast<int>(std::min<opus_uint32>(head->input_sample_rate, INT_MAX)) : 0;
    const int mapping = head ? head->mapping_family : 0;
    int track_gain = 0, album_gain = 0;
    const bool has_track = tags && opus_tags_get_track_gain(tags, &track_gain) == 0;
    const bool has_album = tags && opus_tags_get_album_gain(tags, &album_gain) == 0;
    auto vendor = tags ? Utf8ToWide(tags->vendor) : L"";
    // Keep missing standard fields consistent with the native backend.
    const auto title = parsed["title"], artist = parsed["artist"], album = parsed["album"];
    if (metadata_revision_ && parsed == info_.tags && vendor == info_.vendor &&
        header_gain == info_.header_gain_q8 && source_channels == info_.source_channels &&
        source_rate == info_.source_sample_rate && mapping == info_.mapping_family &&
        has_track == info_.has_track_gain && has_album == info_.has_album_gain &&
        track_gain == info_.track_gain_q8 && album_gain == info_.album_gain_q8) {
        // Link identity is technical metadata used by the selected-link editor;
        // the host still filters title notifications by actual tags/title.
        ++metadata_revision_; return;
    }
    info_.tags = std::move(parsed); info_.vendor = std::move(vendor);
    info_.header_gain_q8 = header_gain; info_.source_channels = source_channels;
    info_.source_sample_rate = source_rate; info_.mapping_family = mapping;
    info_.has_track_gain = has_track; info_.has_album_gain = has_album;
    info_.track_gain_q8 = track_gain; info_.album_gain_q8 = album_gain;
    info_.title = title; info_.artist = artist; info_.album = album;
    const auto& name = info_.title.empty() ? fallback_title_ : info_.title;
    info_.display_title = info_.artist.empty() ? name : info_.artist + L" - " + name;
    ++metadata_revision_;
}
bool Decoder::Open(const wchar_t* path, int* error, const PlaybackSettings& settings) {
    Close();
    if (error) *error = OP_EFAULT;
    if (!path || !*path) return false;
    int code = 0;
    settings_ = ValidateSettings(settings);
    try {
        {
            std::lock_guard<std::mutex> lock(transport_mutex_);
            http_ = std::make_shared<InputStream>();
        }
        if (cancelled_.load()) { http_->Cancel(); if (error) *error = OP_EREAD; return false; }
        if (!http_->Open(path, settings_, &last_error_)) {
            const auto failure = last_error_; Close(); last_error_ = failure;
            if (error) *error = OP_EREAD; return false;
        }
        if (cancelled_.load()) { http_->Cancel(); if (error) *error = OP_EREAD; return false; }
        native_input_ = std::make_shared<NativeTransport>(http_.get());
        fallback_title_ = IsHttpUrl(path) ? (http_->Station().name.empty() ? StreamUrl(path) : http_->Station().name)
            : std::filesystem::path(path).stem().wstring();
        auto decoder_settings = settings_;
        // Speaker mixing happens once in PcmPipeline, after decoding/rendering.
        if (decoder_settings.output_mode != OutputMode::Stereo) decoder_settings.output_mode = OutputMode::SourceChannels;
        native_ = OpenNativeOpus(native_input_, decoder_settings, last_error_);
        if (!native_) {
            const auto failure = last_error_; Close(); last_error_ = failure;
            if (error) *error = OP_EBADLINK; return false;
        }
        if (IsHttpUrl(path) || native_->Needed()) {
            if (http_) http_->FinishProbe();
            native_->SetGain(settings_, false);
            native_revision_ = (std::numeric_limits<unsigned>::max)();
            RefreshNativeInfo();
            if (error) *error = 0; return true;
        }
        native_.reset(); native_input_.reset();
        // Reuse the existing handle/cache, rather than opening and reading a local
        // file twice when the ordinary libopusfile path is sufficient.
        if (!http_->Seek(0, SEEK_SET)) { Close(); if (error) *error = OP_EREAD; return false; }
        file_ = op_open_callbacks(http_.get(), &kCallbacks, nullptr, 0, &code);
        if (!file_) { Close(); if (error) *error = code; return false; }
        op_set_decode_callback(file_, DecodeCallback, this);
        const auto* head = op_head(file_, 0);
        if (!head) { Close(); if (error) *error = OP_EBADHEADER; return false; }
        info_.file_size = http_->SourceSize();
        info_.seekable = op_seekable(file_) != 0;
        info_.total_frames = info_.seekable ? op_pcm_total(file_, -1) : -1;
        if (info_.seekable && info_.total_frames < 0) { Close(); if (error) *error = OP_EBADLINK; return false; }
        info_.length_ms = info_.seekable ? static_cast<int>(std::min<std::int64_t>(info_.total_frames / 48, INT_MAX)) : -1;
        info_.source_channels = head->channel_count;
        info_.source_sample_rate = static_cast<int>(std::min<opus_uint32>(head->input_sample_rate, INT_MAX));
        info_.bitrate = std::max(0, op_bitrate(file_, -1)); info_.links = op_link_count(file_);
        info_.native_channels = info_.seekable ? head->channel_count : 0;
        for (int link = 0; info_.native_channels && link < info_.links; ++link) {
            const auto* layout = op_head(file_, link);
            if (!layout || layout->channel_count != head->channel_count || layout->channel_count < 1 ||
                layout->channel_count > kMaxChannels || (layout->mapping_family != 1 &&
                !(layout->mapping_family == 0 && layout->channel_count <= 2))) info_.native_channels = 0;
        }
        info_.decoder_note = L"libopusfile 48000 Hz; standard decoding; OSCE/BWE off";
        RefreshTags(); SetGain(settings_, false);
    } catch (...) { Close(); throw; }
    if (error) *error = 0; return true;
}
void Decoder::Cancel() noexcept {
    cancelled_.store(true);
    std::shared_ptr<HttpStream> transport;
    { std::lock_guard<std::mutex> lock(transport_mutex_); transport = http_; }
    if (transport) transport->Cancel();
}
void Decoder::Close() noexcept {
    native_.reset(); native_input_.reset(); last_error_.clear();
    if (file_) op_free(std::exchange(file_, nullptr));
    { std::lock_guard<std::mutex> lock(transport_mutex_); http_.reset(); }
    info_ = {}; fallback_title_.clear(); pending_error_ = 0;
    forced_eof_ = native_output_ = false; output_channels_ = kStereoChannels;
    metadata_revision_ = native_revision_ = station_revision_ = 0; metadata_link_ = -1; shuffle_ = false;
}
bool Decoder::SetOutputMode(OutputMode mode) noexcept {
    // Custom speaker modes are post-decoder operations; ask the codec for all
    // available Windows-order channels, never interpret their enum as stereo.
    if (mode != OutputMode::Stereo) mode = OutputMode::SourceChannels;
    if (native_) { try { const bool ok = native_->SetOutputMode(mode); native_revision_ = (std::numeric_limits<unsigned>::max)(); RefreshNativeInfo(); return ok; } catch (...) { return false; } }
    if (!file_ || (mode == OutputMode::SourceChannels && info_.native_channels == 0)) return false;
    native_output_ = mode == OutputMode::SourceChannels;
    output_channels_ = native_output_ ? info_.native_channels : kStereoChannels; return true;
}
void Decoder::SetGain(const PlaybackSettings& settings, bool shuffle) noexcept {
    if (&settings != &settings_) {
        // Copy only scalar audio policy. Network/path strings are immutable for
        // this decoder and must not allocate during a logical-link callback.
        settings_.gain_mode=settings.gain_mode; settings_.pre_gain_q8=settings.pre_gain_q8;
        settings_.radio_gain_q8=settings.radio_gain_q8; settings_.target_lufs=settings.target_lufs;
        settings_.preferred_gain_only=settings.preferred_gain_only;
        settings_.prevent_clipping=settings.prevent_clipping; settings_.peak_mode=settings.peak_mode;
        settings_.non_rg_q8=settings.non_rg_q8; settings_.dither=settings.dither;
    }
    shuffle_ = shuffle;
    if (native_) { try { native_->SetGain(settings_, shuffle); } catch (...) { } return; }
    if (!file_) return;
    // The shared policy includes OpusHead gain, R128, target, fallback and peaks.
    // Absolute mode prevents libopusfile from applying header/R128 a second time.
    op_set_gain_offset(file_, OP_ABSOLUTE_GAIN, OpusGainQ8(info_, settings_, shuffle));
    op_set_dither_enabled(file_, settings_.dither ? 1 : 0);
}
template<typename Sample>
int Decoder::ReadSamples(Sample* pcm, int capacity_frames) {
    if ((!file_ && !native_) || !pcm || capacity_frames <= 0 || capacity_frames > 5760) return OP_EINVAL;
    if (native_) {
        int n;
        if constexpr (std::is_same_v<Sample, float>) n = native_->Read(pcm, capacity_frames);
        else {
            std::array<float, 5760 * kMaxChannels> buffer{};
            n = native_->Read(buffer.data(), capacity_frames);
            if (n > capacity_frames) return OP_EFAULT;
            if (n > 0) for (int i = 0; i < n * OutputChannels(); ++i)
                pcm[i] = static_cast<std::int16_t>(std::lround(std::clamp(buffer[i] * 32768.0f, -32768.0f, 32767.0f)));
        }
        RefreshNativeInfo();
        if (n < 0) last_error_ = native_->Error();
        return n;
    }
    if (pending_error_) return std::exchange(pending_error_, 0);
    if (forced_eof_) return 0;
    int frames = 0, holes = 0;
    while (frames < capacity_frames) {
        auto* output = pcm + frames * output_channels_;
        const int samples = (capacity_frames - frames) * output_channels_;
        int got;
        if constexpr (std::is_same_v<Sample, float>)
            got = native_output_ ? op_read_float(file_, output, samples, nullptr) : op_read_float_stereo(file_, output, samples);
        else got = native_output_ ? op_read(file_, output, samples, nullptr) : op_read_stereo(file_, output, samples);
        if (got > 0) {
            if (native_output_) ReorderToWindows(output, got, output_channels_);
            frames += got; holes = 0;
            if (op_current_link(file_) != metadata_link_) RefreshTags();
        } else if (!got) break;
        else if (got == OP_HOLE && ++holes <= 32) continue;
        else {
            const int failure = got == OP_HOLE ? OP_EBADLINK : got;
            if (!frames) return failure;
            pending_error_ = failure; break;
        }
    }
    return frames;
}
int Decoder::ReadFrames(std::int16_t* pcm, int capacity) { return ReadSamples(pcm, capacity); }
int Decoder::ReadFloatFrames(float* pcm, int capacity) { return ReadSamples(pcm, capacity); }
int Decoder::InstantBitrate() noexcept { return native_ ? native_->InstantBitrate() : file_ ? op_bitrate_instant(file_) : -1; }
bool Decoder::SeekFrame(std::int64_t frame) {
    if (native_) {
        if (!Info().seekable) return false;
        const bool ok = native_->SeekFrame(std::clamp<std::int64_t>(frame, 0, Info().total_frames));
        if (ok) { try { RefreshNativeInfo(); } catch (...) { return false; } }
        return ok;
    }
    if (!file_ || !info_.seekable) return false;
    frame = std::clamp<std::int64_t>(frame, 0, info_.total_frames);
    if (frame == info_.total_frames) { forced_eof_ = true; pending_error_ = 0; return true; }
    if (op_pcm_seek(file_, frame) != 0) return false;
    forced_eof_ = false; pending_error_ = 0;
    if (op_current_link(file_) != metadata_link_) { RefreshTags(); SetGain(settings_, shuffle_); }
    return true;
}
bool Decoder::SeekMs(int milliseconds) { return SeekFrame(static_cast<std::int64_t>(std::max(0, milliseconds)) * 48); }
std::int64_t Decoder::PositionFrames() const noexcept {
    return native_ ? native_->PositionFrames() : forced_eof_ ? info_.total_frames : file_ ? op_pcm_tell(file_) : 0;
}
const wchar_t* ErrorText(int code) noexcept {
    switch (code) {
    case OP_EREAD: return L"Cannot read the file or HTTP stream.";
    case OP_ENOTFORMAT: return L"This is not an Ogg Opus stream.";
    case OP_EVERSION: return L"Unsupported Opus header version.";
    case OP_EBADHEADER: return L"Invalid Opus header.";
    case OP_EIMPL: return L"Unsupported channel mapping.";
    case OP_EBADLINK: return L"The Ogg Opus stream is damaged.";
    case OP_EBADTIMESTAMP: return L"Invalid Ogg timestamps.";
    case OP_EFAULT: return L"Cannot allocate decoder resources.";
    default: return L"Cannot decode the Opus stream.";
    }
}
} // namespace winamp_opus
