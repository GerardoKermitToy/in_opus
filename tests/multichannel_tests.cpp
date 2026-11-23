#include "fixtures.hpp"
#include "opus_decoder.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>

namespace {
int Read(void* file, unsigned char* data, int bytes) {
    const auto count = std::fread(data, 1, bytes, static_cast<FILE*>(file));
    return count == 0 && std::ferror(static_cast<FILE*>(file)) ? -1 : static_cast<int>(count);
}
int Seek(void* file, opus_int64 offset, int origin) { return _fseeki64(static_cast<FILE*>(file), offset, origin); }
opus_int64 Tell(void* file) { return _ftelli64(static_cast<FILE*>(file)); }
int Close(void* file) { return std::fclose(static_cast<FILE*>(file)); }
using OpusFile = std::unique_ptr<OggOpusFile, decltype(&op_free)>;
OpusFile Open(const wchar_t* path) {
    FILE* raw = nullptr;
    Check(_wfopen_s(&raw, path, L"rb") == 0, "open reference stream");
    std::unique_ptr<FILE, decltype(&std::fclose)> stream(raw, &std::fclose);
    const OpusFileCallbacks callbacks{Read, Seek, Tell, Close};
    int error = 0;
    OpusFile result(op_open_callbacks(stream.get(), &callbacks, nullptr, 0, &error), &op_free);
    Check(result != nullptr, "open reference Opus decoder");
    stream.release();
    return result;
}

// Vorbis/Opus channel order. Explicit coefficients verify the downmix separately
// from the plugin's op_read_stereo call, including centre, surrounds and LFE.
constexpr float kSurround51[6][2]{
    {0.529F,0}, {0.3741F,0.3741F}, {0,0.529F},
    {0.4582F,0.2645F}, {0.2645F,0.4582F}, {0.3741F,0.3741F}};
constexpr float kSurround71[8][2]{
    {0.3886F,0}, {0.2748F,0.2748F}, {0,0.3886F},
    {0.3366F,0.1943F}, {0.1943F,0.3366F},
    {0.3366F,0.1943F}, {0.1943F,0.3366F}, {0.2748F,0.2748F}};

void Validate(const wchar_t* path) {
    auto native = Open(path);
    auto stereo = Open(path);
    const auto* head = op_head(native.get(), 0);
    Check(head && head->mapping_family == 1, "expected standard surround channel mapping");
    const int channels = head->channel_count;
    Check(channels == 6 || channels == 8, "test supports 5.1/7.1 input");
    Check(op_link_count(native.get()) == 1, "this matrix diagnostic expects one Ogg link");
    const auto* matrix = channels == 6 ? kSurround51 : kSurround71;
    winamp_opus::Decoder plugin_decoder;
    Check(plugin_decoder.Open(path), "open production decoder");
    std::array<float, 576 * 8> source{};
    std::array<float, 576 * 2> output{};
    std::array<std::int16_t, 576 * 2> pcm{};
    std::array<long double, 8> channel_power{};
    std::int64_t frames = 0;
    double max_matrix_error = 0, max_pcm_error = 0, peak = 0;
    std::int64_t fullscale_samples = 0;
    for (;;) {
        const int count = op_read_float(native.get(), source.data(), 576 * channels, nullptr);
        const int stereo_count = op_read_float_stereo(stereo.get(), output.data(), static_cast<int>(output.size()));
        Check(count >= 0 && stereo_count == count, "reference decode error/frame mismatch");
        if (count == 0) break;
        Check(plugin_decoder.ReadFrames(pcm.data(), count) == count, "production decoder sample count");
        for (int i = 0; i < count; ++i) {
            double mixed[2]{};
            for (int channel = 0; channel < channels; ++channel) {
                const float sample = source[i * channels + channel];
                channel_power[channel] += static_cast<long double>(sample) * sample;
                for (int side = 0; side < 2; ++side) mixed[side] += matrix[channel][side] * sample;
            }
            for (int side = 0; side < 2; ++side) {
                const auto index = i * 2 + side;
                Check(std::isfinite(output[index]), "non-finite output sample");
                max_matrix_error = std::max(max_matrix_error, std::abs(mixed[side] - output[index]));
                peak = std::max(peak, std::abs(static_cast<double>(output[index])));
                max_pcm_error = std::max(max_pcm_error, std::abs(output[index] * 32768.0 - pcm[index]));
                if (std::abs(static_cast<int>(pcm[index])) >= 32767) ++fullscale_samples;
            }
        }
        frames += count;
    }
    Check(frames == op_pcm_total(native.get(), -1), "lost frames or incorrect final trim");
    Check(plugin_decoder.ReadFrames(pcm.data(), 576) == 0, "production decoder EOF");
    Check(max_matrix_error < 0.000002, "incorrect channel order/downmix matrix");
    // For unclipped material, the S16 decoder must match the float reference to
    // within a few noise-shaped dither LSBs. Loud input may legitimately soft-clip.
    if (peak < 1.0) Check(max_pcm_error < 32, "S16 output differs from the verified downmix");
    std::cout << "channels=" << channels << " mapping_family=" << head->mapping_family
              << " streams=" << head->stream_count << " coupled=" << head->coupled_count
              << " frames=" << frames << " duration_ms=" << frames / 48
              << " matrix_max_error=" << max_matrix_error
              << " pcm_max_error_lsb=" << max_pcm_error << " float_peak=" << peak
              << " fullscale_samples=" << fullscale_samples << '\n';
    std::cout << "source_channel_rms=";
    for (int channel = 0; channel < channels; ++channel)
        std::cout << (channel ? "," : "") << std::sqrt(static_cast<double>(channel_power[channel] / frames));
    std::cout << "\nMultichannel downmix passed (including centre, surrounds and LFE).\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 3, "usage: multichannel_tests --self-test directory | --file audio.opus");
        if (std::wstring(argv[1]) == L"--file") Validate(argv[2]);
        else {
            Check(std::wstring(argv[1]) == L"--self-test", "unknown mode");
            const std::filesystem::path directory(argv[2]);
            std::filesystem::create_directories(directory);
            for (const int channels : {6, 8}) {
                const auto path = directory / (std::to_wstring(channels) + L"-channels.opus");
                MakeOpus(path, channels, 12017, 400 + channels);
                Validate(path.c_str());
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
