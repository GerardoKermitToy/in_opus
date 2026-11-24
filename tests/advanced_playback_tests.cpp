#include <winsock2.h>
#include <ws2tcpip.h>
#include "fixtures.hpp"
#include "pcm_pipeline.hpp"
#include "metadata.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

using namespace winamp_opus;
namespace {
std::vector<char> Bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
class Server final {
public:
    Server(std::vector<char> opus, std::vector<char> vorbis) : opus_(std::move(opus)), vorbis_(std::move(vorbis)) {
        WSADATA data{}; Check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup");
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Check(socket_ != INVALID_SOCKET, "HTTP fixture socket");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind loopback HTTP fixture");
        int size = sizeof(address);
        Check(getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) == 0 && listen(socket_, 5) == 0, "listen HTTP fixture");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Run(); });
    }
    ~Server() {
        stop_ = true; closesocket(socket_);
        if (thread_.joinable()) thread_.join();
        WSACleanup();
    }
    std::wstring Url(const wchar_t* path) const { return L"http://127.0.0.1:" + std::to_wstring(port_) + path; }
private:
    static bool Send(SOCKET socket, const char* data, int size) {
        while (size) { const int n = send(socket, data, size, 0); if (n <= 0) return false; data += n; size -= n; }
        return true;
    }
    void Run() noexcept {
        while (!stop_) {
            fd_set ready; FD_ZERO(&ready); FD_SET(socket_, &ready); timeval timeout{0, 100000};
            if (select(0, &ready, nullptr, nullptr, &timeout) <= 0) continue;
            const auto client = accept(socket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            DWORD ms = 1000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
            std::string request;
            char buffer[1024];
            while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos) {
                const int n = recv(client, buffer, sizeof(buffer), 0); if (n <= 0) break; request.append(buffer, n);
            }
            if (request.find("GET /redirect ") == 0) {
                constexpr char redirect[] = "HTTP/1.1 302 Found\r\nLocation: /stream\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                Send(client, redirect, sizeof(redirect) - 1);
            } else {
                constexpr char headers[] = "HTTP/1.1 200 OK\r\nContent-Type: audio/ogg\r\nicy-name: Loopback Opus\r\nicy-br: 128\r\nConnection: close\r\n\r\n";
                Send(client, headers, sizeof(headers) - 1);
                if (request.find("GET /slow ") == 0) {
                    while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                } else {
                    const auto& body = request.find("GET /vorbis ") == 0 ? vorbis_ : opus_;
                    Send(client, body.data(), static_cast<int>(body.size()));
                }
            }
            closesocket(client);
        }
    }
    SOCKET socket_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::vector<char> opus_, vorbis_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
