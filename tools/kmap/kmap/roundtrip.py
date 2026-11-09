"""kmap push and kmap pull: the round trip between mapping/ and the private Ghidra project.

push applies every entry (guesses included) and records, per entry, what it asked for and what the project
showed afterwards in $KMAP_HOME/ghidra/pushed.json. pull compares the project with that record: a change
made in Ghidra becomes a proposed edit of mapping/, shown as a diff and written only with --write. It never
raises a status or adds evidence, and an entry changed on both sides is reported as a conflict."""
import difflib
import json
import re

from . import db as dbmod
from . import ghidra, rules
from .proto import parse, render
from .util import KmapError, dump_json, say, toml_str, warn, write_if_changed

EXPORTED = ('verified', 'candidate')


def comment(e):
    lines = [f'kyubu: {e.get("status")} ({e.get("source")})']
    if e.get('evidence'):
        lines.append('evidence: ' + '; '.join(e['evidence']))
    if e.get('aliases'):
        lines.append('aliases: ' + ', '.join(e['aliases']))
    if e.get('notes'):
        lines.append(e['notes'])
    return '\n'.join(lines)


def applies(e, bid):
    return not e.get('builds') or bid in e['builds']


def desired(db, bid, keep=None):
    """What push applies to one build: every entry (guesses included), or only those `keep` accepts (the
    exported view in mapping/ghidra/mappings.json)."""
    keep = keep or (lambda e: True)
    symbols = []
    for e in db.functions(bid):
        if not keep(e):
            continue
        kind = e.get('kind', 'function')
        item = {'key': f'{"f" if kind == "function" else "s"}:{e["address"]:x}', 'address': e['address'],
                'name': e['name'], 'aliases': list(e.get('aliases', [])),
                'kind': 'function' if kind == 'function' else 'label', 'convention': e.get('convention'),
                'prototype': None, 'comment': comment(e)}
        if e.get('prototype') and kind == 'function':
            p = parse(e['prototype'])
            p['name'] = e['name']
            item['prototype'] = p
        symbols.append(item)
    for g in db.globals(bid):
        if not keep(g):
            continue
        symbols.append({'key': f'g:{g["address"]:x}', 'address': g['address'], 'name': g['name'],
                        'aliases': list(g.get('aliases', [])), 'kind': 'global', 'type': g.get('type'),
                        'comment': comment(g)})
    for c in db.classes(bid):
        if not keep(c):
            continue
        for i, vt in enumerate(c.get('vtable', [])):
            suffix = '' if not vt.get('offset') else f'_{vt["offset"]:x}'
            symbols.append({'key': f'v:{vt["address"]:x}', 'address': vt['address'], 'kind': 'label',
                            'name': f'{c["name"]}::vftable{suffix}', 'aliases': [],
                            'comment': f'kyubu: vtable of {c["name"]} ({c.get("status")}, {len(vt.get("slots", []))} slots)'})
    types = []
    for t in db.types:
        if not applies(t, bid) or not keep(t):
            continue
        types.append({'key': 't:' + t['name'], 'name': t['name'], 'kind': 'union' if t.get('kind') == 'union' else 'struct',
                      'size': t.get('size', 0), 'base': t.get('base'),
                      'fields': [{'offset': f['offset'], 'name': f['name'], 'type': f['type'],
                                  'comment': f'{f.get("status")}'} for f in t.get('field', [])
                                 if applies(f, bid) and keep(f)]})
    for en in db.enums:
        if applies(en, bid) and keep(en):
            types.append({'key': 't:' + en['name'], 'name': en['name'], 'kind': 'enum', 'size': en.get('size', 4),
                          'values': [[v['name'], v['value']] for v in en.get('value', [])]})
    return {'symbols': symbols, 'types': types}


def pushed_path(ctx):
    return ctx.project_dir / 'pushed.json'


def load_pushed(ctx):
    p = pushed_path(ctx)
    return json.loads(p.read_text(encoding='utf-8')) if p.is_file() else {}


def require_clean(db):
    problems = list(db.problems)
    for check in rules.ALL_CHECKS:
        check(db, problems)
    if problems:
        for p in problems:
            print(f'FAIL {p}')
        raise KmapError(f'mapping/ has {len(problems)} problem(s); fix them (kmap check) before pushing')


def _require_project(ctx):
    if not (ctx.project_dir / 'kyubu.gpr').is_file():
        raise KmapError(f'no Ghidra project in {ctx.project_dir}: run `kmap init --game <folder>` first')


