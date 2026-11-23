// SPDX-License-Identifier: GPL-3.0-only
#include "credentials.hpp"
#include "language.hpp"
#include <array>

namespace winamp_opus {
void ClearPassword(std::wstring& password) noexcept {
    if (!password.empty()) SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    password.clear();
}
namespace {
constexpr int kUser = 100, kPassword = 101, kConsent = 102;
struct Prompt {
    const std::atomic<bool>& cancelled;
    const std::atomic<bool>& expired;
    const std::wstring& server;
    std::wstring& user;
    std::wstring& password;
    bool proxy, insecure;
};
HWND Control(HWND dialog, const wchar_t* type, const wchar_t* text, DWORD style,
    int id, int x, int y, int width, int height) noexcept {
    RECT rect{x, y, x + width, y + height};
    MapDialogRect(dialog, &rect);
    const auto window = CreateWindowExW(type[0] == L'E' ? WS_EX_CLIENTEDGE : 0,
        type, text, WS_CHILD | WS_VISIBLE | style, rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (window) SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return window;
}
INT_PTR CALLBACK Procedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    try {
    auto* prompt = reinterpret_cast<Prompt*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        prompt = reinterpret_cast<Prompt*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        if (prompt->cancelled || prompt->expired) { EndDialog(dialog, IDCANCEL); return TRUE; }
        SetWindowTextW(dialog, Localized(prompt->proxy ? L"Opus Proxy Authentication" : L"Opus Stream Authentication").c_str());
        bool ok = true;
        ok = Control(dialog, L"STATIC", Localized(L"Credentials are used only for this connection and are not saved.").c_str(),
            0, 0, 8, 7, 254, 23) && ok;
        ok = Control(dialog, L"STATIC", prompt->server.c_str(), SS_PATHELLIPSIS | SS_NOPREFIX,
            0, 8, 31, 254, 12) && ok;
        ok = Control(dialog, L"STATIC", Localized(L"&User name:").c_str(), 0, 0, 8, 48, 64, 12) && ok;
        ok = Control(dialog, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
            kUser, 73, 46, 189, 14) && ok;
        ok = Control(dialog, L"STATIC", Localized(L"&Password:").c_str(), 0, 0, 8, 67, 64, 12) && ok;
        ok = Control(dialog, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
            kPassword, 73, 65, 189, 14) && ok;
        SendDlgItemMessageW(dialog, kUser, EM_SETLIMITTEXT, 512, 0);
        SendDlgItemMessageW(dialog, kPassword, EM_SETLIMITTEXT, 512, 0);
        if (prompt->insecure) {
            ok = Control(dialog, L"STATIC", Localized(L"Warning: Basic authentication on this connection can expose your password.").c_str(),
                0, 0, 8, 85, 254, 24) && ok;
            ok = Control(dialog, L"BUTTON", Localized(L"I trust this connection and accept this &risk").c_str(), WS_TABSTOP | BS_AUTOCHECKBOX,
                kConsent, 8, 110, 254, 13) && ok;
        }
        ok = Control(dialog, L"BUTTON", Localized(L"OK").c_str(), WS_TABSTOP | BS_DEFPUSHBUTTON,
            IDOK, 148, 133, 54, 16) && ok;
        ok = Control(dialog, L"BUTTON", Localized(L"Cancel").c_str(), WS_TABSTOP | BS_PUSHBUTTON,
            IDCANCEL, 208, 133, 54, 16) && ok;
        EnableWindow(GetDlgItem(dialog, IDOK), !prompt->insecure);
        if (!ok || !SetTimer(dialog, 1, 75, nullptr)) { EndDialog(dialog, IDCANCEL); return TRUE; }
        SetFocus(GetDlgItem(dialog, kUser));
        return FALSE;
    }
    if (!prompt) return FALSE;
    if (message == WM_TIMER && (prompt->cancelled || prompt->expired)) {
        SetDlgItemTextW(dialog, kPassword, L"");
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        const auto id = LOWORD(wparam);
        if (id == kConsent) {
            EnableWindow(GetDlgItem(dialog, IDOK), IsDlgButtonChecked(dialog, kConsent) == BST_CHECKED);
            return TRUE;
        }
        if (id == IDCANCEL) {
            SetDlgItemTextW(dialog, kPassword, L"");
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (id == IDOK) {
            if (prompt->insecure && IsDlgButtonChecked(dialog, kConsent) != BST_CHECKED) return TRUE;
            std::array<wchar_t, 513> user{}, secret{};
            GetDlgItemTextW(dialog, kUser, user.data(), static_cast<int>(user.size()));
            GetDlgItemTextW(dialog, kPassword, secret.data(), static_cast<int>(secret.size()));
            bool accepted = !prompt->cancelled && !prompt->expired;
            try {
                if (accepted) { prompt->user = user.data(); prompt->password = secret.data(); }
            } catch (...) { accepted = false; ClearPassword(prompt->password); }
            SecureZeroMemory(secret.data(), sizeof(secret));
            SecureZeroMemory(user.data(), sizeof(user));
            SetDlgItemTextW(dialog, kPassword, L"");
            EndDialog(dialog, accepted ? IDOK : IDCANCEL);
            return TRUE;
        }
    }
    if (message == WM_CLOSE) { SetDlgItemTextW(dialog, kPassword, L""); EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
    } catch (...) {
        SetDlgItemTextW(dialog, kPassword, L"");
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
}
}
bool AskCredentials(HWND parent, bool proxy, const std::wstring& server,
    const std::atomic<bool>& cancelled, const std::atomic<bool>& expired,
    std::wstring& user, std::wstring& password, bool insecure_basic) {
    ClearPassword(password);
    if (cancelled || expired) return false;
    // Runtime native dialog: no resource dependency, OS credential cache, save
    // checkbox, or uncancellable credential-broker call on a decoder thread.
    struct alignas(DWORD) Template {
        DLGTEMPLATE dialog;
        WORD menu, window_class, title;
    } form{};
    form.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    form.dialog.cx = 270; form.dialog.cy = 157;
    Prompt prompt{cancelled, expired, server, user, password, proxy, insecure_basic};
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &form.dialog,
        parent, Procedure, reinterpret_cast<LPARAM>(&prompt));
    if (result != IDOK || cancelled || expired) { ClearPassword(password); user.clear(); return false; }
    return true;
}
} // namespace winamp_opus
