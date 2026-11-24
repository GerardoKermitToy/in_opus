// SPDX-License-Identifier: GPL-3.0-only
// Deterministic encoder-generated fixtures only; the only network is loopback.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "fixtures.hpp"
#include "native_opus.hpp"
#include "opus_packet.hpp"
#include "ambisonics.hpp"
#include "pcm_pipeline.hpp"
#include <ogg/ogg.h>
#include <opus_multistream.h>
#include <opus_projection.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace winamp_opus;
namespace {
constexpr double Pi = 3.14159265358979323846;
void Le(std::vector<unsigned char>& data, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) data.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
void Put(std::vector<unsigned char>& data, std::size_t at, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) data.at(at + i) = static_cast<unsigned char>(value >> (8 * i));
}
std::vector<unsigned char> Bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Check(file.good(), "open generated fixture");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Save(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(file.good(), "write generated fixture");
}
std::size_t PageSize(const std::vector<unsigned char>& bytes, std::size_t at) {
    Check(at + 27 <= bytes.size() && !std::memcmp(bytes.data() + at, "OggS", 4), "fixture page boundary");
    const int segments = bytes[at + 26]; Check(at + 27 + segments <= bytes.size(), "fixture lacing boundary");
    std::size_t count = 27 + segments;
    for (int i = 0; i < segments; ++i) count += bytes[at + 27 + i];
    Check(at + count <= bytes.size(), "fixture page extent"); return count;
}
std::vector<std::size_t> Pages(const std::vector<unsigned char>& bytes) {
    std::vector<std::size_t> pages;
    for (std::size_t at = 0; at < bytes.size(); at += PageSize(bytes, at)) pages.push_back(at);
    return pages;
}
void Crc(std::vector<unsigned char>& bytes, std::size_t at) {
    const auto size = PageSize(bytes, at); const long header = 27 + bytes[at + 26];
    ogg_page page{bytes.data() + at, header, bytes.data() + at + header, static_cast<long>(size - header)};
    ogg_page_checksum_set(&page);
}
std::vector<unsigned char> WithoutPages(const std::vector<unsigned char>& bytes, std::size_t first, std::size_t count) {
    const auto pages = Pages(bytes); Check(first + count < pages.size(), "bounded page removal");
    auto result = bytes;
    result.erase(result.begin() + pages[first], result.begin() + pages[first + count]); return result;
}
std::vector<unsigned char> TagsPacket(const std::vector<std::string>& extra) {
    std::vector<unsigned char> tags{'O','p','u','s','T','a','g','s'};
    const std::string vendor = "Generated native Opus fixture";
    Le(tags, vendor.size(), 4); tags.insert(tags.end(), vendor.begin(), vendor.end());
    std::vector<std::string> comments{"TITLE=Native Opus", "ARTIST=Generated signal"};
    comments.insert(comments.end(), extra.begin(), extra.end());
    Le(tags, comments.size(), 4);
    for (const auto& text : comments) { Le(tags, text.size(), 4); tags.insert(tags.end(), text.begin(), text.end()); }
    return tags;
}
struct Writer {
    ogg_stream_state state{};
    std::ofstream file;
    ogg_int64_t number = 0;
    Writer(const std::filesystem::path& path, int serial) : file(path, std::ios::binary | std::ios::trunc) {
        Check(file.good() && ogg_stream_init(&state, serial) == 0, "fixture Ogg writer");
    }
    ~Writer() { ogg_stream_clear(&state); }
    void Packet(std::vector<unsigned char>& data, std::int64_t granule, bool first, bool last, bool flush = true) {
        ogg_packet packet{data.data(), static_cast<long>(data.size()), first ? 1L : 0L, last ? 1L : 0L, granule, number++};
        Check(ogg_stream_packetin(&state, &packet) == 0, "queue fixture packet");
        ogg_page page{};
        while (flush ? ogg_stream_flush(&state, &page) : ogg_stream_pageout(&state, &page)) {
            file.write(reinterpret_cast<char*>(page.header), page.header_len);
            file.write(reinterpret_cast<char*>(page.body), page.body_len);
        }
        Check(file.good(), "write fixture Ogg page");
    }
};
struct Options {
    int family = 0, channels = 2, frames48 = 16013, serial = 51001;
    int hint = 48000, group = 1, late_qext = 0, projection_rows = 0;
    int gain_q8 = 0;
    std::int64_t granule_offset = 0;
    bool hd = false, speech = false, intensity = false, fec = false, dred = false;
    std::vector<std::string> tags;
};
struct Encoder {
    OpusMSEncoder* ms = nullptr;
    OpusProjectionEncoder* projection = nullptr;
    int streams = 0, coupled = 0;
    std::vector<unsigned char> mapping;
    ~Encoder() {
        if (ms) opus_multistream_encoder_destroy(ms);
        if (projection) opus_projection_encoder_destroy(projection);
    }
    int Ctl(int request, int value) {
        return projection ? opus_projection_encoder_ctl(projection, request, value) : opus_multistream_encoder_ctl(ms, request, value);
    }
    void Qext(bool enabled) {
        for (int i = 0; i < streams; ++i) {
            OpusEncoder* state = nullptr;
            const int n = projection ? opus_projection_encoder_ctl(projection, OPUS_MULTISTREAM_GET_ENCODER_STATE(i, &state)) :
                opus_multistream_encoder_ctl(ms, OPUS_MULTISTREAM_GET_ENCODER_STATE(i, &state));
            Check(n == OPUS_OK && state && opus_encoder_ctl(state, OPUS_SET_QEXT(enabled ? 1 : 0)) == OPUS_OK, "set encoder QEXT");
        }
    }
};
void Fixture(const std::filesystem::path& path, Options o) {
    const int rate = o.hd ? 96000 : o.speech ? 16000 : 48000, block = rate / 50;
    Encoder enc; enc.mapping.resize(o.channels); int error = OPUS_OK;
    const int application = o.speech ? OPUS_APPLICATION_VOIP : o.intensity ? OPUS_APPLICATION_RESTRICTED_LOWDELAY : OPUS_APPLICATION_AUDIO;
    if (o.family == 3 && !o.projection_rows)
        enc.projection = opus_projection_ambisonics_encoder_create(rate, o.channels, 3, &enc.streams, &enc.coupled, application, &error);
    else enc.ms = opus_multistream_surround_encoder_create(rate, o.channels, o.projection_rows ? 255 : o.family,
        &enc.streams, &enc.coupled, enc.mapping.data(), application, &error);
    Check((enc.ms || enc.projection) && error == OPUS_OK, "create native fixture encoder");
    int lookahead = 0;
    Check((enc.projection ? opus_projection_encoder_ctl(enc.projection, OPUS_GET_LOOKAHEAD(&lookahead)) :
        opus_multistream_encoder_ctl(enc.ms, OPUS_GET_LOOKAHEAD(&lookahead))) == OPUS_OK, "fixture lookahead");
    const int preskip = lookahead * 48000 / rate;
    Check(lookahead * 48000 % rate == 0, "48k preskip conversion");
    if (o.hd) enc.Qext(true);
    Check(enc.Ctl(OPUS_SET_BITRATE_REQUEST, o.hd ? o.channels * 640000 : o.intensity ? 32000 :
        o.fec || o.dred ? 64000 : o.speech ? 12000 : o.channels * 64000) == OPUS_OK, "fixture bitrate");
    if (o.speech) {
        Check(enc.Ctl(OPUS_SET_MAX_BANDWIDTH_REQUEST, OPUS_BANDWIDTH_WIDEBAND) == OPUS_OK, "wideband speech");
        Check(enc.Ctl(OPUS_SET_SIGNAL_REQUEST, OPUS_SIGNAL_VOICE) == OPUS_OK, "speech signal");
        Check(enc.Ctl(OPUS_SET_VBR_REQUEST, 0) == OPUS_OK, "speech CBR");
    }
    if (o.intensity) {
        Check(enc.Ctl(OPUS_SET_FORCE_CHANNELS_REQUEST, 2) == OPUS_OK, "force intensity stereo");
        Check(enc.Ctl(OPUS_SET_BANDWIDTH_REQUEST, OPUS_BANDWIDTH_FULLBAND) == OPUS_OK, "force fullband intensity fixture");
        Check(enc.Ctl(OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST, 0) == OPUS_OK, "allow encoded intensity phase inversion");
    }
    if (o.fec) {
        Check(enc.Ctl(OPUS_SET_INBAND_FEC_REQUEST, 1) == OPUS_OK, "enable encoder FEC");
        Check(enc.Ctl(OPUS_SET_PACKET_LOSS_PERC_REQUEST, 30) == OPUS_OK, "FEC loss probability");
    }
    if (o.dred) for (int stream = 0; stream < enc.streams; ++stream) {
        OpusEncoder* state = nullptr;
        const int n = enc.projection ? opus_projection_encoder_ctl(enc.projection, OPUS_MULTISTREAM_GET_ENCODER_STATE(stream, &state)) :
            opus_multistream_encoder_ctl(enc.ms, OPUS_MULTISTREAM_GET_ENCODER_STATE(stream, &state));
        Check(n == OPUS_OK && state && opus_encoder_ctl(state, OPUS_SET_DRED_DURATION(12)) == OPUS_OK, "enable elementary encoder DRED");
    }
    int matrix_gain = 0;
    std::vector<unsigned char> matrix;
    if (enc.projection) {
        int size = 0;
        Check(opus_projection_encoder_ctl(enc.projection, OPUS_PROJECTION_GET_DEMIXING_MATRIX_SIZE(&size)) == OPUS_OK && size > 0, "demix size");
        matrix.resize(size);
        Check(opus_projection_encoder_ctl(enc.projection, OPUS_PROJECTION_GET_DEMIXING_MATRIX(matrix.data(), size)) == OPUS_OK, "demix bytes");
        Check(opus_projection_encoder_ctl(enc.projection, OPUS_PROJECTION_GET_DEMIXING_MATRIX_GAIN(&matrix_gain)) == OPUS_OK, "matrix gain");
    }
    const int rows = o.projection_rows ? o.projection_rows : o.channels;
    if (o.projection_rows) for (int column = 0; column < o.channels; ++column) for (int row = 0; row < rows; ++row)
        Le(matrix, column + 1 == o.channels && !row ? 32767 : 0, 2); // Last coded column into W only.
    std::vector<unsigned char> head{'O','p','u','s','H','e','a','d',1,static_cast<unsigned char>(rows)};
    Le(head, preskip, 2); Le(head, o.hint, 4); Le(head, static_cast<unsigned>(matrix_gain + o.gain_q8), 2);
    head.push_back(static_cast<unsigned char>(o.family));
    if (o.family) {
        head.push_back(static_cast<unsigned char>(enc.streams)); head.push_back(static_cast<unsigned char>(enc.coupled));
        if (o.family == 3) head.insert(head.end(), matrix.begin(), matrix.end());
        else head.insert(head.end(), enc.mapping.begin(), enc.mapping.end());
    }
    Writer writer(path, o.serial);
    writer.Packet(head, 0, true, false);
    auto tags = TagsPacket(o.tags); writer.Packet(tags, 0, false, false);
    const int packets = (o.frames48 + preskip + 959) / 960;
    std::vector<float> pcm(static_cast<std::size_t>(block) * o.channels);
    std::vector<unsigned char> data(512 * 1024);
    std::uint32_t rng = 0x95a731cd;
    for (int p = 0; p < packets; ++p) {
        if (o.hd && o.late_qext) enc.Qext(p >= o.late_qext);
        for (int i = 0; i < block; ++i) {
            const auto sample = static_cast<std::int64_t>(p) * block + i;
            const double t = static_cast<double>(sample) / rate;
            for (int c = 0; c < o.channels; ++c) {
                double value = .16 * std::sin(2 * Pi * (300 + 137 * c) * t);
                if (o.hd) value = .12 * std::sin(2 * Pi * 1000 * t) + .22 * std::sin(2 * Pi * 30000 * t);
                if (o.speech) {
                    const double phase = 2 * Pi * (135 * t + .7 * std::sin(2 * Pi * 1.7 * t));
                    value = .19 * (std::sin(phase) + .45 * std::sin(2 * phase) + .24 * std::sin(3 * phase) + .12 * std::sin(5 * phase));
                    value *= .65 + .35 * std::sin(2 * Pi * 3.2 * t);
                }
                if (o.intensity) {
                    // Anti-correlated broadband channels force an encoded
                    // inverted intensity band, unlike independent random noise.
                    if (!c) {
                        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                        value = .22 * (static_cast<double>(rng) / 2147483648. - 1.);
                    } else value = -pcm[static_cast<std::size_t>(i) * o.channels];
                }
                pcm[static_cast<std::size_t>(i) * o.channels + c] = sample * 48000 < static_cast<std::int64_t>(o.frames48) * rate ? static_cast<float>(value) : 0.f;
            }
        }
        const int bytes = enc.projection ? opus_projection_encode_float(enc.projection, pcm.data(), block, data.data(), static_cast<opus_int32>(data.size())) :
            opus_multistream_encode_float(enc.ms, pcm.data(), block, data.data(), static_cast<opus_int32>(data.size()));
        Check(bytes > 0, "encode native fixture packet");
        std::vector<unsigned char> packet(data.begin(), data.begin() + bytes);
        const bool last = p + 1 == packets;
        const auto granule = o.granule_offset + (last ? o.frames48 + preskip : (p + 1) * 960);
        writer.Packet(packet, granule, false, last, last || (p + 1) % o.group == 0);
    }
}

