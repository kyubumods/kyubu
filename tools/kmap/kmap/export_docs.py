"""kmap export, reference docs: docs/game/ for mod authors (mapping-exports, "Reference documentation").

  README.md          index: builds, subsystems, every type page, the status levels
  <subsystem>.md     one page per subsystem of workspace.toml: its types, functions and globals
  types/<name>.md    one page per exported type, enum and RTTI class: fields (offset, type, status), methods
                     (prototype, addresses, status), RTTI and vtables
  probes.md          the recorded probes and what each one confirms

Every entry shows the probes that confirm it: evidence `probe:<id>`, or a probe's [[confirms]] naming it (or one of
its aliases). Library classes (statically linked third-party code, no atlas page) are listed on library.md instead
of getting a page each. Pages only link to each other and to docs/, so they work in the released docs/ folder
without mapping/. Atlas pages (mapping/atlas/) link here, and tools/check_docs.py checks both.
"""
import hashlib
import posixpath
import re

from . import db as dbmod
from .export import GENERATED_NOTE
from .export_headers import Headers, ident
from .rules import _split, is_library, parse_type

DIR = 'docs/game'
MAX_SLUG = 60


def slug(name):
    s = re.sub(r'[^A-Za-z0-9_]+', '_', name.replace('::', '_')).strip('_') or 'type'
    if len(s) > MAX_SLUG:
        s = s[:MAX_SLUG - 12].rstrip('_') + '_' + hashlib.sha1(name.encode()).hexdigest()[:10]
    return s


def code(s):
    """Inline code that survives a table cell (backticks and pipes in names are rare, but possible)."""
    s = str(s).replace('|', '\\|')
    return f'`` {s} ``' if '`' in s else f'`{s}`'


def text(s):
    return ' '.join(str(s).split()).replace('|', '\\|')


def hexa(n):
    return f'0x{n:X}'


def table(head, rows):
    """A Markdown table. A column whose heading is None is left out, and so is an empty Notes column."""
    keep = [i for i, h in enumerate(head) if h is not None and (h != 'Notes' or any(r[i] for r in rows))]
    return (['| ' + ' | '.join(head[i] for i in keep) + ' |', '|' + '---|' * len(keep)] +
            ['| ' + ' | '.join(r[i] for i in keep) + ' |' for r in rows])


