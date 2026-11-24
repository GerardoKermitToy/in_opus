// SPDX-License-Identifier: GPL-3.0-only
#include "opus_packet.hpp"
#include "ambisonics.hpp"
#include "channel_layout.hpp"
#include <opus.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace winamp_opus {
namespace {
using opus_native::MaxPacketBytes;
std::uint32_t Le32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
int Le16(const unsigned char* p) noexcept { return p[0] | (p[1] << 8); }

// Parse only the packet envelope, not CELT/SILK entropy data. All pointer
// movement is checked against the enclosing Ogg packet. Self-delimited sizes
// are necessary for every elementary stream except the last in multistream.
bool FrameSize(const unsigned char*& p, const unsigned char* end, int& size) noexcept {
    if (p == end) return false;
    const int first = *p++;
    if (first < 252) { size = first; return true; }
    if (p == end) return false;
    size = first + 4 * *p++;
    return true;
}
bool ExtensionPayload(const unsigned char*& p, const unsigned char* end,
                      unsigned byte, std::size_t trailing, std::size_t& payload) noexcept {
    const unsigned id = byte >> 1, l = byte & 1;
    payload = 0;
    if ((id == 0 && l) || id == 2) return true;
    if (id > 0 && id < 32) payload = l;
    else if (!l) {
        if (trailing > static_cast<std::size_t>(end - p)) return false;
        payload = static_cast<std::size_t>(end - p) - trailing;
    } else {
        unsigned n = 0;
        do {
            if (p == end) return false;
            n = *p++;
            payload += n;
            if (payload > static_cast<std::size_t>(end - p)) return false;
        } while (n == 255);
    }
    if (payload > static_cast<std::size_t>(end - p)) return false;
    p += payload;
    return true;
}
bool QextPadding(const unsigned char* p, std::size_t bytes, int frames, bool& found) noexcept {
    const unsigned char* end = p + bytes;
    // There can be at most bytes extension headers. Store only the current
    // repetition group as offsets; no allocation in this untrusted parser.
    const unsigned char* group = p;
    const unsigned char* last_long_end = nullptr;
    std::size_t trailing_short = 0;
    int frame = 0;
    while (p < end) {
        const unsigned char* header = p;
        const unsigned byte = *p++, id = byte >> 1, l = byte & 1;
        std::size_t payload = 0;
        if (!ExtensionPayload(p, end, byte, 0, payload)) return false;
        if (id == 1) {
            const int increment = l ? header[1] : 1;
            if (!increment) continue;
            frame += increment;
            if (frame >= frames) return false;
            group = p; last_long_end = nullptr; trailing_short = 0;
        } else if (id == 2) {
            // Repeated long extensions may have different payload lengths.
            // Skip their payloads with the original headers, not by searching
            // arbitrary padding bytes for 0xf8 (which would produce false HD).
            for (int f = frame + 1; f < frames; ++f) {
                const unsigned char* s = group;
                while (s < header) {
                    unsigned repeated = *s++;
                    std::size_t ignored = 0;
                    if (!ExtensionPayload(s, header, repeated, 0, ignored)) return false;
                    if (repeated <= 3) continue;
                    if (!l && f + 1 == frames && s == last_long_end) repeated &= ~1U;
                    if (!ExtensionPayload(p, end, repeated, trailing_short, ignored)) return false;
                }
            }
            group = p; last_long_end = nullptr; trailing_short = 0;
            if (!l && ++frame >= frames) return true;
        } else if (id > 2) {
            if (id >= 32) { last_long_end = p; trailing_short = 0; }
            else trailing_short += l;
            if (id == 124 && payload) found = true; // Pinned libopus 1.6.1 QEXT ID.
        }
    }
    return true;
}
bool ElementaryPacket(const unsigned char* data, std::size_t bytes, bool self_delimited,
                      std::size_t& consumed, int& duration, bool& qext) noexcept {
    if (!bytes || bytes > MaxPacketBytes) return false;
    const unsigned char* p = data;
    const unsigned char* physical_end = data + bytes;
    const unsigned toc = *p++;
    int count = 1;
    bool cbr = false;
    std::size_t pad = 0;
    std::array<int, 48> sizes{};
    if ((toc & 3) == 1) { count = 2; cbr = true; }
    else if ((toc & 3) == 2) {
        count = 2;
        if (!FrameSize(p, physical_end, sizes[0])) return false;
    } else if ((toc & 3) == 3) {
        if (p == physical_end) return false;
        const unsigned flags = *p++;
        count = flags & 63;
        if (count < 1 || count > 48) return false;
        if (flags & 64) {
            unsigned n = 0;
            do {
                if (p == physical_end) return false;
                n = *p++;
                pad += n == 255 ? 254 : n;
                if (pad > static_cast<std::size_t>(physical_end - p)) return false;
            } while (n == 255);
        }
        cbr = !(flags & 128);
        if (!cbr) {
            const unsigned char* header_end = physical_end - pad;
            for (int i = 0; i < count - 1; ++i)
                if (!FrameSize(p, header_end, sizes[i])) return false;
        }
    }
    if (pad > static_cast<std::size_t>(physical_end - p)) return false;
    const unsigned char* end = physical_end - pad;
    if (self_delimited) {
        if (!FrameSize(p, end, sizes[count - 1])) return false;
        if (cbr) std::fill_n(sizes.data(), count, sizes[count - 1]);
    } else {
        const std::size_t available = static_cast<std::size_t>(end - p);
        if (cbr) {
            if (available % count || available / count > 1275) return false;
            std::fill_n(sizes.data(), count, static_cast<int>(available / count));
        } else {
            std::size_t prior = 0;
            for (int i = 0; i < count - 1; ++i) prior += sizes[i];
            if (prior > available || available - prior > 1275) return false;
            sizes[count - 1] = static_cast<int>(available - prior);
        }
    }
    std::size_t payload = 0;
    for (int i = 0; i < count; ++i) {
        if (sizes[i] < 0 || sizes[i] > 1275) return false;
        payload += sizes[i];
    }
    if (payload > static_cast<std::size_t>(end - p)) return false;
    const unsigned char* padding = p + payload;
    consumed = static_cast<std::size_t>(padding - data) + pad;
    if (consumed > bytes || (!self_delimited && consumed != bytes)) return false;
    duration = opus_packet_get_samples_per_frame(data, 48000) * count;
    if (duration < 120 || duration > 5760) return false;
    bool extension = false;
    if (pad && !QextPadding(padding, pad, count, extension)) {
        // Invalid/unknown extension syntax is ignored by ordinary Opus
        // decoders as padding. It must never be mistaken for positive HD.
        extension = false;
    }
    qext = qext || ((toc & 128) && extension);
    return true;
}
}

