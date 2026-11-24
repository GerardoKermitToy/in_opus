// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace winamp_opus {
using OpusTagChanges = std::map<std::string, std::wstring>;
struct OpusTagField {
    std::string name;
    std::wstring value;
    bool operator==(const OpusTagField& other) const { return name == other.name && value == other.value; }
    bool operator!=(const OpusTagField& other) const { return !(*this == other); }
};
using OpusTagFields = std::vector<OpusTagField>;
// The editor keeps ordered, repeated text fields separate from the display map.
// Opaque comments and artwork never enter this projection and remain on disk.
struct OpusTagDocument {
    int link = 0;
    std::uint32_t serial = 0;
    OpusTagFields fields;
};
bool ReadOpusTagDocument(const wchar_t* path, int link, OpusTagDocument&, std::wstring& error);
bool WriteOpusTagDocument(const wchar_t* path, const OpusTagDocument& original,
    const OpusTagFields& edited, std::wstring& error, bool* changed = nullptr);
struct OpusTagLink {
    OpusTagChanges tags; // Lower-case names; artwork deliberately excluded.
    std::wstring vendor;
    int header_gain_q8 = 0;
    std::uint32_t serial = 0;
    std::int64_t file_size = -1;
};
// Validate Ogg framing, checksums and stream lifecycle without decoding audio.
// Comments with duplicate names are joined with "; " for display only.
bool ReadOpusTagLinks(const wchar_t* path, std::vector<OpusTagLink>& links, std::wstring& error);
// -1 applies the patch to all links, otherwise link is a zero-based index.
// Empty values delete a field. Unmentioned fields, vendor, padding and artwork
// are preserved. Non-Opus streams and unsafe layouts are rejected. Successful
// no-op requests never replace the source file.
bool WriteOpusTags(const wchar_t* path, int link, const OpusTagChanges& changes, std::wstring& error,
    bool* changed = nullptr);
// Atomically apply different patches to the current logical links. The vector
// must match the link count; needed for equal ReplayGain with unequal header gains.
bool WriteOpusTagLinks(const wchar_t* path, const std::vector<OpusTagChanges>& changes,
    std::wstring& error, bool* changed = nullptr);
} // namespace winamp_opus