class Docs:
    def __init__(self, ctx, db):
        self.ctx = ctx
        self.db = db
        self.builds = list(ctx.ws.builds)
        self.label = {b: ctx.ws.build(b).file for b in self.builds}
        self.subsystems = list(ctx.ws.subsystems)
        self.types = {t['name']: t for t in db.types if dbmod.exported(t)}
        self.enums = {e['name']: e for e in db.enums if dbmod.exported(e)}
        self.classes = {}     # name -> {build: entry}
        for b in self.builds:
            for c in db.classes(b):
                if dbmod.exported(c):
                    self.classes.setdefault(c['name'], {})[b] = c
        self.symbols = {}     # (kind, name) -> {build: entry}; kind is function or global
        for b in self.builds:
            for e in db.symbols(b):
                if dbmod.exported(e):
                    self.symbols.setdefault((e['_kind'], e['name']), {})[b] = e
        headers = Headers(ctx, db)
        headers.functions()                   # collects the classes that prototypes point to
        self.opaque = headers.opaque
        self.pages = self._pages()            # mapped name -> path relative to docs/game
        self.by_address = {}                  # (build, address) -> exported function name, for vtable slots
        for (kind, name), ents in self.symbols.items():
            for b, e in ents.items():
                if kind == 'function' and e.get('kind', 'function') == 'function':
                    self.by_address.setdefault((b, e['address']), name)
        self.confirmed = self._confirmations()

    # --- pages and links ---------------------------------------------------------------------------------
    def _pages(self):
        names = set(self.types) | set(self.enums) | {n for n, ents in self.classes.items()
                                                     if not any(is_library(c) for c in ents.values())}
        slugs = {n: slug(n) for n in names}
        seen = {}
        for n, s in slugs.items():
            seen.setdefault(s.lower(), []).append(n)      # case-insensitive file systems
        for s, ns in seen.items():
            if len(ns) > 1:
                for n in ns:
                    slugs[n] = slug(n)[:MAX_SLUG - 11].rstrip('_') + '_' + hashlib.sha1(n.encode()).hexdigest()[:10]
        return {n: f'types/{s}.md' for n, s in sorted(slugs.items())}

    def page_of(self, name):
        """The page name a mapped name (or alias) refers to, or None."""
        if name in self.pages:
            return name
        for find in (self.db.find_type, self.db.find_enum):
            e = find(name)
            if e is not None and e['name'] in self.pages:
                return e['name']
        return None

    def subsystem_of(self, name):
        """Subsystem of a type page (its own, or its class's), or None."""
        t = self.types.get(name) or self.enums.get(name)
        if t is not None and t.get('subsystem'):
            return t['subsystem']
        ents = self.classes.get(name)
        return next(iter(ents.values()))['subsystem'] if ents else None

    @staticmethod
    def href(target, frm):
        return posixpath.relpath(target, posixpath.dirname(frm) or '.')

    def link(self, name, frm, shown=None):
        """A name as inline code, linked to its page when it has one (library classes: to library.md)."""
        shown = code(shown if shown is not None else name)
        page = self.page_of(name)
        if page:
            return f'[{shown}]({self.href(self.pages[page], frm)})'
        if name in self.classes:
            return f'[{shown}]({self.href(self.sub_page(self.subsystem_of(name)), frm)})'
        return shown

    def type_link(self, t, frm):
        """A field or global type, linked through its pointers and arrays to the named type's page."""
        inner = t
        while True:
            p = parse_type(inner)
            if p[0] == 'named':
                break
            inner = p[1]
        return self.link(inner, frm, shown=t) if (self.page_of(inner) or inner in self.classes) else code(t)

    @staticmethod
    def sub_page(sub):
        return f'{sub}.md'

    def owner(self, name):
        """The type page a function belongs to (cube::Database::get -> cube::Database), or None."""
        parts = _split(name)
        return self.page_of('::'.join(parts[:-1])) if len(parts) > 1 else None

    def symbol_page(self, name, sub):
        owner = self.owner(name)
        return self.pages[owner] if owner else self.sub_page(sub)

    def symbol_link(self, name, frm, shown=None):
        """An exported function, linked to the page that lists it."""
        sub = next(iter(self.symbols[('function', name)].values()))['subsystem']
        return f'[{code(shown if shown is not None else name)}]({self.href(self.symbol_page(name, sub), frm)})'

    # --- probes ------------------------------------------------------------------------------------------
    def _confirmations(self):
        """key -> sorted probe ids. Keys: ('sym', build, name), ('class', build, name), ('type', name),
        ('field', type, field), ('value', enum, value)."""
        out = {}

        def add(key, pid):
            out.setdefault(key, set()).add(pid)

        for pid, p in sorted(self.db.probes.items()):
            pb = p.get('build')
            for c in p.get('confirms', []) or []:
                if isinstance(c, dict) and isinstance(c.get('entry'), str):
                    for key in self.resolve(pb, c['entry']):
                        add(key, pid)
        for b in self.builds:
            for e in self.db.symbols(b):
                for pid in self.cited(e):
                    add(('sym', b, e['name']), pid)
            for c in self.db.classes(b):
                for pid in self.cited(c):
                    add(('class', b, c['name']), pid)
        for t in self.db.types + self.db.enums:
            for pid in self.cited(t):
                add(('type', t['name']), pid)
            for f in t.get('field', []) + t.get('value', []):
                for pid in self.cited(f):
                    add(('field' if f['_kind'] == 'field' else 'value', t['name'], f['name']), pid)
        return {k: sorted(v) for k, v in out.items()}

    @staticmethod
    def cited(e):
        return [i[6:] for i in e.get('evidence', []) or [] if isinstance(i, str) and i.startswith('probe:')]

    def resolve(self, build, name):
        """Every entry a probe's `entry` may name (a probe resolves in its own build)."""
        keys = []
        if build in self.builds:
            f = self.db.find_function(build, name)
            if f is not None:
                keys.append(('sym', build, f['name']))
            c = self.db.find_class(build, name)
            if c is not None:
                keys.append(('class', build, c['name']))
        for find in (self.db.find_type, self.db.find_enum):
            t = find(name)
            if t is not None:
                keys.append(('type', t['name']))
        parts = _split(name)
        splits = [('::'.join(parts[:-1]), parts[-1])] if len(parts) > 1 else []
        if '.' in name:
            splits.append(tuple(name.rsplit('.', 1)))
        for parent, member in splits:
            for find, sub, kind in ((self.db.find_type, 'field', 'field'), (self.db.find_enum, 'value', 'value')):
                t = find(parent)
                if t is None:
                    continue
                m = next((m for m in t.get(sub, []) if m.get('name') == member or member in m.get('aliases', [])),
                         None)
                if m is not None:
                    keys.append((kind, t['name'], m['name']))
        return keys

    def probes(self, keys, frm):
        ids = sorted({pid for k in keys for pid in self.confirmed.get(k, [])})
        return ', '.join(f'[{pid}]({self.href("probes.md", frm)}#{pid})' for pid in ids)

    # --- shared tables -----------------------------------------------------------------------------------
    def status(self, ents):
        statuses = {e['status'] for e in ents.values()}
        if len(statuses) == 1:
            return statuses.pop()
        return ', '.join(f'{self.label[b]} {e["status"]}' for b, e in ents.items())

    def per_build(self, ents, key):
        vals = {b: e.get(key) for b, e in ents.items() if e.get(key)}
        if len(set(vals.values())) <= 1:
            return list(vals.values())[:1]
        return [f'{self.label[b]}: {v}' for b, v in vals.items()]

    def symbol_rows(self, kind, names, frm, method=False):
        """Table of functions (or globals) merged by name across builds."""
        if kind == 'function':
            head = ['Method' if method else 'Function', 'Prototype']
        else:
            head = ['Global', 'Type']
        head += [self.label[b] for b in self.builds] + ['Status', 'Confirmed by', 'Notes']
        rows = []
        for name in names:
            ents = self.symbols[(kind, name)]
            first = next(iter(ents.values()))
            owner = None if method else self.owner(name)
            cell = f'[{code(name)}]({self.href(self.pages[owner], frm)})' if owner else code(name)
            if first.get('kind') == 'site':
                cell += ' (mid-hook site)'
            aliases = sorted({a for e in ents.values() for a in e.get('aliases', [])})
            if aliases:
                cell += '<br>aliases: ' + ', '.join(code(a) for a in aliases)
            if kind == 'function':
                protos = self.per_build(ents, 'prototype')
                detail = '<br>'.join(code(p) for p in protos) or '—'
            else:
                detail = '<br>'.join(self.type_link(t, frm) for t in self.per_build(ents, 'type')) or '—'
            addrs = [hexa(ents[b]['address']) if b in ents else '—' for b in self.builds]
            notes = '<br>'.join(text(n) for n in self.per_build(ents, 'notes'))
            confirmed = self.probes([('sym', b, name) for b in ents], frm)
            rows.append([cell, detail] + addrs + [self.status(ents), confirmed, notes])
        return table(head, rows)

    def type_rows(self, names, frm):
        rows = ['| Type | Kind | Size | Status |', '|---|---|---|---|']
        for name in names:
            kind, size, status = self.summary(name)
            rows.append(f'| {self.link(name, frm)} | {kind} | {size} | {status} |')
        return rows

    def summary(self, name):
        """(kind, size, status) of a type page."""
        if name in self.types:
            t = self.types[name]
            return t.get('kind', 'struct'), hexa(t['size']) if t.get('size') else '—', t['status']
        if name in self.enums:
            e = self.enums[name]
            return 'enum', f'{e["size"]}' if e.get('size') else '—', e['status']
        ents = self.classes[name]
        sizes = {c.get('size') for c in ents.values() if c.get('size')}
        return 'class (RTTI)', hexa(sizes.pop()) if len(sizes) == 1 else '—', self.status(ents)

    # --- pages -------------------------------------------------------------------------------------------
    def header(self, title, crumbs):
        return [f'<!-- {GENERATED_NOTE} -->', f'# {title}', '', crumbs, '']

    def index(self):
        frm = 'README.md'
        out = self.header('Game reference', '[Documentation](../README.md) › Game reference') + [
            'The types, functions and probes of Cube World Alpha v0.1.1 that Kyubu has mapped, generated from the '
            'mapping database (`mapping/` in the Kyubu source) by `kmap export`. Only entries at `verified` or '
            '`candidate` status are listed:',
            '',
            '| Status | Meaning |',
            '|---|---|',
            '| verified | confirmed; loader features may rely on it |',
            '| candidate | probably right, but not confirmed yet |',
            '',
            'Function and global names are what `resolve_symbol`, `kyubu.symbol` and the hooks take '
            '(see [hooks.md](../hooks.md)); addresses are at the preferred image base 0x400000. The C types are in '
            '`sdk/include/cubeworld/` and the Lua types in `require("cubeworld")` (see [lua.md](../lua.md)).',
            '',
            '## Builds',
            '',
            '| Build | File | SHA-256 |',
            '|---|---|---|',
        ]
        for b in self.builds:
            out.append(f'| `{b}` | {self.label[b]} | `{self.ctx.ws.build(b).sha256}` |')
        out += ['', '## Subsystems', '', '| Subsystem | Types | Functions | Globals |', '|---|---|---|---|']
        for sub in self.subsystems:
            types = len(self.types_of(sub))
            functions = len(self.symbols_of('function', sub))
            globals_ = len(self.symbols_of('global', sub))
            out.append(f'| [{sub}]({self.sub_page(sub)}) | {types} | {functions} | {globals_} |')
        out += ['', '## Types', '', 'Every type with a page. Types without a subsystem are listed only here.', ''] + \
            self.type_rows(sorted(self.pages), frm) + [
                '', '## Probes', '',
                f'[probes.md](probes.md): the {len(self.db.probes)} recorded probe(s) and what each one confirms.', '']
        return '\n'.join(out)

    def types_of(self, sub):
        """Type pages of a subsystem, and for library its classes (which have no page of their own)."""
        return sorted(n for n in set(self.pages) | set(self.classes) if self.subsystem_of(n) == sub)

    def symbols_of(self, kind, sub):
        return sorted(name for (k, name), ents in self.symbols.items()
                      if k == kind and next(iter(ents.values()))['subsystem'] == sub)

    def subsystem(self, sub):
        frm = self.sub_page(sub)
        out = self.header(f'Subsystem: {sub}', f'[Game reference](README.md) › {sub}')
        types = self.types_of(sub)
        functions = self.symbols_of('function', sub)
        globals_ = self.symbols_of('global', sub)
        if not (types or functions or globals_):
            out += ['Nothing in this subsystem is mapped at `candidate` or better yet.', '']
        paged = [n for n in types if n in self.pages]
        listed = [n for n in types if n not in self.pages]
        if paged:
            out += ['## Types', ''] + self.type_rows(paged, frm) + ['']
        if listed:
            out += ['## Library classes', '',
                    'RTTI classes of statically linked third-party code. They have no page of their own.', '',
                    '| Class | Builds | Bases | Status |', '|---|---|---|---|']
            for n in listed:
                ents = self.classes[n]
                bases = sorted({b for c in ents.values() for b in c.get('bases', [])})
                out.append(f'| {code(n)} | {", ".join(self.label[b] for b in ents)} | '
                           f'{", ".join(code(b) for b in bases) or "—"} | {self.status(ents)} |')
            out.append('')
        if functions:
            out += ['## Functions', '']
            owned = [n for n in functions if self.owner(n)]
            if owned:
                out += ['Methods of a type with a page are also listed there.', '']
            out += self.symbol_rows('function', functions, frm) + ['']
        if globals_:
            out += ['## Globals', ''] + self.symbol_rows('global', globals_, frm) + ['']
        return '\n'.join(out)

    def type_page(self, name):
        frm = self.pages[name]
        sub = self.subsystem_of(name)
        crumbs = '[Game reference](../README.md) › ' + (f'[{sub}](../{sub}.md) › ' if sub else '') + code(name)
        out = self.header(code(name), crumbs)
        t = self.types.get(name)
        en = self.enums.get(name)
        ents = self.classes.get(name, {})
        kind, size, status = self.summary(name)
        facts = [f'- **Kind:** {kind}' + (f', {size} bytes' if size != '—' and kind != 'enum' else '') +
                 (f', {size}-byte values' if kind == 'enum' and size != '—' else '')]
        main = t or en
        if ents and main is None:
            facts.append(f'- **Status:** {self.status(ents)} (source: RTTI)')
        if main is not None:
            facts.append(f'- **Status:** {main["status"]} (source: {text(main.get("source", "?"))})')
            if main.get('base'):
                facts.append(f'- **Base:** {self.link(main["base"], frm)}')
            if main.get('aliases'):
                facts.append('- **Aliases:** ' + ', '.join(code(a) for a in main['aliases']))
            if main.get('builds'):
                facts.append('- **Builds:** ' + ', '.join(self.label.get(b, b) for b in main['builds']) + ' only')
            facts.append(f'- **C:** {code(ident(name))} in `cubeworld/types.h`')
            facts.append('- **Lua:** ' + (code(f'cubeworld.cast("{ident(name)}", address)') if t else
                                          code(f'cubeworld.enums["{name}"]')))
            if main.get('evidence'):
                facts.append('- **Evidence:** ' + ', '.join(code(e) for e in main['evidence']))
        elif name in self.opaque:
            facts.append(f'- **C:** {code(ident(name))} in `cubeworld/types.h` (opaque: for pointers only)')
        keys = [('type', name)] + [('class', b, name) for b in ents]
        confirmed = self.probes(keys, frm)
        if confirmed:
            facts.append(f'- **Confirmed by:** {confirmed}')
        out += facts + ['']
        if main is not None and main.get('notes'):
            out += [text(main['notes']), '']
        if t is not None:
            out += ['## Fields', '']
            fields = sorted((f for f in t.get('field', []) if dbmod.exported(f)), key=lambda f: (f['offset'], f['name']))
            if fields:
                rows = []
                for f in fields:
                    cell = code(f['name'])
                    if f.get('aliases'):
                        cell += '<br>aliases: ' + ', '.join(code(a) for a in f['aliases'])
                    notes = text(f.get('notes', ''))
                    if f.get('builds'):
                        notes = (', '.join(self.label.get(b, b) for b in f['builds']) + ' only. ' + notes).strip()
                    rows.append([hexa(f['offset']), cell, self.type_link(f['type'], frm), f['status'],
                                 self.probes([('field', name, f['name'])], frm), notes])
                out += table(['Offset', 'Name', 'Type', 'Status', 'Confirmed by', 'Notes'], rows)
            else:
                out.append('No fields are mapped at `candidate` or better yet.')
            out.append('')
        if en is not None:
            rows = [[code(v['name']), str(v['value']), v.get('status', en['status']),
                     self.probes([('value', name, v['name'])], frm), text(v.get('notes', ''))]
                    for v in sorted(Headers.enum_values(en), key=lambda v: (v['value'], v['name']))]
            own = any('status' in v for v in en.get('value', []))
            out += ['## Values', ''] + table(['Name', 'Value', 'Status' if own else None, 'Confirmed by', 'Notes'],
                                             rows) + ['']
        methods = sorted(n for (k, n) in self.symbols if k == 'function' and self.owner(n) == name)
        if methods or t is not None or ents:
            out += ['## Methods', '']
            out += self.symbol_rows('function', methods, frm, method=True) if methods else \
                ['No methods are mapped at `candidate` or better yet.']
            out.append('')
        if ents:
            out += ['## RTTI', '', '| Build | RTTI name | Type descriptor | Bases |', '|---|---|---|---|']
            for b, c in ents.items():
                bases = ', '.join(self.link(x, frm) for x in c.get('bases', [])) or '—'
                td = hexa(c['type_descriptor']) if c.get('type_descriptor') else '—'
                out.append(f'| {self.label[b]} | {code(c.get("rtti", "?"))} | {td} | {bases} |')
            out.append('')
            vt = [(b, v) for b, c in ents.items() for v in c.get('vtable', [])]
            if vt:
                out += ['### Vtables', '', 'Slots are listed where the function is mapped.', '',
                        '| Build | Address | Sub-object offset | Slots | Mapped slots |', '|---|---|---|---|---|']
                for b, v in vt:
                    slots = v.get('slots', [])
                    named = [f'{i}: {self.symbol_link(self.by_address[(b, a)], frm)}' for i, a in enumerate(slots)
                             if (b, a) in self.by_address]
                    out.append(f'| {self.label[b]} | {hexa(v["address"])} | {hexa(v.get("offset", 0))} | {len(slots)} | '
                               f'{"<br>".join(named) or "—"} |')
                out.append('')
        return '\n'.join(out)

    def probes_page(self):
        frm = 'probes.md'
        out = self.header('Probes', '[Game reference](README.md) › Probes') + [
            'Runs of the real game that confirm mapped entries. The source files are `mapping/probes/<id>.toml` in the '
            'Kyubu source; entries cite them as evidence `probe:<id>`.', '']
        if not self.db.probes:
            out += ['No probes are recorded yet.', '']
        for pid, p in sorted(self.db.probes.items()):
            build = p.get('build')
            out += [f'## {pid}', '',
                    f'- **Build:** {self.label.get(build, build)} (`{build}`)',
                    f'- **Kyubu:** {text(p.get("kyubu", "?"))}']
            if p.get('date'):
                out.append(f'- **Date:** {p["date"]}')
            out.append('')
            if p.get('steps'):
                out += ['Steps:', ''] + [f'{i}. {text(s)}' for i, s in enumerate(p['steps'], 1)] + ['']
            if p.get('observations'):
                out += ['Observations:', ''] + [f'- {text(s)}' for s in p['observations']] + ['']
            rows = []
            for c in p.get('confirms', []) or []:
                target = self.confirm_target(build, c.get('entry', ''), frm)
                if target:
                    rows.append(f'| {target} | {text(c.get("observed", ""))} |')
            if rows:
                out += ['| Confirms | Observed |', '|---|---|'] + rows + ['']
        return '\n'.join(out)

    def confirm_target(self, build, name, frm):
        """The confirmed entry linked to the page that lists it, or None when it is not exported."""
        for key in self.resolve(build, name):
            if key[0] == 'sym' and ('function', key[2]) in self.symbols:
                return self.symbol_link(key[2], frm, shown=name)
            if key[0] == 'sym' and ('global', key[2]) in self.symbols:
                ents = self.symbols[('global', key[2])]
                return f'[{code(name)}]({self.href(self.sub_page(next(iter(ents.values()))["subsystem"]), frm)})'
            if key[0] == 'class' and key[2] in self.classes:
                return self.link(key[2], frm, shown=name)
            if key[0] in ('type', 'field', 'value') and self.page_of(key[1]):
                parent = self.types.get(key[1]) or self.enums.get(key[1])
                if key[0] != 'type':
                    sub = 'field' if key[0] == 'field' else 'value'
                    m = next(m for m in parent.get(sub, []) if m['name'] == key[2])
                    if not (dbmod.exported(m) if key[0] == 'field' else m in Headers.enum_values(parent)):
                        continue
                return self.link(key[1], frm, shown=name)
        return None

    def generate(self):
        out = {f'{DIR}/README.md': self.index(), f'{DIR}/probes.md': self.probes_page()}
        for sub in self.subsystems:
            out[f'{DIR}/{self.sub_page(sub)}'] = self.subsystem(sub)
        for name, rel in self.pages.items():
            out[f'{DIR}/{rel}'] = self.type_page(name)
        return {k: v if v.endswith('\n') else v + '\n' for k, v in out.items()}


def generate(ctx, db):
    return Docs(ctx, db).generate()
