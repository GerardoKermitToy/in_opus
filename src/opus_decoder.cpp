#include "opus_decoder.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>

namespace winamp_opus {
namespace {
FILE* OpenLocalFile(const wchar_t* path) noexcept {
    if (!path || !*path) return nullptr;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") == 0) return file;
    if (!std::wcschr(path, L'|')) return nullptr;
    try {
        // Some host paths arrive after ANSI best-fit conversion of U+FF5C to
        // U+007C. Retry exactly one repaired path, only after the original fails.
        // "rb" opens existing files only; nothing is created, renamed or written.
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
int Seek(void* stream, opus_int64 offset, int origin) {
    return _fseeki64(static_cast<FILE*>(stream), offset, origin);
}
opus_int64 Tell(void* stream) { return _ftelli64(static_cast<FILE*>(stream)); }
int CloseFile(void* stream) { return std::fclose(static_cast<FILE*>(stream)); }
constexpr OpusFileCallbacks kCallbacks{Read, Seek, Tell, CloseFile};

std::wstring Tag(const OpusTags* tags, const char* name) {
    return tags ? Utf8ToWide(opus_tags_query(tags, name, 0)) : std::wstring{};
}
} // namespace

bool HasOpusContent(const wchar_t* path) noexcept {
    if (!path || !*path) return false;
    FILE* raw = OpenLocalFile(path);
    if (!raw) return false;
    std::unique_ptr<FILE, decltype(&std::fclose)> stream(raw, &std::fclose);
    // Bounded probe: usually one 512-byte read. Let libopusfile validate the Ogg
    // identification packet rather than searching arbitrary bytes for OpusHead.
    std::array<unsigned char, 65536> header{};
    std::size_t used = 0;
    while (used < header.size()) {
        const auto count = std::fread(header.data() + used, 1, 512, stream.get());
        used += count;
        const int result = op_test(nullptr, header.data(), used);
        if (result == 0) return true;
        if (result != OP_FALSE || count < 512) return false;
    }
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

bool Decoder::Open(const wchar_t* path, int* error) {
    Close();
    if (error) *error = OP_EFAULT;
    if (!path || !*path) return false;
    FILE* raw = OpenLocalFile(path);
    if (!raw) {
        if (error) *error = OP_EREAD;
        return false;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> stream(raw, &std::fclose);
    int code = 0;
    file_ = op_open_callbacks(stream.get(), &kCallbacks, nullptr, 0, &code);
    if (!file_) {
        if (error) *error = code;
        return false; // libopusfile does not close the stream on open failure.
    }
    stream.release(); // op_free now owns the stream and calls CloseFile.
    try {
        const auto* head = op_head(file_, 0);
        info_.total_frames = op_pcm_total(file_, -1);
        if (!head || info_.total_frames < 0 || !op_seekable(file_)) {
            Close();
            if (error) *error = OP_EBADLINK;
            return false;
        }
        info_.length_ms = static_cast<int>(std::min<std::int64_t>(
            info_.total_frames / (kSampleRate / 1000), std::numeric_limits<int>::max()));
        info_.source_channels = head->channel_count;
        info_.source_sample_rate = static_cast<int>(std::min<opus_uint32>(
            head->input_sample_rate, std::numeric_limits<int>::max()));
        info_.bitrate = std::max(0, op_bitrate(file_, -1));
        info_.links = op_link_count(file_);
        // Keep one output format for the entire file. Mixed-layout chains and
        // unspecified/ambisonic mappings cannot be represented safely by OUT.H,
        // which has a channel count but no speaker mask.
        info_.native_channels = head->channel_count;
        for (int link = 0; link < info_.links; ++link) {
            const auto* layout = op_head(file_, link);
            if (!layout || layout->channel_count != head->channel_count ||
                layout->channel_count < 1 || layout->channel_count > kMaxChannels ||
                (layout->mapping_family != 1 &&
                 !(layout->mapping_family == 0 && layout->channel_count <= 2))) {
                info_.native_channels = 0;
                break;
            }
        }
        const auto* tags = op_tags(file_, 0);
        info_.title = Tag(tags, "TITLE");
        info_.artist = Tag(tags, "ARTIST");
        info_.album = Tag(tags, "ALBUM");
        const auto name = info_.title.empty() ? std::filesystem::path(path).stem().wstring() : info_.title;
        info_.display_title = info_.artist.empty() ? name : info_.artist + L" - " + name;
    } catch (...) {
        Close();
        throw;
    }
    if (error) *error = 0;
    return true;
}

void Decoder::Close() noexcept {
    if (file_) op_free(std::exchange(file_, nullptr));
    info_ = {};
    pending_error_ = 0;
    forced_eof_ = false;
    native_output_ = false;
    output_channels_ = kStereoChannels;
}

bool Decoder::SetOutputMode(OutputMode mode) noexcept {
    if (!file_ || (mode == OutputMode::SourceChannels && info_.native_channels == 0)) return false;
    native_output_ = mode == OutputMode::SourceChannels;
    output_channels_ = native_output_ ? info_.native_channels : kStereoChannels;
    return true;
}

int Decoder::ReadFrames(std::int16_t* pcm, int capacity_frames) {
    if (!file_ || !pcm || capacity_frames <= 0 || capacity_frames > 5760) return OP_EINVAL;
    if (pending_error_ != 0) return std::exchange(pending_error_, 0);
    if (forced_eof_) return 0;
    int frames = 0;
    int holes = 0;
    while (frames < capacity_frames) {
        auto* output = pcm + frames * output_channels_;
        const int samples = (capacity_frames - frames) * output_channels_;
        const int got = native_output_ ? op_read(file_, output, samples, nullptr)
                                       : op_read_stereo(file_, output, samples);
        if (got > 0) {
            if (native_output_) ReorderToWindows(output, got, output_channels_);
            frames += got;
            holes = 0;
        } else if (got == 0) {
            break;
        } else if (got == OP_HOLE && ++holes <= 32) {
            continue; // A damaged Ogg page may be recoverable, but never spin forever.
        } else {
            const int failure = got == OP_HOLE ? OP_EBADLINK : got;
            if (frames == 0) return failure;
            pending_error_ = failure; // Do not discard already decoded audio.
            break;
        }
    }
    return frames;
}

bool Decoder::SeekFrame(std::int64_t frame) {
    if (!file_) return false;
    frame = std::clamp<std::int64_t>(frame, 0, info_.total_frames);
    // libopusfile accepts only positions strictly before the final frame.
    // Represent a seek to/past EOF logically, without seeking one sample early.
    if (frame == info_.total_frames) {
        forced_eof_ = true;
        pending_error_ = 0;
        return true;
    }
    if (op_pcm_seek(file_, frame) != 0) return false;
    forced_eof_ = false;
    pending_error_ = 0;
    return true;
}
bool Decoder::SeekMs(int milliseconds) {
    return SeekFrame(static_cast<std::int64_t>(std::max(0, milliseconds)) * (kSampleRate / 1000));
}
std::int64_t Decoder::PositionFrames() const noexcept {
    return forced_eof_ ? info_.total_frames : (file_ ? op_pcm_tell(file_) : 0);
}

const wchar_t* ErrorText(int code) noexcept {
    switch (code) {
    case OP_EREAD: return L"Cannot read the file.";
    case OP_ENOTFORMAT: return L"This is not an Ogg Opus file.";
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
