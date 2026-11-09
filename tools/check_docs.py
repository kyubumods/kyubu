#!/usr/bin/env python3
"""check_docs - keeps docs/ honest.

  python tools/check_docs.py

1. Every event name the loader emits (or marks unavailable) appears in docs/events.md.
2. Every code block preceded by `<!-- sample: <path> -->` is identical to that file in the repository,
   so documented examples are the ones that build and run.
3. Relative links point at files that exist, in every page under docs/ (including the generated reference in
   docs/game/) and in the atlas pages (mapping/atlas/*.md), which link to the reference.
4. Every game-API member of KyubuApi (after the game marker in sdk/include/kyubu_api.h) has an entry in
   docs/game-api.md, a `### ` heading with its name in backticks and **Side:** and **Errors:** lines, and every
   primitive of loader/src/game/entries.inc has a row in that page's primitives table.
Exits non-zero and lists the problems when a check fails.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_MARKER = '/* ---- game API ----'


def loader_sources():
    for base, _, files in os.walk(os.path.join(ROOT, 'loader', 'src')):
        for f in files:
            if f.endswith(('.cpp', '.h')):
                yield os.path.join(base, f)


def emitted_events():
    names = set()
    for path in loader_sources():
        text = open(path, encoding='utf-8').read()
        names.update(re.findall(r'emit(?:_active)?\(\s*"([a-z_]+\.[a-z_]+)"', text))
        names.update(re.findall(r'set_unavailable\(\s*"([a-z_]+\.[a-z_]+)"', text))
        names.update('lifecycle.' + n for n in re.findall(r'set_phase\([^,]+,\s*"([a-z_]+)"\)', text))
    return names


def check_events(problems):
    doc = open(os.path.join(ROOT, 'docs', 'events.md'), encoding='utf-8').read()
    names = emitted_events()
    if len(names) < 20:
        problems.append(f'events: only {len(names)} event names found in the loader source; the extractor is broken')
    for n in sorted(names):
        if f'`{n}`' not in doc:
            problems.append(f'events: `{n}` is emitted by the loader but missing from docs/events.md')
    return len(names)


def pages(root, folder, recursive=True):
    """Markdown files under root/folder, as (repository-relative name, path), sorted."""
    out = []
    for d, dirs, files in os.walk(os.path.join(root, folder)):
        dirs.sort()
        out += [os.path.join(d, f) for f in files if f.endswith('.md')]
        if not recursive:
            break
    return sorted((os.path.relpath(p, root).replace(os.sep, '/'), p) for p in out)


def check_samples(problems, root=ROOT):
    count = 0
    pat = re.compile(r'<!-- sample: (\S+) -->\n```[\w-]*\n(.*?)```', re.S)
    for f, path in pages(root, 'docs'):
        text = open(path, encoding='utf-8').read()
        for rel, body in pat.findall(text):
            count += 1
            src = os.path.join(root, rel)
            if not os.path.isfile(src):
                problems.append(f'{f}: sample {rel} does not exist')
            elif open(src, encoding='utf-8').read() != body:
                problems.append(f'{f}: sample block differs from {rel} (re-copy the file into the doc)')
    return count


def check_links(problems, root=ROOT):
    count = 0
    for f, path in pages(root, 'docs') + pages(root, os.path.join('mapping', 'atlas'), recursive=False):
        text = open(path, encoding='utf-8').read()
        text = re.sub(r'```.*?```', '', text, flags=re.S)   # code is not prose: ignore its brackets
        text = re.sub(r'`[^`\n]*`', '', text)
        for target in re.findall(r'\]\(([^)#:]+)(?:#[^)]*)?\)', text):
            count += 1
            if not os.path.exists(os.path.normpath(os.path.join(os.path.dirname(path), target))):
                problems.append(f'{f}: broken link to {target}')
    return count


def game_members(header):
    """The KyubuApi members after the game marker, up to the next section (the mod options) or the table's end, and the members of every later section marked the same way; None
    without a marker."""
    start = header.find(GAME_MARKER)
    if start < 0:
        return None
    end_of_table = header.find('} KyubuApi;', start)
    found = []
    while 0 <= start < end_of_table:
        end = end_of_table
        section = header.find('/* ---- ', start + len(GAME_MARKER))
        if 0 <= section < end:
            end = section
        found += re.findall(r'\(\s*KYUBU_CALL\s*\*\s*(\w+)\s*\)', header[start:end])
        start = header.find(GAME_MARKER, end)   # a later section marked the same way (abilities, add-ability-api 6.1)
    return found


def check_game_api(problems, root=ROOT):
    def read(*rel):
        path = os.path.join(root, *rel)
        return open(path, encoding='utf-8').read() if os.path.isfile(path) else None

    header = read('sdk', 'include', 'kyubu_api.h')
    entries = read('loader', 'src', 'game', 'entries.inc')
    doc = read('docs', 'game-api.md')
    if doc is None:
        problems.append('game API: docs/game-api.md is missing')
        return 0
    members = game_members(header or '')
    if members is None:
        problems.append('game API: kyubu_api.h has no game marker ("' + GAME_MARKER + '")')
        members = []
    entries = re.sub(r'/\*.*?\*/', '', entries or '', flags=re.S)   # the list's own documentation
    prims = re.findall(r'^\s*GAME_PRIMITIVE\(\s*(\w+)\s*,', entries, re.M)
    if not members or not prims:
        problems.append('game API: no game-API members or primitives found; the extractor is broken')
    for m in members:
        entry = re.search(r'^### `' + re.escape(m) + r'`[ \t]*\n(.*?)(?=^##|\Z)', doc, re.M | re.S)
        if not entry:
            problems.append(f'docs/game-api.md: game-API member `{m}` has no entry (a "### `{m}`" heading)')
            continue
        for line in ('**Side:**', '**Errors:**'):
            if line not in entry.group(1):
                problems.append(f'docs/game-api.md: the entry of `{m}` has no {line} line')
    for p in prims:
        if not re.search(r'^\|\s*`' + re.escape(p) + r'`\s*\|', doc, re.M):
            problems.append(f'docs/game-api.md: primitive `{p}` has no row in the primitives table')
    return len(members)


def main():
    problems = []
    n_events = check_events(problems)
    n_samples = check_samples(problems)
    n_links = check_links(problems)
    n_game = check_game_api(problems)
    for p in problems:
        print('FAIL ' + p)
    print(f'{n_events} event names, {n_samples} sample blocks, {n_links} links, {n_game} game-API functions checked: '
          + ('ok' if not problems else f'{len(problems)} problem(s)'))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
