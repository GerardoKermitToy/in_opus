// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace winamp_opus::opus_native {
inline constexpr std::size_t MaxPacketBytes = 1024 * 1024;
struct Header {
    int channels = 0, family = 0, streams = 0, coupled = 0;
    int preskip = 0, gain_q8 = 0;
    std::uint32_t input_rate = 0;
    std::vector<unsigned char> mapping, matrix;
};
// Bounded envelopes only, not entropy data. QEXT is identified in extension
// headers, never by an input-rate hint or byte-searching arbitrary padding.
bool ParseHeader(const unsigned char*, std::size_t, Header&) noexcept;
bool InspectPacket(const unsigned char*, std::size_t, int streams,
    int& duration48, bool& qext) noexcept;
} // namespace winamp_opus::opus_native
