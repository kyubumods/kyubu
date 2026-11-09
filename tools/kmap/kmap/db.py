"""The mapping database (mapping/): loading, with the file and line of every entry kept for messages.

Entries are plain dicts from the TOML files plus private keys: _file (repository-relative path), _line
(line of the entry's `name =`), _kind (function, global, class, type, field, enum, value, probe) and
_build (the build id, or None for shared types).
"""
import re
import tomllib
from pathlib import Path

HEADER = re.compile(r'^\s*\[\[\s*([A-Za-z0-9_.]+)\s*\]\]')
NAME_LINE = re.compile(r'^\s*name\s*=')

STATUSES = ('verified', 'candidate', 'guess')


class Problem:
    def __init__(self, where, message, entry=None):
        self.where = where
        self.message = message
        self.entry = entry

    def __str__(self):
        return f'{self.where}: {self.message}'


def where(e):
    return f'{e.get("_file", "?")}:{e.get("_line", "?")}'


def label(e):
    kind = e.get('_kind', 'entry')
    if kind == 'field':
        return f'field {e.get("_parent", "?")}.{e.get("name", "?")}'
    if kind == 'value':
        return f'enum value {e.get("_parent", "?")}::{e.get("name", "?")}'
    return f'{kind} {e.get("name", "?")}'


class _Lines:
    """Line numbers of each [[header]] in file order, and of the `name =` line that follows it."""

    def __init__(self, text):
        self.pos = {}
        lines = text.splitlines()
        heads = [(i, HEADER.match(l).group(1)) for i, l in enumerate(lines) if HEADER.match(l)]
        for n, (i, h) in enumerate(heads):
            end = heads[n + 1][0] if n + 1 < len(heads) else len(lines)
            line = next((j for j in range(i + 1, end) if NAME_LINE.match(lines[j])), i)
            self.pos.setdefault(h, []).append(line + 1)
        self.used = {}

    def next(self, header):
        k = self.used.get(header, 0)
        self.used[header] = k + 1
        lst = self.pos.get(header, [])
        return lst[k] if k < len(lst) else None


