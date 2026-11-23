// SPDX-License-Identifier: GPL-3.0-only
#include "tag_edit.hpp"
#include "language.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace winamp_opus {
namespace {
constexpr std::size_t kMaxValueChars = 65536;
std::mutex configurationMutex;
HWND hostWindow = nullptr;
void (*invalidateMetadata)(const wchar_t*) = nullptr;

// Winamp's setter/write sequence is a transaction on the calling thread.
// A background rating update cannot overwrite an open basic-info transaction.
struct StagedFile { std::wstring path; OpusTagChanges fields; std::vector<int> headerGains; };
thread_local StagedFile staged;

bool LocalPath(const wchar_t* path, std::wstring& absolute) {
    if (!path || !*path || std::wcslen(path) > 32767) return false;
    // Decode/edit disk files only. Never let a metadata call open a network URL,
    // device or named pipe, even if the suffix resembles .opus.
    if (std::wcsstr(path, L"://") || !std::wcsncmp(path, L"\\\\.\\", 4) ||
        !std::wcsncmp(path, L"\\\\?\\GLOBALROOT", 14)) return false;
    const DWORD count = GetFullPathNameW(path, 0, nullptr, nullptr);
    if (!count || count > 32768) return false;
    std::vector<wchar_t> buffer(count);
    const DWORD length = GetFullPathNameW(path, count, buffer.data(), nullptr);
    if (!length || length >= count) return false;
    absolute.assign(buffer.data(), length);
    const auto attributes = GetFileAttributesW(absolute.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

void Committed(const std::wstring& path) noexcept {
    HWND host = nullptr;
    void (*callback)(const wchar_t*) = nullptr;
    try {
        std::lock_guard<std::mutex> guard(configurationMutex); host = hostWindow; callback = invalidateMetadata;
    } catch (...) { return; }
    try { if (callback) callback(path.c_str()); }
    catch (...) { /* A committed file must not turn into a reported write failure. */ }
    if (!callback && host && IsWindow(host)) PostMessageW(host, WM_USER, 0, 243);
}

bool Name(std::string& key, bool hostAliases) {
    if (key.empty() || key.size() > 128) return false;
    for (char& c : key) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 32 || byte > 125 || c == '=') return false;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    if (hostAliases) {
        if (key == "year") key = "date";
        else if (key == "track") key = "tracknumber";
        else if (key == "disc" || key == "disk") key = "discnumber";
        else if (key == "album artist" || key == "album_artist") key = "albumartist";
        else if (key == "publisher") key = "organization";
        else if (key == "url") key = "purl";
        else if (key == "category") key = "genre";
        else if (key == "grouping") key = "contentgroup";
        else if (key == "tracktotal" || key == "totaltracks") key = "tracktotal";
        else if (key == "disctotal" || key == "totaldiscs") key = "disctotal";
    }
    for (const char* reserved : {"length", "bitrate", "channels", "samplerate", "type", "lossless", "family",
        "formatinformation", "mime", "rateable", "serial", "gain", "stereo", "vendor",
        "metadata_block_picture", "coverart", "coverartmime"}) if (key == reserved) return false;
    return true;
}
bool Unicode(const std::wstring& text) {
    return text.size() <= kMaxValueChars && text.find(L'\0') == std::wstring::npos &&
        (text.empty() || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr) > 0);
}
bool Integer(const std::wstring& text, int& value) {
    if (text.empty() || text.size() > 12) return false;
    std::array<char, 13> bytes{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] > 127) return false;
        bytes[i] = static_cast<char>(text[i]);
    }
    const char* first = bytes.data();
    if (*first == '+') ++first;
    const auto parsed = std::from_chars(first, bytes.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == bytes.data() + text.size();
}
bool Gain(const std::wstring& text, double& db) {
    if (text.empty() || text.size() > 128) return false;
    std::array<char, 129> bytes{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] > 127) return false;
        bytes[i] = static_cast<char>(text[i]);
    }
    const char* first = bytes.data();
    const char* end = first + text.size();
    while (first != end && (*first == ' ' || *first == '\t')) ++first;
    if (first != end && *first == '+') ++first;
    const auto parsed = std::from_chars(first, end, db);
    if (parsed.ec != std::errc{} || !std::isfinite(db)) return false;
    auto tail = parsed.ptr;
    while (tail != end && (*tail == ' ' || *tail == '\t')) ++tail;
    if (end - tail >= 2 && (tail[0] == 'd' || tail[0] == 'D') && (tail[1] == 'b' || tail[1] == 'B')) tail += 2;
    while (tail != end && (*tail == ' ' || *tail == '\t')) ++tail;
    return tail == end;
}
bool R128(double db, int header, std::wstring& value) {
    const double q8 = db * 256.0 - header - 5 * 256.0;
    if (!std::isfinite(q8) || q8 < -32768 || q8 > 32767) return false;
    value = std::to_wstring(static_cast<int>(std::lround(q8)));
    return true;
}
bool ValidateRawValue(const std::string& key, const std::wstring& value) {
    if (!Unicode(value)) return false;
    if (!value.empty() && key == "rating") {
        int rating = 0;
        if (!Integer(value, rating) || rating < 0 || rating > 100) return false;
    }
    if (!value.empty() && (key == "r128_track_gain" || key == "r128_album_gain")) {
        int gain = 0;
        return Integer(value, gain) && gain >= -32768 && gain <= 32767;
    }
    return true;
}
// Header gains can differ between chained links. Convert the host's -18 LUFS
// ReplayGain request independently, then replace the whole file only once.
bool Patches(const std::vector<OpusTagLink>& links, const OpusTagChanges& fields,
             std::vector<OpusTagChanges>& patches) {
    patches.assign(links.size(), {});
    for (std::size_t i = 0; i < links.size(); ++i) {
        for (const auto& item : fields) {
            if (item.first == "replaygain_track_gain" || item.first == "replaygain_album_gain") {
                const auto native = item.first == "replaygain_track_gain" ? "r128_track_gain" : "r128_album_gain";
                std::wstring value;
                if (!item.second.empty()) {
                    double db = 0;
                    if (!Gain(item.second, db) || !R128(db, links[i].header_gain_q8, value)) return false;
                }
                patches[i][item.first] = L""; // Remove stale conventional fields.
                patches[i][native] = std::move(value);
            } else {
                patches[i][item.first] = item.second;
                if (item.first == "r128_track_gain") patches[i]["replaygain_track_gain"] = L"";
                if (item.first == "r128_album_gain") patches[i]["replaygain_album_gain"] = L"";
                // Clear legacy spellings as part of the same logical field edit,
                // including deletion, so metadata fallback cannot resurrect it.
                const std::pair<const char*, const char*> aliases[] = {
                    {"comment", "description"}, {"date", "year"}, {"albumartist", "album artist"},
                    {"albumartist", "album_artist"}, {"organization", "publisher"}, {"purl", "url"},
                    {"genre", "category"}, {"contentgroup", "grouping"},
                    {"tracktotal", "totaltracks"}, {"disctotal", "totaldiscs"}};
                for (const auto& alias : aliases) if (item.first == alias.first) patches[i][alias.second] = L"";
            }
        }
    }
    return true;
}

