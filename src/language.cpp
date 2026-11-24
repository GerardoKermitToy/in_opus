// SPDX-License-Identifier: GPL-3.0-only
#include "language.hpp"
#include "host_settings.hpp"
#include "resource.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>

namespace winamp_opus {
namespace {
// Stable independent Opus STRINGTABLE IDs. Append only; never renumber/reuse.
constexpr LanguageStringEntry kStrings[] = {
    {30000, L"Winamp Opus Input"},
    {30001, L"Opus File Information"},
    {30002, L"Winamp Opus Settings"},
    {30003, L"Read-only technical information. Use Edit tags for supported local files."},
    {30004, L"Artist:"},
    {30005, L"Album:"},
    {30006, L"Audio"},
    {30007, L"Format:"},
    {30008, L"Duration:"},
    {30009, L"Average bitrate:"},
    {30010, L"Source channels:"},
    {30011, L"PCM output:"},
    {30012, L"Tags and stream information:"},
    {30013, L"&File / URL:"},
    {30014, L"&Edit tags..."},
    {30015, L"&Close"},
    {30016, L"Output channels"},
    {30017, L"S&ource channels (default)"},
    {30018, L"&Stereo"},
    {30019, L"Channel mode:"},
    {30020, L"PCM output"},
    {30021, L"Rate (0 = native):"},
    {30022, L"Resolution:"},
    {30023, L"Resampler quality:"},
    {30024, L"0: fastest, 10: highest. Bypassed at the native rate."},
    {30025, L"ReplayGain / R128"},
    {30026, L"Gain mode:"},
    {30027, L"File gain (dB):"},
    {30028, L"Radio gain:"},
    {30029, L"Target LUFS:"},
    {30030, L"Target loudness applies only when an R128 track/album gain is present."},
    {30031, L"Enable &dithering"},
    {30032, L"Show &instant bitrate"},
    {30033, L"Use Winamp's &unified file information dialog (local files)"},
    {30034, L"Audio changes take effect on the next playback."},
    {30035, L"&Advanced..."},
    {30036, L"&More settings..."},
    {30037, L"&OK"},
    {30038, L"&Cancel"},
    {30039, L"Advanced Opus Decoding"},
    {30040, L"Decode actual Opus HD / QEXT at native 96 kHz"},
    {30041, L"PCM rate 0 selects native output. A fixed PCM rate resamples afterwards."},
    {30042, L"Optional processing (changes decoded audio)"},
    {30043, L"Speech enhancement:"},
    {30044, L"Speech BWE (48 kHz only; synthetic high frequencies)"},
    {30045, L"Mono-compatible intensity stereo (disable phase inversion)"},
    {30046, L"Ignore packet extensions (compatibility / comparison; not DRED)"},
    {30047, L"Ambisonics output:"},
    {30048, L"Headphones use an analytic approximation, not measured/personalized HRTF."},
    {30049, L"Bounded recovery of provable interior packet loss"},
    {30050, L"Recovery mode:"},
    {30051, L"Maximum gap (ms):"},
    {30052, L"20-1000; CRC, headers and truncation stay strict. Not TCP repair."},
    {30053, L"More Opus Settings"},
    {30054, L"Use &custom integer quantization (off by default)"},
    {30055, L"Dither distribution:"},
    {30056, L"Noise shaping:"},
    {30057, L"Custom quantization is opt-in. With it off, the original 16-bit decoder fast path remains available. Dithering is enabled on the main settings page."},
    {30058, L"ATH shaping uses first-order shaping at unsupported rates. Surround matrices may clip; allow headroom with the file/radio gain controls."},
    {30059, L"&Buffer small local files"},
    {30060, L"Use &Winamp ReplayGain settings when available"},
    {30061, L"Host settings are read once at playback start, never written. Local gain choices remain the fallback and are saved independently."},
    {30062, L"Use only the &preferred track/album gain (no fallback)"},
    {30063, L"&Prevent clipping with tagged peak"},
    {30064, L"Peak handling:"},
    {30065, L"Untagged gain (dB):"},
    {30066, L"Untagged gain accepts -60 through +30 dB. Opus header gain is preserved except in explicit Raw mode. Peak options need valid peak tags."},
    {30067, L"Use Winamp &proxy settings when available"},
    {30068, L"Timeout (ms):"},
    {30069, L"Probe limit (KiB):"},
    {30070, L"Reconnect attempts:"},
    {30071, L"Retry delay (ms):"},
    {30072, L"Stream buffer (KiB):"},
    {30073, L"Prebuffer (KiB):"},
    {30074, L"Rebuffer threshold (%):"},
    {30075, L"Proxy use:"},
    {30076, L"Manual HTTP proxy (no credentials):"},
    {30077, L"&Save streams"},
    {30078, L"&Interactive authentication"},
    {30079, L"HTTP &seek"},
    {30080, L"Save directory (optional):"},
    {30081, L"Blank proxy: system settings. Prebuffer <= buffer and half the probe. Credentials are never saved; HTTP seeking requires server support."},
    {30082, L"mono"},
    {30083, L"stereo"},
    {30084, L"5.1 (6 channels)"},
    {30085, L"7.1 (8 channels)"},
    {30086, L" channels"},
    {30087, L"native rate"},
    {30088, L"Off (standard decoding)"},
    {30089, L"LACE (complexity 6)"},
    {30090, L"NoLACE (complexity 7)"},
    {30091, L"Stereo speakers"},
    {30092, L"Headphones (analytic approximate binaural)"},
    {30093, L"Quad speakers"},
    {30094, L"5.1 speakers (silent LFE)"},
    {30095, L"7.1 speakers (silent LFE)"},
    {30096, L"Strict (stop on later loss; default)"},
    {30097, L"Basic packet-loss concealment (PLC)"},
    {30098, L"Neural PLC"},
    {30099, L"FEC, then neural PLC"},
    {30100, L"DRED, then FEC / neural PLC"},
    {30101, L"Use a recovery limit from 20 to 1000 ms."},
    {30102, L"Invalid recovery limit"},
    {30103, L"Rectangular"},
    {30104, L"Triangular"},
    {30105, L"Gaussian"},
    {30106, L"None"},
    {30107, L"First-order"},
    {30108, L"ATH psychoacoustic"},
    {30109, L"ATH (low amplitude)"},
    {30110, L"Apply gain (allow clipping)"},
    {30111, L"Apply gain with peak protection"},
    {30112, L"Normalize using tagged peak"},
    {30113, L"Prevent clipping only"},
    {30114, L"Never"},
    {30115, L"HTTP port 80 only"},
    {30116, L"Always"},
    {30117, L"Select a valid dither distribution and noise shaping mode."},
    {30118, L"Invalid processing settings"},
    {30119, L"Select a peak mode and use an untagged gain from -60 to +30 dB."},
    {30120, L"Invalid gain settings"},
    {30121, L"Use timeout 1000-30000 ms, probe 64-4096 KiB, retries 0-10, delay 250-30000 ms, buffer 32-4096 KiB, prebuffer no larger than buffer or half the probe, and rebuffer threshold 0-100%."},
    {30122, L"Invalid network settings"},
    {30123, L"Enter a single credential-free HTTP proxy endpoint and a save path without control characters."},
    {30124, L"Processing"},
    {30125, L"Gain / host"},
    {30126, L"Network"},
    {30127, L"Stereo"},
    {30128, L"Source (Windows channel order)"},
    {30129, L"Mono"},
    {30130, L"Quad (downmix 5.1+)"},
    {30131, L"Source (unmapped codec channel order)"},
    {30132, L"Stereo surround matrix (DS)"},
    {30133, L"Stereo surround matrix (DS2)"},
    {30134, L"8-bit"},
    {30135, L"16-bit"},
    {30136, L"24-bit"},
    {30137, L"32-bit"},
    {30138, L"Header gain only (default)"},
    {30139, L"Album gain"},
    {30140, L"Track gain"},
    {30141, L"Automatic: track on shuffle, album otherwise"},
    {30142, L"Raw: ignore header and R128 gain"},
    {30143, L"Could not open advanced decoder settings."},
    {30144, L"Could not open additional settings."},
    {30145, L"Use rate 0 (native) or 8,000 to 192,000 Hz, gains from -60 to +30 dB, and a target from -40 to 0 LUFS."},
    {30146, L"Invalid audio settings"},
    {30147, L"Could not save settings for the current user."},
    {30148, L"Could not open the plugin settings dialog."},
    {30149, L"File Information"},
    {30150, L"File / URL"},
    {30151, L"Location:"},
    {30152, L"Tags"},
    {30153, L"Title:"},
    {30154, L"Track:"},
    {30155, L"Date:"},
    {30156, L"Genre:"},
    {30157, L"Comment:"},
    {30158, L"Other tags:"},
    {30159, L"Name:"},
    {30160, L"Value:"},
    {30161, L"Name"},
    {30162, L"Value"},
    {30163, L"Add / Update"},
    {30164, L"Delete"},
    {30165, L"Save"},
    {30166, L"Close"},
    {30167, L"Cancel"},
    {30168, L"OK"},
    {30169, L"Next"},
    {30170, L"Previous"},
    {30171, L"Live / unknown"},
    {30172, L"Current file"},
    {30173, L"Ogg Opus HTTP radio"},
    {30174, L"Ogg Opus (links: "},
    {30175, L"Ogg Opus"},
    {30176, L" (mono)"},
    {30177, L" (stereo)"},
    {30178, L"Could not open the file information dialog."},
    {30179, L"Live radio: stereo output supports changing stream layouts."},
    {30180, L"Stereo fallback: the channel layout changes within the file or is unsupported."},
    {30181, L"Stereo fallback: the output plugin rejected the source channel count."},
    {30182, L"Source channels. Speaker routing depends on the output plugin."},
    {30183, L"Stereo mode: multichannel recordings are downmixed to 2 channels."},
    {30184, L"Explicit channel processing. Matrix sums may require preamp headroom."},
    {30185, L"Could not open this Ogg Opus file."},
    {30186, L"Planned format; output support is checked when playback starts."},
    {30187, L"Live radio uses stereo. Start playback to read station information."},
    {30188, L"About Winamp Opus Input"},
    {30189, L"Playback is not currently running."},
    {30190, L"Opus (read-only)"},
    {30191, L"Read-only: radio and URL streams cannot be written."},
    {30192, L"Changes are written only when Save is selected."},
    {30193, L"Discard unsaved tag edits? No tags have been written automatically."},
    {30194, L"Unable to Save Tags"},
    {30195, L"The file could not be updated."},
    {30196, L"Opus tag editor"},
    {30197, L"(different values across links)"},
    {30198, L"Field names must use printable ASCII without '='."},
    {30199, L"This field name is invalid, technical, or reserved for artwork."},
    {30200, L"Invalid tag text. R128 gains must be signed 16-bit Q8 integers."},
    {30201, L"Only supported local Ogg Opus files can be edited."},
    {30202, L"Logical &link:"},
    {30203, L"Field &name:"},
    {30204, L"&Value:"},
    {30205, L"&Set value"},
    {30206, L"&Delete field"},
    {30207, L"Blank value deletes the field. Unchanged fields and embedded artwork are preserved. R128 gains use Q8 integers; rating uses 0-100."},
    {30208, L"All logical links"},
    {30209, L"Link"},
    {30210, L"Could not complete the tag operation."},
    {30211, L"Not enough memory to process Ogg tags safely."},
    {30212, L"Unexpected failure while writing Ogg tags."},
    {30213, L"Opus Proxy Authentication"},
    {30214, L"Opus Stream Authentication"},
    {30215, L"Credentials are used only for this connection and are not saved."},
    {30216, L"&User name:"},
    {30217, L"&Password:"},
    {30218, L"Warning: Basic authentication on this connection can expose your password."},
    {30219, L"I trust this connection and accept this &risk"},
    {30220, L"&Save"},
    {30221, L"Advanced"},
    {30222, L"Add New"},
    {30223, L"Delete All"},
    {30224, L"Advanced (read-only)"},
    {30225, L"A tag name must contain printable ASCII characters other than '='."},
    {30226, L"Yes"},
    {30227, L"No"},
    {30228, L"Length: "},
    {30229, L"Average bitrate: "},
    {30230, L"kbps"},
    {30231, L"File size: "},
    {30232, L"bytes"},
    {30233, L"Channels: "},
    {30234, L"Sampling rate: "},
    {30235, L"Hz"},
    {30236, L"Serial number: "},
    {30237, L"Version: "},
    {30238, L"Vendor:"},
    {30239, L"Codec: "},
    {30240, L"Duration: "},
    {30241, L"Samples per channel (48 kHz): "},
    {30242, L"Logical stream: "},
    {30243, L"Seekable: "},
    {30244, L"Mapping family: "},
    {30245, L"Opus streams: "},
    {30246, L"Coupled streams: "},
    {30247, L"Pre-skip (48 kHz): "},
    {30248, L"Input sample-rate hint: "},
    {30249, L"Normalized decoder output: "},
    {30250, L"QEXT: "},
    {30251, L"Header gain: "},
    {30252, L"R128 track gain: "},
    {30253, L"R128 album gain: "},
    {30254, L"Last packet bandwidth: "},
    {30255, L"Last packet duration: "},
    {30256, L"Entropy final range (diagnostic): "},
    {30257, L"Last voiced pitch period (48 kHz): "},
    {30258, L"Recovery frames (48 kHz): "},
    {30259, L"Station: "},
    {30260, L"Station genre: "},
    {30261, L"Station description: "},
    {30262, L"Station website: "},
    {30263, L"Recording: "},
    {30264, L"Recording error: "},
    {30265, L"Transport status: "},
};
constexpr std::size_t kStringCount = sizeof(kStrings) / sizeof(kStrings[0]);
constexpr std::size_t kCapacity = 4096;
constexpr bool SameText(const wchar_t* a, const wchar_t* b) noexcept {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
constexpr bool ValidCatalogue() noexcept {
    for (std::size_t i = 0; i < kStringCount; ++i) {
        if (!kStrings[i].id || kStrings[i].id >= 65535 || !*kStrings[i].fallback) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (kStrings[i].id == kStrings[j].id || SameText(kStrings[i].fallback, kStrings[j].fallback)) return false;
    }
    return true;
}
static_assert(ValidCatalogue(), "Language IDs and English phrases must be unique.");
SRWLOCK cache_lock = SRWLOCK_INIT;
std::array<std::wstring, kStringCount> translations;
struct SharedLock {
    SharedLock() noexcept { AcquireSRWLockShared(&cache_lock); }
    ~SharedLock() { ReleaseSRWLockShared(&cache_lock); }
};
std::size_t Index(const wchar_t* text) noexcept {
    if (text && *text) for (std::size_t i = 0; i < kStringCount; ++i)
        if (std::wcscmp(text, kStrings[i].fallback) == 0) return i;
    return kStringCount;
}
bool OnWindowThread(HWND window) noexcept {
    DWORD process = 0;
    return window && GetWindowThreadProcessId(window, &process) == GetCurrentThreadId() && process == GetCurrentProcessId();
}
bool Readable(const void* pointer, std::size_t bytes) noexcept {
    auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (address < 65536 || bytes > (std::numeric_limits<std::uintptr_t>::max)() - address) return false;
    const auto end = address + bytes;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        switch (region.Protect & 0xff) {
        case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
        default: return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.RegionSize > (std::numeric_limits<std::uintptr_t>::max)() - base) return false;
        const std::uintptr_t next = base + static_cast<std::uintptr_t>(region.RegionSize);
        if (next <= address) return false;
        address = (std::min)(end, next);
    }
    return true;
}
int FaultFilter(DWORD code) noexcept {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
bool CopyChecked(void* to, const void* from, std::size_t bytes) noexcept {
    if (!Readable(from, bytes)) return false;
    __try { std::memcpy(to, from, bytes); return true; }
    __except (FaultFilter(GetExceptionCode())) { return false; }
}
using host_settings::Dispatchable;
bool Callable(Dispatchable* object) noexcept {
    const void* table = nullptr;
    const void* function = nullptr;
    if (!CopyChecked(&table, object, sizeof(table)) || !CopyChecked(&function, table, sizeof(function))) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(function, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    switch (region.Protect & 0xff) {
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}
bool GuardedDispatch(Dispatchable* object, int message, void* result, void** args, int count) {
    __try { return object->_dispatch(message, result, args, count) != 0; }
    __except (FaultFilter(GetExceptionCode())) { return false; }
}
bool Dispatch(Dispatchable* object, int message, void* result, void** args, int count) noexcept {
    if (!Callable(object)) return false;
    try { return GuardedDispatch(object, message, result, args, count); }
    catch (...) { return false; }
}
struct LanguageInterface {
    Dispatchable* factory = nullptr;
    Dispatchable* service = nullptr;
    ~LanguageInterface() {
        if (service) {
            void* args[]{&service}; int ignored = 0;
            (void)Dispatch(factory, 310, &ignored, args, 1);
        }
    }
};
std::wstring Translation(Dispatchable* service, HINSTANCE module, unsigned id) {
    // A missing/empty actual entry must fall back to English, never to a host
    // diagnostic or unrelated original DLL resource with a colliding number.
    const wchar_t* resource = nullptr;
    const int length = LoadStringW(module, id, reinterpret_cast<LPWSTR>(&resource), 0);
    if (length <= 0 || length >= static_cast<int>(kCapacity) - 1 || !resource) return {};
    std::array<wchar_t, kCapacity> buffer;
    buffer.fill(static_cast<wchar_t>(0xffff));
    auto* output = buffer.data();
    auto capacity = buffer.size();
    HINSTANCE original = module;
    void* args[]{&module, &original, &id, &output, &capacity};
    wchar_t* returned = nullptr;
    if (!Dispatch(service, language::Message::GetStringW, &returned, args, 5) || returned != buffer.data()) return {};
    const auto end = std::find(buffer.begin(), buffer.end(), L'\0');
    if (end == buffer.begin() || end == buffer.end() || end == buffer.end() - 1) return {};
    return {buffer.data(), static_cast<std::size_t>(end - buffer.begin())};
}
void TranslateWindow(HWND window) {
    const auto length = GetWindowTextLengthW(window);
    if (length <= 0 || length >= static_cast<int>(kCapacity) - 1) return;
    std::array<wchar_t, kCapacity> text{};
    if (GetWindowTextW(window, text.data(), static_cast<int>(text.size())) != length || !LanguageStringId(text.data())) return;
    const auto localized = Localized(text.data());
    if (localized != text.data()) SetWindowTextW(window, localized.c_str());
}
BOOL CALLBACK TranslateChild(HWND child, LPARAM parent) noexcept {
    if (GetParent(child) != reinterpret_cast<HWND>(parent)) return TRUE;
    try {
        wchar_t name[32]{};
        if (!GetClassNameW(child, name, 32)) return TRUE;
        const auto style = GetWindowLongPtrW(child, GWL_STYLE);
        if (_wcsicmp(name, L"Static") == 0) {
            const auto type = style & SS_TYPEMASK;
            if (type != SS_LEFT && type != SS_CENTER && type != SS_RIGHT && type != SS_SIMPLE && type != SS_LEFTNOWORDWRAP) return TRUE;
            // IDs used for actual titles, tags and runtime status are never UI
            // labels, even when a user's data equals an English catalogue entry.
            switch (GetDlgCtrlID(child)) {
            case IDC_INFO_TITLE: case IDC_INFO_ARTIST: case IDC_INFO_ALBUM: case IDC_INFO_FORMAT:
            case IDC_INFO_DURATION: case IDC_INFO_BITRATE: case IDC_INFO_CHANNELS:
            case IDC_INFO_OUTPUT: case IDC_INFO_PATH: case IDC_INFO_TAGS: case IDC_INFO_STATUS: case IDC_OUTPUT_STATUS: return TRUE;
            default: break;
            }
        } else if (_wcsicmp(name, L"Button") == 0) {
            if ((style & (BS_ICON | BS_BITMAP)) || (style & BS_TYPEMASK) == BS_OWNERDRAW) return TRUE;
        } else return TRUE;
        TranslateWindow(child);
    } catch (...) { }
    return TRUE;
}
}
const LanguageStringEntry* LanguageStrings(std::size_t& count) noexcept { count = kStringCount; return kStrings; }
unsigned LanguageStringId(const wchar_t* text) noexcept {
    const auto index = Index(text); return index == kStringCount ? 0 : kStrings[index].id;
}
std::wstring Localized(const wchar_t* fallback) {
    if (!fallback) return {};
    const auto index = Index(fallback);
    if (index != kStringCount) {
        SharedLock guard;
        if (!translations[index].empty()) return translations[index];
    }
    return fallback;
}
void ShutdownLanguage() noexcept {
    AcquireSRWLockExclusive(&cache_lock);
    for (auto& text : translations) text.clear();
    ReleaseSRWLockExclusive(&cache_lock);
}
void InitializeLanguage(HWND host, HINSTANCE own_module) noexcept {
    if (host && !OnWindowThread(host)) return;
    ShutdownLanguage();
    if (!host || reinterpret_cast<std::uintptr_t>(own_module) < 65536) return;
    try {
        DWORD_PTR reply = 0;
        if (!SendMessageTimeoutW(host, WM_USER, 0, 3025, SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &reply)) return;
        auto* manager = reinterpret_cast<Dispatchable*>(reply);
        GUID service_guid = language::kServiceGuid;
        void* lookup[]{&service_guid};
        LanguageInterface interface;
        if (!Dispatch(manager, 50, &interface.factory, lookup, 1) || !Callable(interface.factory)) return;
        int global_lock = 0;
        void* request[]{&global_lock};
        Dispatchable* service = nullptr;
        if (!Dispatch(interface.factory, 300, &service, request, 1) || !Callable(service)) return;
        interface.service = service;
        GUID module_guid = language::kModuleGuid;
        void* startup[]{&own_module, &module_guid};
        HINSTANCE module = nullptr;
        if (!Dispatch(service, language::Message::StartLanguageSupport, &module, startup, 2) ||
            reinterpret_cast<std::uintptr_t>(module) < 65536 || module == own_module) return;
        std::array<std::wstring, kStringCount> snapshot;
        for (std::size_t i = 0; i < kStringCount; ++i) snapshot[i] = Translation(service, module, kStrings[i].id);
        AcquireSRWLockExclusive(&cache_lock);
        translations.swap(snapshot);
        ReleaseSRWLockExclusive(&cache_lock);
    } catch (...) { /* Exact English fallback on optional host/allocation failure. */ }
}
void LocalizeDialog(HWND dialog) {
    if (!OnWindowThread(dialog)) return;
    TranslateWindow(dialog); EnumChildWindows(dialog, TranslateChild, reinterpret_cast<LPARAM>(dialog));
}
}
