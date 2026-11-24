// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real unified Advanced pane against a small synchronous Winamp host.
#include "fixtures.hpp"
#include "dialog_capture.hpp"
#include "metadata.hpp"
#include "opus_decoder.hpp"
#include "registry_sandbox.hpp"
#include "resource.h"
#include "tag_writer.hpp"
#include "winamp_input.hpp"

#include <commctrl.h>
#include <opusfile.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace winamp_opus;
using AddPane = HWND(__cdecl*)(int, const wchar_t*, HWND, wchar_t*, int);
using GetTag = int(__cdecl*)(const wchar_t*, const char*, wchar_t*, int);
using Bytes = std::vector<unsigned char>;

void Pump(DWORD milliseconds = 0) {
    const auto end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetTickCount64() >= end) return;
        Sleep(1);
    } while (true);
}

struct Host {
    HWND window = nullptr;
    HWND pane = nullptr;
    HWND formatInfo = nullptr;
    LONG_PTR formatStyle = 0;
    std::map<std::wstring, std::wstring> fields;
    int broadcasts = 0;
    int titleMessages = 0;
    bool recursion = false;

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            host = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        }
        if (message == WM_USER && host) {
            if (lParam == 243) {
                ++host->titleMessages;
                return 0;
            }
            if (wParam && lParam) {
                // Real unified hosts synchronously send an edited Basic Info value
                // to every tab, including the tab which originated the edit.
                ++host->broadcasts;
                if (host->broadcasts > 1000) {
                    host->recursion = true;
                    return 0;
                }
                const std::wstring name = reinterpret_cast<const wchar_t*>(wParam);
                const std::wstring value = reinterpret_cast<const wchar_t*>(lParam);
                host->fields[name] = value;
                if (host->pane) {
                    SendMessageW(host->pane, WM_USER, reinterpret_cast<WPARAM>(name.c_str()),
                                 reinterpret_cast<LPARAM>(value.c_str()));
                }
                return 0;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    Host() {
        WNDCLASSW cls{};
        cls.lpfnWndProc = Proc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"InOpusUnifiedTagEditorTestHost";
        Check(RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
              "register unified metadata host");
        window = CreateWindowExW(0, cls.lpszClassName, L"Unified metadata test host", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 700, 430, nullptr, nullptr, cls.hInstance, this);
        Check(window != nullptr, "create hidden unified metadata host");
        // This control belongs to the host, rather than to the plug-in pane.
        formatInfo = CreateWindowExW(0, L"Edit", L"Line 1\r\nLine 2\r\nLine 3\r\nLine 4\r\nLine 5\r\nLine 6\r\nLine 7",
            WS_CHILD | ES_MULTILINE | ES_READONLY, 0, 0, 180, 40, window,
            reinterpret_cast<HMENU>(1327), cls.hInstance, nullptr);
        Check(formatInfo != nullptr, "create host-owned Format Info field");
        formatStyle = GetWindowLongPtrW(formatInfo, GWL_STYLE);
    }

    ~Host() { if (window) DestroyWindow(window); }

    void Close() {
        if (pane) DestroyWindow(pane);
        pane = nullptr;
    }

    void Open(AddPane add, const std::filesystem::path& path) {
        Close();
        wchar_t name[64]{};
        pane = add(0, path.c_str(), window, name, static_cast<int>(std::size(name)));
        Check(pane != nullptr && std::wstring(name) == L"Advanced", "open the Advanced unified pane");
        Check(GetParent(pane) == window && GetPropW(window, L"INBUILT_NOWRITEINFO"),
              "Advanced owns the unified write transaction");
        Pump(); // Let the pane find a host control created after its WM_INITDIALOG.
    }

    void Field(const wchar_t* name, const wchar_t* value) const {
        SendMessageW(pane, WM_USER, reinterpret_cast<WPARAM>(name), reinterpret_cast<LPARAM>(value));
    }
};

void Click(HWND pane, int id) {
    SendMessageW(pane, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(pane, id)));
}

std::wstring Text(HWND pane, int id) {
    const HWND control = GetDlgItem(pane, id);
    Check(control != nullptr, "find editor control");
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    value.resize(std::wcslen(value.c_str()));
    return value;
}