class Database:
    def __init__(self, ctx):
        self.ctx = ctx
        self.root = ctx.mapping
        self.ws = ctx.ws
        self.problems = []
        self._functions = {}
        self._globals = {}
        self._classes = {}
        self.types = []
        self.enums = []
        self.probes = {}
        self.atlas = {}
        self.files = []

    # --- loading ---------------------------------------------------------------------------------
    def rel(self, path):
        return Path(path).relative_to(self.ctx.repo).as_posix()

    def _read(self, path):
        text = Path(path).read_text(encoding='utf-8')
        self.files.append(path)
        try:
            return tomllib.loads(text), _Lines(text)
        except tomllib.TOMLDecodeError as e:
            self.problems.append(Problem(self.rel(path), f'not valid TOML: {e}'))
            return None, None

    def _entries(self, path, data, lines, key, kind, build, subkeys=()):
        out = []
        items = data.get(key, [])
        if not isinstance(items, list) or not all(isinstance(x, dict) for x in items):
            self.problems.append(Problem(self.rel(path), f'`{key}` must be an array of tables ([[{key}]])'))
            return out
        for item in items:
            e = dict(item)
            e.update(_file=self.rel(path), _line=lines.next(key), _kind=kind, _build=build)
            for sub, subkind in subkeys:
                children = e.get(sub, [])
                if not isinstance(children, list):
                    continue
                e[sub] = []
                for c in children:
                    if not isinstance(c, dict):
                        continue
                    c = dict(c)
                    c.update(_file=self.rel(path), _line=lines.next(f'{key}.{sub}'), _kind=subkind,
                             _build=build, _parent=e.get('name'))
                    e[sub].append(c)
            out.append(e)
        return out

    def load(self):
        root = self.root
        for bid in self.ws.builds:
            self._functions[bid] = []
            for p in sorted((root / bid / 'functions').glob('*.toml')):
                data, lines = self._read(p)
                if data is not None:
                    self._check_toplevel(p, data, {'function'})
                    self._functions[bid] += self._entries(p, data, lines, 'function', 'function', bid)
            self._globals[bid] = []
            p = root / bid / 'globals.toml'
            if p.is_file():
                data, lines = self._read(p)
                if data is not None:
                    self._check_toplevel(p, data, {'global'})
                    self._globals[bid] = self._entries(p, data, lines, 'global', 'global', bid)
            self._classes[bid] = []
            p = root / bid / 'classes.toml'
            if p.is_file():
                data, lines = self._read(p)
                if data is not None:
                    self._check_toplevel(p, data, {'class'})
                    self._classes[bid] = self._entries(p, data, lines, 'class', 'class', bid,
                                                       subkeys=(('vtable', 'vtable'),))
        for p in sorted((root / 'types').glob('*.toml')):
            data, lines = self._read(p)
            if data is None:
                continue
            self._check_toplevel(p, data, {'type', 'enum'})
            self.types += self._entries(p, data, lines, 'type', 'type', None, subkeys=(('field', 'field'),))
            self.enums += self._entries(p, data, lines, 'enum', 'enum', None, subkeys=(('value', 'value'),))
        for p in sorted((root / 'probes').glob('*.toml')):
            data, lines = self._read(p)
            if data is not None:
                data['_file'] = self.rel(p)
                data['_line'] = 1
                data['_kind'] = 'probe'
                self.probes[p.stem] = data
        for p in sorted((root / 'atlas').glob('*.md')):
            if not p.name.startswith('_'):          # _template.md is the page template, not a page
                self.atlas[p.stem] = p
        return self

    def _check_toplevel(self, path, data, allowed):
        for k in data:
            if k not in allowed:
                self.problems.append(Problem(self.rel(path), f'unexpected top-level key `{k}` '
                                             f'(this file holds {", ".join("[[" + a + "]]" for a in sorted(allowed))})'))

    # --- queries -------------------------------------------------------------------------------------
    def functions(self, bid):
        return self._functions.get(bid, [])

    def globals(self, bid):
        return self._globals.get(bid, [])

    def classes(self, bid):
        return self._classes.get(bid, [])

    def symbols(self, bid):
        """Functions and globals of a build (what symbol maps export)."""
        return self.functions(bid) + self.globals(bid)

    def all_entries(self):
        for bid in self.ws.builds:
            yield from self.functions(bid)
            yield from self.globals(bid)
            for c in self.classes(bid):
                yield c
        for t in self.types:
            yield t
            yield from t.get('field', [])
        for en in self.enums:
            yield en
            yield from en.get('value', [])

    def find_function(self, bid, name):
        for f in self.symbols(bid):
            if f.get('name') == name or name in f.get('aliases', []):
                return f
        return None

    def find_class(self, bid, name):
        return next((c for c in self.classes(bid) if c.get('name') == name), None)

    def find_type(self, name):
        return next((t for t in self.types if t.get('name') == name or name in t.get('aliases', [])), None)

    def find_enum(self, name):
        return next((e for e in self.enums if e.get('name') == name or name in e.get('aliases', [])), None)

    def names(self):
        """Every mapped name and alias (functions, globals, classes, types, enums; fields as Type::field)."""
        out = set()
        for e in self.all_entries():
            kind = e.get('_kind')
            if kind in ('field', 'value'):
                out.add(f'{e.get("_parent")}::{e.get("name")}')
                out.add(f'{e.get("_parent")}.{e.get("name")}')
                for a in e.get('aliases', []):
                    out.add(f'{e.get("_parent")}::{a}')
                    out.add(f'{e.get("_parent")}.{a}')
            else:
                out.add(e.get('name'))
                out.update(e.get('aliases', []))
        out.discard(None)
        return out


def load(ctx):
    return Database(ctx).load()


def exported(e):
    """Entries at candidate or better are exported; guesses stay in Ghidra projects."""
    return e.get('status') in ('verified', 'candidate')


def public(d):
    return {k: v for k, v in d.items() if not k.startswith('_')}
