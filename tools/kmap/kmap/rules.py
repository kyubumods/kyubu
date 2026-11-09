"""The rules kmap check enforces on mapping/: schema, status and evidence, provenance, type layout and
naming (mapping/conventions.md explains them for people). Each check appends Problem objects."""
import re

from .db import STATUSES, Problem, label, where

# --- schema --------------------------------------------------------------------------------------------
STR, INT, BOOL, STRLIST, INTLIST, TABLES = 'string', 'integer', 'boolean', 'list of strings', 'list of integers', 'tables'
COMMON = {'status': STR, 'evidence': STRLIST, 'source': STR}
SCHEMA = {
    'function': ({'name': STR, 'address': INT, 'convention': STR, 'subsystem': STR, **COMMON},
                 {'prototype': STR, 'aliases': STRLIST, 'notes': STR, 'kind': STR, 'sig_len': INT}),
    'global': ({'name': STR, 'address': INT, 'type': STR, 'subsystem': STR, **COMMON},
               {'aliases': STRLIST, 'notes': STR}),
    'class': ({'name': STR, 'rtti': STR, **COMMON},
              {'type_descriptor': INT, 'vtable': TABLES, 'bases': STRLIST, 'size': INT, 'subsystem': STR,
               'constructors': INTLIST, 'destructors': INTLIST, 'aliases': STRLIST, 'notes': STR}),
    'vtable': ({'address': INT, 'slots': INTLIST}, {'offset': INT}),
    'type': ({'name': STR, 'kind': STR, 'size': INT, **COMMON},
             {'base': STR, 'builds': STRLIST, 'aliases': STRLIST, 'notes': STR, 'subsystem': STR, 'field': TABLES}),
    'field': ({'offset': INT, 'name': STR, 'type': STR, **COMMON},
              {'notes': STR, 'aliases': STRLIST, 'builds': STRLIST}),
    'enum': ({'name': STR, 'size': INT, **COMMON},
             {'aliases': STRLIST, 'notes': STR, 'subsystem': STR, 'value': TABLES, 'builds': STRLIST}),
    'value': ({'name': STR, 'value': INT}, {'status': STR, 'evidence': STRLIST, 'source': STR, 'notes': STR,
                                            'aliases': STRLIST}),
}
CONVENTIONS = ('cdecl', 'stdcall', 'thiscall', 'fastcall', 'usercall')
TYPE_KINDS = ('struct', 'class', 'union')


def _is(v, kind):
    if kind == STR:
        return isinstance(v, str)
    if kind == INT:
        return isinstance(v, int) and not isinstance(v, bool)
    if kind == BOOL:
        return isinstance(v, bool)
    if kind == STRLIST:
        return isinstance(v, list) and all(isinstance(x, str) for x in v)
    if kind == INTLIST:
        return isinstance(v, list) and all(isinstance(x, int) and not isinstance(x, bool) for x in v)
    if kind == TABLES:
        return isinstance(v, list) and all(isinstance(x, dict) for x in v)
    return False


def check_schema(db, problems):
    for e in db.all_entries():
        _schema_one(e, problems)
        for vt in e.get('vtable', []) if e.get('_kind') == 'class' else []:
            _schema_one(vt, problems)


def _schema_one(e, problems):
    kind = e.get('_kind')
    required, optional = SCHEMA[kind]
    for k, t in required.items():
        if k not in e:
            problems.append(Problem(where(e), f'{label(e)}: missing `{k}` ({t})'))
        elif not _is(e[k], t):
            problems.append(Problem(where(e), f'{label(e)}: `{k}` must be a {t}'))
    for k, v in e.items():
        if k.startswith('_') or k in required:
            continue
        if k not in optional:
            problems.append(Problem(where(e), f'{label(e)}: unknown key `{k}`'))
        elif not _is(v, optional[k]):
            problems.append(Problem(where(e), f'{label(e)}: `{k}` must be a {optional[k]}'))
    if kind == 'function':
        if e.get('convention') not in CONVENTIONS and isinstance(e.get('convention'), str):
            problems.append(Problem(where(e), f'{label(e)}: convention must be one of {", ".join(CONVENTIONS)}'))
        if e.get('kind', 'function') not in ('function', 'site'):
            problems.append(Problem(where(e), f'{label(e)}: kind must be "function" or "site"'))
        if 'sig_len' in e and isinstance(e['sig_len'], int) and not 4 <= e['sig_len'] <= 64:
            problems.append(Problem(where(e), f'{label(e)}: sig_len must be 4-64'))
    if kind == 'type' and isinstance(e.get('kind'), str) and e['kind'] not in TYPE_KINDS:
        problems.append(Problem(where(e), f'{label(e)}: kind must be one of {", ".join(TYPE_KINDS)}'))
    if kind == 'enum' and e.get('size') not in (1, 2, 4, 8, None):
        problems.append(Problem(where(e), f'{label(e)}: size must be 1, 2, 4 or 8'))