int Find(HWND pane, const wchar_t* name, int occurrence = 0) {
    const HWND list = GetDlgItem(pane, IDC_TAG_LIST);
    for (int row = 0; row < ListView_GetItemCount(list); ++row) {
        std::array<wchar_t, 1024> text{};
        ListView_GetItemText(list, row, 0, text.data(), static_cast<int>(text.size()));
        if (!_wcsicmp(text.data(), name) && occurrence-- == 0) return row;
    }
    return -1;
}

void Select(HWND pane, int item) {
    Check(item >= 0, "find Advanced row");
    const HWND list = GetDlgItem(pane, IDC_TAG_LIST);
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void Change(HWND pane, int id, const wchar_t* value) {
    SetDlgItemTextW(pane, id, value);
    // Explicitly emulate the notification from a human edit. WM_SETTEXT alone
    // is not a reliable source of EN_CHANGE for a multiline control.
    SendMessageW(pane, WM_COMMAND, MAKEWPARAM(id, EN_CHANGE),
                 reinterpret_cast<LPARAM>(GetDlgItem(pane, id)));
}

void SetValue(HWND pane, const wchar_t* value) { Change(pane, IDC_TAG_VALUE, value); }
void SetName(HWND pane, const wchar_t* value) { Change(pane, IDC_TAG_NAME, value); }

void New(HWND pane, const wchar_t* name, const wchar_t* value) {
    Click(pane, IDC_TAG_ADD_NEW);
    SetName(pane, name);
    SetValue(pane, value);
}

BOOL CALLBACK Bounds(HWND child, LPARAM data) {
    const HWND pane = reinterpret_cast<HWND>(data);
    RECT rect{}, client{};
    GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, pane, reinterpret_cast<POINT*>(&rect), 2);
    GetClientRect(pane, &client);
    if (rect.left < 0 || rect.top < 0 || rect.right > client.right || rect.bottom > client.bottom)
        SetPropW(pane, L"OutOfBounds", reinterpret_cast<HANDLE>(1));
    return TRUE;
}

Bytes ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(input.good(), "read fixture bytes");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool Contains(const Bytes& bytes, const std::string& needle) {
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end(),
        [](unsigned char byte, char expected) { return byte == static_cast<unsigned char>(expected); }) != bytes.end();
}

BY_HANDLE_FILE_INFORMATION Identity(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "open source identity");
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL found = GetFileInformationByHandle(file, &info);
    CloseHandle(file);
    Check(found != FALSE, "get source identity");
    return info;
}

bool SameIdentity(const BY_HANDLE_FILE_INFORMATION& left, const BY_HANDLE_FILE_INFORMATION& right) {
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
        left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow &&
        left.ftLastWriteTime.dwHighDateTime == right.ftLastWriteTime.dwHighDateTime &&
        left.ftLastWriteTime.dwLowDateTime == right.ftLastWriteTime.dwLowDateTime;
}

OpusTagDocument Document(const std::filesystem::path& path, int link = 0) {
    OpusTagDocument document;
    std::wstring error;
    Check(ReadOpusTagDocument(path.c_str(), link, document, error), "read ordered Opus tag document");
    return document;
}

std::vector<std::wstring> Values(const OpusTagDocument& document, const char* name) {
    std::vector<std::wstring> result;
    for (const auto& field : document.fields)
        if (!_stricmp(field.name.c_str(), name)) result.push_back(field.value);
    return result;
}

std::vector<float> Decode(const std::filesystem::path& path) {
    const Bytes bytes = ReadBytes(path);
    int error = 0;
    std::unique_ptr<OggOpusFile, decltype(&op_free)> file(
        op_open_memory(bytes.data(), bytes.size(), &error), &op_free);
    Check(file != nullptr, "open edited Opus stream in libopusfile");
    std::array<float, 576 * 2> block{};
    std::vector<float> samples;
    for (;;) {
        const int frames = op_read_float_stereo(file.get(), block.data(), static_cast<int>(block.size()));
        Check(frames >= 0, "decode edited Opus PCM");
        if (!frames) return samples;
        samples.insert(samples.end(), block.begin(), block.begin() + frames * 2);
    }
}

