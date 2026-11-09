"""mapping/exported-names.lock: every exported name that shipped in a release (mapping-exports, "Name stability").

Mods pass these names to resolve_symbol or compile against them, so each one has to keep resolving:

  kmap check                 fails when a locked name is no longer the name or an alias of an exported entry
  kmap lock                  lists what the next release adds to the lock (and what it would drop)
  kmap lock --release [ver]  adds every exported name that isn't locked yet, as shipped in `ver` (default: the
                             project version in CMakeLists.txt); scripts/package.py --release refuses to package a
                             release whose names aren't all locked for its version

A locked name is dropped only when KYUBU_API_VERSION_MAJOR (sdk/include/kyubu_api.h) is higher than when the lock
was last updated: mods built for the old major version no longer load, so nothing still uses the name.

Sections: [symbols.<build>] function and global names (the symbol maps), [types] types and enums, [fields]
`Type.field`, [values] `Enum::VALUE`. Each name maps to the release that first shipped it.
"""
import json
import re
import tomllib

from . import db as dbmod
from .util import KmapError, say, toml_key, toml_str, write_if_changed

PATH = 'mapping/exported-names.lock'
SECTIONS = ('types', 'fields', 'values')
VERSION = re.compile(r'^\d+(\.\d+){0,3}$')
HEAD = '''\
# Exported names that shipped in a release, each with the release that first shipped it (mapping-exports, "Name
# stability"). Mods pass these names to resolve_symbol or compile against them, so kmap check fails when one is no
# longer the name or an alias of an exported entry in mapping/: a rename keeps the old name in `aliases`.
# Written by `kmap lock --release` on release. A name is dropped only when KYUBU_API_VERSION_MAJOR goes up.'''
KEY_LINE = re.compile(r'^\s*(?:"((?:[^"\\]|\\.)*)"|([A-Za-z0-9_-]+))\s*=')


class Lock:
    def __init__(self, release=None, api_major=None, names=None, lines=None):
        self.release = release
        self.api_major = api_major
        self.names = names or {}      # section -> {name: release}
        self.lines = lines or {}      # (section, name) -> line in the file


def parse(text, builds):
    """The Lock in `text`; ValueError says what is wrong with it."""
    try:
        data = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise ValueError(f'not valid TOML: {e}')
    if not isinstance(data.get('release'), str) or not VERSION.match(data['release']):
        raise ValueError('`release` must be the version that last updated the lock, like "1.0.1"')
    if not isinstance(data.get('api_major'), int) or isinstance(data['api_major'], bool):
        raise ValueError('`api_major` must be the KYUBU_API_VERSION_MAJOR that last updated the lock')
    names = {}
    for key, value in data.items():
        if key in ('release', 'api_major'):
            continue
        if key not in ('symbols',) + SECTIONS:
            raise ValueError(f'unknown key `{key}` (sections: [symbols.<build>], {", ".join(f"[{s}]" for s in SECTIONS)})')
        tables = value.items() if key == 'symbols' and isinstance(value, dict) else [(None, value)]
        for build, table in tables:
            section = f'symbols.{build}' if build else key
            if build and build not in builds:
                raise ValueError(f'[{section}]: {build} is not a build in mapping/workspace.toml')
            if not isinstance(table, dict) or not all(isinstance(v, str) and VERSION.match(v) for v in table.values()):
                raise ValueError(f'[{section}] must map each name to the release that shipped it, like "1.0.1"')
            names[section] = dict(table)
    lines, section = {}, None
    for n, line in enumerate(text.splitlines(), 1):
        head = re.match(r'^\s*\[([^\[\]]+)\]\s*$', line)
        if head:
            section = head.group(1).strip()
            continue
        m = KEY_LINE.match(line)
        if m and section:
            lines[(section, json.loads(f'"{m.group(1)}"') if m.group(1) is not None else m.group(2))] = n
    return Lock(data['release'], data['api_major'], names, lines)


def load(ctx, builds):
    """The committed lock, or None when there is none yet; ValueError when it can't be read."""
    p = ctx.repo / PATH
    if not p.is_file():
        return None
    return parse(p.read_text(encoding='utf-8'), builds)


def render(lock, builds):
    out = [HEAD, f'release = {toml_str(lock.release)}', f'api_major = {lock.api_major}']
    for section in [f'symbols.{b}' for b in builds] + list(SECTIONS):
        names = lock.names.get(section)
        if names:
            out += ['', f'[{section}]'] + [f'{toml_key(n)} = {toml_str(v)}' for n, v in sorted(names.items())]
    return '\n'.join(out) + '\n'


# --- what the exports carry ---------------------------------------------------------------------------------

def _names(e):
    return [n for n in [e.get('name')] + list(e.get('aliases', [])) if isinstance(n, str)]


def exported_names(db, keep=dbmod.exported, owners='canonical'):
    """{section: set of names} that the exports carry, aliases included. Fields and values are named after their
    type's name; with owners='all', also after each of its aliases (what still resolves after a type rename)."""
    out = {f'symbols.{b}': set() for b in db.ws.builds}
    out.update((s, set()) for s in SECTIONS)
    for b in db.ws.builds:
        for e in db.symbols(b):
            if keep(e):
                out[f'symbols.{b}'].update(_names(e))
    for t in db.types + db.enums:
        if not keep(t):
            continue
        out['types'].update(_names(t))
        heads = _names(t) if owners == 'all' else [t.get('name')]
        for f in t.get('field', []):
            if keep(f):
                out['fields'].update(f'{h}.{n}' for h in heads for n in _names(f))
        for v in t.get('value', []):
            if keep({**v, 'status': v.get('status', t.get('status'))}):
                out['values'].update(f'{h}::{n}' for h in heads for n in _names(v))
    return out