bool Stage(const wchar_t* path, const char* rawKey, const wchar_t* rawValue) {
    if (!rawKey || !rawValue || strnlen_s(rawKey, 129) > 128 || wcsnlen_s(rawValue, kMaxValueChars + 1) > kMaxValueChars)
        return false;
    std::wstring absolute;
    if (!LocalPath(path, absolute)) return false;
    std::string key(rawKey);
    if (!Name(key, true)) return false;
    std::wstring value(rawValue);
    if (!ValidateRawValue(key, value)) return false;
    if (key == "rating" && !value.empty()) {
        int rating = 0;
        if (!Integer(value, rating) || rating < 0 || rating > 5) return false;
        value = std::to_wstring(rating * 20);
    }
    const bool differentPath = _wcsicmp(staged.path.c_str(), absolute.c_str()) != 0;
    std::vector<int> headers;
    if (differentPath || staged.headerGains.empty()) {
        std::vector<OpusTagLink> links;
        std::wstring error;
        if (!ReadOpusTagLinks(absolute.c_str(), links, error)) return false;
        for (const auto& link : links) headers.push_back(link.header_gain_q8);
    } else headers = staged.headerGains;
    if (key == "replaygain_track_gain" || key == "replaygain_album_gain") {
        double db = 0;
        if (!value.empty() && !Gain(value, db)) return false;
        for (int header : headers) {
            std::wstring converted;
            if (!value.empty() && !R128(db, header, converted)) return false;
        }
    }
    if (differentPath) { staged.path = absolute; staged.fields.clear(); }
    staged.headerGains = std::move(headers);
    staged.fields[key] = std::move(value);
    // Last setter wins even if a caller mixes the host and native gain names.
    if (key == "r128_track_gain") staged.fields.erase("replaygain_track_gain");
    if (key == "r128_album_gain") staged.fields.erase("replaygain_album_gain");
    if (key == "replaygain_track_gain") staged.fields.erase("r128_track_gain");
    if (key == "replaygain_album_gain") staged.fields.erase("r128_album_gain");
    return true;
}
bool CommitStaged() {
    if (staged.path.empty() || staged.fields.empty()) return false;
    std::vector<OpusTagLink> links;
    std::wstring error;
    if (!ReadOpusTagLinks(staged.path.c_str(), links, error)) return false;
    std::vector<OpusTagChanges> patches;
    if (!Patches(links, staged.fields, patches)) return false;
    bool changed = false;
    if (!WriteOpusTagLinks(staged.path.c_str(), patches, error, &changed)) return false;
    const auto path = std::move(staged.path);
    staged.fields.clear(); staged.headerGains.clear();
    if (changed) Committed(path);
    return true;
}
bool Ansi(const char* input, std::wstring& output) {
    if (!input) return false;
    const int size = MultiByteToWideChar(CP_ACP, 0, input, -1, nullptr, 0);
    if (size <= 0 || size > static_cast<int>(kMaxValueChars + 1)) return false;
    output.resize(size);
    if (MultiByteToWideChar(CP_ACP, 0, input, -1, output.data(), size) != size) return false;
    output.pop_back();
    return true;
}

