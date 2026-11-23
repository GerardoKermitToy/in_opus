// SPDX-License-Identifier: GPL-3.0-only
#include "fixtures.hpp"
#include "registry_sandbox.hpp"
#include "winamp_input.hpp"
#include <opusfile.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {
// These are the actual Winamp extended-read ABI signatures, resolved only from
// the built DLL. The test never compiles extended_read.cpp into its host.
using OpenW = std::intptr_t (__cdecl*)(const wchar_t*, int*, int*, int*, int*);
using OpenA = std::intptr_t (__cdecl*)(const char*, int*, int*, int*, int*);
using GetData = std::size_t (__cdecl*)(std::intptr_t, char*, std::size_t, int*);
using SetTime = int (__cdecl*)(std::intptr_t, int);
using Close = void (__cdecl*)(std::intptr_t);
struct Api { OpenW open, floating; OpenA ansi; GetData data; SetTime seek; Close close; };
struct Reader {
    const Api& api;
    std::intptr_t handle = 0;
    int size = 0, bits = 0, channels = 0, rate = 0;
    Reader(const Api& a, const std::filesystem::path& path, bool floating, int b, int c, int r)
        : api(a), bits(b), channels(c), rate(r) {
        handle = (floating ? api.floating : api.open)(path.c_str(), &size, &bits, &channels, &rate);
        Check(handle != 0, "open independent extended-read session");
    }
    ~Reader() { api.close(handle); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
};
std::vector<unsigned char> Bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(input.good(), "read encoded fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::vector<unsigned char> Drain(Reader& reader, int chunk = 577) {
    const int frame = reader.channels * (reader.bits / 8);
    // Deliberately unaligned destination and a non-frame-multiple capacity.
    std::vector<char> block(static_cast<std::size_t>(chunk) * frame + 5, char(0x6a));
    std::vector<unsigned char> result;
    int kill = 0;
    for (int calls = 0; calls < 100000; ++calls) {
        const auto capacity = block.size() - 2;
        const auto count = reader.api.data(reader.handle, block.data() + 1, capacity, &kill);
        Check(count <= capacity && count % frame == 0, "extended read returns complete bounded frames");
        Check(block.front() == char(0x6a) && block.back() == char(0x6a), "unaligned output guard bytes");
        if (!count) return result;
        result.insert(result.end(), reinterpret_cast<unsigned char*>(block.data() + 1),
            reinterpret_cast<unsigned char*>(block.data() + 1) + count);
    }
    Check(false, "extended read eventually reaches EOF");
    return {};
}
std::vector<float> Reference(const std::filesystem::path& path) {
    const auto bytes = Bytes(path);
    int error = 0;
    std::unique_ptr<OggOpusFile, decltype(&op_free)> file(op_open_memory(bytes.data(), bytes.size(), &error), &op_free);
    Check(file != nullptr, "libopusfile float reference");
    std::array<float, 576 * 2> block{};
    std::vector<float> pcm;
    for (;;) {
        const int count = op_read_float_stereo(file.get(), block.data(), static_cast<int>(block.size()));
        Check(count >= 0, "float reference decode");
        if (!count) break;
        pcm.insert(pcm.end(), block.begin(), block.begin() + count * 2);
    }
    return pcm;
}
void IntegerRequests(const Api& api, const std::filesystem::path& path, int frames) {
    for (int bits : {8, 16, 24, 32}) for (int channels : {1, 2, 4}) {
        Reader reader(api, path, false, bits, channels, 24000);
        const int actualChannels = channels == 4 ? 2 : channels; // Quad does not upmix a stereo source.
        Check(reader.bits == bits && reader.channels == actualChannels && reader.rate == 24000,
            "honor requested integer resolution rate and negotiated channel layout");
        const auto pcm = Drain(reader, 17);
        const auto expected = static_cast<std::size_t>(frames / 2) * actualChannels * (bits / 8);
        Check(pcm.size() == expected && reader.size == static_cast<int>(expected), "exact resampled size in output bytes");
        Check(std::any_of(pcm.begin(), pcm.end(), [bits](unsigned char c) { return c != (bits == 8 ? 128 : 0); }),
            "integer output contains audio");
    }
    Reader defaults(api, path, false, 7, 0, -1);
    Check(defaults.bits == 16 && defaults.channels == 2 && defaults.rate == 48000, "unknown requests use independent defaults");
    const auto all = Drain(defaults, 576);
    Check(all.size() == static_cast<std::size_t>(frames) * 4 && defaults.size == static_cast<int>(all.size()),
        "native size uses exact frames not millisecond truncation");
}
void FloatRequests(const Api& api, const std::filesystem::path& path, int frames) {
    Reader reader(api, path, true, 16, 2, 48000);
    Check(reader.bits == 32 && reader.channels == 2 && reader.rate == 48000, "float export negotiates float32");
    const auto pcm = Drain(reader, 17);
    const auto reference = Reference(path);
    Check(pcm.size() == static_cast<std::size_t>(frames) * 8 && pcm.size() == reference.size() * sizeof(float), "float duration");
    Check(std::memcmp(pcm.data(), reference.data(), pcm.size()) == 0, "float path is sample-identical unquantized libopusfile");
    Check(std::any_of(reference.begin(), reference.end(), [](float f) { return std::abs(f) > 1; }), "float preserves above-full-scale data");
    Check(std::any_of(reference.begin(), reference.end(), [](float f) {
        return std::abs(static_cast<double>(f) * 32768 - std::round(static_cast<double>(f) * 32768)) > .01;
    }), "float is not a widened PCM16 signal");
    Check(api.seek(reader.handle, 0) == 1, "float seek to start");
    const auto repeated = Drain(reader, 576);
    Check(repeated == pcm, "seek resets per-session float/pipeline state");
}
void Independent(const Api& api, const std::filesystem::path& path, int frames) {
    Reader first(api, path, false, 24, 1, 32000), second(api, path, false, 16, 4, 48000);
    std::array<char, 2000> data{};
    int kill = 1;
    Check(api.data(first.handle, data.data(), data.size(), &kill) == 0, "kill flag prevents decoding");
    kill = 0;
    const auto count = api.data(first.handle, data.data(), data.size(), &kill);
    Check(count > 0 && count % 3 == 0, "resumable kill flag");
    Check(api.seek(first.handle, 0) == 1, "seek one session");
    std::vector<unsigned char> a, b;
    std::exception_ptr errorA, errorB;
    std::thread one([&] { try { a = Drain(first, 1); } catch (...) { errorA = std::current_exception(); } });
    std::thread two([&] { try { b = Drain(second, 576); } catch (...) { errorB = std::current_exception(); } });
    one.join(); two.join();
    if (errorA) std::rethrow_exception(errorA);
    if (errorB) std::rethrow_exception(errorB);
    Check(a.size() == static_cast<std::size_t>(frames) * 32000 / 48000 * 3 &&
        b.size() == static_cast<std::size_t>(frames) * 4, "concurrent sessions retain independent negotiated formats");
    Check(!api.seek(first.handle, -1), "negative seek is rejected");
    Check(api.seek(first.handle, 20), "bounded millisecond seek");
    const auto tail = Drain(first);
    Check(tail.size() == static_cast<std::size_t>(frames - 960) * 32000 / 48000 * 3, "seek resets resampler tail and frame accounting");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    HMODULE library = nullptr;
    try {
        Check(argc == 3, "usage: extended_read_tests DLL directory");
        RegistrySandbox registry;
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        const auto stereo = directory / L"Распаковка Opus.ogg";
        constexpr int frames = 10017;
        MakeOpus(stereo, 2, frames, 8101, {}, 18 * 256);
        library = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(library != nullptr, "load real DLL");
        Api api{reinterpret_cast<OpenW>(GetProcAddress(library, "winampGetExtendedRead_openW")),
            reinterpret_cast<OpenW>(GetProcAddress(library, "winampGetExtendedRead_openW_float")),
            reinterpret_cast<OpenA>(GetProcAddress(library, "winampGetExtendedRead_open")),
            reinterpret_cast<GetData>(GetProcAddress(library, "winampGetExtendedRead_getData")),
            reinterpret_cast<SetTime>(GetProcAddress(library, "winampGetExtendedRead_setTime")),
            reinterpret_cast<Close>(GetProcAddress(library, "winampGetExtendedRead_close"))};
        Check(api.open && api.floating && api.ansi && api.data && api.seek && api.close, "all undecorated extended-read exports");
        IntegerRequests(api, stereo, frames);
        FloatRequests(api, stereo, frames);
        Independent(api, stereo, frames);
        const auto six = directory / L"six.opus";
        MakeOpus(six, 6, 2417, 8102);
        { Reader reader(api, six, false, 16, 0, 0); Check(reader.channels == 6, "unknown channels preserve stable source speakers");
          Check(Drain(reader).size() == 2417u * 12, "source-channel extended decode"); }
        { Reader reader(api, six, false, 24, 4, 24000); Check(reader.channels == 4, "quad downmix negotiates four channels from surround");
          Check(Drain(reader).size() == 1208u * 12, "quad output frame accounting"); }
        const auto mono = directory / L"mono.opus";
        MakeOpus(mono, 1, 1017, 8103);
        const auto mixed = directory / L"mixed.opus";
        std::filesystem::copy_file(stereo, mixed, std::filesystem::copy_options::overwrite_existing);
        AppendFile(mixed, mono);
        { Reader reader(api, mixed, false, 16, 4, 0); Check(reader.channels == 2, "mixed-link fallback negotiates stable stereo without upmixing");
          Check(Drain(reader).size() == static_cast<std::size_t>(frames + 1017) * 4, "mixed source layouts use stable decode fallback"); }
        const auto ansi = directory / L"ansi.opus";
        std::filesystem::copy_file(stereo, ansi, std::filesystem::copy_options::overwrite_existing);
        int size = 0, bits = 16, channels = 2, rate = 48000;
        const auto ansiPath = ansi.string();
        auto handle = api.ansi(ansiPath.c_str(), &size, &bits, &channels, &rate);
        Check(handle != 0, "ANSI open export");
        api.close(handle); api.close(handle);
        std::array<char, 16> small{};
        Check(!api.data(handle, small.data(), small.size(), nullptr) && !api.seek(handle, 0), "closed and duplicate handles are safe");
        Check(!api.data(0, small.data(), small.size(), nullptr) && !api.data(1, nullptr, small.size(), nullptr), "null and invalid handles");
        Check(!api.open(stereo.c_str(), nullptr, &bits, &channels, &rate), "null negotiation pointer");
        Check(!api.floating(stereo.c_str(), &size, nullptr, &channels, &rate), "null float negotiation pointer");
        const auto vorbis = directory / L"not-opus.ogg";
        MakeVorbisHeaders(vorbis, 8104);
        const auto fake = directory / L"not-opus.opus";
        { std::ofstream output(fake, std::ios::binary); output << "OggS OpusHead is not validated framing"; }
        const auto matroska = directory / L"not-opus.mka";
        { std::ofstream output(matroska, std::ios::binary); output << "\x1a\x45\xdf\xa3 OpusHead"; }
        for (const auto& path : {vorbis, fake, matroska, directory / L"missing.opus"})
            Check(!api.open(path.c_str(), &size, &bits, &channels, &rate), "reject non-Opus invalid or missing content");
        Check(!api.open(L"https://example.invalid/live.opus", &size, &bits, &channels, &rate), "no network decoding in extended-read exports");
        FreeLibrary(library); library = nullptr;
        std::cout << "Extended-read DLL ABI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (library) FreeLibrary(library);
        std::cerr << error.what() << '\n'; return 1;
    }
}
