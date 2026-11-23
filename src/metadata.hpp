#pragma once
#include "opus_decoder.hpp"
namespace winamp_opus {
bool MetadataValue(const TrackInfo& info, const char* key, std::wstring& value);
std::wstring FormatInformation(const TrackInfo& info);
std::wstring TagInformation(const TrackInfo& info);
} // namespace winamp_opus