class MemoryInput final : public NativeInput {
    std::vector<unsigned char> bytes_;
    std::int64_t at_ = 0;
    bool seekable_ = true, cancelled_ = false;
public:
    bool fail_next_seek = false;
    explicit MemoryInput(std::vector<unsigned char> bytes, bool seekable = true) : bytes_(std::move(bytes)), seekable_(seekable) {}
    int Read(unsigned char* data, int bytes) noexcept override {
        if (cancelled_) return -1;
        if (bytes < 0 || at_ < 0 || static_cast<std::uint64_t>(at_) > bytes_.size()) return -1;
        const int count = static_cast<int>((std::min)({bytes_.size() - static_cast<std::size_t>(at_), static_cast<std::size_t>(bytes), std::size_t(257)}));
        if (count) std::memcpy(data, bytes_.data() + static_cast<std::size_t>(at_), count);
        at_ += count; return count;
    }
    bool Seek(std::int64_t offset, int origin) noexcept override {
        if (!seekable_) return false;
        if (fail_next_seek) { fail_next_seek = false; at_ = static_cast<std::int64_t>(bytes_.size()); return false; }
        const auto base = origin == SEEK_SET ? INT64_C(0) : origin == SEEK_CUR ? at_ : origin == SEEK_END ? static_cast<std::int64_t>(bytes_.size()) : INT64_MIN;
        if (base < 0 || (offset > 0 && base > INT64_MAX - offset) || (offset < 0 && offset < -base)) return false;
        const auto next = base + offset;
        if (next < 0 || static_cast<std::uint64_t>(next) > bytes_.size()) return false;
        at_ = next; return true;
    }
    std::int64_t Tell() const noexcept override { return at_; }
    bool Seekable() const noexcept override { return seekable_; }
    void Cancel() noexcept override { cancelled_ = true; }
};
std::unique_ptr<NativeOpus> Reader(std::vector<unsigned char> bytes, PlaybackSettings settings = {}, bool live = false) {
    std::wstring error;
    auto reader = OpenNativeOpus(std::make_shared<MemoryInput>(std::move(bytes), !live), settings, error);
    if (!reader) std::wcerr << L"Native open: " << error << L'\n';
    Check(static_cast<bool>(reader), "open native fixture"); return reader;
}
std::unique_ptr<NativeOpus> Reader(const std::filesystem::path& path, PlaybackSettings settings = {}) { return Reader(Bytes(path), settings); }
bool Rejects(const std::vector<unsigned char>& bytes, PlaybackSettings settings = {}) {
    std::wstring error;
    return !OpenNativeOpus(std::make_shared<MemoryInput>(bytes), settings, error);
}
std::vector<float> Drain(NativeOpus& reader, int capacity = 37) {
    std::vector<float> pcm;
    const int rate = reader.SampleRate(), channels = reader.OutputChannels();
    std::vector<float> buffer(static_cast<std::size_t>(capacity) * channels + 16);
    for (int guard = 0; guard < 100000; ++guard) {
        std::fill(buffer.begin(), buffer.end(), 123456.f);
        const int n = reader.Read(buffer.data() + 8, capacity);
        if (n < 0) std::wcerr << L"Native read: " << reader.Error() << L'\n';
        Check(n >= 0 && n <= capacity, "bounded native read");
        Check(reader.SampleRate() == rate && reader.OutputChannels() == channels, "fixed native output format across links");
        for (int i = 0; i < 8; ++i) Check(buffer[i] == 123456.f, "native output lower canary");
        for (std::size_t i = 8 + static_cast<std::size_t>(n) * channels; i < buffer.size(); ++i) Check(buffer[i] == 123456.f, "native output upper canary");
        if (!n) return pcm;
        for (int i = 0; i < n * channels; ++i) Check(std::isfinite(buffer[8 + i]), "finite native PCM");
        pcm.insert(pcm.end(), buffer.begin() + 8, buffer.begin() + 8 + n * channels);
    }
    throw std::runtime_error("native read guard");
}
std::vector<float> Reference(const std::filesystem::path& path, bool source) {
    const auto bytes = Bytes(path); int error = 0;
    std::unique_ptr<OggOpusFile, decltype(&op_free)> file(op_open_memory(bytes.data(), bytes.size(), &error), &op_free);
    Check(file && !error, "reference libopusfile open");
    const int channels = source ? op_channel_count(file.get(), -1) : 2;
    std::array<float, 5760 * kMaxChannels> buffer{};
    std::vector<float> result;
    for (;;) {
        const int n = source ? op_read_float(file.get(), buffer.data(), static_cast<int>(buffer.size()), nullptr) :
            op_read_float_stereo(file.get(), buffer.data(), static_cast<int>(buffer.size()));
        Check(n >= 0, "reference float decode");
        if (!n) return result;
        if (source) ReorderToWindows(buffer.data(), n, channels);
        result.insert(result.end(), buffer.begin(), buffer.begin() + n * channels);
    }
}
double Difference(const std::vector<float>& a, const std::vector<float>& b) {
    Check(a.size() == b.size(), "differential PCM exact size");
    double difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) difference += std::abs(a[i] - b[i]);
    return difference / (std::max)(std::size_t(1), a.size());
}
double Tone(const std::vector<float>& pcm, int rate, int channels, double frequency) {
    const auto frames = pcm.size() / channels, first = (std::min)(std::size_t(2000), frames / 4);
    double real = 0, imaginary = 0;
    for (std::size_t i = first; i < frames; ++i) {
        const double phase = 2 * Pi * frequency * i / rate;
        real += pcm[i * channels] * std::cos(phase); imaginary -= pcm[i * channels] * std::sin(phase);
    }
    return 2 * std::hypot(real, imaginary) / (frames - first);
}
void ExactSeek(NativeOpus& reader, const std::vector<float>& reference, const std::vector<std::int64_t>& points) {
    const int ratio = reader.SampleRate() / 48000, channels = reader.OutputChannels();
    for (auto target : points) {
        Check(reader.SeekFrame(target) && reader.PositionFrames() == target, "sample-exact 48k native seek");
        const auto tail = Drain(reader, 113);
        const auto offset = static_cast<std::size_t>(target * ratio) * channels;
        Check(offset <= reference.size() && tail.size() == reference.size() - offset, "native seek exact remaining length");
        Check(std::equal(tail.begin(), tail.end(), reference.begin() + offset), "native seek matches sequential decoder and renderer history");
    }
}

