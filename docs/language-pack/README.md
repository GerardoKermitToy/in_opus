# Opus input language packs

This plugin has its **own** Wasabi language identity,
`{5E2FDCEA-F625-48AB-8AC4-C50F099E32B8}`. Do not use the historical `in_vorbis`
GUID or the independent sibling Ogg input module's GUID. Their resource IDs and
layouts are not interchangeable.

## Translation files

- `in_opus_english.rc` is the generated English STRINGTABLE template.
- `catalogue.tsv` lists each stable ID and its JSON-escaped English fallback.
- `generate_catalogue.py` regenerates both files from the explicit catalogue in
  `src/language.cpp`. Run `python -I docs/language-pack/generate_catalogue.py
  --check` to check generated files and coverage of native resource labels.

Copy the template into your language-pack project, translate string values, and
change its `LANGUAGE` declaration as appropriate. Keep the IDs and the module
GUID string at resource ID 65535 unchanged. Preserve accelerator markers (`&`),
line breaks and intentional leading/trailing spaces. Do not add translated
DIALOG resources: the plugin owns its accessible native layouts and control IDs.
Build an x86 resource-only DLL with the usual Windows SDK resource compiler and
linker, for example from an x86 Native Tools prompt:

```bat
rc /nologo /fo in_opus_language.res in_opus_english.rc
link /dll /noentry /machine:x86 /out:in_opus_language.dll in_opus_language.res
```

Integrate that resource DLL into a Winamp language pack using Winamp's normal
language-pack tooling/module registration, then select the pack in Winamp.
Simply copying an arbitrary DLL next to `in_opus.dll` is not a registration
mechanism. The optional public Wasabi language service discovers the module by
this plugin's GUID. Hosts without that service continue in English.

## Fallback and lifetime

Initialize/refresh on Winamp's UI thread with `InitializeLanguage(HWND,
HINSTANCE)`; `ShutdownLanguage()` drops the cache. No host interface or module
handle is retained after initialization. `Localized(const wchar_t*)` returns an
owned, thread-safe string snapshot, never calls Winamp, and preserves exact
English for unknown, empty, unsupported, missing or malformed entries.

Partial packs are supported. A string must actually exist in the independent
module's STRINGTABLE before the service is asked to load it; an absent entry
cannot display a host diagnostic or a colliding fallback resource. Translations
are bounded to fewer than 4095 UTF-16 code units. Invalid pointers and malformed
service replies are ignored. Language initialization is not a sandbox for
malicious code in the same Winamp process.

`LocalizeDialog(HWND)` changes only the caption and direct text-only static and
button labels. It does not translate paths, tag values, edit controls, combo/list
contents, nested/foreign dialogs, or runtime file-information/status controls.
The owner supplies localized combo choices and fixed status/prompts separately.
Append new catalogue entries with new IDs; never renumber or reuse existing IDs.

## Settings and host inheritance

The original main and Advanced Opus dialogs keep their control IDs. More settings
adds keyboard-navigable Processing, Gain / host, and Network child pages without
expanding the main dialog. Closing/canceling either modal child discards that
child's changes; canceling the main dialog discards even accepted child changes.
Only main OK writes persistent settings.

Custom integer quantization is **off by default**, preserving the original
16-bit decoder fast path. Winamp ReplayGain inheritance defaults off; Winamp
proxy inheritance defaults on. They are independent switches. Public Wasabi
configuration is read once on the UI thread before playback workers start;
there are no host writes, retained interfaces, or automatic saves of inherited
values. Missing/invalid host items retain their local fallback. Disabled host
ReplayGain still preserves Opus header gain rather than selecting Raw mode.

Network settings are bounded: timeout 1000–30000 ms, probe 64–4096 KiB,
reconnect attempts 0–10, delay 250–30000 ms, buffer 32–4096 KiB, prebuffer no
larger than the buffer or half the probe, rebuffer threshold 0–100%, proxy
policy 0–2. A proxy is a single credential-free HTTP endpoint of at most 2048
characters; registry strings must be valid terminated REG_SZ without hidden
embedded NULs. Save-directory strings accept at most 32767 UTF-16 code units
without control characters. Credentials are never persisted in proxy settings.
No chained-source output-reopen preference is introduced here.

## Verification targets

The focused tests require MSVC Win32/C++17 and Unicode definitions, not Winamp or
external network access. They redirect HKCU to a unique temporary key; the user's
real settings are untouched. The following targets are registered in
`tests/CMakeLists.txt`:

- `feature_settings_tests`: `tests/feature_settings_tests.cpp`,
  `src/output_settings.cpp`, `src/language.cpp`, `src/file_info_dialog.rc`;
  link `user32 advapi32 comctl32`.
- `host_feature_tests`: `tests/host_feature_tests.cpp`, `src/host_settings.cpp`,
  `src/output_settings.cpp`, `src/language.cpp`; link `user32 advapi32 comctl32`.
- `language_feature_tests`: `tests/language_feature_tests.cpp`,
  `src/language.cpp`; link `user32 advapi32`.

Include `src` and `tests` and define `UNICODE _UNICODE WIN32_LEAN_AND_MEAN
NOMINMAX`. These standalone tests need no codec fixtures/library linkage. All
three executables take no arguments; `language_feature_tests --template` can
also print a catalogue-derived RC template. Set CTest timeouts (for example 30
seconds), especially for the native modal-dialog test. Existing targets which
compile `output_settings.cpp` also need `language.cpp` and `comctl32` linkage.

Tests cover every persisted field, signed/enumerated bounds, malformed registry
strings, opt-in defaults, seven channel modes, native tab bounds, UI validation,
parent/child OK and Cancel, read-only Wasabi snapshots, independent host toggles,
malformed host values, partial language fallback, exact resource IDs, no retained
interfaces, user-data preservation, and concurrent cache reset/read behavior.