def pending(db, lock):
    """(added, dropped) as {section: sorted names}: exported names that aren't locked yet, and locked names that no
    longer resolve."""
    now, have = exported_names(db), exported_names(db, owners='all')
    added = {s: sorted(names - set(lock.names.get(s, {}))) for s, names in now.items()}
    dropped = {s: sorted(n for n in names if n not in have.get(s, ())) for s, names in lock.names.items()}
    return {s: n for s, n in added.items() if n}, {s: n for s, n in dropped.items() if n}


def _what(section):
    return {'types': 'type', 'fields': 'field', 'values': 'enum value'}.get(section, f'symbol of {section[8:]}')


def check_lock(ctx, db, problems, *args):
    try:
        lock = load(ctx, db.ws.builds)
    except ValueError as e:
        problems.append(dbmod.Problem(PATH, str(e)))
        return
    if lock is None:
        return
    have = exported_names(db, owners='all')
    anywhere = exported_names(db, keep=lambda e: True, owners='all')
    for section in sorted(lock.names):
        for name, since in sorted(lock.names[section].items()):
            if name in have[section]:
                continue
            gone = ('is only a guess now, so it is no longer exported' if name in anywhere[section]
                    else 'is no longer the name or an alias of anything in mapping/')
            old = name.rpartition('.' if section == 'fields' else '::')[2] if section in ('fields', 'values') else name
            problems.append(dbmod.Problem(
                f'{PATH}:{lock.lines.get((section, name), 1)}',
                f'locked {_what(section)} {name} (shipped in {since}) {gone}. Mods use it: keep it as an alias of the '
                f'entry that replaced it (aliases = [{toml_str(old)}]); locked names are removed only with a major API '
                'version'))


# --- on release ---------------------------------------------------------------------------------------------

def project_version(ctx):
    p = ctx.repo / 'CMakeLists.txt'
    m = re.search(r'project\(kyubu VERSION ([0-9.]+)', p.read_text(encoding='utf-8')) if p.is_file() else None
    if not m:
        raise KmapError('no `project(kyubu VERSION ...)` in CMakeLists.txt: give the version (kmap lock --release 1.2.0)')
    return m.group(1)


def api_major(ctx):
    p = ctx.repo / 'sdk' / 'include' / 'kyubu_api.h'
    m = re.search(r'#define\s+KYUBU_API_VERSION_MAJOR\s+(\d+)', p.read_text(encoding='utf-8')) if p.is_file() else None
    if not m:
        raise KmapError('KYUBU_API_VERSION_MAJOR not found in sdk/include/kyubu_api.h')
    return int(m.group(1))


def _version(v):
    return tuple(int(x) for x in v.split('.'))


def _count(d):
    return sum(len(v) for v in d.values())


def _load_or_fail(ctx, db):
    try:
        return load(ctx, db.ws.builds) or Lock()
    except ValueError as e:
        raise KmapError(f'{PATH}: {e}')


def release_problems(ctx, version):
    """Why the lock isn't ready for releasing `version`, or [] (scripts/package.py --release)."""
    db = dbmod.load(ctx)
    lock = _load_or_fail(ctx, db)
    out = []
    if lock.release != version:
        out.append(f'{PATH} was last updated for {lock.release or "no release"}, not {version}')
    added, dropped = pending(db, lock)
    if added:
        out.append(f'{_count(added)} exported name(s) are not locked yet (`kmap lock` lists them)')
    if dropped:
        out.append(f'{_count(dropped)} locked name(s) no longer resolve (`kmap check` lists them)')
    if out:
        out.append(f'run `kmap lock --release {version}` and commit {PATH}')
    return out


def cmd_lock(ctx, release=None):
    """release: None (list only), True (the project version) or a version string."""
    from . import rules
    db = dbmod.load(ctx)
    problems = list(db.problems)
    for check in rules.ALL_CHECKS:
        check(db, problems)
    if problems:
        for p in problems:
            print(f'FAIL {p}')
        raise KmapError(f'{len(problems)} problem(s) in mapping/: fix them first')
    lock = _load_or_fail(ctx, db)
    added, dropped = pending(db, lock)
    if release is None:
        for sign, found in (('+', added), ('-', dropped)):
            for section, names in found.items():
                for n in names:
                    say(f'{sign} [{section}] {n}')
        say(f'kmap lock: {_count(added)} exported name(s) not locked yet, {_count(dropped)} locked name(s) gone'
            + (f' (last updated for {lock.release})' if lock.release else ' (no lock yet)')
            + '; `kmap lock --release` updates it')
        return 0
    version = project_version(ctx) if release is True else release
    if not VERSION.match(version):
        raise KmapError(f'{version!r} is not a version like 1.2.0')
    if lock.release and _version(version) < _version(lock.release):
        raise KmapError(f'the lock was last updated for {lock.release}, which is newer than {version}')
    major = api_major(ctx)
    if dropped:
        if lock.api_major is not None and major <= lock.api_major:
            names = ', '.join(n for found in dropped.values() for n in found)
            raise KmapError(f'{_count(dropped)} locked name(s) no longer resolve: {names}. They can be dropped only '
                            f'with a new major API version (KYUBU_API_VERSION_MAJOR is {major}, as when the lock was '
                            'last updated): keep them as aliases instead')
        for section, names in dropped.items():
            for n in names:
                del lock.names[section][n]
    for section, names in added.items():
        lock.names.setdefault(section, {}).update((n, version) for n in names)
    lock.release, lock.api_major = version, major
    changed = write_if_changed(ctx.repo / PATH, render(lock, db.ws.builds))
    say(f'kmap lock: {PATH} for {version}: {_count(added)} name(s) added, {_count(dropped)} dropped (API version '
        f'{major})' + ('' if changed else ', unchanged'))
    return 0