void ParserTests() {
    opus_native::Header header;
    std::vector<unsigned char> head{'O','p','u','s','H','e','a','d',1,4,0,0,0,0,0,0,0,0,2,4,0,0,1,2,3};
    Check(IsNativeOpusHead(head.data(), head.size()), "recognize complete family2 head");
    for (std::size_t i = 0; i < head.size(); ++i) Check(!IsNativeOpusHead(head.data(), i), "reject every incomplete header prefix");
    auto bad = head; bad[9] = 5; Check(!IsNativeOpusHead(bad.data(), bad.size()), "reject incomplete harmonic order");
    bad = head; bad[19] = 0; Check(!IsNativeOpusHead(bad.data(), bad.size()), "reject zero elementary streams");
    bad = head; bad[20] = 5; Check(!IsNativeOpusHead(bad.data(), bad.size()), "reject coupled stream overflow");
    bad = head; bad[21] = 4; Check(!IsNativeOpusHead(bad.data(), bad.size()), "reject invalid map index");
    bad = head; bad[21] = 255; Check(IsNativeOpusHead(bad.data(), bad.size()), "allow explicitly silent mapped channel");
    bad = head; bad[8] = 16; Check(!IsNativeOpusHead(bad.data(), bad.size()), "reject future major version");
    bad = head; bad[18] = 255; Check(!IsNativeOpusHead(bad.data(), bad.size()), "reject unsupported mapping family");
    bad.assign(head.begin(), head.begin() + 21); bad[18] = 3;
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) Le(bad, c == r ? 32767 : 0, 2);
    Check(opus_native::ParseHeader(bad.data(), bad.size(), header), "parse projection matrix");
    bad.pop_back(); Check(!IsNativeOpusHead(bad.data(), bad.size()), "reject truncated projection matrix");
    bad.push_back(0); bad.push_back(0); Check(!IsNativeOpusHead(bad.data(), bad.size()), "reject excess projection matrix data");
    Check(!IsNativeOpusHead(head.data(), 65026), "reject oversized head before pointer movement");
    int duration = 0; bool qext = false;
    const std::array<unsigned char, 6> extension{0x83,0x41,2,0,0xf8,0};
    Check(opus_native::InspectPacket(extension.data(), extension.size(), 1, duration, qext) && qext, "detect actual long QEXT extension");
    auto fake = extension; fake[4] = 7; fake[5] = 0xf8;
    Check(opus_native::InspectPacket(fake.data(), fake.size(), 1, duration, qext) && !qext, "do not find QEXT in another extension payload");
    const std::array<unsigned char, 3> unequal{0x80,0,0x88};
    Check(!opus_native::InspectPacket(unequal.data(), unequal.size(), 2, duration, qext), "reject unequal elementary durations");
    Check(!opus_native::InspectPacket(fake.data(), opus_native::MaxPacketBytes + 1, 1, duration, qext), "reject oversized packet before pointer movement");
    for (unsigned n = 0; n < 65536; ++n) {
        const std::array<unsigned char, 3> packet{static_cast<unsigned char>(n), static_cast<unsigned char>(n >> 8), 0xff};
        opus_native::InspectPacket(packet.data(), packet.size(), 1 + (n & 3), duration, qext);
    }
    int order = 0, harmonics = 0;
    Check(AmbisonicShape(227, order, harmonics) && order == 14 && harmonics == 225, "bounded maximum ambisonic order");
    Check(!AmbisonicShape(255, order, harmonics), "reject ambiguous harmonic shape");
    AmbisonicRenderer renderer; std::array<float, 8> out{};
    Check(renderer.Init(4, 48000, AmbisonicMode::Surround71), "initialize Windows ambisonic renderer");
    const std::array<float, 4> y{0,1,0,0}; renderer.Render(y.data(), out.data(), 1);
    Check(out[3] == 0 && out[0] > 0 && out[1] < 0 && out[4] > 0 && out[5] < 0 && out[6] > 0 && out[7] < 0, "ACN Y and silent LFE in Windows7.1 order");
    Check(renderer.Init(6, 48000, AmbisonicMode::Surround51), "initialize non-diegetic stereo");
    const std::array<float, 6> pair{0,0,0,0,.25f,-.5f}; renderer.Render(pair.data(), out.data(), 1);
    Check(out[0] == .25f && out[1] == -.5f && out[2] == 0 && out[3] == 0 && out[4] == 0 && out[5] == 0, "non-diegetic pair routes directly");
}

