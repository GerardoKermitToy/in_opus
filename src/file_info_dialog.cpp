#include "file_info_dialog.hpp"
#include "resource.h"
#include "output_settings.hpp"
#include "metadata.hpp"
#include "tag_edit.hpp"
#include "language.hpp"
#include <algorithm>
#include <cwchar>
#include <charconv>
#include <filesystem>
#include <string>

namespace winamp_opus {
namespace {
constexpr int kEditTagsId = 1060; // IDC_EDIT_TAGS when provided by the optional resource.
std::wstring Duration(int milliseconds) {
    if (milliseconds < 0) return L"Live / unknown";
    const auto total = static_cast<long long>(std::max(0, milliseconds));
    const auto seconds = total / 1000;
    wchar_t text[32]{};
    if (seconds >= 3600) {
        swprintf_s(text, L"%lld:%02lld:%02lld.%03lld", seconds / 3600,
            seconds / 60 % 60, seconds % 60, total % 1000);
    } else {
        swprintf_s(text, L"%lld:%02lld.%03lld", seconds / 60, seconds % 60, total % 1000);
    }
    return text;
}
struct DialogData {
    TrackInfo info;
    const wchar_t* path;
    std::wstring duration;
    std::wstring bitrate;
    std::wstring channels;
    std::wstring format;
    std::wstring output;
    const std::wstring& note;
    HFONT title_font = nullptr;
    ~DialogData() { if (title_font) DeleteObject(title_font); }
};

void CenterOnOwner(HWND dialog) noexcept {
    const HWND owner = GetWindow(dialog, GW_OWNER);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(owner ? owner : dialog, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    RECT window{}, anchor = monitor.rcWork;
    GetWindowRect(dialog, &window);
    if (owner && IsWindowVisible(owner) && !IsIconic(owner)) GetWindowRect(owner, &anchor);
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    const int x = std::max(monitor.rcWork.left,
        std::min(anchor.left + (anchor.right - anchor.left - width) / 2, monitor.rcWork.right - width));
    const int y = std::max(monitor.rcWork.top,
        std::min(anchor.top + (anchor.bottom - anchor.top - height) / 2, monitor.rcWork.bottom - height));
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_INITDIALOG) {
        auto& data = *reinterpret_cast<DialogData*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        const auto& info = data.info;
        SetDlgItemTextW(dialog, IDC_INFO_TITLE, (info.title.empty() ? info.display_title : info.title).c_str());
        SetDlgItemTextW(dialog, IDC_INFO_ARTIST, info.artist.empty() ? L"—" : info.artist.c_str());
        SetDlgItemTextW(dialog, IDC_INFO_ALBUM, info.album.empty() ? L"—" : info.album.c_str());
        SetDlgItemTextW(dialog, IDC_INFO_FORMAT, data.format.c_str());
        SetDlgItemTextW(dialog, IDC_INFO_DURATION, data.duration.c_str());
        SetDlgItemTextW(dialog, IDC_INFO_BITRATE, data.bitrate.c_str());
        SetDlgItemTextW(dialog, IDC_INFO_CHANNELS, data.channels.c_str());
        SetDlgItemTextW(dialog, IDC_INFO_OUTPUT, data.output.c_str());
        SetDlgItemTextW(dialog, IDC_INFO_STATUS, data.note.c_str());
        const auto tags = TagInformation(info);
        SetDlgItemTextW(dialog, IDC_INFO_TAGS, tags.c_str());
        SetDlgItemTextW(dialog, IDC_INFO_PATH, data.path && *data.path ? data.path : L"Current file");

        LOGFONTW font{};
        const auto normal = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
        if (normal && GetObjectW(normal, sizeof(font), &font)) {
            font.lfWeight = FW_SEMIBOLD;
            font.lfHeight = MulDiv(font.lfHeight, 11, 9);
            data.title_font = CreateFontIndirectW(&font);
            if (data.title_font) SendDlgItemMessageW(dialog, IDC_INFO_TITLE, WM_SETFONT,
                reinterpret_cast<WPARAM>(data.title_font), TRUE);
        }
        HWND edit = GetDlgItem(dialog, kEditTagsId);
        if (!edit) {
            RECT area{14, 322, 96, 338}; MapDialogRect(dialog, &area);
            edit = CreateWindowExW(0, L"BUTTON", L"&Edit tags...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                area.left, area.top, area.right - area.left, area.bottom - area.top, dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditTagsId)),
                reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)), nullptr);
            if (edit) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(normal), FALSE);
        }
        if (edit) EnableWindow(edit, !info.radio && data.path && *data.path && !IsHttpUrl(data.path));
        LocalizeDialog(dialog);
        CenterOnOwner(dialog);
        SetFocus(GetDlgItem(dialog, IDOK));
        return FALSE; // Focus is explicitly on Close, not on the read-only path.
    }
    if (message == WM_COMMAND && LOWORD(wparam) == kEditTagsId) {
        try {
            auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(dialog, DWLP_USER));
            const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
            if (data && EditOpusTags(instance, dialog, data->path)) {
                std::vector<OpusTagLink> links;
                std::wstring error;
                if (ReadOpusTagLinks(data->path, links, error) && !links.empty()) {
                    auto& info = data->info;
                    const auto link = info.current_link >= 0 && static_cast<std::size_t>(info.current_link) < links.size()
                        ? static_cast<std::size_t>(info.current_link) : 0;
                    info.tags = links[link].tags;
                    info.vendor = links[link].vendor;
                    info.header_gain_q8 = links[link].header_gain_q8;
                    auto gain = [&](const char* key, int& result) {
                        const auto at = info.tags.find(key);
                        if (at == info.tags.end() || at->second.empty() || at->second.size() > 12) return false;
                        std::string text;
                        for (wchar_t c : at->second) { if (c > 127) return false; text.push_back(static_cast<char>(c)); }
                        auto first = text.data(); if (*first == '+') ++first;
                        const auto parsed = std::from_chars(first, text.data() + text.size(), result);
                        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && result >= -32768 && result <= 32767;
                    };
                    info.has_track_gain = gain("r128_track_gain", info.track_gain_q8);
                    info.has_album_gain = gain("r128_album_gain", info.album_gain_q8);
                    info.title = info.tags["title"]; info.artist = info.tags["artist"]; info.album = info.tags["album"];
                    const auto title = info.title.empty() ? std::filesystem::path(data->path).stem().wstring() : info.title;
                    info.display_title = info.artist.empty() ? title : info.artist + L" - " + title;
                    SetDlgItemTextW(dialog, IDC_INFO_TITLE, info.title.empty() ? info.display_title.c_str() : info.title.c_str());
                    SetDlgItemTextW(dialog, IDC_INFO_ARTIST, info.artist.empty() ? L"—" : info.artist.c_str());
                    SetDlgItemTextW(dialog, IDC_INFO_ALBUM, info.album.empty() ? L"—" : info.album.c_str());
                    SetDlgItemTextW(dialog, IDC_INFO_TAGS, TagInformation(info).c_str());
                }
            }
        } catch (...) {}
        return TRUE;
    }
    if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
        EndDialog(dialog, LOWORD(wparam));
        return TRUE;
    }
    if (message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
} // namespace