def check_ranges_and_uniqueness(db, problems):
    ws = db.ws
    for bid, build in ws.builds.items():
        names = {}
        addrs = {}
        for e in db.symbols(bid):
            for n in [e.get('name')] + list(e.get('aliases', [])):
                if not isinstance(n, str):
                    continue
                if n in names:
                    first = names[n]
                    problems.append(Problem(where(e), f'{label(e)}: name {n!r} is already used by {label(first)} '
                                                      f'at {where(first)} (names and aliases are unique per build)'))
                else:
                    names[n] = e
            a = e.get('address')
            if not isinstance(a, int) or isinstance(a, bool):
                continue
            kind = e['_kind'] if e['_kind'] == 'global' else e.get('kind', 'function')
            if kind == 'global':
                if not build.in_image(a):
                    problems.append(Problem(where(e), f'{label(e)}: address 0x{a:X} is outside the {bid} image '
                                                      f'({build.image_range()})'))
            elif not build.in_text(a):
                problems.append(Problem(where(e), f'{label(e)}: address 0x{a:X} is outside {bid} .text '
                                                  f'(0x{build.text_start:X}-0x{build.text_end - 1:X}; image '
                                                  f'{build.image_range()})'))
            key = (kind, a)
            if key in addrs:
                problems.append(Problem(where(e), f'{label(e)}: address 0x{a:X} is already mapped as '
                                                  f'{label(addrs[key])} at {where(addrs[key])} (use aliases)'))
            else:
                addrs[key] = e
            if e.get('subsystem') not in ws.subsystems and isinstance(e.get('subsystem'), str):
                problems.append(Problem(where(e), f'{label(e)}: subsystem {e["subsystem"]!r} is not one of '
                                                  f'{", ".join(ws.subsystems)} (mapping/workspace.toml)'))
        cls_names = {}
        for c in db.classes(bid):
            n = c.get('name')
            if n in cls_names:
                problems.append(Problem(where(c), f'{label(c)}: class listed twice (also at {where(cls_names[n])})'))
            cls_names[n] = c
            for vt in c.get('vtable', []):
                a = vt.get('address')
                if isinstance(a, int) and not build.in_image(a):
                    problems.append(Problem(where(vt), f'{label(c)}: vtable 0x{a:X} is outside the {bid} image '
                                                       f'({build.image_range()})'))
                for s in vt.get('slots', []):
                    if isinstance(s, int) and s and not build.in_text(s):
                        problems.append(Problem(where(vt), f'{label(c)}: vtable slot 0x{s:X} is outside .text'))
    seen = {}
    for e in db.types + db.enums:
        for n in [e.get('name')] + list(e.get('aliases', [])):
            if n in seen:
                problems.append(Problem(where(e), f'{label(e)}: name {n!r} is already used by {label(seen[n])} at '
                                                  f'{where(seen[n])}'))
            elif isinstance(n, str):
                seen[n] = e
        for b in e.get('builds', []):
            if b not in ws.builds:
                problems.append(Problem(where(e), f'{label(e)}: unknown build {b!r} in builds'))
        members = []    # (name or alias, entry): unique in their type, like C members and enum constants
        for sub in e.get('field', []) + e.get('value', []):
            for b in sub.get('builds', []):
                if b not in ws.builds:
                    problems.append(Problem(where(sub), f'{label(sub)}: unknown build {b!r} in builds'))
            for n in [sub.get('name')] + list(sub.get('aliases', [])):
                other = next((o for m, o in members if m == n and _same_build(o, sub)), None)
                if other is not None:
                    problems.append(Problem(where(sub), f'{label(sub)}: name {n!r} is already used by {label(other)} at '
                                                        f'{where(other)} (field and value names and aliases are unique '
                                                        'in their type)'))
                elif isinstance(n, str):
                    members.append((n, sub))
        if e.get('subsystem') is not None and e.get('subsystem') not in ws.subsystems:
            problems.append(Problem(where(e), f'{label(e)}: subsystem {e["subsystem"]!r} is not in workspace.toml'))