void SpeakerTests(const std::filesystem::path& directory) {
    for (int channels = 1; channels <= 8; ++channels) {
        const auto path = directory / (L"speakers-" + std::to_wstring(channels) + L".opus");
        MakeOpus(path, channels, 7013, 52000 + channels);
        auto reader = Reader(path);
        Check(!reader->Needed() && reader->OutputChannels() == 2 && reader->Info().native_channels == channels, "ordinary local default remains legacy eligible");
        Check(Difference(Drain(*reader, 5760), Reference(path, false)) < 2e-7, "speaker downmix agrees with libopusfile");
        Check(reader->SetOutputMode(OutputMode::SourceChannels), "select homogeneous source channels");
        Check(reader->SeekFrame(0), "rewind source channel test");
        Check(Difference(Drain(*reader, 5760), Reference(path, true)) < 2e-7, "native channels agree with Windows reordered libopusfile");
        Decoder decoder;
        Check(decoder.Open(path.c_str()) && !decoder.NativeActive(), "public default uses original libopusfile integer path");
        PlaybackSettings enhanced; enhanced.mono_compatible = true;
        Check(decoder.Open(path.c_str(), nullptr, enhanced) && decoder.NativeActive(), "public explicit mono control activates native backend");
    }
}
void GainTests(const std::filesystem::path& directory) {
    const auto path = directory / L"gains.opus";
    Options options; options.gain_q8 = 2 * 256; options.tags = {"R128_TRACK_GAIN=256", "R128_ALBUM_GAIN=1280"}; Fixture(path, options);
    PlaybackSettings settings; settings.gain_mode = GainMode::Raw;
    auto raw = Reader(path, settings); const auto baseline = Drain(*raw);
    for (const auto mode : {GainMode::Header, GainMode::Track, GainMode::Album, GainMode::Auto, GainMode::Raw}) {
        for (bool shuffle : {false, true}) {
            settings.gain_mode = mode; settings.pre_gain_q8 = 128; settings.radio_gain_q8 = 9 * 256; settings.target_lufs = -18;
            auto reader = Reader(path, settings); reader->SetGain(settings, shuffle);
            const auto pcm = Drain(*reader);
            const bool track = mode == GainMode::Track || (mode == GainMode::Auto && shuffle);
            const bool album = mode == GainMode::Album || (mode == GainMode::Auto && !shuffle);
            const int gain = 128 + (mode == GainMode::Raw ? 0 : 512) + (track ? 256 + 5 * 256 : album ? 1280 + 5 * 256 : 0);
            const float factor = static_cast<float>(std::pow(10., gain / (20. * 256)));
            Check(pcm.size() == baseline.size(), "gain exact sample count");
            for (std::size_t i = 0; i < pcm.size(); ++i) Check(std::abs(pcm[i] - baseline[i] * factor) < 2e-6f, "header/R128/auto/raw gain applied exactly once");
        }
    }
    options.tags = {"R128_TRACK_GAIN=256"}; Fixture(path, options);
    settings.gain_mode = GainMode::Album; settings.pre_gain_q8 = 0; settings.target_lufs = -23;
    auto album = Reader(path, settings); const auto a = Drain(*album);
    settings.gain_mode = GainMode::Track;
    auto track = Reader(path, settings); Check(a == Drain(*track), "album gain falls back to track");
    auto live = Reader(Bytes(path), settings, true); auto local = Reader(path, settings);
    const auto l = Drain(*live), f = Drain(*local);
    const float radio = static_cast<float>(std::pow(10., settings.radio_gain_q8 / (20. * 256)));
    for (std::size_t i = 0; i < f.size(); ++i) Check(std::abs(l[i] - f[i] * radio) < 2e-6f, "radio gain separate from local pregain");
}
void HdAndEnhancementTests(const std::filesystem::path& directory) {
    Options ordinary; ordinary.hint = 96000; Fixture(directory / L"ordinary.opus", ordinary);
    auto normal = Reader(directory / L"ordinary.opus");
    Check(!normal->Needed() && normal->SampleRate() == 48000 && !normal->Info().qext, "96k input hint never activates HD");
    Options hd; hd.hd = true; hd.hint = 8000; hd.granule_offset = 120; Fixture(directory / L"hd.opus", hd);
    auto reader = Reader(directory / L"hd.opus");
    Check(reader->Needed() && reader->SampleRate() == 96000 && reader->Info().qext && reader->Info().total_frames == hd.frames48, "actual QEXT activation and 48k granule duration");
    const auto high = Drain(*reader);
    Check(high.size() == static_cast<std::size_t>(hd.frames48) * 4 && reader->PositionFrames() == hd.frames48, "native96k exact trimmed count and48k position");
    const double ultrasonic = Tone(high, 96000, 2, 30000);
    Check(ultrasonic > .025, "real 30kHz QEXT, not ordinary48k upsampling");
    std::cout << "Native QEXT 30kHz amplitude=" << ultrasonic << '\n';
    Check(reader->Info().packet_duration_us == 20000 && reader->Info().bandwidth_hz > 0 && reader->Info().final_range != 0 && reader->InstantBitrate() > 0, "packet diagnostics from actual decoder state");
    ExactSeek(*reader, high, {0,1,137,961,12345,hd.frames48});
    PlaybackSettings settings; settings.ignore_extensions = true;
    auto ignored = Reader(directory / L"hd.opus", settings);
    Check(Difference(high, Drain(*ignored)) > 1e-4, "elementary ignore_extensions actually changes QEXT PCM");
    settings = {}; settings.opus_hd = false;
    auto low = Reader(directory / L"hd.opus", settings);
    Check(low->SampleRate() == 48000 && Drain(*low).size() == static_cast<std::size_t>(hd.frames48) * 2, "HD disabled preserves exact48k length");
    hd.late_qext = 3; hd.granule_offset = 0; Fixture(directory / L"late-hd.opus", hd);
    auto late = Reader(directory / L"late-hd.opus"); Check(late->SampleRate() == 96000, "seekable scan detects late QEXT");
    auto forward = Reader(Bytes(directory / L"late-hd.opus"), {}, true); std::array<float, 11520> temporary{}; int n = 1, guard = 0;
    while (n > 0 && ++guard < 1000) n = forward->Read(temporary.data(), 5760);
    Check(n < 0 && guard < 1000, "late live QEXT fails rather than changing stable output or faking HD");
    Options speech; speech.channels = 1; speech.speech = true; speech.frames48 = 48000; Fixture(directory / L"speech.opus", speech);
    auto plain = Reader(directory / L"speech.opus"); const auto original = Drain(*plain, 5760);
    settings = {}; settings.speech = SpeechMode::Lace;
    auto lace = Reader(directory / L"speech.opus", settings); const auto lace_pcm = Drain(*lace, 5760);
    settings.speech = SpeechMode::NoLace;
    auto nolace = Reader(directory / L"speech.opus", settings); const auto nolace_pcm = Drain(*nolace, 5760);
    settings = {}; settings.opus_bwe = true;
    auto bwe = Reader(directory / L"speech.opus", settings); const auto bwe_pcm = Drain(*bwe, 5760);
    Check(Difference(original, lace_pcm) > 1e-6 && Difference(original, nolace_pcm) > 1e-6 && Difference(lace_pcm, nolace_pcm) > 1e-6, "LACE and NoLACE actual eligible speech differential");
    Check(Difference(original, bwe_pcm) > 1e-6, "per-elementary OSCE BWE actual PCM differential");
    std::cout << "Speech differences: LACE=" << Difference(original, lace_pcm) << ", NoLACE=" <<
        Difference(original, nolace_pcm) << ", BWE=" << Difference(original, bwe_pcm) << '\n';
    Options intensity; intensity.intensity = true; intensity.frames48 = 24000; Fixture(directory / L"intensity.opus", intensity);
    auto phase = Reader(directory / L"intensity.opus"); const auto phase_pcm = Drain(*phase, 5760);
    settings = {}; settings.mono_compatible = true;
    auto mono = Reader(directory / L"intensity.opus", settings);
    Check(Difference(phase_pcm, Drain(*mono, 5760)) > 1e-6, "phase inversion control changes actual intensity stereo PCM");
}