def cmd_push(ctx, dry_run=False, builds=None, quiet=False, force=False):
    db = dbmod.load(ctx)
    require_clean(db)
    _require_project(ctx)
    ids = builds or list(ctx.ws.builds)
    pushed = load_pushed(ctx)
    r = ghidra.run_worker(ctx, 'push', {'project': str(ctx.project_dir), 'dry_run': dry_run, 'force': force,
                                        'builds': {bid: desired(db, bid) for bid in ids},
                                        'pushed': {bid: pushed.get(bid, {'symbols': {}, 'types': {}}) for bid in ids}},
                          quiet=quiet)
    total = 0
    for bid, res in r['builds'].items():
        total += res['changes']
        say(f'{bid}: {res["changes"]} change(s), {res["unchanged"]} unchanged, {len(res["skipped"])} skipped'
            + (' (dry run)' if dry_run else ''))
        for s in res['skipped']:
            warn(f'  skipped: {s}; run `kmap pull` first (or push --force to overwrite)')
        for w in res['warnings']:
            warn(f'  warning: {w}')
        if not dry_run:
            pushed[bid] = res['pushed']
    if not dry_run:
        write_if_changed(pushed_path(ctx), dump_json(pushed))
    say(f'kmap push: {total} change(s)' + (' (dry run, nothing written)' if dry_run else ''))
    return 0


# --- pull: proposed edits of the TOML files -------------------------------------------------------------

HEAD = re.compile(r'^\s*\[\[')


class Edits:
    """Line edits of mapping files, applied bottom-up so earlier line numbers stay valid."""

    def __init__(self, repo):
        self.repo = repo
        self.ops = {}       # rel path -> list of (line index, kind, payload)
        self.count = 0

    def _lines(self, rel):
        p = self.repo / rel
        return p.read_text(encoding='utf-8').splitlines() if p.is_file() else []

    def _block(self, lines, name_line):
        i = name_line - 1
        start = i
        while start > 0 and not HEAD.match(lines[start]):
            start -= 1
        end = i + 1
        while end < len(lines) and not HEAD.match(lines[end]):
            end += 1
        return start, end

    def set_key(self, entry, key, value):
        rel = entry['_file']
        lines = self._lines(rel)
        start, end = self._block(lines, entry['_line'])
        for j in range(start, end):
            if re.match(rf'^\s*{key}\s*=', lines[j]):
                self.ops.setdefault(rel, []).append((j, 'replace', f'{key} = {value}'))
                break
        else:
            self.ops.setdefault(rel, []).append((entry['_line'], 'insert', f'{key} = {value}'))
        self.count += 1

    def insert_after(self, entry, block):
        rel = entry['_file']
        lines = self._lines(rel)
        _, end = self._block(lines, entry['_line'])
        while end > 0 and not lines[end - 1].strip():
            end -= 1
        self.ops.setdefault(rel, []).append((end, 'insert', '\n' + block))
        self.count += 1

    def append(self, rel, block, header=None):
        self.ops.setdefault(rel, []).append((10 ** 9, 'append', (block, header)))
        self.count += 1

    def result(self):
        out = {}
        for rel, ops in self.ops.items():
            lines = self._lines(rel)
            # bottom-up; several inserts at one line go in reverse so they end up in the order they were made
            order = sorted(range(len(ops)), key=lambda n: (-ops[n][0], -n))
            for idx, kind, payload in (ops[n] for n in order):
                if kind == 'replace':
                    lines[idx] = payload
                elif kind == 'insert':
                    lines.insert(idx, payload)
            appends = [p for i, k, p in ops if k == 'append']
            if appends and not lines and appends[0][1]:
                lines = [appends[0][1]]
            for block, _ in appends:
                if lines and lines[-1].strip():
                    lines.append('')
                lines.extend(block.splitlines())
            out[rel] = '\n'.join(lines) + '\n'
        return out


def function_block(name, address, subsystem, prototype=None, kind='function', convention='cdecl'):
    lines = ['[[function]]', f'name = {toml_str(name)}', f'address = 0x{address:X}', f'convention = "{convention}"']
    if prototype:
        lines.append(f'prototype = {toml_str(prototype)}')
    if kind == 'site':
        lines.append('kind = "site"')
    lines += [f'subsystem = "{subsystem}"', 'status = "guess"', 'evidence = []', 'source = "kyubu"',
              'notes = "named in Ghidra (kmap pull)"']
    return '\n'.join(lines)


def target_file(db, bid, name):
    parent = name.rsplit('::', 1)[0] if '::' in name else ''
    if parent:
        same = [e for e in db.functions(bid) if e['name'].startswith(parent + '::')]
        if same:
            return same[0]['_file'], same[0]['subsystem']
        cls = db.find_class(bid, parent)
        if cls and cls.get('subsystem'):
            return f'mapping/{bid}/functions/{cls["subsystem"]}.toml', cls['subsystem']
    return f'mapping/{bid}/functions/core.toml', 'core'


