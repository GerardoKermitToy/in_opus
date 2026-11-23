#include "fixtures.hpp"
#include "opus_decoder.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace winamp_opus;
namespace {
std::int64_t DecodeAll(Decoder& decoder, double* rms = nullptr) {
    std::array<std::int16_t, 576 * 2> pcm{};
    std::int64_t total = 0;
    long double energy = 0;
    for (;;) {
        const int frames = decoder.ReadFrames(pcm.data(), 576);
        Check(frames >= 0, "decode error");
        if (frames == 0) break;
        for (int i = 0; i < frames * 2; ++i) energy += static_cast<long double>(pcm[i]) * pcm[i];
        total += frames;
    }
    if (rms) *rms = total ? std::sqrt(static_cast<double>(energy / (total * 2))) : 0;
    return total;
}
void ValidateFile(const wchar_t* path) {
    Decoder decoder;
    int error = 0;
    Check(decoder.Open(path, &error), "cannot open sample");
    const auto info = decoder.Info();
    double rms = 0;
    const auto decoded = DecodeAll(decoder, &rms);
    Check(decoded == info.total_frames, "sample count differs from Ogg granule duration");
    Check(rms > 0, "decoded audio is silent");
    Check(decoder.SeekFrame(info.total_frames / 2), "seek to middle");
    Check(DecodeAll(decoder) == info.total_frames - info.total_frames / 2, "sample seek count");
    std::cout << "frames=" << decoded << " duration_ms=" << info.length_ms
              << " source_channels=" << info.source_channels << " bitrate=" << info.bitrate
              << " output=48000Hz/stereo/s16 rms=" << rms << '\n';
}
void SelfTest(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto mono = directory / L"Кантри рок.opus";
    const auto stereo = directory / L"stereo.opus";
    const auto surround = directory / L"surround.opus";
    const auto chained = directory / L"chained.opus";
    MakeOpus(mono, 1, 19213, 101);
    MakeOpus(stereo, 2, 48117, 102);
    MakeOpus(surround, 6, 12017, 103);
    for (const auto& path : {mono, stereo, surround}) ValidateFile(path.c_str());
    std::filesystem::copy_file(mono, chained, std::filesystem::copy_options::overwrite_existing);
    AppendFile(chained, stereo);
    Decoder decoder;
    Check(decoder.Open(chained.c_str()), "open chained file");
    Check(decoder.Info().links == 2, "link count");
    Check(decoder.Info().source_sample_rate == 44100, "input sample rate marker");
    Check(decoder.Info().title == L"Проверка Opus", "UTF-8 title");
    Check(decoder.Info().artist == L"Тест", "UTF-8 artist");
    Check(decoder.Info().total_frames == 19213 + 48117, "chain duration");
    Check(DecodeAll(decoder) == 19213 + 48117, "chain decode");
    Check(decoder.SeekFrame(19213 + 123), "seek into next link");
    Check(DecodeAll(decoder) == 48117 - 123, "chain seek");
    Check(decoder.SeekMs(-1) && decoder.PositionFrames() == 0, "negative seek clamp");
    Check(decoder.SeekFrame(INT64_MAX), "seek past end");
    std::array<std::int16_t, 1152> pcm{};
    Check(decoder.ReadFrames(pcm.data(), 576) == 0, "EOF after seek");
    Check(decoder.ReadFrames(nullptr, 576) == OP_EINVAL, "null decode buffer");
    const auto invalid = directory / L"invalid.opus";
    { std::ofstream file(invalid, std::ios::binary); file << "not an opus file"; }
    Check(!decoder.Open(invalid.c_str()), "reject invalid input");
    Check(!decoder.Open((directory / L"missing.opus").c_str()), "reject missing file");
    Check(!decoder.Open(nullptr), "reject null filename");
    Check(decoder.Open(mono.c_str()), "reuse after failures");
    std::array<std::int16_t, 1152> mono_pcm{};
    const int frames = decoder.ReadFrames(mono_pcm.data(), 576);
    Check(frames == 576, "read block");
    for (int i = 0; i < frames; ++i) Check(mono_pcm[2*i] == mono_pcm[2*i+1], "mono stereo duplication");
    decoder.Close();
    Check(decoder.ReadFrames(pcm.data(), 576) == OP_EINVAL, "closed decoder");
    std::cout << "Decoder self-test passed: mono/stereo/5.1, Unicode, tags, chaining, seeks, invalid input.\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 3, "usage: decoder_tests --self-test directory | --file audio.opus");
        if (std::wstring(argv[1]) == L"--file") ValidateFile(argv[2]);
        else { Check(std::wstring(argv[1]) == L"--self-test", "invalid mode"); SelfTest(argv[2]); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