void AmbisonicAndChainTests(const std::filesystem::path& directory) {
    PlaybackSettings settings;
    for (int family : {2,3}) {
        const auto path = directory / (family == 2 ? L"ambisonic.opus" : L"projection.opus");
        Options o; o.family = family; o.channels = 4; o.frames48 = 7013; o.serial += family; Fixture(path, o);
        for (const auto mode : {AmbisonicMode::Stereo, AmbisonicMode::Headphones, AmbisonicMode::Quad, AmbisonicMode::Surround51, AmbisonicMode::Surround71}) {
            settings.ambisonic = mode;
            auto reader = Reader(path, settings);
            Check(reader->Info().source_channels == 4 && reader->OutputChannels() == 2, "original ACN source count and safe initial stereo");
            const auto stereo = Drain(*reader);
            Check(stereo.size() == 7013 * 2 && std::any_of(stereo.begin(), stereo.end(), [](float f) { return std::abs(f) > 1e-7f; }), "family2/3 stereo render nonzero and exact");
            Check(reader->SeekFrame(0) && reader->SetOutputMode(OutputMode::SourceChannels), "select ambisonic preferred render layout");
            const int channels = mode == AmbisonicMode::Quad ? 4 : mode == AmbisonicMode::Surround51 ? 6 : mode == AmbisonicMode::Surround71 ? 8 : 2;
            Check(reader->OutputChannels() == channels && reader->Info().native_channels == channels, "ambisonic rendered layout is native output");
            const auto pcm = Drain(*reader);
            Check(pcm.size() == static_cast<std::size_t>(7013) * channels, "exact ambisonic render count");
            if (channels >= 6) for (int i = 0; i < 7013; ++i) Check(pcm[static_cast<std::size_t>(i) * channels + 3] == 0, "ambisonic LFE silent");
            ExactSeek(*reader, pcm, {137,960,7013});
            if (mode == AmbisonicMode::Headphones) Check(reader->Info().decoder_note.find(L"not measured HRTF") != std::wstring::npos, "approximate headphone rendering identified honestly");
        }
    }
    for (int coded : {1,6}) {
        const auto path = directory / (L"rectangular-" + std::to_wstring(coded) + L".opus");
        Options o; o.family = 3; o.channels = coded; o.projection_rows = 4; o.frames48 = 7013; Fixture(path, o);
        auto reader = Reader(path); const auto pcm = Drain(*reader);
        Check(pcm.size() == 7013 * 2 && Tone(pcm, 48000, 2, 300 + 137 * (coded - 1)) > .003, "rectangular CxK matrix includes LAST coded column for K<C and K>C");
        for (int i = 0; i < 7013; ++i) Check(pcm[2 * i] == pcm[2 * i + 1], "W-only rectangular demix centered");
        Check(reader->Info().source_channels == 4 && reader->Info().decoder_note.find(L"rectangular Q15") != std::wstring::npos, "rectangular demix source channels and diagnostics");
    }
    auto chain = Bytes(directory / L"ordinary.opus"); const auto hd = Bytes(directory / L"hd.opus");
    chain.insert(chain.end(), hd.begin(), hd.end()); Save(directory / L"mixed.opus", chain);
    settings = {};
    auto mixed = Reader(chain, settings);
    Check(mixed->SampleRate() == 96000 && mixed->Info().links == 2 && mixed->Info().total_frames == 32026, "mixed48/96 fixed max rate and48k indexed total");
    const auto pcm = Drain(*mixed);
    Check(pcm.size() == 32026 * 4 && mixed->PositionFrames() == 32026, "mixed48/96 exact normalized count");
    ExactSeek(*mixed, pcm, {1,137,16012,16013,16014,30000,32026});
    auto surround_chain = Bytes(directory / L"ordinary.opus"); const auto ambi = Bytes(directory / L"ambisonic.opus");
    surround_chain.insert(surround_chain.end(), ambi.begin(), ambi.end());
    settings.ambisonic = AmbisonicMode::Surround51;
    auto changing = Reader(surround_chain, settings);
    Check(changing->Info().native_channels == 0 && !changing->SetOutputMode(OutputMode::SourceChannels), "mixed rendered channel counts reject SourceChannels");
    const auto revision = changing->MetadataRevision();
    Check(Drain(*changing).size() == (16013 + 7013) * 2 && changing->MetadataRevision() > revision && changing->Info().source_channels == 4, "mixed channel chain stays stereo and refreshes source metadata");
    auto gain_chain = Bytes(directory / L"ordinary.opus"); const auto gains = Bytes(directory / L"gains.opus");
    gain_chain.insert(gain_chain.end(), gains.begin(), gains.end());
    settings = {}; settings.gain_mode = GainMode::Album;
    auto both = Reader(gain_chain, settings); const auto chained = Drain(*both);
    auto first = Reader(directory / L"ordinary.opus", settings), second = Reader(directory / L"gains.opus", settings);
    auto expected = Drain(*first); const auto tail = Drain(*second); expected.insert(expected.end(), tail.begin(), tail.end());
    Check(chained == expected, "current chain gain/header/tags take effect before returning chain samples");
}

