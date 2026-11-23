#include "opus_decoder.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace winamp_opus {
namespace {
FILE* OpenLocalFile(const wchar_t* path) noexcept {
    if (!path || !*path) return nullptr;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") == 0) return file;
    if (!std::wcschr(path, L'|')) return nullptr;
    try {
        // Retry only after the original fails. Never create, rename or write files.
        std::wstring repaired(path);
        std::replace(repaired.begin(), repaired.end(), L'|', L'｜');
        if (_wfopen_s(&file, repaired.c_str(), L"rb") == 0) return file;
    } catch (...) { }
    return nullptr;
}
int Read(void* stream, unsigned char* data, int bytes) {
    if (bytes <= 0) return 0;
    auto* file = static_cast<FILE*>(stream);
    const auto count = std::fread(data, 1, static_cast<std::size_t>(bytes), file);
    return count == 0 && std::ferror(file) ? -1 : static_cast<int>(count);
}
int Seek(void* stream, opus_int64 offset, int origin) { return _fseeki64(static_cast<FILE*>(stream), offset, origin); }
opus_int64 Tell(void* stream) { return _ftelli64(static_cast<FILE*>(stream)); }
int CloseFile(void* stream) { return std::fclose(static_cast<FILE*>(stream)); }
int ReadHttp(void* stream, unsigned char* data, int bytes) { return static_cast<HttpStream*>(stream)->Read(data, bytes); }
constexpr OpusFileCallbacks kCallbacks{Read, Seek, Tell, CloseFile};
constexpr OpusFileCallbacks kHttpCallbacks{ReadHttp, nullptr, nullptr, nullptr};
}

