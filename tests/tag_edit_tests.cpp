// SPDX-License-Identifier: GPL-3.0-only
#include "fixtures.hpp"
#include "registry_sandbox.hpp"
#include "tag_writer.hpp"
#include "winamp_input.hpp"
#include <ogg/ogg.h>
#include <opusfile.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace {
using Bytes = std::vector<unsigned char>;
Bytes Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary); Check(input.good(), "read fixture bytes");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Store(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(output.good(), "write malformed fixture");
}
BY_HANDLE_FILE_INFORMATION Identity(const std::filesystem::path& path) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "open file identity");
    BY_HANDLE_FILE_INFORMATION info{};
    const auto good = GetFileInformationByHandle(file, &info); CloseHandle(file);
    Check(good != FALSE, "file identity"); return info;
}
bool SameIdentity(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexLow == b.nFileIndexLow &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime &&
        a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime;
}
std::vector<float> Pcm(const std::filesystem::path& path) {
    const auto bytes = Read(path); int error = 0;
    std::unique_ptr<OggOpusFile, decltype(&op_free)> file(op_open_memory(bytes.data(), bytes.size(), &error), &op_free);
    Check(file != nullptr, "decode edited file with libopusfile");
    std::array<float, 576 * 2> block{}; std::vector<float> samples;
    for (;;) {
        const int frames = op_read_float_stereo(file.get(), block.data(), static_cast<int>(block.size()));
        Check(frames >= 0, "decoded PCM has no errors"); if (!frames) return samples;
        samples.insert(samples.end(), block.begin(), block.begin() + frames * 2);
    }
}
std::uint32_t Le32(const unsigned char* p) {
    return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::size_t PageEnd(const Bytes& bytes, std::size_t at) {
    Check(bytes.size() - at >= 27 && std::memcmp(bytes.data() + at, "OggS", 4) == 0, "test Ogg page");
    const auto count = bytes[at + 26]; std::size_t end = at + 27 + count;
    Check(end <= bytes.size(), "test lacing bounds");
    for (unsigned i = 0; i < count; ++i) end += bytes[at + 27 + i];
    Check(end <= bytes.size(), "test body bounds"); return end;
}
void Checksum(Bytes& bytes, std::size_t at) {
    const auto end = PageEnd(bytes, at); const auto header = 27u + bytes[at + 26];
    ogg_page page{bytes.data() + at, static_cast<long>(header), bytes.data() + at + header, static_cast<long>(end - at - header)};
    ogg_page_checksum_set(&page);
}
std::vector<Bytes> AudioPackets(const Bytes& bytes) {
    struct Stream { int number = 0; Bytes packet; };
    std::map<std::uint32_t, Stream> streams; std::vector<Bytes> result;
    for (std::size_t at = 0; at < bytes.size();) {
        const auto end = PageEnd(bytes, at); const auto segments = bytes[at + 26];
        auto& stream = streams[Le32(bytes.data() + at + 14)];
        std::size_t body = at + 27 + segments;
        for (unsigned i = 0; i < segments; ++i) {
            const auto size = bytes[at + 27 + i];
            stream.packet.insert(stream.packet.end(), bytes.begin() + body, bytes.begin() + body + size); body += size;
            if (size < 255) { if (stream.number++ >= 2) result.push_back(stream.packet); stream.packet.clear(); }
        }
        at = end;
    }
    return result;
}
bool HasComment(const std::filesystem::path& path, int link, const std::string& comment) {
    const auto bytes = Read(path); int error = 0;
    std::unique_ptr<OggOpusFile, decltype(&op_free)> file(op_open_memory(bytes.data(), bytes.size(), &error), &op_free);
    Check(file != nullptr, "parse retained original comments");
    const auto* tags = op_tags(file.get(), link); Check(tags != nullptr, "selected link comments");
    for (int i = 0; i < tags->comments; ++i)
        if (std::string(tags->user_comments[i], tags->comment_lengths[i]) == comment) return true;
    return false;
}
void Writer(const std::filesystem::path& directory) {
    using namespace winamp_opus;
    const auto first = directory / L"first.opus", second = directory / L"second.opus", chain = directory / L"chain.ogg";
    const std::string picture = "METADATA_BLOCK_PICTURE=AAECAwQFBgcICQoL";
    MakeOpus(first, 2, 10017, 8201, {picture, "COVERART=AQID", "X-KEEP=opaque", "DUP=a", "DUP=b"}, 2 * 256);
    MakeOpus(second, 1, 2017, 8202, {picture, "COMMENT=remove me"}, -3 * 256);
    std::filesystem::copy_file(first, chain, std::filesystem::copy_options::overwrite_existing); AppendFile(chain, second);
    const auto original = Read(chain), prefix = Read(first);
    const auto samples = Pcm(chain);
    const auto packets = AudioPackets(original);
    std::wstring error; bool changed = true;
    auto identity = Identity(chain);
    Check(WriteOpusTags(chain.c_str(), -1, {}, error, &changed) && !changed, "empty patch is a no-op");
    Check(WriteOpusTags(chain.c_str(), -1, {{"TITLE", L"Проверка Opus"}}, error, &changed) && !changed,
        "equal Unicode field value is a no-op");
    Check(SameIdentity(identity, Identity(chain)) && Read(chain) == original, "no-op preserves source identity timestamp and bytes");
    std::vector<OpusTagLink> links;
    Check(ReadOpusTagLinks(chain.c_str(), links, error) && links.size() == 2, "read chained tags without decoding");
    Check(links[0].header_gain_q8 == 512 && links[1].header_gain_q8 == -768 && links[0].tags["dup"] == L"a; b", "per-link header gains and displayed duplicate fields");
    Check(links[0].tags.count("metadata_block_picture") == 0, "artwork excluded from editable projection");
    Check(WriteOpusTags(chain.c_str(), 1, {{"title", L"第二リンク"}, {"comment", L""}, {"custom", std::wstring(150000, L'x')}}, error, &changed) && changed,
        "selected-link transactional repagination");
    const auto rewritten = Read(chain);
    Check(rewritten.size() > original.size() && std::equal(prefix.begin(), prefix.end(), rewritten.begin()), "unselected link byte-for-byte preserved");
    Check(AudioPackets(rewritten) == packets && Pcm(chain) == samples, "all encoded audio packets and decoded PCM preserved");
    Check(HasComment(chain, 0, picture) && HasComment(chain, 1, picture) && HasComment(chain, 0, "COVERART=AQID") &&
        HasComment(chain, 0, "DUP=a") && HasComment(chain, 0, "DUP=b"), "embedded artwork and untouched duplicate comments preserved");
    Check(ReadOpusTagLinks(chain.c_str(), links, error) && links[1].tags["title"] == L"第二リンク" && !links[1].tags.count("comment"), "selected edit and empty-value deletion");
    const auto stable = Read(chain);
    const std::vector<OpusTagChanges> invalid{{{"bad=name", L"x"}}, {{"metadata_block_picture", L""}},
        {{"same", L"a"}, {"SAME", L"b"}}, {{"title", std::wstring(1, wchar_t(0xd800))}}, {{"title", std::wstring(L"a\0b", 3)}}};
    for (const auto& patch : invalid) {
        Check(!WriteOpusTags(chain.c_str(), -1, patch, error) && !error.empty(), "invalid edit rejected before replacement");
        Check(Read(chain) == stable, "invalid edit leaves source untouched");
    }
    Check(!WriteOpusTags(chain.c_str(), 2, {{"title", L"no"}}, error) && Read(chain) == stable, "invalid link rejected");
    Check(!WriteOpusTagLinks(chain.c_str(), std::vector<OpusTagChanges>(1), error), "per-link patch requires exact link count");
    std::vector<Bytes> corrupt;
    auto bytes = prefix; bytes.back() ^= 0x40; corrupt.push_back(bytes); // CRC.
    bytes = prefix; bytes.pop_back(); corrupt.push_back(bytes); // Truncation.
    bytes = prefix; bytes[27 + bytes[26] + 9] = 0; Checksum(bytes, 0); corrupt.push_back(bytes); // Invalid OpusHead channel count.
    bytes = prefix; const auto page2 = PageEnd(bytes, 0); bytes[page2 + 18] ^= 1; Checksum(bytes, page2); corrupt.push_back(bytes); // Sequence hole.
    bytes = prefix; bytes[page2 + 5] ^= 1; Checksum(bytes, page2); corrupt.push_back(bytes); // Continuation.
    bytes = prefix; bytes[page2 + 5] |= 4; Checksum(bytes, page2); corrupt.push_back(bytes); // EOS on header.
    for (std::size_t i = 0; i < corrupt.size(); ++i) {
        const auto path = directory / (L"invalid-" + std::to_wstring(i) + L".opus"); Store(path, corrupt[i]);
        Check(!WriteOpusTags(path.c_str(), -1, {{"title", L"unsafe"}}, error), "unsafe framing or lifecycle rejected");
        Check(Read(path) == corrupt[i], "malformed source never modified");
    }
    const auto vorbis = directory / L"vorbis.ogg"; MakeVorbisHeaders(vorbis, 8203);
    Check(!WriteOpusTags(vorbis.c_str(), -1, {{"title", L"not Opus"}}, error), "do not edit Vorbis");
    const auto hard = directory / L"hard.opus";
    DeleteFileW(hard.c_str());
    if (CreateHardLinkW(hard.c_str(), first.c_str(), nullptr)) {
        const auto before = Read(first);
        Check(!WriteOpusTags(first.c_str(), -1, {{"title", L"hardlink"}}, error) && Read(first) == before,
            "reject hard-linked sources"); DeleteFileW(hard.c_str());
    } else std::cout << "Hard-link safety test skipped: filesystem cannot create hard links\n";
    const auto locked = CreateFileW(first.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "open non-delete-shared source");
    const auto before = Read(first);
    const bool result = WriteOpusTags(first.c_str(), -1, {{"title", L"locked"}}, error);
    CloseHandle(locked);
    Check(!result && Read(first) == before, "failed atomic replacement leaves original bytes intact");
}

using SetW = int (__cdecl*)(const wchar_t*, const char*, const wchar_t*);
using SetA = int (__cdecl*)(const char*, const char*, const char*);
using WriteTags = int (__cdecl*)();
using Metadata = int (__cdecl*)(const wchar_t*, const char*, wchar_t*, int);
std::atomic<int> opens{0}, closes{0}, flushes{0}, writes{0}, titleMessages{0};
int __cdecl Open(int, int, int, int, int) { ++opens; return 0; }
void __cdecl Close() { ++closes; }
int __cdecl Write(char*, int) { ++writes; return 0; }
int __cdecl CanWrite() { return 0; }
int __cdecl IsPlaying() { return 0; }
int __cdecl Pause(int value) { return value; }
void __cdecl IntegerVoid(int) {}
void __cdecl Flush(int) { ++flushes; }
int __cdecl Time() { return 0; }
void __cdecl VisInit(int, int) {}
void __cdecl VisDeinit() {}
void __cdecl Vis(void*, int, int, int) {}
void __cdecl SetInfo(int, int, int, int) {}
void __cdecl VisInfo(int, int) {}
int __cdecl DspActive() { return 0; }
LRESULT CALLBACK Host(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_USER && lparam == 243) { ++titleMessages; return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
void Pump(DWORD ms = 0) {
    const auto end = GetTickCount64() + ms;
    do { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (GetTickCount64() >= end) return; Sleep(1);
    } while (true);
}
HHOOK dialogHook = nullptr;
std::string dialogFailure;
bool editorOpened = false, editorInspected = false;
void CALLBACK Inspect(HWND dialog, UINT, UINT_PTR id, DWORD) noexcept {
    KillTimer(dialog, id);
    try {
        if (GetDlgItem(dialog, 5301)) {
            Check(GetDlgItem(dialog, 5303) && GetDlgItem(dialog, 5304), "native dynamic tag editor controls");
            SendDlgItemMessageW(dialog, 5301, CB_SETCURSEL, 2, 0); // Only second logical link.
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(5301, CBN_SELCHANGE), 0);
            SetDlgItemTextW(dialog, 5303, L"custom_editor"); SetDlgItemTextW(dialog, 5304, L"値" );
            editorInspected = true;
            PostMessageW(dialog, WM_COMMAND, IDOK, 0);
        } else if (GetDlgItem(dialog, 1060)) {
            if (!editorOpened) {
                Check(IsWindowEnabled(GetDlgItem(dialog, 1060)) != FALSE, "local file Edit tags enabled");
                editorOpened = true; PostMessageW(dialog, WM_COMMAND, 1060, 0);
            } else if (editorInspected && IsWindowEnabled(dialog)) { PostMessageW(dialog, WM_COMMAND, IDOK, 0); return; }
            SetTimer(dialog, 63, 30, Inspect);
        } else {
            dialogFailure = "unexpected error dialog during native edit";
            const auto owner = GetWindow(dialog, GW_OWNER);
            PostMessageW(dialog, WM_COMMAND, IDOK, 0);
            if (owner && GetDlgItem(owner, 5301)) PostMessageW(owner, WM_COMMAND, IDCANCEL, 0);
        }
    } catch (const std::exception& error) { dialogFailure = error.what(); PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0); }
    catch (...) { dialogFailure = "unexpected native tag editor failure"; PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0); }
}
LRESULT CALLBACK OnDialog(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0) {
        const auto& message = *reinterpret_cast<const CWPRETSTRUCT*>(lparam);
        if (message.message == WM_INITDIALOG) SetTimer(message.hwnd, 63, 30, Inspect);
    }
    return CallNextHookEx(dialogHook, code, wparam, lparam);
}
void Dll(HMODULE library, const std::filesystem::path& directory) {
    RegistrySandbox sandbox;
    const auto get = reinterpret_cast<winamp::InModule* (__cdecl*)()>(GetProcAddress(library, "winampGetInModule2"));
    const auto set = reinterpret_cast<SetW>(GetProcAddress(library, "winampSetExtendedFileInfoW"));
    const auto ansi = reinterpret_cast<SetA>(GetProcAddress(library, "winampSetExtendedFileInfo"));
    const auto write = reinterpret_cast<WriteTags>(GetProcAddress(library, "winampWriteExtendedFileInfo"));
    const auto metadata = reinterpret_cast<Metadata>(GetProcAddress(library, "winampGetExtendedFileInfoW"));
    Check(get && set && ansi && write && metadata, "real undecorated tag editing DLL ABI");
    const auto path = directory / L"staged.opus";
    MakeOpus(path, 2, 96017, 8210, {"DESCRIPTION=legacy comment", "REPLAYGAIN_TRACK_GAIN=9 dB"}, 2 * 256);
    WNDCLASSW cls{}; cls.lpfnWndProc = Host; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"OpusTagAbiHost";
    Check(RegisterClassW(&cls) != 0, "register tag ABI host");
    const auto host = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    Check(host != nullptr, "create tag ABI host");
    auto* input = get(); input->hDllInstance = library; input->hMainWindow = host;
    winamp::OutModule output{}; output.Open = Open; output.Close = Close; output.Write = Write; output.CanWrite = CanWrite;
    output.IsPlaying = IsPlaying; output.Pause = Pause; output.SetVolume = IntegerVoid; output.SetPan = IntegerVoid;
    output.Flush = Flush; output.GetOutputTime = Time; output.GetWrittenTime = Time;
    input->outMod = &output; input->SAVSAInit = VisInit; input->SAVSADeInit = VisDeinit;
    input->SAAddPCMData = Vis; input->VSAAddPCMData = Vis; input->VSASetInfo = VisInfo; input->SetInfo = SetInfo;
    input->dsp_isactive = DspActive; input->Init();
    Check(!write(), "write without staged fields is rejected");
    Check(set(path.c_str(), "RaTiNg", L"4") && set(path.c_str(), "YEAR", L"2026") &&
        set(path.c_str(), "replaygain_track_gain", L"-6.25 dB") && set(path.c_str(), "comment", L""), "stage rating aliases gain and deletion");
    const auto unchanged = Read(path);
    Check(!set(path.c_str(), "rating", L"6") && !set(path.c_str(), "type", L"1") &&
        !set(path.c_str(), "bad=name", L"x") && !set(path.c_str(), "metadata_block_picture", L""), "invalid staged edits rejected");
    Check(Read(path) == unchanged, "staging does not touch source");
    Check(write() == 1, "commit staged fields through DLL");
    std::vector<winamp_opus::OpusTagLink> links; std::wstring error;
    Check(winamp_opus::ReadOpusTagLinks(path.c_str(), links, error) && links[0].tags["rating"] == L"80" &&
        links[0].tags["date"] == L"2026" && links[0].tags["r128_track_gain"] == L"-3392" &&
        !links[0].tags.count("replaygain_track_gain") && !links[0].tags.count("description"), "stored rating and R128 conversion remove stale aliases");
    wchar_t text[2048]{};
    Check(metadata(path.c_str(), "rating", text, 2048) && std::wstring(text) == L"4", "host rating reads stars");
    Check(metadata(path.c_str(), "rateable", text, 2048) && std::wstring(text) == L"1", "local Opus is rateable");
    Check(metadata(path.c_str(), "replaygain_track_gain", text, 2048) && std::wstring(text) == L"-6.25", "R128 ReplayGain read behavior preserved");
    const auto identity = Identity(path);
    Check(set(path.c_str(), "year", L"2026") && write(), "same-value host save succeeds");
    Check(SameIdentity(identity, Identity(path)), "same-value host save does not replace file");
    const auto ansiPath = path.string();
    Check(ansi(ansiPath.c_str(), "title", "ANSI title") && write(), "ANSI setter ABI");
    Check(set(path.c_str(), "title", L"Main thread pending"), "stage foreground transaction");
    bool background = false;
    std::thread other([&] { background = set(path.c_str(), "rating", L"5") && write(); }); other.join();
    Check(background && write(), "independent concurrent staged transactions");
    Check(metadata(path.c_str(), "rating", text, 2048) && std::wstring(text) == L"5", "concurrent rating not lost by title write");
    // The file is open for audio, paused and backpressured. Tag save must replace
    // the directory entry without reopening, seeking or flushing that decoder.
    Check(input->Play(path.c_str()) == 0, "play tag ABI fixture"); input->Pause(); Pump(40); titleMessages = 0;
    const int initialOpens = opens.load(), initialFlushes = flushes.load();
    Check(set(path.c_str(), "title", L"New title") && set(path.c_str(), "artist", L"New artist") && write(), "save tags while audio is open");
    int duration = -1; input->GetFileInfo(nullptr, text, &duration);
    Check(std::wstring(text) == L"New artist - New title" && duration > 0, "active GetFileInfo immediately reflects saved tags");
    Check(metadata(nullptr, "formatinformation", text, 2048) &&
        std::wstring(text).find(L"File size: " + std::to_wstring(std::filesystem::file_size(path)) + L" bytes") != std::wstring::npos,
        "active technical metadata reflects the new container size without reopening audio");
    Pump(40); Check(titleMessages == 1, "exactly one changed-tags title notification");
    Check(opens == initialOpens && flushes == initialFlushes && writes == 0 && closes == 0 && input->IsPaused(),
        "tag editing does not reopen flush decode or unpause audio");
    titleMessages = 0;
    Check(set(path.c_str(), "title", L"New title") && write(), "no-op active save"); Pump(30);
    Check(titleMessages == 0 && opens == initialOpens && flushes == initialFlushes, "no-op active save has no IPC or audio effects");
    input->Stop(); Check(closes == opens, "playback resources balanced after edits");
    // Unequal header gains must produce equal effective host ReplayGain for all
    // links. Per-link patches are committed together, not through two replaces.
    const auto chain = directory / L"chain.ogg";
    Check(set(chain.c_str(), "replaygain_album_gain", L"-3 dB") && write(), "all-links staged ReplayGain");
    Check(winamp_opus::ReadOpusTagLinks(chain.c_str(), links, error) &&
        links[0].tags["r128_album_gain"] == L"-2560" && links[1].tags["r128_album_gain"] == L"-1280", "per-link header gain conversion");
    dialogHook = SetWindowsHookExW(WH_CALLWNDPROCRET, OnDialog, nullptr, GetCurrentThreadId());
    Check(dialogHook != nullptr, "install own-thread native editor hook");
    Check(input->InfoBox(chain.c_str(), host) == 1, "native tag editor InfoBox entry");
    UnhookWindowsHookEx(dialogHook); dialogHook = nullptr;
    Check(dialogFailure.empty() && editorOpened && editorInspected, dialogFailure.empty() ? "native editor was not exercised" : dialogFailure.c_str());
    Check(winamp_opus::ReadOpusTagLinks(chain.c_str(), links, error) && !links[0].tags.count("custom_editor") &&
        links[1].tags["custom_editor"] == L"値", "native editor selected link and custom Unicode field");
    input->Quit(); DestroyWindow(host); UnregisterClassW(cls.lpszClassName, cls.hInstance);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    HMODULE library = nullptr;
    try {
        Check(argc == 3, "usage: tag_edit_tests DLL directory (or --writer-only directory)");
        const std::filesystem::path directory(argv[2]); std::filesystem::create_directories(directory);
        Writer(directory);
        if (std::wstring(argv[1]) != L"--writer-only") {
            library = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            Check(library != nullptr, "load real tag editing DLL"); Dll(library, directory);
            FreeLibrary(library); library = nullptr;
        }
        std::cout << "Transactional tag tests passed\n"; return 0;
    } catch (const std::exception& error) {
        if (dialogHook) UnhookWindowsHookEx(dialogHook);
        // On playback failure process exit, not FreeLibrary, owns the worker:
        // unloading a DLL with an active decoding thread would hide the failure.
        std::cerr << error.what() << '\n'; return 1;
    }
}