# --- status, evidence, provenance -----------------------------------------------------------------------
STRONG = ('rtti', 'vtable', 'import', 'library', 'probe')
WEAK = ('string', 'xref', 'correlated', 'cuwo', 'manual')
EVIDENCE_MAX = 80
NOTES_MAX = 300
SOURCES = ('kyubu', 'rtti', 'survey', 'cuwo')
CUWO_PATH = re.compile(r'^[\w.-]+(/[\w.-]+)*\.\w+$')
CODE_LIKE = re.compile(r'undefined[1248]?\b|\bFUN_[0-9a-fA-F]{6,}|\*\s*\((?:int|uint|char|undefined)\w*\s*\*\)|'
                       r'\breturn\b[^.]*;|;\s*\}|\{\s*\n')


def evidence_tag(item):
    return item.split(':', 1)[0] if ':' in item else ''


def _evidence_format(e, item, db, problems):
    tag = evidence_tag(item)
    value = item.split(':', 1)[1] if ':' in item else ''
    if tag not in STRONG + WEAK:
        problems.append(Problem(where(e), f'{label(e)}: evidence {item!r} has no known tag '
                                          f'(one of {", ".join(STRONG + WEAK)}, as tag:value)'))
        return
    if not value:
        problems.append(Problem(where(e), f'{label(e)}: evidence {item!r} has an empty value'))
    elif tag == 'vtable' and not re.match(r'^.+:\d+$', value):
        problems.append(Problem(where(e), f'{label(e)}: evidence {item!r} must be vtable:<class>:<slot>'))
    elif tag == 'string' and not (value.startswith('"') and value.endswith('"') and len(value) >= 2):
        problems.append(Problem(where(e), f'{label(e)}: evidence {item!r} must quote the text: string:"..."'))
    elif tag == 'import' and '!' not in value:
        problems.append(Problem(where(e), f'{label(e)}: evidence {item!r} must be import:<dll>!<function>'))
    elif tag == 'correlated':
        b = value.split(':', 1)[0]
        if b not in db.ws.builds or ':' not in value:
            problems.append(Problem(where(e), f'{label(e)}: evidence {item!r} must be correlated:<build>:<symbol> '
                                              f'with a supported build'))
    elif tag == 'cuwo' and not CUWO_PATH.match(value):
        problems.append(Problem(where(e), f'{label(e)}: evidence {item!r} must name a file in the cuwo repository '
                                          '(cuwo:<path>, e.g. cuwo:cuwo/entity.pyx)'))