void MalformedAndRecoveryTests(const std::filesystem::path& directory) {
    const auto bytes = Bytes(directory / L"hd.opus");
    const auto pages = Pages(bytes);
    Check(pages.size() > 10, "recovery fixture page count");
    PlaybackSettings recover; recover.recovery = RecoveryMode::Plc;
    const auto gap = WithoutPages(bytes, 6, 2);
    Check(Rejects(gap), "Strict default rejects later complete-page loss");
    for (const auto mode : {RecoveryMode::Plc, RecoveryMode::NeuralPlc, RecoveryMode::Fec, RecoveryMode::Dred}) {
        recover.recovery = mode;
        auto reader = Reader(gap, recover); const auto pcm = Drain(*reader);
        Check(pcm.size() == 16013 * 4 && reader->Info().concealed_frames48 == 1920 && !reader->Info().fec_frames48 && !reader->Info().dred_frames48, "bounded PLC exact gap length and truthful ineligible redundancy counters");
        if (mode == RecoveryMode::Fec || mode == RecoveryMode::Dred) Check(reader->Info().decoder_note.find(L"fallback") != std::wstring::npos, "ineligible redundancy explains PLC fallback");
    }
    recover.recovery = RecoveryMode::Plc; recover.recovery_limit_ms = 20;
    Check(Rejects(gap, recover), "configured recovery cap enforced");
    recover.recovery_limit_ms = 120;
    Check(Rejects(WithoutPages(bytes, 3, 7), recover), "excessive interior gap rejected");
    Check(Rejects(WithoutPages(bytes, 2, 1), recover), "local initial uncertainty never concealed");
    Check(Rejects(WithoutPages(bytes, pages.size() - 2, 1), recover), "EOS gap uncertainty never concealed");
    auto bad = bytes; bad[pages[5] + PageSize(bytes, pages[5]) - 1] ^= 1;
    Check(Rejects(bad, recover), "CRC violation never concealed");
    Save(directory / L"bad-crc.opus", bad);
    bad = bytes; bad[30] ^= 1; Check(Rejects(bad, recover), "header CRC violation rejected"); Save(directory / L"bad-head-crc.opus", bad);
    Check(!HasOpusContent((directory / L"bad-head-crc.opus").c_str()), "prefix content probe rejects invalid head CRC");
    bad = bytes; bad.pop_back(); Check(Rejects(bad, recover), "truncation never concealed");
    bad = bytes; bad[pages[5] + 5] |= 1; Crc(bad, pages[5]); Check(Rejects(bad, recover), "orphan continuation never concealed");
    bad = bytes; Put(bad, pages[5] + 6, 999999, 8); Crc(bad, pages[5]); Check(Rejects(bad, recover), "granule discontinuity without a page gap never concealed");
    bad = bytes; Put(bad, pages[5] + 18, 99, 4); Crc(bad, pages[5]); Check(Rejects(bad, recover), "sequence gap without matching missing duration rejected");
    bad = bytes; Put(bad, pages[5] + 6, UINT64_MAX - 1, 8); Crc(bad, pages[5]); Check(Rejects(bad, recover), "signed granule overflow rejected");
    const auto other = Bytes(directory / L"ordinary.opus");
    bad.assign(bytes.begin(), bytes.begin() + pages[1]);
    bad.insert(bad.end(), other.begin(), other.begin() + PageSize(other, 0));
    bad.insert(bad.end(), bytes.begin() + pages[1], bytes.end());
    Check(Rejects(bad, recover), "ambiguous overlapping Opus logical streams rejected");
    const auto projection = Bytes(directory / L"projection.opus"); bad = projection;
    const auto head_bytes = PageSize(bad, 0); bad.erase(bad.begin() + head_bytes - 1); --bad[27]; Crc(bad, 0);
    Check(Rejects(bad, recover), "short family3 matrix never concealed");
    auto input = std::make_shared<MemoryInput>(bytes); std::wstring error;
    auto seeking = OpenNativeOpus(input, {}, error); Check(static_cast<bool>(seeking), "open transactional seek test");
    const auto sequential = Drain(*seeking); Check(seeking->SeekFrame(137), "prepare failed-seek position");
    const auto old_cursor = input->Tell(); input->fail_next_seek = true;
    Check(!seeking->SeekFrame(3000) && seeking->PositionFrames() == 137 && input->Tell() == old_cursor, "failed seek restores transport and logical cursor");
    const auto after = Drain(*seeking);
    Check(std::equal(after.begin(), after.end(), sequential.begin() + 137 * 4), "failed seek leaves exact playable decoder history intact");
    Check(!seeking->SeekFrame(INT64_MAX) && !seeking->SeekFrame(-1), "out-of-range seek rejected");
    Save(directory / L"recoverable.opus", gap);
    Decoder facade;
    Check(!facade.Open((directory / L"recoverable.opus").c_str()), "public strict default rejects later gap");
    Check(facade.Open((directory / L"recoverable.opus").c_str(), nullptr, recover) && facade.NativeActive(), "public recovery selection activates native backend");
}