bool HasOpusContent(const wchar_t* path) noexcept {
    if (!path || !*path) return false;
    try {
        std::unique_ptr<FILE, decltype(&std::fclose)> local(nullptr, &std::fclose);
        std::unique_ptr<HttpStream> http;
        if (IsHttpUrl(path)) {
            http = std::make_unique<HttpStream>();
            if (!http->Open(path)) return false;
        } else {
            local.reset(OpenLocalFile(path));
            if (!local) return false;
        }
        // No whole-file scans, full decoder, or unbounded tag download.
        std::array<unsigned char, 65536> header{};
        std::size_t used = 0;
        while (used < header.size()) {
            const int count = http ? http->Read(header.data() + used, 512)
                : Read(local.get(), header.data() + used, 512);
            if (count < 0) return false;
            used += count;
            const int result = op_test(nullptr, header.data(), used);
            if (result == 0) return true;
            if (result != OP_FALSE || count == 0) return false;
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
    result.pop_back();
    return result;
}
Decoder::~Decoder() { Close(); }

void Decoder::RefreshTags() {
    const auto* tags = op_tags(file_, -1);
    const auto* head = op_head(file_, -1);
    info_.tags.clear();
    info_.vendor = tags ? Utf8ToWide(tags->vendor) : L"";
    info_.header_gain_q8 = head ? head->output_gain : 0;
    if (head) {
        info_.source_channels = head->channel_count;
        info_.source_sample_rate = static_cast<int>(std::min<opus_uint32>(head->input_sample_rate, INT_MAX));
    }
    info_.has_track_gain = tags && opus_tags_get_track_gain(tags, &info_.track_gain_q8) == 0;
    info_.has_album_gain = tags && opus_tags_get_album_gain(tags, &info_.album_gain_q8) == 0;
    // Bound metadata kept in memory. Embedded pictures are not textual tags.
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
        auto& stored = info_.tags[key];
        if (!stored.empty() && !value.empty()) stored += L"; ";
        stored += value;
        budget -= length;
    }
    info_.title = info_.tags["title"];
    info_.artist = info_.tags["artist"];
    info_.album = info_.tags["album"];
    const auto& name = info_.title.empty() ? fallback_title_ : info_.title;
    info_.display_title = info_.artist.empty() ? name : info_.artist + L" - " + name;
    ++metadata_revision_;
}
bool Decoder::Open(const wchar_t* path, int* error) {
    Close();
    if (error) *error = OP_EFAULT;
    if (!path || !*path) return false;
    int code = 0;
    try {
        if (IsHttpUrl(path)) {
            http_ = std::make_unique<HttpStream>();
            if (!http_->Open(path)) { Close(); if (error) *error = OP_EREAD; return false; }
            file_ = op_open_callbacks(http_.get(), &kHttpCallbacks, nullptr, 0, &code);
        } else {
            std::unique_ptr<FILE, decltype(&std::fclose)> stream(OpenLocalFile(path), &std::fclose);
            if (!stream) { if (error) *error = OP_EREAD; return false; }
            file_ = op_open_callbacks(stream.get(), &kCallbacks, nullptr, 0, &code);
            if (file_) stream.release();
        }
        if (!file_) { Close(); if (error) *error = code; return false; }
        const auto* head = op_head(file_, 0);
        if (!head) { Close(); if (error) *error = OP_EBADHEADER; return false; }
        info_.radio = http_ != nullptr;
        info_.seekable = op_seekable(file_) != 0;
        info_.total_frames = info_.seekable ? op_pcm_total(file_, -1) : -1;
        if (info_.seekable && info_.total_frames < 0) { Close(); if (error) *error = OP_EBADLINK; return false; }
        info_.length_ms = info_.seekable ? static_cast<int>(std::min<std::int64_t>(
            info_.total_frames / 48, std::numeric_limits<int>::max())) : -1;
        info_.source_channels = head->channel_count;
        info_.source_sample_rate = static_cast<int>(std::min<opus_uint32>(head->input_sample_rate, INT_MAX));
        info_.bitrate = std::max(0, op_bitrate(file_, -1));
        info_.links = op_link_count(file_);
        // A live chained stream may change layout without notice: keep stereo.
        info_.native_channels = info_.seekable ? head->channel_count : 0;
        for (int link = 0; info_.native_channels && link < info_.links; ++link) {
            const auto* layout = op_head(file_, link);
            if (!layout || layout->channel_count != head->channel_count || layout->channel_count < 1 ||
                layout->channel_count > kMaxChannels || (layout->mapping_family != 1 &&
                !(layout->mapping_family == 0 && layout->channel_count <= 2))) info_.native_channels = 0;
        }
        if (http_) {
            http_->FinishProbe();
            info_.station = http_->Station();
            if (!info_.bitrate) info_.bitrate = info_.station.bitrate;
            fallback_title_ = info_.station.name.empty() ? StreamUrl(path) : info_.station.name;
        } else fallback_title_ = std::filesystem::path(path).stem().wstring();
        metadata_serial_ = op_serialno(file_, -1);
        RefreshTags();
    } catch (...) { Close(); throw; }
    if (error) *error = 0;
    return true;
}
void Decoder::Cancel() noexcept { if (http_) http_->Cancel(); }
void Decoder::Close() noexcept {
    if (file_) op_free(std::exchange(file_, nullptr));
    http_.reset();
    info_ = {};
    fallback_title_.clear();
    pending_error_ = 0;
    forced_eof_ = native_output_ = false;
    output_channels_ = kStereoChannels;
    metadata_revision_ = 0;
}
bool Decoder::SetOutputMode(OutputMode mode) noexcept {
    if (!file_ || (mode == OutputMode::SourceChannels && info_.native_channels == 0)) return false;
    native_output_ = mode == OutputMode::SourceChannels;
    output_channels_ = native_output_ ? info_.native_channels : kStereoChannels;
    return true;
}
void Decoder::SetGain(const PlaybackSettings& settings, bool shuffle) noexcept {
    if (!file_) return;
    int type = OP_HEADER_GAIN;
    switch (settings.gain_mode) {
    case GainMode::Album: type = OP_ALBUM_GAIN; break;
    case GainMode::Track: type = OP_TRACK_GAIN; break;
    case GainMode::Auto: type = shuffle ? OP_TRACK_GAIN : OP_ALBUM_GAIN; break;
    case GainMode::Raw: type = OP_ABSOLUTE_GAIN; break;
    default: break;
    }
    // R128 gains are relative to -23 LUFS. Never change header-only loudness
    // simply because the user changed the ReplayGain target.
    const bool tagged = type == OP_TRACK_GAIN ? info_.has_track_gain :
        type == OP_ALBUM_GAIN && (info_.has_album_gain || info_.has_track_gain);
    const int offset = (info_.radio ? settings.radio_gain_q8 : settings.pre_gain_q8) +
        (tagged ? (settings.target_lufs + 23) * 256 : 0);
    op_set_gain_offset(file_, type, offset);
    op_set_dither_enabled(file_, settings.dither ? 1 : 0);
}
template<typename Sample>
int Decoder::ReadSamples(Sample* pcm, int capacity_frames) {
    if (!file_ || !pcm || capacity_frames <= 0 || capacity_frames > 5760) return OP_EINVAL;
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
            frames += got;
            holes = 0;
            const auto serial = op_serialno(file_, -1);
            if (serial != metadata_serial_) {
                metadata_serial_ = serial;
                info_.source_channels = op_channel_count(file_, -1);
                RefreshTags();
            }
        } else if (!got) break;
        else if (got == OP_HOLE && ++holes <= 32) continue;
        else {
            const int failure = got == OP_HOLE ? OP_EBADLINK : got;
            if (!frames) return failure;
            pending_error_ = failure;
            break;
        }
    }
    return frames;
}
int Decoder::ReadFrames(std::int16_t* pcm, int capacity) { return ReadSamples(pcm, capacity); }
int Decoder::ReadFloatFrames(float* pcm, int capacity) { return ReadSamples(pcm, capacity); }
int Decoder::InstantBitrate() noexcept { return file_ ? op_bitrate_instant(file_) : -1; }
bool Decoder::SeekFrame(std::int64_t frame) {
    if (!file_ || !info_.seekable) return false;
    frame = std::clamp<std::int64_t>(frame, 0, info_.total_frames);
    if (frame == info_.total_frames) { forced_eof_ = true; pending_error_ = 0; return true; }
    if (op_pcm_seek(file_, frame) != 0) return false;
    forced_eof_ = false; pending_error_ = 0;
    return true;
}
bool Decoder::SeekMs(int milliseconds) { return SeekFrame(static_cast<std::int64_t>(std::max(0, milliseconds)) * 48); }
std::int64_t Decoder::PositionFrames() const noexcept { return forced_eof_ ? info_.total_frames : (file_ ? op_pcm_tell(file_) : 0); }
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