namespace opus_native {
bool ParseHeader(const unsigned char* data, std::size_t size, Header& result) noexcept {
    try {
        Header h;
        if (!data || size < 19 || size > 65025 || std::memcmp(data, "OpusHead", 8) ||
            data[8] < 1 || data[8] > 15 || !data[9]) return false;
        h.version = data[8];
        h.channels = data[9]; h.preskip = Le16(data + 10); h.input_rate = Le32(data + 12);
        const int gain = Le16(data + 16);
        h.gain_q8 = gain >= 32768 ? gain - 65536 : gain;
        h.family = data[18];
        if (h.family == 0) {
            if (h.channels > 2 || size != 19) return false;
            h.streams = 1; h.coupled = h.channels - 1;
            h.mapping = h.channels == 2 ? std::vector<unsigned char>{0, 1} : std::vector<unsigned char>{0};
        } else {
            if (size < 21) return false;
            h.streams = data[19]; h.coupled = data[20];
            if (!h.streams || h.coupled > h.streams || h.streams + h.coupled > 255) return false;
            if (h.family == 1) {
                if (h.channels > kMaxChannels) return false;
            } else if (h.family == 2 || h.family == 3) {
                int order = 0, harmonics = 0;
                if (!AmbisonicShape(h.channels, order, harmonics)) return false;
            } else return false;
            if (h.family == 3) {
                const std::size_t matrix_size = static_cast<std::size_t>(h.channels) * (h.streams + h.coupled) * 2;
                if (size != 21 + matrix_size) return false;
                h.matrix.assign(data + 21, data + size);
            } else {
                if (size != 21 + static_cast<std::size_t>(h.channels)) return false;
                h.mapping.assign(data + 21, data + size);
                for (unsigned channel : h.mapping)
                    if (channel != 255 && channel >= static_cast<unsigned>(h.streams + h.coupled)) return false;
            }
        }
        result = std::move(h);
        return true;
    } catch (...) { return false; }
}

bool InspectPacket(const unsigned char* data, std::size_t size, int streams,
                   int& duration_48k, bool& qext) noexcept {
    duration_48k = 0; qext = false;
    if (!data || !size || size > MaxPacketBytes || streams < 1 || streams > 255) return false;
    std::size_t at = 0;
    for (int stream = 0; stream < streams; ++stream) {
        std::size_t used = 0; int duration = 0;
        if (!ElementaryPacket(data + at, size - at, stream + 1 < streams, used, duration, qext) || !used) return false;
        if (stream && duration != duration_48k) return false;
        duration_48k = duration;
        at += used;
        if (at > size) return false;
    }
    return at == size;
}
}

} // namespace winamp_opus