std::size_t RedundantGapPage(const std::vector<unsigned char>& bytes, bool dred) {
    const auto pages = Pages(bytes);
    int error = OPUS_OK;
    std::unique_ptr<OpusDREDDecoder, decltype(&opus_dred_decoder_destroy)> decoder(dred ? opus_dred_decoder_create(&error) : nullptr, &opus_dred_decoder_destroy);
    std::unique_ptr<OpusDRED, decltype(&opus_dred_free)> packet(dred ? opus_dred_alloc(&error) : nullptr, &opus_dred_free);
    if (dred) Check(decoder && packet && error == OPUS_OK, "DRED model decoder creation");
    int maximum_coverage = 0, minimum_end = INT_MAX;
    for (std::size_t page = 8; page + 2 < pages.size(); ++page) {
        const auto next = pages[page + 1];
        const std::size_t header = 27 + bytes[next + 26];
        const auto count = PageSize(bytes, next) - header;
        const auto* data = bytes.data() + next + header;
        if (dred) {
            int end = 0;
            const int n = opus_dred_parse(decoder.get(), packet.get(), data, static_cast<opus_int32>(count), 960, 48000, &end, 0);
            maximum_coverage = (std::max)(maximum_coverage, n); minimum_end = (std::min)(minimum_end, end);
            if (n >= 120 && end == 0) return page;
        } else if (opus_packet_has_lbrr(data, static_cast<opus_int32>(count)) > 0) return page;
    }
    if (dred) std::cout << "DRED fixture: maximum coverage=" << maximum_coverage << ", minimum end=" << minimum_end << '\n';
    throw std::runtime_error(dred ? "encoder did not emit usable DRED redundancy" : "encoder did not emit usable FEC redundancy");
}
void RedundancyTests(const std::filesystem::path& directory) {
    for (bool dred : {false, true}) {
        Options o; o.channels = 1; o.speech = true; o.frames48 = 57600; o.fec = !dred; o.dred = dred;
        const auto path = directory / (dred ? L"dred.opus" : L"fec.opus");
        if (dred) {
            int error = 0; OpusEncoder* encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &error);
            Check(encoder && error == OPUS_OK, "DRED capability encoder");
            const int enabled = opus_encoder_ctl(encoder, OPUS_SET_DRED_DURATION(12)); opus_encoder_destroy(encoder);
            if (enabled == OPUS_UNIMPLEMENTED) { std::cout << "DRED encoder unavailable in this libopus build; bounded fallback tested\n"; continue; }
            Check(enabled == OPUS_OK, "DRED encoder control");
        }
        Fixture(path, o); const auto bytes = Bytes(path);
        const auto gap = WithoutPages(bytes, RedundantGapPage(bytes, dred), 1);
        Check(Rejects(gap), "strict rejects redundancy-bearing page gap");
        PlaybackSettings settings; settings.recovery = dred ? RecoveryMode::Dred : RecoveryMode::Fec;
        auto reader = Reader(gap, settings); const auto pcm = Drain(*reader, 5760);
        Check(pcm.size() == static_cast<std::size_t>(o.frames48) * 2 && reader->PositionFrames() == o.frames48, "redundancy recovery exact PCM and timeline counts");
        Check(reader->Info().dred_frames48 + reader->Info().fec_frames48 + reader->Info().concealed_frames48 == 960,
            "recovery counters partition the exact missing interval");
        if (dred) Check(reader->Info().dred_frames48 > 0, "actual DRED used for covered sub-packet interval");
        else Check(reader->Info().fec_frames48 == 960 && reader->Info().concealed_frames48 == 0, "actual FEC is not relabelled PLC");
        std::cout << (dred ? "DRED" : "FEC") << " recovery: PLC=" << reader->Info().concealed_frames48 << ", FEC=" <<
            reader->Info().fec_frames48 << ", DRED=" << reader->Info().dred_frames48 << " frames at48k\n";
        if (!dred) {
            settings.recovery = RecoveryMode::Dred;
            auto fallback = Reader(gap, settings); Drain(*fallback, 5760);
            Check(fallback->Info().fec_frames48 == 960 && fallback->Info().dred_frames48 == 0 &&
                fallback->Info().concealed_frames48 == 0, "DRED mode uses actual FEC when DRED is absent");
        }
        settings.recovery = RecoveryMode::Plc;
        auto basic = Reader(gap, settings); const auto plc = Drain(*basic, 5760);
        Check(Difference(pcm, plc) > 1e-7, "redundancy PCM differs from PLC fallback");
        if (dred) {
            settings.recovery = RecoveryMode::NeuralPlc;
            auto neural = Reader(gap, settings);
            Check(neural->Info().concealed_frames48 == 0, "scan alone does not count unplayed recovery");
            Check(Difference(plc, Drain(*neural, 5760)) > 1e-8, "neural PLC actually changes eligible speech recovery");
        }
    }
}