def check_status_evidence(db, problems):
    for e in db.all_entries():
        kind = e.get('_kind')
        if kind == 'value' and 'status' not in e:
            continue      # enum values inherit the enum's status
        st = e.get('status')
        if st is not None and st not in STATUSES:
            problems.append(Problem(where(e), f'{label(e)}: status must be one of {", ".join(STATUSES)}'))
        ev = e.get('evidence', []) if isinstance(e.get('evidence', []), list) else []
        for item in ev:
            if not isinstance(item, str):
                continue
            if len(item) > EVIDENCE_MAX:
                problems.append(Problem(where(e), f'{label(e)}: evidence is {len(item)} characters (limit '
                                                  f'{EVIDENCE_MAX}): {item[:40]!r}...; quote at most a few words, '
                                                  'never game code or decompiler output'))
            _evidence_format(e, item, db, problems)
        if st == 'verified':
            tags = [evidence_tag(i) for i in ev if isinstance(i, str)]
            strong = [t for t in tags if t in STRONG]
            weak = {t for t in tags if t in WEAK}
            if not strong and len(weak) < 2:
                problems.append(Problem(where(e), f'{label(e)}: marked verified with evidence {ev}, but the rule '
                                                  'needs one strong item (rtti, vtable, import, library, probe) or '
                                                  'two weak items with different tags'))
        src = e.get('source')
        if 'source' in e and isinstance(src, str):
            if not (src in SOURCES or re.match(r'^contributor:[A-Za-z0-9_.-]+$', src)):
                problems.append(Problem(where(e), f'{label(e)}: source {src!r} must be kyubu, rtti, survey, cuwo or '
                                                  'contributor:<handle>'))
            cites_cuwo = src == 'cuwo' or any(isinstance(i, str) and i.startswith('cuwo:') for i in ev)
            if cites_cuwo and not any(isinstance(i, str) and i.startswith('cuwo:') and CUWO_PATH.match(i[5:])
                                      for i in ev):
                problems.append(Problem(where(e), f'{label(e)}: cites cuwo but names no file of the cuwo repository '
                                                  '(add evidence cuwo:<path>)'))
        notes = e.get('notes')
        if isinstance(notes, str):
            if len(notes) > NOTES_MAX:
                problems.append(Problem(where(e), f'{label(e)}: notes are {len(notes)} characters (limit '
                                                  f'{NOTES_MAX}). Summarise in your own words: game code and '
                                                  'decompiler output must never be committed'))
            elif CODE_LIKE.search(notes):
                problems.append(Problem(where(e), f'{label(e)}: notes look like code or decompiler output; describe '
                                                  'the behaviour in your own words instead'))


def check_cross_references(db, problems):
    """vtable:/xref:/correlated:/probe: evidence must agree with the rest of the database."""
    for bid in db.ws.builds:
        classes = {c.get('name'): c for c in db.classes(bid)}
        for e in db.symbols(bid):
            for item in e.get('evidence', []) or []:
                if not isinstance(item, str):
                    continue
                tag, _, value = item.partition(':')
                if tag == 'vtable' and re.match(r'^.+:\d+$', value):
                    cname, slot = value.rsplit(':', 1)
                    c = classes.get(cname)
                    if c and c.get('vtable'):
                        slots = c['vtable'][0].get('slots', [])
                        n = int(slot)
                        if n >= len(slots) or slots[n] != e.get('address'):
                            got = f'0x{slots[n]:X}' if n < len(slots) else 'no such slot'
                            problems.append(Problem(where(e), f'{label(e)}: evidence {item} disagrees with '
                                                              f'{bid}/classes.toml (slot {n} is {got})'))
                elif tag == 'xref' and '!' not in value and not db.find_function(bid, value):
                    problems.append(Problem(where(e), f'{label(e)}: evidence {item} names no mapped symbol of {bid}'))
                elif tag == 'correlated' and ':' in value:
                    other, _, sym = value.partition(':')
                    if other in db.ws.builds and not db.find_function(other, sym):
                        problems.append(Problem(where(e), f'{label(e)}: evidence {item} names no mapped symbol of '
                                                          f'{other}'))


def check_probes(db, problems):
    from .probes import record_problems
    for pid, p in db.probes.items():
        problems.extend(Problem(where(p), text) for text in record_problems(pid, p, db.ctx.repo))
        for k, t in (('build', STR), ('kyubu', STR), ('steps', STRLIST), ('observations', STRLIST)):
            if not _is(p.get(k), t):
                problems.append(Problem(where(p), f'probe {pid}: `{k}` must be a {t}'))
        if isinstance(p.get('build'), str) and p['build'] not in db.ws.builds:
            problems.append(Problem(where(p), f'probe {pid}: unknown build {p["build"]!r}'))
        confirms = p.get('confirms', [])
        if not _is(confirms, TABLES) or not confirms:
            problems.append(Problem(where(p), f'probe {pid}: needs at least one [[confirms]] table'))
            confirms = []
        names = db.names()
        for c in confirms:
            if not isinstance(c.get('entry'), str) or not isinstance(c.get('observed'), str):
                problems.append(Problem(where(p), f'probe {pid}: every [[confirms]] needs entry and observed'))
            elif c['entry'] not in names:
                problems.append(Problem(where(p), f'probe {pid}: confirms {c["entry"]!r}, which is not mapped'))
    for e in db.all_entries():
        for item in e.get('evidence', []) or []:
            if not isinstance(item, str) or not item.startswith('probe:'):
                continue
            pid = item[6:]
            p = db.probes.get(pid)
            if p is None:
                problems.append(Problem(where(e), f'{label(e)}: cites probe:{pid}, but mapping/probes/{pid}.toml '
                                                  'does not exist'))
                continue
            pb = p.get('build')
            allowed = [e['_build']] if e.get('_build') else (e.get('builds') or list(db.ws.builds))
            if pb not in allowed:
                problems.append(Problem(where(e), f'{label(e)}: an entry of {", ".join(allowed)} cites probe:{pid}, '
                                                  f'which was recorded on {pb}'))