int blockedDialogs = 0;
LRESULT CALLBACK BlockModalDialog(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_CREATEWND) {
        const auto* creation = reinterpret_cast<const CBT_CREATEWNDW*>(lParam);
        wchar_t name[32]{};
        GetClassNameW(reinterpret_cast<HWND>(wParam), name, static_cast<int>(std::size(name)));
        if (!(creation->lpcs->style & WS_CHILD) && !std::wcscmp(name, L"#32770")) {
            ++blockedDialogs;
            // Refuse unexpected error boxes before they can become an unattended
            // test hang. Each normal UI test avoids deliberately invalid saves.
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

struct ModalGuard {
    HHOOK hook = nullptr;
    ModalGuard() : hook(SetWindowsHookExW(WH_CBT, BlockModalDialog, nullptr, GetCurrentThreadId())) {
        Check(hook != nullptr, "install CBT modal-dialog guard");
    }
    ~ModalGuard() { if (hook) UnhookWindowsHookEx(hook); }
};

void EditingTests(Host& host, AddPane add, GetTag get, const std::filesystem::path& directory) {
    const auto path = directory / L"editable.opus";
    const auto tail = directory / L"editable-tail.opus";
    const std::string longName(200, 'N');
    const std::wstring longValue(70000, L'v');
    const std::string picture = "METADATA_BLOCK_PICTURE=AAECAwQFBgcICQoL";
    const std::string coverArt = "COVERART=AQID";
    const std::string opaque{"OPAQUE=\xff", 8};
    const std::string opaquePerformer{"PERFORMER=\xff", 11};
    MakeOpus(path, 2, 10017, 7013, {"DESCRIPTION=First line\r\nSecond line",
        "PERFORMER=First performer", "PERFORMER=Second performer", "ALBUM ARTIST=Ensemble name",
        picture, coverArt, opaque, opaquePerformer, longName + "=Long name"}, 256);
    MakeOpus(tail, 1, 2017, 7014, {"TAIL=untouched"}, -256);
    AppendFile(path, tail);

    const Bytes before = ReadBytes(path);
    const auto pcm = Decode(path);
    const auto secondLink = Document(path, 1);
    wchar_t cached[256]{};
    Check(get(path.c_str(), "title", cached, static_cast<int>(std::size(cached))), "seed metadata cache");

    host.Open(add, path);
    Check((GetWindowLongPtrW(host.formatInfo, GWL_STYLE) & (WS_VSCROLL | ES_AUTOVSCROLL)) ==
        (WS_VSCROLL | ES_AUTOVSCROLL), "enhance the host-owned Format Info field only");
    const auto firstLine = SendMessageW(host.formatInfo, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(host.formatInfo, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    Check(SendMessageW(host.formatInfo, EM_GETFIRSTVISIBLELINE, 0, 0) > firstLine,
        "host-owned Format Info scrolls to its additional lines");
    Check(!GetDlgItem(host.pane, IDOK) && !GetDlgItem(host.pane, IDCANCEL),
          "Advanced does not duplicate host OK or Cancel buttons");
    Check((GetWindowLongPtrW(GetDlgItem(host.pane, IDC_TAG_VALUE), GWL_STYLE) & ES_MULTILINE) != 0,
          "Advanced value control is multiline");
    Check(Find(host.pane, L"METADATA_BLOCK_PICTURE") < 0 && Find(host.pane, L"COVERART") < 0,
          "artwork comments do not enter editable text rows");
    Check(Find(host.pane, L"PERFORMER", 1) >= 0 &&
        Find(host.pane, std::wstring(200, L'N').c_str()) >= 0,
        "repeated and 200-character tag names remain individual rows");

    Select(host.pane, Find(host.pane, L"DESCRIPTION"));
    Check(Text(host.pane, IDC_TAG_VALUE) == L"First line\r\nSecond line", "preserve multiline text");
    SetValue(host.pane, L"Updated comment\r\nUnicode — テスト");
    Check(host.fields[L"comment"] == L"Updated comment\r\nUnicode — テスト",
          "Advanced description updates Basic Info comment");
    Check(!host.recursion, "synchronous host echo cannot recurse");

    host.Field(L"title", L"Basic Info title");
    Select(host.pane, Find(host.pane, L"TITLE"));
    Check(Text(host.pane, IDC_TAG_VALUE) == L"Basic Info title", "Basic Info updates Advanced title");
    host.Field(L"albumartist", L"");
    Check(Find(host.pane, L"ALBUM ARTIST") < 0, "alias deletion removes the legacy album-artist row");

    Select(host.pane, Find(host.pane, L"PERFORMER", 1));
    SetValue(host.pane, L"Only second performer changed");
    New(host.pane, L"PERFORMER", L"Third performer");
    New(host.pane, L"empty-field", L"");
    New(host.pane, L"large-value", longValue.c_str());
    Select(host.pane, Find(host.pane, L"large-value"));
    Check(Text(host.pane, IDC_TAG_VALUE) == longValue, "70,000-character value is not truncated");
    SetName(host.pane, L"renamed-value");
    Check(Find(host.pane, L"large-value") < 0 && Find(host.pane, L"renamed-value") >= 0,
          "rename changes the selected row");
    New(host.pane, L"X-Tag_123!}", L"arbitrary ASCII name — 値");
    New(host.pane, L"delete-me", L"Temporary");
    NMLVKEYDOWN key{};
    key.hdr.hwndFrom = GetDlgItem(host.pane, IDC_TAG_LIST);
    key.hdr.idFrom = IDC_TAG_LIST;
    key.hdr.code = LVN_KEYDOWN;
    key.wVKey = VK_DELETE;
    SendMessageW(host.pane, WM_NOTIFY, IDC_TAG_LIST, reinterpret_cast<LPARAM>(&key));
    Check(Find(host.pane, L"delete-me") < 0, "Delete key removes the selected row");
    Check(ReadBytes(path) == before, "draft edits cannot write before host OK");

    EnumChildWindows(host.pane, Bounds, reinterpret_cast<LPARAM>(host.pane));
    Check(!GetPropW(host.pane, L"OutOfBounds"), "Advanced controls fit inside its host pane");
    if (GetEnvironmentVariableW(L"OGG_TEST_CAPTURE_UI", nullptr, 0)) {
        ShowWindow(host.window, SW_SHOWNOACTIVATE);
        RedrawWindow(host.window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        CaptureDialog(host.pane, (directory / L"advanced-pane.bmp").c_str());
        ShowWindow(host.window, SW_HIDE);
    }

    host.titleMessages = 0;
    Click(host.pane, IDOK);
    Pump();
    const auto saved = Document(path);
    Check(Values(saved, "title") == std::vector<std::wstring>{L"Basic Info title"},
          "host title edit saves through the Advanced pane");
    Check(Values(saved, "performer") == std::vector<std::wstring>{
        L"First performer", L"Only second performer changed", L"Third performer"},
        "edit one duplicate and add another without coalescing values");
    Check(Values(saved, "empty-field") == std::vector<std::wstring>{L""},
          "an empty comment value is distinct from deleting a row");
    Check(Values(saved, "renamed-value") == std::vector<std::wstring>{longValue},
          "full renamed long value is saved");
    Check(Values(saved, longName.c_str()) == std::vector<std::wstring>{L"Long name"},
          "unchanged 200-character name survives a save");
    Check(Values(saved, "x-tag_123!}") == std::vector<std::wstring>{L"arbitrary ASCII name — 値"},
          "arbitrary valid ASCII comment name and Unicode value save");
    Check(Document(path, 1).fields == secondLink.fields, "unrelated chained logical link is unchanged");
    Check(Decode(path) == pcm, "tag write preserves decoded PCM bit-exactly");
    const Bytes written = ReadBytes(path);
    for (const auto& token : {picture, coverArt, opaque, opaquePerformer})
        Check(Contains(written, token), "artwork and opaque invalid UTF-8 bytes are retained");
    Check(host.titleMessages == 1, "changed tag save posts exactly one title IPC");
    Check(get(path.c_str(), "title", cached, static_cast<int>(std::size(cached))) &&
        std::wstring(cached) == L"Basic Info title", "successful save invalidates cached metadata");

    const auto identity = Identity(path);
    const int titleMessages = host.titleMessages;
    Click(host.pane, IDOK);
    Pump();
    Check(ReadBytes(path) == written && SameIdentity(identity, Identity(path)) &&
        host.titleMessages == titleMessages, "repeated no-op save preserves source identity and sends no title IPC");
    host.Close();
    Check(!GetPropW(host.window, L"INBUILT_NOWRITEINFO"), "editor removes its host write marker");
    Check(GetWindowLongPtrW(host.formatInfo, GWL_STYLE) == host.formatStyle,
          "editor restores the exact host Format Info style");

    host.Open(add, path);
    Click(host.pane, IDC_TAG_DELETE_ALL);
    Check(ListView_GetItemCount(GetDlgItem(host.pane, IDC_TAG_LIST)) == 0, "Delete All clears editable rows");
    Check(host.fields[L"title"].empty(), "Delete All synchronizes a blank Basic Info title");
    Click(host.pane, IDCANCEL);
    host.Close();
    Check(ReadBytes(path) == written, "Cancel discards Delete All");

    host.Open(add, path);
    Click(host.pane, IDC_TAG_DELETE_ALL);
    Click(host.pane, IDOK);
    Check(Document(path).fields.empty(), "Delete All commits every editable field on OK");
    const Bytes deleted = ReadBytes(path);
    Check(Contains(deleted, picture) && Contains(deleted, coverArt) && Contains(deleted, opaque) && Contains(deleted, opaquePerformer),
          "Delete All retains artwork and opaque comments");
    Check(Decode(path) == pcm, "Delete All preserves decoded PCM");
    host.Close();
}

void WriterGuardTests(Host& host, AddPane add, const std::filesystem::path& directory) {
    const auto path = directory / L"guarded.opus";
    MakeOpus(path, 2, 7013, 7201);

    // A pre-existing host marker belongs to the host and must be restored.
    SetPropW(host.window, L"INBUILT_NOWRITEINFO", reinterpret_cast<HANDLE>(7));
    host.Open(add, path);
    host.Close();
    Check(GetPropW(host.window, L"INBUILT_NOWRITEINFO") == reinterpret_cast<HANDLE>(7),
          "preserve a pre-existing host write marker");
    RemovePropW(host.window, L"INBUILT_NOWRITEINFO");

    const auto original = Document(path);
    auto edited = original.fields;
    for (auto& field : edited) if (!_stricmp(field.name.c_str(), "title")) field.value = L"unsaved title";
    std::wstring error;
    bool changed = false;
    Check(WriteOpusTags(path.c_str(), 0, {{"artist", L"external edit"}}, error, &changed) && changed,
          "make an external writer update");
    const Bytes concurrent = ReadBytes(path);
    Check(!WriteOpusTagDocument(path.c_str(), original, edited, error) && !error.empty() &&
        ReadBytes(path) == concurrent, "stale document cannot overwrite newer tags");

    const auto current = Document(path);
    auto invalid = current.fields;
    invalid.push_back({"bad=name", L"must fail"});
    Check(!WriteOpusTagDocument(path.c_str(), current, invalid, error) && !error.empty() &&
        ReadBytes(path) == concurrent, "invalid field name is rejected before replacement");

    auto tooLarge = current.fields;
    tooLarge.push_back({"large", std::wstring(16u * 1024u * 1024u, L'x')});
    Check(!WriteOpusTagDocument(path.c_str(), current, tooLarge, error) && !error.empty() &&
        ReadBytes(path) == concurrent, "16 MiB comment-header bound rejects oversized direct writes");

    // A valid Opus decoder can read a hard-linked file, while the writer must
    // leave it read-only rather than risk replacing a source alias.
    const auto source = directory / L"unsafe-source.opus";
    const auto hard = directory / L"unsafe-hardlink.opus";
    MakeOpus(source, 2, 7013, 7202);
    DeleteFileW(hard.c_str());
    if (CreateHardLinkW(hard.c_str(), source.c_str(), nullptr)) {
        const Bytes safeBytes = ReadBytes(source);
        host.Open(add, hard);
        Check(!IsWindowEnabled(GetDlgItem(host.pane, IDC_TAG_ADD_NEW)),
              "unsafe hard-linked input is a read-only Advanced pane");
        host.Field(L"title", L"must not edit unsafe source");
        Click(host.pane, IDOK);
        host.Close();
        Check(ReadBytes(source) == safeBytes, "read-only unsafe pane cannot change source bytes");
        DeleteFileW(hard.c_str());
    } else {
        std::cout << "Hard-link read-only pane test skipped: filesystem does not support hard links\n";
    }
}

struct BasicField {
    const char* host;
    const wchar_t* native;
    const wchar_t* value;
};

constexpr BasicField kBasicFields[] = {
    {"track", L"tracknumber", L"2/12"}, {"disc", L"discnumber", L"1/2"},
    {"bpm", L"bpm", L"123.5"}, {"title", L"title", L"Title — テスト"},
    {"artist", L"artist", L"Artist"}, {"album", L"album", L"Album"},
    {"albumartist", L"albumartist", L"Album artist"}, {"year", L"date", L"2026"},
    {"genre", L"genre", L"Genre"}, {"comment", L"comment", L"Line one\r\nLine two"},
    {"composer", L"composer", L"Composer"}, {"publisher", L"organization", L"Publisher"},
};

void BasicInfoTests(Host& host, AddPane add, GetTag get, const std::filesystem::path& directory) {
    // Keep this exact fixture name: CTest supplies this directory as unified-editor-data.
    const auto path = directory / L"basic-info.opus";
    MakeOpus(path, 2, 96000, 7301);
    std::wstring error;
    bool changed = false;
    const auto initial = Document(path);
    Check(WriteOpusTagDocument(path.c_str(), initial, {}, error, &changed) && changed,
          "make blank Basic Info fixture");

    wchar_t value[4096]{};
    for (const auto& field : kBasicFields) {
        std::fill(std::begin(value), std::end(value), L'!');
        Check(get(path.c_str(), field.host, value, static_cast<int>(std::size(value))) && !value[0],
              (std::string("supported missing Basic Info field returns empty: ") + field.host).c_str());
    }
    Check(!get(path.c_str(), "not_a_supported_or_present_tag", value, static_cast<int>(std::size(value))),
          "unknown absent metadata field is not advertised");

    host.Open(add, path);
    for (const auto& field : kBasicFields) {
        const std::wstring name(field.host, field.host + std::strlen(field.host));
        host.Field(name.c_str(), field.value);
        Select(host.pane, Find(host.pane, field.native));
        Check(Text(host.pane, IDC_TAG_VALUE) == field.value, "all Basic Info fields synchronize into Advanced");
    }
    Click(host.pane, IDOK);
    host.Close();
    for (const auto& field : kBasicFields)
        Check(get(path.c_str(), field.host, value, static_cast<int>(std::size(value))) &&
            std::wstring(value) == field.value, "all twelve Basic Info fields save and reload");

    host.Open(add, path);
    for (const auto& field : kBasicFields) {
        Select(host.pane, Find(host.pane, field.native));
        const std::wstring updated = std::wstring(field.value) + L" updated";
        SetValue(host.pane, updated.c_str());
        const std::wstring name(field.host, field.host + std::strlen(field.host));
        Check(host.fields[name] == updated, "all Advanced standard fields synchronize to Basic Info");
    }
    Click(host.pane, IDOK);
    host.Close();

    host.Open(add, path);
    for (const auto& field : kBasicFields) {
        const std::wstring name(field.host, field.host + std::strlen(field.host));
        host.Field(name.c_str(), L"");
    }
    Click(host.pane, IDOK);
    host.Close();
    Check(Document(path).fields.empty(), "Basic Info deletion removes primary fields and aliases");
    for (const auto& field : kBasicFields)
        Check(get(path.c_str(), field.host, value, static_cast<int>(std::size(value))) && !value[0],
              "cleared Basic Info field remains supported with an empty value");
}

void TechnicalInfoTests() {
    TrackInfo info;
    info.length_ms = 30000;
    info.total_frames = 1440000;
    info.file_size = 415902;
    info.format_version = 0;
    info.source_channels = 2;
    info.decoded_rate = 96000;
    info.output_rate = 96000;
    info.source_sample_rate = 44100;
    info.serial = 688178262;
    info.vendor = L"Lavf63.16.100";
    info.mapping_family = 1;
    info.stream_count = 2;
    info.coupled_count = 1;
    info.pre_skip48 = 312;
    info.qext = true;
    info.header_gain_q8 = -320;
    info.decoder_note = L"Actual decoder detail";

    const auto bitrate = static_cast<long long>(info.file_size * 8LL * 48000LL /
        (info.total_frames * 1000LL));
    const std::wstring legacy = L"Length: 30\r\nAverage bitrate: " + std::to_wstring(bitrate) +
        L" kbps\r\nFile size: 415902 bytes\r\nChannels: 2\r\nSampling rate: 96000 Hz\r\n"
        L"Serial number: 688178262\r\nVersion: 0\r\nVendor:\r\nLavf63.16.100\r\n";
    const auto text = FormatInformation(info);
    Check(text.find(legacy) == 0, "legacy Format Info block has exact historical order and CRLF");
    Check(text.find(L"\r\nCodec: Ogg Opus\r\nDuration: 30000 ms\r\nSamples per channel (48 kHz): 1440000") !=
        std::wstring::npos, "modern Opus information follows the legacy block");
    Check(text.find(L"Input sample-rate hint: 44100 Hz") != std::wstring::npos &&
        text.find(L"QEXT: Yes") != std::wstring::npos &&
        text.find(L"Header gain: -1.25 dB") != std::wstring::npos &&
        text.find(L"Pre-skip (48 kHz): 312") != std::wstring::npos,
        "report input-rate hint, QEXT, header gain and 48 kHz pre-skip");
    Check(text.find(L"Source bit depth") == std::wstring::npos, "do not invent a lossy Opus bit depth");

    for (const auto& duration : {std::pair{61000, L"Length: 1:01\r\n"},
            std::pair{3661000, L"Length: 1:01:01\r\n"}}) {
        info.length_ms = duration.first;
        Check(FormatInformation(info).find(duration.second) == 0, "legacy minute and hour duration formatting");
    }
    info.file_size = 5000000000LL;
    Check(FormatInformation(info).find(L"File size: 5000000000 bytes") != std::wstring::npos,
          "technical file size remains 64-bit");

    TrackInfo unknown;
    unknown.length_ms = -1;
    unknown.total_frames = -1;
    unknown.file_size = -1;
    unknown.format_version = -1;
    unknown.radio = true;
    const auto noFacts = FormatInformation(unknown);
    Check(noFacts.find(L"Length:") == std::wstring::npos && noFacts.find(L"Average bitrate:") == std::wstring::npos &&
        noFacts.find(L"File size:") == std::wstring::npos && noFacts.find(L"Version:") == std::wstring::npos,
        "unknown streaming facts are not fabricated");

    TrackInfo tags;
    tags.tags = {{"description", L"Fallback comment"}, {"year", L"2026"}, {"track", L"3"},
        {"disc", L"2"}, {"ensemble", L"Fallback ensemble"}, {"organization", L"Publisher"}};
    for (const auto& field : {std::pair{"comment", L"Fallback comment"}, {"year", L"2026"},
            {"track", L"3"}, {"disc", L"2"}, {"albumartist", L"Fallback ensemble"},
            {"publisher", L"Publisher"}}) {
        std::wstring value;
        Check(MetadataValue(tags, field.first, value) && value == field.second,
              "metadata aliases use native and historical spellings");
    }
    tags.tags = {{"replaygain_track_gain", L"-2.25 dB"}};
    std::wstring replayGain;
    Check(MetadataValue(tags, "replaygain_track_gain", replayGain) && replayGain == L"-2.25",
          "conventional ReplayGain query is unitless");
    tags.tags.clear();
    tags.has_track_gain = true;
    tags.header_gain_q8 = 256;
    tags.track_gain_q8 = -2112;
    Check(MetadataValue(tags, "replaygain_track_gain", replayGain) && replayGain == L"-2.25",
          "native R128 ReplayGain includes Opus header gain at the host reference");
    tags.tags = {{"rating", L"60"}};
    std::wstring rating;
    Check(MetadataValue(tags, "rating", rating) && rating == L"3", "native 0-100 rating maps to host stars");
}

void OpusConversionTests(Host& host, AddPane add, const std::filesystem::path& directory) {
    const auto path = directory / L"gain-rating.opus";
    MakeOpus(path, 2, 7013, 7401, {}, 256);
    host.Open(add, path);
    host.Field(L"replaygain_track_gain", L"-2.25 dB");
    Select(host.pane, Find(host.pane, L"r128_track_gain"));
    Check(Text(host.pane, IDC_TAG_VALUE) == L"-2112",
          "host -18 LUFS ReplayGain maps to R128 Q8 including header gain");
    host.Field(L"rating", L"4");
    Select(host.pane, Find(host.pane, L"rating"));
    Check(Text(host.pane, IDC_TAG_VALUE) == L"80", "host star rating maps to native 0-100 rating");
    SetValue(host.pane, L"60");
    Check(host.fields[L"rating"] == L"3", "Advanced native rating maps back to host stars");
    New(host.pane, L"r128_album_gain", L"-512");
    Check(host.fields[L"replaygain_album_gain"] == L"+4.00",
          "Advanced accepts raw native R128 fields and reports host gain");
    Click(host.pane, IDOK);
    const auto document = Document(path);
    Check(Values(document, "r128_track_gain") == std::vector<std::wstring>{L"-2112"} &&
        Values(document, "r128_album_gain") == std::vector<std::wstring>{L"-512"} &&
        Values(document, "rating") == std::vector<std::wstring>{L"60"},
        "native gain and rating values persist in ordered Opus comments");
    host.Close();

    host.Open(add, path);
    host.Field(L"replaygain_track_gain", L"");
    Check(Find(host.pane, L"r128_track_gain") < 0, "blank host ReplayGain deletes raw native R128 value");
    Click(host.pane, IDOK);
    Check(Values(Document(path), "r128_track_gain").empty(), "native ReplayGain deletion is saved");
    host.Close();
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    RegistrySandbox registry;
    HMODULE library = nullptr;
    winamp::InModule* module = nullptr;
    try {
        Check(argc == 3, "usage: tag_editor_tests DLL DIRECTORY");
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        Host host;
        library = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(library != nullptr, "load unified tag editor DLL");
        const auto getModule = reinterpret_cast<winamp::InModule* (__cdecl*)()>(GetProcAddress(library, "winampGetInModule2"));
        const auto addPane = reinterpret_cast<AddPane>(GetProcAddress(library, "winampAddUnifiedFileInfoPane"));
        const auto getTag = reinterpret_cast<GetTag>(GetProcAddress(library, "winampGetExtendedFileInfoW"));
        Check(getModule && addPane && getTag, "find unified metadata exports");
        module = getModule();
        module->hDllInstance = library;
        module->hMainWindow = host.window;
        module->Init();
        ModalGuard noDialogs;
        wchar_t name[64]{};
        Check(!addPane(1, L"unused", host.window, name, static_cast<int>(std::size(name))),
              "only one plug-in-owned Advanced tab exists");

        EditingTests(host, addPane, getTag, directory);
        WriterGuardTests(host, addPane, directory);
        BasicInfoTests(host, addPane, getTag, directory);
        TechnicalInfoTests();
        OpusConversionTests(host, addPane, directory);
        Check(!host.recursion && blockedDialogs == 0, "unified UI test did not recurse or open a modal dialog");
        host.Close();
        module->Quit();
        module = nullptr;
        FreeLibrary(library);
        library = nullptr;
        std::cout << "Unified Advanced tag editor tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (module) module->Quit();
        if (library) FreeLibrary(library);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
