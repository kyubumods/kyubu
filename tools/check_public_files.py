#!/usr/bin/env python3
"""check_public_files - keeps files derived from the game's code out of the repository.

  python tools/check_public_files.py            check every tracked and staged file (git ls-files)
  python tools/check_public_files.py <paths>    check these paths instead

Rejects Ghidra projects (*.gpr, *.rep/, *.gzf), type archives (*.gdt: release assets only), anything under
a decomp/ folder, and the game's own files. Exits non-zero and names each offending file.
(Minimal version for add-mapping-workspace; add-feature-testing task 5.5 turns it into an allow-list.)
"""
import fnmatch
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RULES = [
    ('*.gpr', 'Ghidra project file'),
    ('*.gzf', 'Ghidra packed program'),
    ('*.gdt', 'Ghidra type archive (attach it to a release instead)'),
]
DIR_RULES = [
    (lambda d: d.endswith('.rep'), 'Ghidra project folder'),
    (lambda d: d == 'decomp', 'decompiler output (keep it in $KMAP_HOME/decomp/)'),
]
GAME_FILES = {'cube.exe', 'server.exe', 'server.zip', 'data1.db', 'data2.db', 'data3.db', 'data4.db'}


def problem(path):
    parts = path.replace('\\', '/').split('/')
    name = parts[-1]
    for pattern, why in RULES:
        if fnmatch.fnmatch(name.lower(), pattern):
            return why
    for test, why in DIR_RULES:
        if any(test(d.lower()) for d in parts[:-1]):
            return why
    if name.lower() in GAME_FILES:
        return 'a game file (the game cannot be redistributed)'
    return None


def tracked_files():
    r = subprocess.run(['git', 'ls-files', '-z', '--cached'], cwd=ROOT, capture_output=True)
    if r.returncode:
        sys.exit('check_public_files: not a git checkout; pass the paths to check')
    return [p for p in r.stdout.decode('utf-8', 'replace').split('\0') if p]


def main(argv):
    paths = argv or tracked_files()
    bad = [(p, problem(p)) for p in paths if problem(p)]
    for p, why in bad:
        print(f'FAIL {p}: {why} must not be committed')
    print(f'{len(paths)} files checked: ' + ('ok' if not bad else f'{len(bad)} problem(s)'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