# --- type layout --------------------------------------------------------------------------------------
PRIMITIVES = {'int8_t': 1, 'uint8_t': 1, 'int16_t': 2, 'uint16_t': 2, 'int32_t': 4, 'uint32_t': 4, 'int64_t': 8,
              'uint64_t': 8, 'float': 4, 'double': 8, 'bool': 1, 'char': 1, 'wchar_t': 2, 'void': 0}
POINTER_SIZE = 4
ARRAY = re.compile(r'^(.*?)\[(0x[0-9A-Fa-f]+|\d+)\]$')


def parse_type(t):
    """('ptr', target) | ('array', elem, n) | ('named', name)."""
    t = t.strip()
    if t.endswith('*'):
        return ('ptr', t[:-1].strip())
    m = ARRAY.match(t)
    if m:
        return ('array', m.group(1).strip(), int(m.group(2), 0))
    return ('named', t)


def type_size(db, t, seen=()):
    """Size in bytes of a field type string, or None when unknown (a mapped type without a known size)."""
    p = parse_type(t)
    if p[0] == 'ptr':
        return POINTER_SIZE
    if p[0] == 'array':
        s = type_size(db, p[1], seen)
        return None if s is None else s * p[2]
    name = p[1]
    if name in PRIMITIVES:
        return PRIMITIVES[name] or None
    if name in seen:
        return None
    ty = db.find_type(name)
    if ty is not None:
        return ty.get('size') or None
    en = db.find_enum(name)
    if en is not None:
        return en.get('size')
    return None


def type_known(db, t):
    p = parse_type(t)
    if p[0] == 'ptr':
        return True
    if p[0] == 'array':
        return type_known(db, p[1])
    return p[1] in PRIMITIVES and p[1] != 'void' or db.find_type(p[1]) is not None or db.find_enum(p[1]) is not None


def base_end(db, ty, depth=0):
    """End offset of the base part of a type (its base's size, or the end of the base's last field)."""
    b = db.find_type(ty['base']) if ty.get('base') else None
    if b is None or depth > 8:
        return 0
    if b.get('size'):
        return b['size']
    ends = [base_end(db, b, depth + 1)]
    for f in b.get('field', []):
        s = type_size(db, f.get('type', '')) or 1
        ends.append(f.get('offset', 0) + s)
    return max(ends)


def check_layout(db, problems):
    for ty in db.types:
        fields = [f for f in ty.get('field', []) if isinstance(f.get('offset'), int) and isinstance(f.get('type'), str)]
        size = ty.get('size') if isinstance(ty.get('size'), int) else 0
        if ty.get('base'):
            b = db.find_type(ty['base'])
            if b is None:
                problems.append(Problem(where(ty), f'{label(ty)}: base {ty["base"]!r} is not a type in mapping/types'))
            else:
                end = base_end(db, ty)
                for f in fields:
                    if f['offset'] < end:
                        problems.append(Problem(where(f), f'{label(f)}: offset 0x{f["offset"]:X} lies inside the base '
                                                          f'{ty["base"]} (0x0-0x{end - 1:X}); a class with one base '
                                                          'has the base at offset 0'))
        for f in fields:
            if not type_known(db, f['type']):
                problems.append(Problem(where(f), f'{label(f)}: type {f["type"]!r} is not a primitive, pointer, array '
                                                  'or a type defined in mapping/'))
            s = type_size(db, f['type'])
            if size and s and f['offset'] + s > size:
                problems.append(Problem(where(f), f'{label(f)}: 0x{f["offset"]:X}+{s} ends past the size of '
                                                  f'{ty.get("name")} (0x{size:X})'))
            if f['offset'] < 0:
                problems.append(Problem(where(f), f'{label(f)}: negative offset'))
        if ty.get('kind') == 'union':
            continue
        ordered = sorted(fields, key=lambda f: f['offset'])
        for a, b in zip(ordered, ordered[1:]):
            sa = type_size(db, a['type']) or 1
            if a['offset'] + sa > b['offset'] and _same_build(a, b):
                lo, hi = b['offset'], min(a['offset'] + sa, b['offset'] + (type_size(db, b['type']) or 1)) - 1
                problems.append(Problem(where(b), f'{label(b)} overlaps {label(a)} (at {where(a)}): bytes '
                                                  f'0x{lo:X}-0x{hi:X} ({a["name"]} is {a["type"]} at '
                                                  f'0x{a["offset"]:X}, {b["name"]} starts at 0x{b["offset"]:X})'))