// The editor deliberately uses ordinary native controls and an in-memory dialog
// template, so it also works when the host doesn't load this DLL's resources.
enum : int { Scope = 5301, TagList, Key, Value, SetValue, DeleteValue, Hint };
struct Editor {
    std::wstring path;
    std::vector<OpusTagLink> links;
    std::vector<OpusTagChanges> patches;
    std::vector<std::string> names;
    int scope = -1;
    bool loading = false, dirty = false;
};
std::wstring Text(HWND control) {
    const int size = GetWindowTextLengthW(control);
    if (size < 0 || size > static_cast<int>(kMaxValueChars)) return {};
    std::wstring text(static_cast<std::size_t>(size) + 1, 0);
    const int copied = GetWindowTextW(control, text.data(), size + 1);
    text.resize(copied > 0 ? copied : 0);
    return text;
}
void Error(HWND dialog, const std::wstring& text) {
    MessageBoxW(dialog, text.c_str(), Localized(L"Opus tag editor").c_str(), MB_OK | MB_ICONERROR);
}
OpusTagChanges Effective(const Editor& editor, std::size_t index) {
    auto fields = editor.links[index].tags;
    for (const auto& field : editor.patches[index]) {
        if (field.second.empty()) fields.erase(field.first);
        else fields[field.first] = field.second;
    }
    return fields;
}
void Refresh(HWND dialog, Editor& editor, const std::string& select = {}) {
    editor.loading = true;
    SendDlgItemMessageW(dialog, TagList, LB_RESETCONTENT, 0, 0);
    editor.names.clear();
    const auto first = editor.scope < 0 ? 0u : static_cast<std::size_t>(editor.scope);
    std::vector<OpusTagChanges> shown;
    if (editor.scope < 0) {
        shown.reserve(editor.links.size());
        for (std::size_t i = 0; i < editor.links.size(); ++i) shown.push_back(Effective(editor, i));
    } else shown.push_back(Effective(editor, first));
    const auto& fields = shown.front();
    std::set<std::string> keys;
    for (const auto& link : shown) for (const auto& field : link) keys.insert(field.first);
    int selected = -1;
    for (const auto& key : keys) {
        const auto it = fields.find(key);
        auto value = it == fields.end() ? std::wstring() : it->second;
        if (editor.scope < 0) for (std::size_t i = 1; i < shown.size(); ++i) {
            const auto& other = shown[i];
            const auto at = other.find(key);
            if ((at == other.end()) != (it == fields.end()) || (at != other.end() && at->second != value)) {
                value = Localized(L"(different values across links)"); break;
            }
        }
        std::wstring line(key.begin(), key.end());
        line += L" = " + value;
        std::replace(line.begin(), line.end(), L'\r', L' ');
        std::replace(line.begin(), line.end(), L'\n', L' ');
        if (line.size() > 512) line.resize(512);
        SendDlgItemMessageW(dialog, TagList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        if (key == select) selected = static_cast<int>(editor.names.size());
        editor.names.push_back(key);
    }
    if (selected >= 0) SendDlgItemMessageW(dialog, TagList, LB_SETCURSEL, selected, 0);
    editor.loading = false;
}
bool ApplyEntry(HWND dialog, Editor& editor, bool remove = false) {
    const auto name = Text(GetDlgItem(dialog, Key));
    std::string key;
    for (wchar_t c : name) {
        if (c > 127) { Error(dialog, Localized(L"Field names must use printable ASCII without '='.")); return false; }
        key.push_back(static_cast<char>(c));
    }
    if (!Name(key, false)) {
        Error(dialog, Localized(L"This field name is invalid, technical, or reserved for artwork.")); return false;
    }
    const auto value = remove ? std::wstring() : Text(GetDlgItem(dialog, Value));
    if (!ValidateRawValue(key, value)) {
        Error(dialog, Localized(L"Invalid tag text. R128 gains must be signed 16-bit Q8 integers.")); return false;
    }
    for (std::size_t i = 0; i < editor.links.size(); ++i)
        if (editor.scope < 0 || i == static_cast<std::size_t>(editor.scope)) editor.patches[i][key] = value;
    editor.dirty = false;
    Refresh(dialog, editor, key);
    if (remove) { editor.loading = true; SetDlgItemTextW(dialog, Value, L""); editor.loading = false; }
    return true;
}
HWND Control(HWND dialog, HINSTANCE instance, const wchar_t* cls, const wchar_t* text,
             DWORD style, int id, int x, int y, int width, int height, DWORD extended = 0) {
    RECT rect{x, y, x + width, y + height};
    MapDialogRect(dialog, &rect);
    HWND child = CreateWindowExW(extended, cls, text, WS_CHILD | WS_VISIBLE | style,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (child) SendMessageW(child, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
    return child;
}
INT_PTR CALLBACK EditorProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    try {
        auto* editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG) {
            editor = reinterpret_cast<Editor*>(lparam);
            SetWindowTextW(dialog, Localized(L"Opus tag editor").c_str());
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
            struct Item { const wchar_t* cls; const wchar_t* text; DWORD style; int id, x, y, w, h; DWORD ex = 0; };
            const Item items[] = {
                {L"STATIC", L"Logical &link:", 0, -1, 10, 11, 64, 12},
                {L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, Scope, 76, 8, 334, 140},
                {L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL, TagList, 10, 32, 400, 119, WS_EX_CLIENTEDGE},
                {L"STATIC", L"Field &name:", 0, -1, 10, 160, 64, 12},
                {L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, Key, 76, 157, 334, 16, WS_EX_CLIENTEDGE},
                {L"STATIC", L"&Value:", 0, -1, 10, 182, 64, 12},
                {L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_TABSTOP | WS_VSCROLL, Value, 76, 179, 334, 55, WS_EX_CLIENTEDGE},
                {L"BUTTON", L"&Set value", BS_PUSHBUTTON | WS_TABSTOP, SetValue, 76, 242, 84, 18},
                {L"BUTTON", L"&Delete field", BS_PUSHBUTTON | WS_TABSTOP, DeleteValue, 170, 242, 84, 18},
                {L"STATIC", L"Blank value deletes the field. Unchanged fields and embedded artwork are preserved. R128 gains use Q8 integers; rating uses 0-100.", 0, Hint, 10, 270, 400, 30},
                {L"BUTTON", L"&Save", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 260, 309, 70, 20},
                {L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 340, 309, 70, 20},
            };
            for (const auto& item : items) {
                const auto text = Localized(item.text);
                if (!Control(dialog, instance, item.cls, text.c_str(), item.style,
                    item.id, item.x, item.y, item.w, item.h, item.ex)) {
                    EndDialog(dialog, -1); return TRUE;
                }
            }
            SendDlgItemMessageW(dialog, Key, EM_SETLIMITTEXT, 128, 0);
            SendDlgItemMessageW(dialog, Value, EM_SETLIMITTEXT, kMaxValueChars, 0);
            SendDlgItemMessageW(dialog, Scope, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Localized(L"All logical links").c_str()));
            for (std::size_t i = 0; i < editor->links.size(); ++i) {
                auto label = Localized(L"Link") + L" " + std::to_wstring(i + 1);
                const auto title = editor->links[i].tags.find("title");
                if (title != editor->links[i].tags.end()) label += L" — " + title->second;
                SendDlgItemMessageW(dialog, Scope, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendDlgItemMessageW(dialog, Scope, CB_SETCURSEL, 0, 0);
            Refresh(dialog, *editor);
            LocalizeDialog(dialog);
            return TRUE;
        }
        if (!editor) return FALSE;
        if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
        if (message != WM_COMMAND) return FALSE;
        const auto id = LOWORD(wparam), event = HIWORD(wparam);
        if ((id == Key || id == Value) && event == EN_CHANGE && !editor->loading) { editor->dirty = true; return TRUE; }
        if (id == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        if (id == SetValue || id == DeleteValue) { ApplyEntry(dialog, *editor, id == DeleteValue); return TRUE; }
        if (id == Scope && event == CBN_SELCHANGE) {
            const int scope = static_cast<int>(SendDlgItemMessageW(dialog, Scope, CB_GETCURSEL, 0, 0)) - 1;
            if (editor->dirty && !ApplyEntry(dialog, *editor)) {
                SendDlgItemMessageW(dialog, Scope, CB_SETCURSEL, editor->scope + 1, 0); return TRUE;
            }
            editor->scope = scope;
            editor->loading = true;
            SetDlgItemTextW(dialog, Key, L""); SetDlgItemTextW(dialog, Value, L"");
            editor->loading = false;
            Refresh(dialog, *editor); return TRUE;
        }
        if (id == TagList && event == LBN_SELCHANGE) {
            const auto selected = static_cast<int>(SendDlgItemMessageW(dialog, TagList, LB_GETCURSEL, 0, 0));
            if (selected < 0 || static_cast<std::size_t>(selected) >= editor->names.size()) return TRUE;
            const auto key = editor->names[selected];
            if (editor->dirty && !ApplyEntry(dialog, *editor)) return TRUE;
            const auto fields = Effective(*editor, editor->scope < 0 ? 0 : static_cast<std::size_t>(editor->scope));
            const auto found = fields.find(key);
            const std::wstring name(key.begin(), key.end());
            editor->loading = true;
            SetDlgItemTextW(dialog, Key, name.c_str());
            SetDlgItemTextW(dialog, Value, found == fields.end() ? L"" : found->second.c_str());
            editor->loading = false; editor->dirty = false;
            const auto at = std::find(editor->names.begin(), editor->names.end(), key);
            if (at != editor->names.end()) SendDlgItemMessageW(dialog, TagList, LB_SETCURSEL,
                static_cast<WPARAM>(at - editor->names.begin()), 0);
            return TRUE;
        }
        if (id == IDOK) {
            if (editor->dirty && !ApplyEntry(dialog, *editor)) return TRUE;
            std::wstring error;
            bool changed = false;
            if (!WriteOpusTagLinks(editor->path.c_str(), editor->patches, error, &changed)) { Error(dialog, error); return TRUE; }
            if (changed) Committed(editor->path);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
    } catch (...) {
        // No exceptions may cross a Win32 callback boundary.
        MessageBoxW(dialog, L"Could not complete the tag operation.", L"Opus tag editor", MB_OK | MB_ICONERROR);
        if (message == WM_INITDIALOG) EndDialog(dialog, -1);
    }
    return FALSE;
}
} // namespace

void ConfigureTagEditing(HWND host, void (*invalidate)(const wchar_t*)) noexcept {
    try { std::lock_guard<std::mutex> guard(configurationMutex); hostWindow = host; invalidateMetadata = invalidate; }
    catch (...) {}
}
bool EditOpusTags(HINSTANCE instance, HWND parent, const wchar_t* path) {
    try {
        Editor editor;
        std::wstring error;
        if (!LocalPath(path, editor.path) || !ReadOpusTagLinks(editor.path.c_str(), editor.links, error)) {
            Error(parent, error.empty() ? Localized(L"Only supported local Ogg Opus files can be edited.") : error);
            return false;
        }
        editor.patches.resize(editor.links.size());
        // Standard DLGTEMPLATE, aligned on DWORD, with no resource references.
        struct alignas(DWORD) Template { DLGTEMPLATE dialog; WORD menu, cls, title, points; wchar_t font[9]; };
        Template memory{};
        memory.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
        memory.points = 9; wcscpy_s(memory.font, L"Segoe UI");
        memory.dialog.cx = 420; memory.dialog.cy = 340;
        const auto result = DialogBoxIndirectParamW(instance, &memory.dialog, parent, EditorProc,
            reinterpret_cast<LPARAM>(&editor));
        return result == IDOK;
    } catch (...) { return false; }
}
} // namespace winamp_opus

extern "C" int __cdecl winampSetExtendedFileInfoW(const wchar_t* path, const char* key, const wchar_t* value) {
    try { return winamp_opus::Stage(path, key, value) ? 1 : 0; } catch (...) { return 0; }
}
extern "C" int __cdecl winampSetExtendedFileInfo(const char* path, const char* key, const char* value) {
    try {
        std::wstring widePath, wideValue;
        if (!winamp_opus::Ansi(path, widePath) || !winamp_opus::Ansi(value, wideValue)) return 0;
        return winamp_opus::Stage(widePath.c_str(), key, wideValue.c_str()) ? 1 : 0;
    } catch (...) { return 0; }
}
extern "C" int __cdecl winampWriteExtendedFileInfo() {
    try { return winamp_opus::CommitStaged() ? 1 : 0; } catch (...) { return 0; }
}