// Exactly one loopback HTTP response; a reconnect is observable and fails.
class LiveSource {
    SOCKET socket_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::atomic<bool> stop_{false};
    std::vector<unsigned char> body_;
    std::thread thread_;
    static bool Send(SOCKET socket, const char* data, int count) noexcept {
        while (count > 0) { const int n = send(socket, data, count, 0); if (n <= 0) return false; data += n; count -= n; }
        return true;
    }
    void Run() noexcept {
        while (!stop_) {
            fd_set ready; FD_ZERO(&ready); FD_SET(socket_, &ready); timeval wait{0,100000};
            if (select(0, &ready, nullptr, nullptr, &wait) <= 0) continue;
            const SOCKET client = accept(socket_, nullptr, nullptr); if (client == INVALID_SOCKET) continue;
            ++requests;
            DWORD timeout = 1000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            std::array<char, 8192> request{}; std::size_t used = 0;
            while (used + 1 < request.size()) {
                const int n = recv(client, request.data() + used, static_cast<int>(request.size() - 1 - used), 0);
                if (n <= 0) break;
                used += n; if (std::strstr(request.data(), "\r\n\r\n")) break;
            }
            constexpr char response[] = "HTTP/1.1 200 OK\r\nContent-Type: audio/ogg\r\nicy-name: Generated native fixture\r\nConnection: close\r\n\r\n";
            if (requests == 1 && Send(client, response, sizeof(response) - 1)) Send(client, reinterpret_cast<const char*>(body_.data()), static_cast<int>(body_.size()));
            shutdown(client, SD_BOTH); closesocket(client);
        }
    }
public:
    std::atomic<int> requests{0};
    explicit LiveSource(std::vector<unsigned char> body) : body_(std::move(body)) {
        WSADATA data{}; Check(WSAStartup(MAKEWORD(2,2), &data) == 0, "loopback startup");
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); Check(socket_ != INVALID_SOCKET, "loopback socket");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "loopback bind");
        int size = sizeof(address);
        Check(getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) == 0 && listen(socket_, 2) == 0, "loopback listen");
        port_ = ntohs(address.sin_port); thread_ = std::thread([this] { Run(); });
    }
    ~LiveSource() { stop_ = true; if (thread_.joinable()) thread_.join(); closesocket(socket_); WSACleanup(); }
    std::wstring Url() const { return L"http://127.0.0.1:" + std::to_wstring(port_) + L"/native.opus"; }
};
void LiveTests(const std::filesystem::path& directory) {
    const auto bytes = Bytes(directory / L"ordinary.opus");
    auto live = Reader(bytes, {}, true);
    Check(live->Needed() && live->Info().radio && !live->Info().seekable && live->Info().native_channels == 0 &&
        live->Info().total_frames == -1 && !live->SeekFrame(0) && !live->SetOutputMode(OutputMode::SourceChannels), "live always native, stereo and unseekable");
    Check(Drain(*live).size() == 16013 * 2, "forward-only ordinary decode");
    auto joined = WithoutPages(bytes, 2, 3);
    auto join = Reader(joined, {}, true);
    Check(Drain(*join).size() == (16013 - 2880) * 2, "one narrow Icecast replayed-header sequence join");
    joined = WithoutPages(joined, 5, 1);
    auto broken = Reader(joined, {}, true); std::array<float, 11520> buffer{}; int n = 1, guard = 0;
    while (n > 0 && ++guard < 1000) n = broken->Read(buffer.data(), 5760);
    Check(n < 0, "initial live join never permits a later strict gap");
    Options short_link; short_link.frames48 = 240; Fixture(directory / L"short.opus", short_link);
    const auto short_bytes = Bytes(directory / L"short.opus"); std::vector<unsigned char> many;
    for (int i = 0; i < 270; ++i) many.insert(many.end(), short_bytes.begin(), short_bytes.end());
    auto radio = Reader(many, {}, true);
    Check(Drain(*radio, 5760).size() == 270 * 240 * 2 && radio->Info().links == 270, "live has no finite indexed-link lifetime cap");
    LiveSource server(bytes); Decoder decoder;
    Check(decoder.Open(server.Url().c_str()) && decoder.NativeActive() && decoder.SampleRate() == 48000 && decoder.Info().radio, "HTTP activates native on first connection");
    std::int64_t frames = 0;
    for (;;) { const int count = decoder.ReadFloatFrames(buffer.data(), 5760); Check(count >= 0, "loopback native read"); if (!count) break; frames += count; }
    Check(frames == 16013 && server.requests == 1, "native HTTP preserves a single connection");
}
void FacadePipelineTests(const std::filesystem::path& directory) {
    for (const auto name : {L"hd.opus", L"mixed.opus"}) {
        const auto path = directory / name;
        const std::int64_t total = name == std::wstring(L"mixed.opus") ? 32026 : 16013;
        for (int rate : {0,48000}) {
            PlaybackSettings settings; settings.sample_rate = rate; settings.bits = 24; settings.dither = false;
            Decoder decoder; Check(decoder.Open(path.c_str(), nullptr, settings) && decoder.NativeActive() && decoder.SampleRate() == 96000, "public Decoder QEXT/native chain activation");
            const auto& technical = decoder.Info();
            Check(technical.file_size == static_cast<std::int64_t>(std::filesystem::file_size(path)), "native technical source size");
            Check(technical.format_version == 1, "native technical OpusHead version");
            Check(technical.pre_skip48 >= 0, "native technical OpusHead pre-skip");
            Check(technical.stream_count > 0, "native technical OpusHead stream count");
            Check(technical.total_frames == total, "native technical 48-kHz granule timeline");
            const int first_link_rate = name == std::wstring(L"mixed.opus") ? 48000 : 96000;
            Check(technical.decoded_rate == first_link_rate && technical.output_rate == 96000,
                "native technical actual link rate and normalized output rate");
            Check(decoder.SetOutputMode(OutputMode::Stereo), "pipeline stereo output"); decoder.SetGain(settings, false);
            PcmPipeline pipeline(decoder, settings); Check(pipeline.Ready(), "native-aware PCM pipeline ready");
            std::array<unsigned char, 576 * kMaxChannels * 4> packed{};
            auto drain = [&] {
                std::int64_t frames = 0;
                for (int guard = 0; guard < 100000; ++guard) {
                    const int n = pipeline.Read(packed.data(), 43); Check(n >= 0 && n <= 43, "native-aware pipeline read");
                    if (!n) return frames; frames += n;
                }
                throw std::runtime_error("pipeline guard");
            };
            const int ratio = rate ? 1 : 2;
            Check(drain() == total * ratio && pipeline.PositionFrames() == total, "native0 and explicit48k pipeline exact duration");
            if (name == std::wstring(L"mixed.opus")) {
                Check(decoder.SeekFrame(16013) && decoder.Info().decoded_rate == 96000 &&
                    decoder.Info().output_rate == 96000 && decoder.Info().qext,
                    "native technical metadata follows the QEXT link after crossing a mixed-rate chain");
            }
            Check(decoder.SeekFrame(137), "public arbitrary48k frame seek"); pipeline.ResetAfterSeek();
            Check(pipeline.PositionFrames() == 137 && drain() == (total - 137) * ratio && pipeline.PositionFrames() == total, "pipeline seek preserves48k timeline and exact remaining frame count");
        }
    }
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        Check(argc == 2, "usage: native_codec_tests DATA_DIR");
        const auto directory = std::filesystem::path(argv[1]); std::filesystem::create_directories(directory);
        ParserTests(); SpeakerTests(directory); GainTests(directory); HdAndEnhancementTests(directory);
        AmbisonicAndChainTests(directory); MalformedAndRecoveryTests(directory); RedundancyTests(directory);
        LiveTests(directory); FacadePipelineTests(directory);
        std::cout << "Native Opus: activation, QEXT30kHz, OSCE/BWE, toggles, exact chains/seeks/gain, bounded recovery/DRED/FEC, ambisonics, loopback and PCM pipeline passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Native codec tests: " << error.what() << '\n'; return 1; }
}
