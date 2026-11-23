#include "metadata.hpp"
#include <algorithm>
#include <cctype>
#include <cwchar>

namespace winamp_opus {
namespace {
std::wstring Db(int q8, bool unit = true) {
    // Integer formatting is locale-independent, including under a host that has
    // changed LC_NUMERIC. Winamp parses ReplayGain using a decimal point.
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
}
std::wstring FormatInformation(const TrackInfo& info) {
    std::wstring text = info.radio ? L"Ogg Opus HTTP radio\n" : L"Ogg Opus\n";
    if (info.length_ms >= 0) text += L"Duration: " + std::to_wstring(info.length_ms / 1000) + L" s\n";
    else text += L"Duration: live / unknown\n";
    text += L"Decoded sample rate: 48000 Hz\nSource channels: " + std::to_wstring(info.source_channels) + L"\n";
    if (info.bitrate > 0) text += L"Average bitrate: " + std::to_wstring(BitrateKbps(info.bitrate)) + L" kbps\n";
    if (info.source_sample_rate > 0) text += L"Input sample-rate hint: " + std::to_wstring(info.source_sample_rate) + L" Hz\n";
    text += L"Header gain: " + Db(info.header_gain_q8) + L"\n";
    if (info.has_track_gain) text += L"R128 track gain: " + Db(info.track_gain_q8) + L"\n";
    if (info.has_album_gain) text += L"R128 album gain: " + Db(info.album_gain_q8) + L"\n";
    if (!info.vendor.empty()) text += L"Vendor: " + info.vendor + L"\n";
    if (!info.station.name.empty()) text += L"Station: " + info.station.name + L"\n";
    if (!info.station.genre.empty()) text += L"Station genre: " + info.station.genre + L"\n";
    if (!info.station.description.empty()) text += L"Station description: " + info.station.description + L"\n";
    if (!info.station.website.empty()) text += L"Station website: " + info.station.website + L"\n";
    return text;
}
std::wstring TagInformation(const TrackInfo& info) {
    std::wstring text = FormatInformation(info);
    for (const auto& tag : info.tags) {
        if (tag.second.empty()) continue;
        text += Utf8ToWide(tag.first.c_str()) + L": " + tag.second + L"\n";
    }
    std::wstring windows;
    windows.reserve(text.size() + 64);
    for (wchar_t c : text) { if (c == L'\n' && (windows.empty() || windows.back() != L'\r')) windows += L'\r'; windows += c; }
    return windows;
}
bool MetadataValue(const TrackInfo& info, const char* raw_key, std::wstring& value) {
    std::string key(raw_key ? raw_key : "");
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key == "type" || key == "lossless") value = L"0";
    else if (key == "family") value = L"Ogg Opus";
    else if (key == "length") value = std::to_wstring(info.length_ms);
    else if (key == "bitrate") value = std::to_wstring(BitrateKbps(info.bitrate));
    else if (key == "samplerate") value = L"48000";
    else if (key == "channels") value = std::to_wstring(info.source_channels);
    else if (key == "stereo") value = info.source_channels > 1 ? L"1" : L"0";
    else if (key == "formatinformation") value = FormatInformation(info);
    else if (key == "gain") value = Db(info.header_gain_q8);
    // Winamp ReplayGain fields use the conventional -18 LUFS reference.
    // Include header gain; return only actual metadata, never invent a gain.
    else if (key == "replaygain_track_gain" || key == "replaygain_album_gain") {
        const bool album = key == "replaygain_album_gain";
        const auto native = Tag(info, key.c_str());
        if (!native.empty()) {
            value = native;
            if (value.size() >= 2 && _wcsicmp(value.c_str() + value.size() - 2, L"dB") == 0)
                value.resize(value.size() - 2);
            while (!value.empty() && value.back() == L' ') value.pop_back();
        }
        else if (album ? info.has_album_gain : info.has_track_gain)
            value = Db(info.header_gain_q8 + (album ? info.album_gain_q8 : info.track_gain_q8) + 5 * 256, false);
        else return false;
    } else {
        if (key == "year") key = "date";
        else if (key == "track") key = "tracknumber";
        else if (key == "disc") key = "discnumber";
        else if (key == "album artist") key = "albumartist";
        else if (key == "publisher") key = "organization";
        else if (key == "url") key = "purl";
        value = Tag(info, key.c_str());
        if (value.empty() && key == "comment") value = Tag(info, "description");
        if (value.empty() && key == "albumartist") value = Tag(info, "album artist");
        if (value.empty() && key == "encoder") value = info.vendor;
        if (value.empty()) return false;
    }
    return true;
}
} // namespace winamp_opus