def _same_build(a, b):
    ba, bb = set(a.get('builds') or []), set(b.get('builds') or [])
    return not ba or not bb or bool(ba & bb)


# --- naming -------------------------------------------------------------------------------------------
GHIDRA_DEFAULT = re.compile(r'(^|::)(FUN|DAT|LAB|PTR|UNK|SUB|EXT|thunk_FUN|switchD|caseD)_[0-9A-Fa-f]+\b')
SNAKE = re.compile(r'^[a-z][a-z0-9_]*$')
LOWER_CAMEL = re.compile(r'^[a-z][A-Za-z0-9]*$')
PASCAL = re.compile(r'^[A-Z][A-Za-z0-9]*$')
UPPER_SNAKE = re.compile(r'^[A-Z][A-Z0-9_]*$')
NAMESPACE = re.compile(r'^[a-z][a-z0-9_]*$')
WELL_KNOWN_FREE = {'WinMain', 'wWinMain', 'main', 'wmain', 'DllMain'}


def rtti_names(db):
    """Unqualified class name -> RTTI qualified names, from every build's classes.toml."""
    out = {}
    for bid in db.ws.builds:
        for c in db.classes(bid):
            n = c.get('name')
            if isinstance(n, str):
                out.setdefault(n.rsplit('::', 1)[-1], set()).add(n)
    return out


def is_library(e):
    return e.get('subsystem') == 'library' or any(isinstance(i, str) and i.startswith('library:')
                                                  for i in e.get('evidence', []) or [])


def _split(name):
    depth, cur, parts = 0, '', []
    i = 0
    while i < len(name):
        ch = name[i]
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        if depth == 0 and name.startswith('::', i):
            parts.append(cur)
            cur = ''
            i += 2
            continue
        cur += ch
        i += 1
    parts.append(cur)
    return parts


