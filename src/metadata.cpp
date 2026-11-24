// SPDX-License-Identifier: GPL-3.0-only
#include "metadata.hpp"
#include "language.hpp"
#include <algorithm>
#include <cctype>
#include <cwchar>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

namespace winamp_opus {
namespace {
std::wstring Db(int q8, bool unit = true) {
    // Keep Winamp's decimal-point ReplayGain representation locale-independent.
    const auto signed_value = static_cast<std::int64_t>(q8);
    const auto magnitude = signed_value < 0 ? -signed_value : signed_value;
    const auto hundredths = (magnitude * 100 + 128) / 256;
    auto text = std::wstring(q8 < 0 ? L"-" : L"+") + std::to_wstring(hundredths / 100) + L".";
    if (hundredths % 100 < 10) text += L"0";
    text += std::to_wstring(hundredths % 100);
    if (unit) text += L" dB";
    return text;
}
std::wstring Tag(const TrackInfo& info, const char* key) {
    const auto it = info.tags.find(key);
    return it == info.tags.end() ? L"" : it->second;
}
bool SupportedTextField(const std::string& key) {
    for (const char* field : {"title", "artist", "album", "genre", "comment", "description",
        "year", "date", "track", "tracknumber", "disc", "disk", "discnumber", "bpm",
        "albumartist", "album artist", "album_artist", "ensemble", "composer", "publisher", "organization",
        "conductor", "tool", "encoded-by", "encoder", "remixing", "subtitle", "version", "isrc",
        "category", "contentgroup", "grouping", "rating", "producer", "url", "purl",
        "tracktotal", "totaltracks", "disctotal", "totaldiscs", "gracenotefileid", "gracenoteextdata",
        "replaygain_track_gain", "replaygain_track_peak", "replaygain_album_gain", "replaygain_album_peak"})
        if (key == field) return true;
    return false;
}
} // namespace

std::wstring FormatInformation(const TrackInfo& info) {
    std::wostringstream out;
    out.imbue(std::locale::classic());
    // Historical File Info block first. Opus granules always use 48 kHz, even
    // when QEXT is decoded at 96 kHz; an input-rate hint is never the clock.
    if (info.length_ms >= 0) {
        const int seconds = info.length_ms / 1000;
        out << Localized(L"Length: ");
        if (seconds >= 3600) out << seconds / 3600 << L':' << std::setw(2) << std::setfill(L'0') << seconds / 60 % 60 << L':';
        else if (seconds >= 60) out << seconds / 60 << L':';
        if (seconds >= 60) out << std::setw(2) << std::setfill(L'0');
        out << seconds % 60 << L"\r\n";
    }
    const long double seconds = info.total_frames > 0 ? info.total_frames / 48000.L : info.length_ms / 1000.L;
    if (info.file_size > 0 && seconds > 0)
        out << Localized(L"Average bitrate: ") << std::fixed << std::setprecision(0)
            << std::floor(info.file_size * 8.L / (seconds * 1000)) << L' ' << Localized(L"kbps") << L"\r\n" << std::defaultfloat;
    if (info.file_size >= 0) out << Localized(L"File size: ") << info.file_size << L' ' << Localized(L"bytes") << L"\r\n";
    if (info.source_channels > 0) out << Localized(L"Channels: ") << info.source_channels << L"\r\n";
    if (info.source_channels > 0 && info.decoded_rate > 0)
        out << Localized(L"Sampling rate: ") << info.decoded_rate << L' ' << Localized(L"Hz") << L"\r\n";
    if (info.source_channels > 0) out << Localized(L"Serial number: ") << info.serial << L"\r\n";
    if (info.format_version >= 0) out << Localized(L"Version: ") << info.format_version << L"\r\n";
    if (!info.vendor.empty()) out << Localized(L"Vendor:") << L"\r\n" << info.vendor << L"\r\n";

    out << L"\r\n" << Localized(L"Codec: ") << Localized(L"Ogg Opus");
    if (info.length_ms >= 0) out << L"\r\n" << Localized(L"Duration: ") << info.length_ms << L" ms";
    if (info.total_frames >= 0) out << L"\r\n" << Localized(L"Samples per channel (48 kHz): ") << info.total_frames;
    if (info.links > 1) out << L"\r\n" << Localized(L"Logical stream: ") << info.current_link + 1 << L" / " << info.links;
    out << L"\r\n" << Localized(L"Seekable: ") << Localized(info.seekable ? L"Yes" : L"No");
    if (info.source_channels > 0) out << L"\r\n" << Localized(L"Mapping family: ") << info.mapping_family;
    if (info.stream_count > 0)
        out << L"\r\n" << Localized(L"Opus streams: ") << info.stream_count
            << L"\r\n" << Localized(L"Coupled streams: ") << info.coupled_count;
    if (info.pre_skip48 >= 0) out << L"\r\n" << Localized(L"Pre-skip (48 kHz): ") << info.pre_skip48;
    if (info.source_sample_rate > 0)
        out << L"\r\n" << Localized(L"Input sample-rate hint: ") << info.source_sample_rate << L' ' << Localized(L"Hz");
    if (info.source_channels > 0 && info.output_rate != info.decoded_rate)
        out << L"\r\n" << Localized(L"Normalized decoder output: ") << info.output_rate << L' ' << Localized(L"Hz");
    if (info.qext) out << L"\r\n" << Localized(L"QEXT: ") << Localized(L"Yes");
    if (info.source_channels > 0) out << L"\r\n" << Localized(L"Header gain: ") << Db(info.header_gain_q8);
    if (info.has_track_gain) out << L"\r\n" << Localized(L"R128 track gain: ") << Db(info.track_gain_q8);
    if (info.has_album_gain) out << L"\r\n" << Localized(L"R128 album gain: ") << Db(info.album_gain_q8);
    if (!info.decoder_note.empty()) out << L"\r\n" << info.decoder_note;
    if (info.bandwidth_hz > 0) out << L"\r\n" << Localized(L"Last packet bandwidth: ") << info.bandwidth_hz << L' ' << Localized(L"Hz");
    if (info.packet_duration_us > 0) {
        out << L"\r\n" << Localized(L"Last packet duration: ") << info.packet_duration_us << L" us";
        out << L"\r\n" << Localized(L"Entropy final range (diagnostic): ") << info.final_range;
    }
    if (info.pitch48 > 0) out << L"\r\n" << Localized(L"Last voiced pitch period (48 kHz): ") << info.pitch48;
    if (info.concealed_frames48 || info.fec_frames48 || info.dred_frames48)
        out << L"\r\n" << Localized(L"Recovery frames (48 kHz): ") << L"PLC " << info.concealed_frames48
            << L", FEC " << info.fec_frames48 << L", DRED " << info.dred_frames48;
    for (const auto& field : {std::pair{L"Station: ", &info.station.name}, {L"Station genre: ", &info.station.genre},
        {L"Station description: ", &info.station.description}, {L"Station website: ", &info.station.website},
        {L"Recording: ", &info.station.recording_path}, {L"Recording error: ", &info.station.recording_error},
        {L"Transport status: ", &info.station.error}})
        if (!field.second->empty()) out << L"\r\n" << Localized(field.first) << *field.second;
    out << L"\r\n";
    return out.str();
}
std::wstring TagInformation(const TrackInfo& info) {
    std::wstring text = FormatInformation(info);
    for (const auto& tag : info.tags) {
        if (!tag.second.empty()) text += Utf8ToWide(tag.first.c_str()) + L": " + tag.second + L"\r\n";
    }
    return text;
}
bool MetadataValue(const TrackInfo& info, const char* raw_key, std::wstring& value) {
    value.clear();
    if (!raw_key) return false;
    std::string key(raw_key);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key == "type" || key == "lossless") value = L"0";
    else if (key == "family") value = L"Ogg Opus";
    else if (key == "mime") value = L"audio/ogg";
    else if (key == "serial" || key == "serialnumber") value = std::to_wstring(info.serial);
    else if (key == "serialnumber_hex") { wchar_t hex[9]{}; swprintf_s(hex, L"%08x", info.serial); value = hex; }
    else if (key == "rateable") value = info.radio ? L"0" : L"1";
    else if (key == "rating") {
        const auto text = Tag(info, "rating");
        if (text.empty() || text.size() > 3) return true;
        std::string digits;
        for (wchar_t c : text) { if (c < L'0' || c > L'9') return true; digits.push_back(static_cast<char>(c)); }
        int rating = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), rating);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || rating > 100) return true;
        value = std::to_wstring(rating > 5 ? (rating + 19) / 20 : rating);
    }
    else if (key == "length") value = std::to_wstring(info.length_ms);
    else if (key == "bitrate") value = std::to_wstring(BitrateKbps(info.bitrate));
    else if (key == "samplerate") value = std::to_wstring(info.decoded_rate);
    else if (key == "channels") value = std::to_wstring(info.source_channels);
    else if (key == "stereo") value = info.source_channels > 1 ? L"1" : L"0";
    else if (key == "formatinformation") value = FormatInformation(info);
    else if (key == "gain") value = Db(info.header_gain_q8);
    else if (key == "replaygain_track_gain" || key == "replaygain_album_gain") {
        const bool album = key == "replaygain_album_gain";
        const auto native = Tag(info, key.c_str());
        if (!native.empty()) {
            value = native;
            if (value.size() >= 2 && _wcsicmp(value.c_str() + value.size() - 2, L"dB") == 0) value.resize(value.size() - 2);
            while (!value.empty() && value.back() == L' ') value.pop_back();
        } else if (album ? info.has_album_gain : info.has_track_gain)
            value = Db(info.header_gain_q8 + (album ? info.album_gain_q8 : info.track_gain_q8) + 5 * 256, false);
        // Success + empty means supported but unset, not a disabled host field.
    } else {
        if (key == "year") key = "date";
        else if (key == "track") key = "tracknumber";
        else if (key == "disc" || key == "disk") key = "discnumber";
        else if (key == "album artist" || key == "album_artist" || key == "ensemble") key = "albumartist";
        else if (key == "category") key = "genre";
        else if (key == "grouping") key = "contentgroup";
        else if (key == "totaltracks") key = "tracktotal";
        else if (key == "totaldiscs") key = "disctotal";
        else if (key == "publisher") key = "organization";
        else if (key == "tool") key = "encoded-by";
        else if (key == "subtitle") key = "version";
        else if (key == "url") key = "purl";
        value = Tag(info, key.c_str());
        const auto fallback = [&](const char* alias) { if (value.empty()) value = Tag(info, alias); };
        if (key == "comment") fallback("description");
        if (key == "albumartist") { fallback("album artist"); fallback("album_artist"); fallback("ensemble"); }
        if (key == "date") fallback("year");
        if (key == "tracknumber") fallback("track");
        if (key == "discnumber") { fallback("disc"); fallback("disk"); }
        if (key == "organization") fallback("publisher");
        if (key == "encoded-by") fallback("tool");
        if (key == "version") fallback("subtitle");
        if (key == "purl") fallback("url");
        if (key == "genre") fallback("category");
        if (key == "contentgroup") fallback("grouping");
        if (key == "tracktotal") fallback("totaltracks");
        if (key == "disctotal") fallback("totaldiscs");
        if (key == "encoder" && value.empty()) value = info.vendor;
        if (value.empty() && !SupportedTextField(key) && !info.tags.count(key)) return false;
    }
    return true;
}
} // namespace winamp_opus