std::int64_t Drain(PcmPipeline& pipeline, int channels, int bits) {
    std::array<std::uint32_t, 576 * 8 + 2> storage{};
    auto* pcm = reinterpret_cast<unsigned char*>(storage.data() + 1);
    const int bytes = 576 * channels * bits / 8;
    std::int64_t frames = 0;
    for (;;) {
        storage[0] = 0x12345678;
        std::memset(pcm + bytes, 0x5a, 4);
        const int n = pipeline.Read(pcm, 576);
        Check(n >= 0 && n <= 576, "converted PCM read");
        Check(storage[0] == 0x12345678, "PCM lower guard");
        for (int i = 0; i < 4; ++i) Check(pcm[bytes + i] == 0x5a, "PCM upper guard");
        if (!n) return frames;
        frames += n;
    }
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--radio") {
            Decoder radio; int error = 0;
            if (!radio.Open(argv[2], &error)) {
                std::cerr << "Radio open failed: " << error << '\n'; return 1;
            }
            Check(radio.Info().radio && !radio.Info().seekable, "live HTTP decoder");
            std::array<float, 576 * 2> samples{};
            int frames = 0;
            for (int i = 0; i < 20; ++i) {
                const int n = radio.ReadFloatFrames(samples.data(), 576);
                Check(n > 0, "live Opus PCM decode"); frames += n;
            }
            std::cout << "Live Opus radio decoded: " << frames << " frames at 48000 Hz, "
                << radio.Info().source_channels << " source channels; forward-only stereo output.\n";
            return 0;
        }
        Check(argc == 2, "usage: advanced_playback_tests DATA_DIR | --radio URL");
        const std::filesystem::path directory(argv[1]); std::filesystem::create_directories(directory);
        const auto path = directory / L"advanced.opus";
        const auto vorbis = directory / L"foreign.ogg";
        constexpr int source_frames = 5761;
        MakeOpus(path, 6, source_frames, 7001);
        MakeVorbisHeaders(vorbis, 7002);
        for (int channels : {2, 6}) for (int rate : {8000, 44100, 48000, 96000, 192000}) for (int bits : {8, 16, 24, 32}) {
            Decoder decoder; Check(decoder.Open(path.c_str()), "open format fixture");
            Check(decoder.SetOutputMode(channels == 6 ? OutputMode::SourceChannels : OutputMode::Stereo), "select output layout");
            PlaybackSettings s; s.sample_rate = rate; s.bits = bits;
            PcmPipeline pipeline(decoder, s); Check(pipeline.Ready(), "resampler initialization");
            const auto expected = static_cast<std::int64_t>(source_frames) * rate / 48000;
            Check(Drain(pipeline, channels, bits) == expected, "resampled length/tail");
            Check(decoder.SeekFrame(1234), "resampler seek"); pipeline.ResetAfterSeek();
            Check(Drain(pipeline, channels, bits) == static_cast<std::int64_t>(source_frames - 1234) * rate / 48000,
                "resampled seek length/tail");
        }
        std::cout << "PCM: stereo/5.1, 8/16/24/32 bits, 8/44.1/48/96/192 kHz, seek, tails, guards passed.\n";
        // Verify pre-gain on the original float decoder path, before quantization.
        Decoder plain, gain; Check(plain.Open(path.c_str()) && gain.Open(path.c_str()), "open gain fixtures");
        PlaybackSettings quieter; quieter.pre_gain_q8 = -6 * 256; gain.SetGain(quieter, false);
        std::array<float, 576 * 2> a{}, b{};
        Check(plain.ReadFloatFrames(a.data(), 576) == 576 && gain.ReadFloatFrames(b.data(), 576) == 576, "read gain fixtures");
        const double ratio = std::pow(10.0, -6.0 / 20);
        for (std::size_t i = 0; i < a.size(); ++i) Check(std::abs(b[i] - a[i] * ratio) < 0.00001, "pre-gain before PCM conversion");
        const auto gain_path = directory / L"r128.opus";
        MakeOpus(gain_path, 2, source_frames, 7003, {"R128_TRACK_GAIN=-512", "R128_ALBUM_GAIN=-1024", "GENRE=Test genre", "DATE=2026"}, 256);
        for (const auto mode : {GainMode::Track, GainMode::Album, GainMode::Auto}) {
            Decoder reference, normalized;
            Check(reference.Open(gain_path.c_str()) && normalized.Open(gain_path.c_str()), "open R128 fixture");
            PlaybackSettings s; s.gain_mode = mode; s.target_lufs = -18;
            normalized.SetGain(s, true);
            Check(reference.ReadFloatFrames(a.data(), 576) == 576 && normalized.ReadFloatFrames(b.data(), 576) == 576, "R128 decode");
            const double offset_db = mode == GainMode::Album ? 1.0 : 3.0;
            const double multiplier = std::pow(10.0, offset_db / 20);
            for (std::size_t i = 0; i < a.size(); ++i) Check(std::abs(b[i] - a[i] * multiplier) < 0.00001, "R128 track/album/auto target LUFS");
        }
        Decoder tagged; Check(tagged.Open(gain_path.c_str()), "open metadata fixture");
        TrackInfo info = tagged.Info();
        Check(info.header_gain_q8 == 256 && info.has_track_gain && info.has_album_gain, "parsed header and R128 gains");
        std::wstring value;
        Check(MetadataValue(info, "year", value) && value == L"2026", "extended date alias");
        Check(MetadataValue(info, "genre", value) && value == L"Test genre", "extended arbitrary text tag");
        Check(MetadataValue(info, "replaygain_track_gain", value) && value == L"+4.00", "R128 to standard ReplayGain reference/format");
        Check(MetadataValue(info, "formatinformation", value) && value.find(L"\r\n") != std::wstring::npos, "native Format Info uses Windows line endings");
        Server server(Bytes(path), Bytes(vorbis));
        Check(HasOpusContent(server.Url(L"/stream").c_str()), "HTTP Opus content probe");
        Check(!HasOpusContent(server.Url(L"/vorbis").c_str()), "HTTP probe must not claim Vorbis");
        Decoder radio; Check(radio.Open(server.Url(L"/redirect").c_str()), "HTTP redirect/open");
        Check(radio.Info().radio && !radio.Info().seekable && radio.Info().length_ms == -1, "forward-only live stream metadata");
        Check(radio.Info().station.name == L"Loopback Opus", "station headers");
        Check(!radio.SetOutputMode(OutputMode::SourceChannels) && !radio.SeekMs(0), "radio layout/seek protection");
        PcmPipeline live(radio, {});
        Check(Drain(live, 2, 16) == source_frames, "HTTP stream decode/tail");
        HttpStream slow; Check(slow.Open(server.Url(L"/slow").c_str()), "open stalled stream");
        auto pending = std::async(std::launch::async, [&slow] { unsigned char data[512]; return slow.Read(data, sizeof(data)); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        slow.Cancel();
        Check(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready && pending.get() < 0, "cancel blocked HTTP read");
        std::cout << "HTTP: content detection, Vorbis rejection, redirect, metadata, decode and cancellation passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
