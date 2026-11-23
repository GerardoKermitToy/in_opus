#!/usr/bin/env python3
"""Regenerate the independent Opus translation template/catalogue; --check is read-only."""
from pathlib import Path
import argparse
import json
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--check', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'src' / 'language.cpp').read_text(encoding='utf-8')
entries = [(int(m[1]), json.loads('"' + m[2] + '"')) for m in
           re.finditer(r'\{(\d+), L"((?:\\.|[^"\\])*)"\}', source)]
assert entries and len({i for i, _ in entries}) == len(entries)
assert len({s for _, s in entries}) == len(entries)
assert all(0 < i < 65535 and s for i, s in entries)
guid = '{5E2FDCEA-F625-48AB-8AC4-C50F099E32B8}'
def escaped(s):
    return json.dumps(s, ensure_ascii=False)
rc = ('// Generated from src/language.cpp; keep IDs and module GUID unchanged.\n'
      '#pragma code_page(65001)\n#include <windows.h>\n'
      'LANGUAGE LANG_ENGLISH, SUBLANG_ENGLISH_US\nSTRINGTABLE\nBEGIN\n'
      f'    65535 "{guid}"\n')
rc += ''.join(f'    {i} {escaped(s)}\n' for i, s in entries) + 'END\n'
tsv = 'id\tenglish_fallback_json\n' + ''.join(f'{i}\t{escaped(s)}\n' for i, s in entries)
for name, text in [('in_opus_english.rc', rc), ('catalogue.tsv', tsv)]:
    path = Path(__file__).resolve().parent / name
    if args.check:
        assert path.read_text(encoding='utf-8') == text, f'stale generated file: {path}'
    else:
        path.write_text(text, encoding='utf-8', newline='\n')
# Every template label/caption must be represented. Never scan text from users.
phrases = {s for _, s in entries}
for line in (root / 'src' / 'file_info_dialog.rc').read_text(encoding='utf-8').splitlines():
    if not re.match(r'\s*(CAPTION|LTEXT|GROUPBOX|CONTROL|PUSHBUTTON|DEFPUSHBUTTON)\s', line):
        continue
    match = re.search(r'"((?:\\.|[^"\\])*)"', line)
    if match:
        phrase = json.loads('"' + match[1] + '"')
        assert not phrase or phrase in phrases, f'uncatalogued resource label: {phrase}'
print(f'{len(entries)} stable independent Opus strings; template/catalogue and resource labels verified.')