void ShowFileInfoDialog(HINSTANCE instance, HWND parent, const wchar_t* path, const TrackInfo& info,
                        int output_channels, const std::wstring& output_note, int output_rate, int output_bits) {
    DialogData data{info, path, Duration(info.length_ms),
        info.bitrate > 0 ? std::to_wstring(BitrateKbps(info.bitrate)) + L" kbps" : L"—",
        std::to_wstring(info.source_channels),
        info.radio ? L"Ogg Opus HTTP radio" : info.links > 1 ? L"Ogg Opus (links: " + std::to_wstring(info.links) + L")" : L"Ogg Opus",
        OutputFormatText(output_channels, output_rate, output_bits), output_note};
    if (info.source_channels == 1) data.channels += L" (mono)";
    else if (info.source_channels == 2) data.channels += L" (stereo)";
    if (DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_OPUS_FILE_INFO), parent,
            DialogProc, reinterpret_cast<LPARAM>(&data)) == -1) {
        MessageBoxW(parent, L"Could not open the file information dialog.", L"Winamp Opus Input", MB_OK | MB_ICONERROR);
    }
}
namespace {
INT_PTR CALLBACK UnifiedProc(HWND dialog, UINT message, WPARAM, LPARAM lparam) noexcept {
    if (message == WM_INITDIALOG) {
        SetDlgItemTextW(dialog, IDC_INFO_TAGS, reinterpret_cast<const wchar_t*>(lparam));
        // Only this technical pane is read-only. The host's basic pane must
        // remain editable now that the setter/write ABI is implemented.
        LocalizeDialog(dialog);
        return TRUE;
    }
    return FALSE;
}
}
HWND CreateUnifiedInfoPane(HINSTANCE instance, HWND parent, const TrackInfo& info) {
    const auto text = TagInformation(info);
    const auto pane = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_OPUS_UNIFIED), parent,
        UnifiedProc, reinterpret_cast<LPARAM>(text.c_str()));
    return pane;
}
} // namespace winamp_opus