def check_naming(db, problems):
    rtti = rtti_names(db)
    rtti_full = {n for s in rtti.values() for n in s}

    def default_name(e, n):
        if GHIDRA_DEFAULT.search(n):
            problems.append(Problem(where(e), f'{label(e)}: {n!r} is a Ghidra default name; give it a real name '
                                              '(mapping/conventions.md) or remove the entry'))
            return True
        return False

    def missing_namespace(e, cls_path):
        """cls_path: the class part of a name ('Creature' or 'Database'); suggest the RTTI name."""
        last = cls_path.rsplit('::', 1)[-1]
        if cls_path not in rtti_full and last in rtti and len(rtti[last]) == 1:
            full = next(iter(rtti[last]))
            if full != cls_path and full.endswith('::' + cls_path):
                return full
        return None

    for bid in db.ws.builds:
        for e in db.symbols(bid):
            n = e.get('name')
            if not isinstance(n, str) or default_name(e, n):
                continue
            if is_library(e):
                continue
            parts = _split(n)
            if e['_kind'] == 'global':
                if not SNAKE.match(parts[-1]):
                    problems.append(Problem(where(e), f'{label(e)}: global names are snake_case ({parts[-1]!r})'))
                continue
            if len(parts) == 1:
                if n not in WELL_KNOWN_FREE and not SNAKE.match(n):
                    problems.append(Problem(where(e), f'{label(e)}: a free function is snake_case; a method is '
                                                      f'Namespace::Class::method ({n!r})'))
                continue
            owner, method = parts[-2], parts[-1]
            full = missing_namespace(e, '::'.join(parts[:-1]))
            if full:
                problems.append(Problem(where(e), f'{label(e)}: RTTI names the class {full}; use '
                                                  f'{full}::{method} (keep {n!r} as an alias)'))
            for seg in parts[:-1]:
                if not (NAMESPACE.match(seg) or PASCAL.match(seg) or seg in rtti or '<' in seg):
                    problems.append(Problem(where(e), f'{label(e)}: {seg!r} is neither a namespace (lower case) '
                                                      'nor a class (PascalCase)'))
            if NAMESPACE.match(owner) and owner not in rtti:
                if not SNAKE.match(method):
                    problems.append(Problem(where(e), f'{label(e)}: a free function in namespace {owner} is '
                                                      f'snake_case ({method!r})'))
            elif not (LOWER_CAMEL.match(method) or method == owner or method == '~' + owner
                      or method.startswith('operator')):
                problems.append(Problem(where(e), f'{label(e)}: methods are lowerCamel ({method!r})'))
        for c in db.classes(bid):
            n, r = c.get('name'), c.get('rtti')
            if isinstance(n, str) and isinstance(r, str):
                d = demangle_class(r)
                if d and d != n:
                    problems.append(Problem(where(c), f'{label(c)}: RTTI {r} names the class {d}; classes use their '
                                                      'RTTI name verbatim'))
    for t in db.types + db.enums:
        n = t.get('name')
        if not isinstance(n, str) or default_name(t, n):
            continue
        parts = _split(n)
        if n in rtti_full:
            pass
        elif len(parts) == 1:
            full = missing_namespace(t, n)
            hint = f'; RTTI names this class {full}: use {full!r}' if full else \
                '; put it in the namespace of the code that uses it (e.g. cube::)'
            problems.append(Problem(where(t), f'{label(t)}: type names carry their namespace{hint}'))
        elif not (PASCAL.match(parts[-1]) or parts[0] == 'std'):
            problems.append(Problem(where(t), f'{label(t)}: type names are PascalCase ({parts[-1]!r})'))
        for f in t.get('field', []):
            fn = f.get('name')
            if not isinstance(fn, str) or default_name(f, fn):
                continue
            if fn.startswith('_unk_'):
                want = f'_unk_{f.get("offset", 0):x}' if isinstance(f.get('offset'), int) else None
                if fn != want:
                    problems.append(Problem(where(f), f'{label(f)}: unknown ranges are named _unk_<offset in lower-'
                                                      f'case hex> ({want!r})'))
            elif not SNAKE.match(fn):
                problems.append(Problem(where(f), f'{label(f)}: field names are snake_case ({fn!r})'))
        for v in t.get('value', []):
            vn = v.get('name')
            if isinstance(vn, str) and not UPPER_SNAKE.match(vn):
                problems.append(Problem(where(v), f'{label(v)}: enum values are UPPER_SNAKE_CASE ({vn!r})'))


def demangle_class(r):
    """.?AVCreature@cube@@ -> cube::Creature (plain names only; templates return None)."""
    m = re.match(r'^\.\?A[VU](.+)@@$', r)
    if not m or '?' in m.group(1) or '$' in m.group(1):
        return None
    return '::'.join(reversed(m.group(1).split('@')))


def check_prototypes(db, problems):
    from .proto import parse
    for bid in db.ws.builds:
        for e in db.functions(bid):
            text = e.get('prototype')
            if not isinstance(text, str):
                continue
            try:
                p = parse(text)
            except ValueError as err:
                problems.append(Problem(where(e), f'{label(e)}: prototype {text!r}: {err}'))
                continue
            if p['name'] not in [e.get('name')] + list(e.get('aliases', [])):
                problems.append(Problem(where(e), f'{label(e)}: the prototype names {p["name"]!r}'))
            if p['conv'] != e.get('convention'):
                problems.append(Problem(where(e), f'{label(e)}: the prototype is __{p["conv"]} but convention is '
                                                  f'{e.get("convention")!r}'))
            if p['conv'] == 'thiscall' and not p['params']:
                problems.append(Problem(where(e), f'{label(e)}: a __thiscall prototype lists the ECX parameter '
                                                  '(`this`) first'))


ALL_CHECKS = (check_schema, check_ranges_and_uniqueness, check_status_evidence, check_cross_references,
              check_probes, check_layout, check_naming, check_prototypes)