def cmd_pull(ctx, write=False, builds=None):
    db = dbmod.load(ctx)
    _require_project(ctx)
    ids = builds or list(ctx.ws.builds)
    pushed = load_pushed(ctx)
    if not pushed:
        warn('note: nothing was pushed to this project yet; every user-defined name counts as new')
    snap = ghidra.run_worker(ctx, 'snapshot', {'project': str(ctx.project_dir), 'builds': ids}, quiet=True)['builds']
    edits = Edits(ctx.repo)
    conflicts = []
    for bid in ids:
        prev = pushed.get(bid, {'symbols': {}, 'types': {}})
        want = desired(db, bid)
        want_sym = {i['key']: i for i in want['symbols']}
        want_types = {i['key']: i for i in want['types']}
        by_addr = {}
        for e in db.symbols(bid):
            by_addr.setdefault(e['address'], e)
        s = snap[bid]
        for a_str, g in sorted(s['functions'].items(), key=lambda kv: int(kv[0])):
            a = int(a_str)
            key = f'f:{a:x}'
            p = prev['symbols'].get(key)
            entry = by_addr.get(a)
            user_name = g['name_source'] == 'USER_DEFINED' and not rules.GHIDRA_DEFAULT.search(g['name'])
            if p is None:
                if entry is None and user_name:
                    rel, sub = target_file(db, bid, g['name'])
                    proto = None
                    if g['signature_source'] == 'USER_DEFINED':
                        proto = render(dict(g['prototype'], name=g['name']))
                    edits.append(rel, function_block(g['name'], a, sub, proto, convention=g['prototype']['conv']
                                                     if g['prototype']['conv'] != 'usercall' else 'cdecl'),
                                 header=f'# {bid}: functions named in Ghidra (kmap pull); sort them into subsystems')
                elif entry is not None and user_name and g['name'] != entry['name'] \
                        and g['name'] not in entry.get('aliases', []):
                    conflicts.append(f'{bid} 0x{a:X}: mapping/ calls it {entry["name"]!r} (not pushed yet), '
                                     f'Ghidra calls it {g["name"]!r}')
                continue
            g_name_changed = g['name'] != p['ghidra'].get('name')
            g_sig_changed = g['signature_source'] == 'USER_DEFINED' and g['signature'] != p['ghidra'].get('signature')
            if not (g_name_changed or g_sig_changed):
                continue
            if want_sym.get(key) != p['db'] or entry is None:
                now = entry['name'] if entry else 'no entry'
                conflicts.append(f'{bid} 0x{a:X}: changed on both sides since the last push: mapping/ has {now!r}, '
                                 f'Ghidra has {g["name"]!r} (pushed as {p["db"]["name"]!r})')
                continue
            new_name = g['name'] if g_name_changed else entry['name']
            if g_name_changed:
                edits.set_key(entry, 'name', toml_str(new_name))
                if entry.get('status') in EXPORTED and entry['name'] not in entry.get('aliases', []):
                    aliases = list(entry.get('aliases', [])) + [entry['name']]
                    edits.set_key(entry, 'aliases', '[' + ', '.join(toml_str(x) for x in aliases) + ']')
            if g_sig_changed:
                pr = dict(g['prototype'], name=new_name)
                edits.set_key(entry, 'prototype', toml_str(render(pr)))
                if pr['conv'] != entry.get('convention') and pr['conv'] != 'usercall':
                    edits.set_key(entry, 'convention', toml_str(pr['conv']))
        for a_str, g in sorted(s['labels'].items(), key=lambda kv: int(kv[0])):
            a = int(a_str)
            keys = [k for k in (f'g:{a:x}', f's:{a:x}', f'v:{a:x}') if k in prev['symbols']]
            entry = by_addr.get(a)
            if rules.GHIDRA_DEFAULT.search(g['name']):
                continue
            if not keys:
                if entry is None and not g['in_function']:
                    rel = f'mapping/{bid}/globals.toml'
                    t = g.get('type') or 'uint8_t'
                    edits.append(rel, '\n'.join([
                        '[[global]]', f'name = {toml_str(g["name"])}', f'address = 0x{a:X}', f'type = {toml_str(t)}',
                        'subsystem = "core"', 'status = "guess"', 'evidence = []', 'source = "kyubu"',
                        'notes = "named in Ghidra (kmap pull)"']), header=f'# {bid}: global variables')
                continue
            p = prev['symbols'][keys[0]]
            if g['name'] == p['ghidra'].get('name') or keys[0].startswith('v:'):
                continue
            if want_sym.get(keys[0]) != p['db'] or entry is None:
                conflicts.append(f'{bid} 0x{a:X}: changed on both sides since the last push: mapping/ has '
                                 f'{entry["name"] if entry else "no entry"!r}, Ghidra has {g["name"]!r}')
                continue
            edits.set_key(entry, 'name', toml_str(g['name']))
            if entry.get('status') in EXPORTED:
                aliases = list(entry.get('aliases', [])) + [entry['name']]
                edits.set_key(entry, 'aliases', '[' + ', '.join(toml_str(x) for x in aliases) + ']')
        for name, st in sorted(s['types'].items()):
            key = 't:' + name
            p = prev['types'].get(key)
            if p is None:
                empty = st.get('kind') != 'enum' and not st.get('fields')
                if not empty and db.find_type(name) is None and db.find_enum(name) is None:
                    edits.append('mapping/types/unsorted.toml', new_type_block(name, st),
                                 header='# Types created in Ghidra (kmap pull); move them to the right file')
                continue
            if st == p['ghidra']:
                continue
            if want_types.get(key) != p['db']:
                conflicts.append(f'type {name}: changed on both sides since the last push')
                continue
            t = db.find_type(name)
            if t is None or st.get('kind') == 'enum':
                conflicts.append(f'type {name}: enum values changed in Ghidra; copy them into mapping/types by hand')
                continue
            fields = {f['offset']: f for f in t.get('field', []) if applies(f, bid)}
            if st['size'] and st['size'] != t.get('size') and t.get('size'):
                edits.set_key(t, 'size', f'0x{st["size"]:X}')
            last = t.get('field', [])[-1] if t.get('field') else t
            for off, fname, ftype, _ in st['fields']:
                if fname == 'base' and off == 0 and t.get('base'):
                    continue
                f = fields.get(off)
                if f is None:
                    edits.insert_after(last, '\n'.join([
                        '[[type.field]]', f'offset = 0x{off:X}', f'name = {toml_str(fname or f"_unk_{off:x}")}',
                        f'type = {toml_str(ftype)}', 'status = "guess"', 'evidence = []', 'source = "kyubu"']))
                    continue
                if fname and fname != f['name']:
                    edits.set_key(f, 'name', toml_str(fname))
                    if f.get('status') in EXPORTED:
                        edits.set_key(f, 'aliases', '[' + ', '.join(toml_str(x) for x in
                                                                    list(f.get('aliases', [])) + [f['name']]) + ']')
                if ftype != f['type']:
                    edits.set_key(f, 'type', toml_str(ftype))
            gone = set(fields) - {off for off, *_ in st['fields']}
            for off in sorted(gone):
                warn(f'note: type {name}: field {fields[off]["name"]} (0x{off:X}) is gone in Ghidra; '
                     'remove it from mapping/ by hand if that was intended')
    changed = edits.result()
    for rel, text in sorted(changed.items()):
        p = ctx.repo / rel
        old = p.read_text(encoding='utf-8').splitlines(keepends=True) if p.is_file() else []
        for line in difflib.unified_diff(old, text.splitlines(keepends=True), fromfile=rel, tofile=f'{rel} (kmap pull)'):
            print(line, end='' if line.endswith('\n') else '\n')
    for c in conflicts:
        print(f'CONFLICT {c}')
    if write:
        for rel, text in changed.items():
            write_if_changed(ctx.repo / rel, text)
    say(f'kmap pull: {edits.count} proposed edit(s) in {len(changed)} file(s), {len(conflicts)} conflict(s)'
        + ('; written' if write and changed else '' if write else '; run with --write to apply'))
    return 1 if conflicts else 0


def new_type_block(name, st):
    if st['kind'] == 'enum':
        lines = ['[[enum]]', f'name = {toml_str(name)}', f'size = {st["size"]}', 'status = "guess"', 'evidence = []',
                 'source = "kyubu"']
        for n, v in st['values']:
            lines += ['', '[[enum.value]]', f'name = {toml_str(n)}', f'value = {v}']
        return '\n'.join(lines)
    lines = ['[[type]]', f'name = {toml_str(name)}', f'kind = "{st["kind"]}"', f'size = 0x{st["size"]:X}',
             'status = "guess"', 'evidence = []', 'source = "kyubu"']
    for off, fname, ftype, _ in st['fields']:
        lines += ['', '[[type.field]]', f'offset = 0x{off:X}', f'name = {toml_str(fname or f"_unk_{off:x}")}',
                  f'type = {toml_str(ftype)}', 'status = "guess"', 'evidence = []', 'source = "kyubu"']
    return '\n'.join(lines)
