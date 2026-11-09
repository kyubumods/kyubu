"""kmap survey: automated passes over both builds.

  facts        Ghidra: one record per function (calls, imports, data references, pushes before calls,
               vtable stores, masked byte and mnemonic hashes) and the RTTI analyzer's names
  rtti         type descriptors, class hierarchies, vtables and slots, constructor/destructor candidates,
               sizes from operator new
  libraries    STL (error strings and forwarders), SQLite 3.7.15.2 (source anchors), Ogg, CRT (Function ID)
  strings      every string of 4+ characters with its referencing functions, and every import's callers
  correlation  Cube.exe <-> Server.exe: exact bytes, exact instructions, then call-graph and string similarity
  cuwo         candidate types and enums from a cuwo checkout (kmap import-cuwo)
  coverage     counts per build and subsystem (kmap report turns them into mapping/survey-summary.md)

Raw output is sorted JSON under $KMAP_HOME/survey/, never committed; re-running on the same project gives
byte-identical files.
"""
import json
import re
import struct
from collections import Counter, defaultdict
from pathlib import Path

from . import db as dbmod
from . import ghidra
from .game import load_input
from .util import KmapError, dump_json, say, toml_str, write_if_changed

PASSES = ('facts', 'rtti', 'libraries', 'strings', 'correlation', 'cuwo', 'coverage')
STL_ERRORS = ('vector<T> too long', 'string too long', 'list<T> too long', 'map/set<T> too long',
              'invalid string position', 'deque<T> too long', 'invalid vector<T> subscript',
              'invalid map/set<T> key', 'bad allocation', 'vector<bool> too long')
OPERATOR_NEW = ('??2@YAPAXI@Z', 'operator_new', 'operator new')


# --- the executable ----------------------------------------------------------------------------------------

class Image:
    def __init__(self, data, build):
        import pefile
        pe = pefile.PE(data=data)
        self.build = build
        base = build.image_base
        self.sections = []
        for s in pe.sections:
            raw = s.get_data()
            size = max(s.Misc_VirtualSize, len(raw))
            self.sections.append((s.Name.rstrip(b'\0').decode(), base + s.VirtualAddress,
                                  raw[:size].ljust(size, b'\0')))

    def section(self, name):
        return next(((va, d) for n, va, d in self.sections if n == name), (0, b''))

    def read(self, va, n):
        for _, start, d in self.sections:
            if start <= va and va + n <= start + len(d):
                return d[va - start:va - start + n]
        return None

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if b else None

    def in_image(self, va):
        return self.build.in_image(va)

    def in_text(self, va):
        return self.build.in_text(va)

    def cstring(self, va, limit=1024):
        b = self.read(va, 1)
        out = bytearray()
        while b and b != b'\0' and len(out) < limit:
            out += b
            va += 1
            b = self.read(va, 1)
        return out.decode('latin-1')


def survey_path(ctx, bid, name):
    return ctx.survey_dir / bid / name


def load_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def save(path, obj):
    write_if_changed(path, dump_json(obj))


def load_facts(ctx, bid):
    p = survey_path(ctx, bid, 'functions.json')
    if not p.is_file():
        raise KmapError(f'{p} not found: run `kmap survey` (the facts pass needs the Ghidra project)')
    return load_json(p)


# --- pass: RTTI --------------------------------------------------------------------------------------------

def pass_rtti(img, facts, gsym):
    rdata = [(va, d) for n, va, d in img.sections if n in ('.rdata', '.data')]
    tds = {}
    for start, d in rdata:
        for m in re.finditer(rb'\.\?A[VU][\x21-\x7E]{1,2000}?@@\x00', d):
            td = start + m.start() - 8
            if td % 4 == 0:
                tds[td] = m.group(0)[:-1].decode('latin-1')
    vft = Counter(img.u32(td) for td in tds).most_common(1)[0][0] if tds else None
    tds = {td: n for td, n in tds.items() if img.u32(td) == vft}
    names = {int(k): v for k, v in gsym['type_descriptors'].items()}

    cols = {}
    rd_va, rd = img.section('.rdata')
    for off in range(0, len(rd) - 20, 4):
        sig, offset, cd, ptd, pchd = struct.unpack_from('<5I', rd, off)
        if sig == 0 and ptd in tds and img.in_image(pchd) and img.u32(pchd) == 0:
            n = img.u32(pchd + 8)
            bca = img.u32(pchd + 12)
            if n is not None and 0 < n < 256 and bca and img.in_image(bca):
                cols[rd_va + off] = {'td': ptd, 'offset': offset, 'chd': pchd}
    col_ptr_at = {}
    for off in range(0, len(rd) - 4, 4):
        v = struct.unpack_from('<I', rd, off)[0]
        if v in cols:
            col_ptr_at[rd_va + off] = v
    vtables = defaultdict(list)
    for at, col in sorted(col_ptr_at.items()):
        vt = at + 4
        slots = []
        p = vt
        while True:
            v = img.u32(p)
            if v is None or not img.in_text(v) or p in col_ptr_at:
                break
            slots.append(v)
            p += 4
        vtables[cols[col]['td']].append({'address': vt, 'col': col, 'offset': cols[col]['offset'], 'slots': slots})

    stores = defaultdict(list)       # vtable -> [[function, displacement]]
    for f in facts:
        for disp, imm in f['vtable_stores']:
            stores[imm].append([f['entry'], disp])
    callers = defaultdict(list)
    new_sizes = defaultdict(Counter)
    for f in facts:
        seq = f['call_seq']
        for i, (target, pushes) in enumerate(seq):
            if isinstance(target, str) and any(o in target for o in OPERATOR_NEW) and pushes:
                for nxt, _ in seq[i + 1:i + 3]:
                    if isinstance(nxt, int):
                        new_sizes[nxt][pushes[-1]] += 1
        for c in f['calls']:
            callers[c].append(f['entry'])

    classes = []
    for td, mangled in sorted(tds.items()):
        name = names.get(td) or demangle_simple(mangled) or mangled
        cls = {'name': name, 'mangled': mangled, 'type_descriptor': td, 'vtables': [], 'bases': [],
               'stores_vtable': [], 'sizes': {}}
        vts = sorted(vtables.get(td, []), key=lambda v: (v['offset'], v['address']))
        chd = cols[vts[0]['col']]['chd'] if vts else None
        if chd:
            n, bca = img.u32(chd + 8), img.u32(chd + 12)
            for k in range(1, n):
                bcd = img.u32(bca + 4 * k)
                btd = img.u32(bcd) if bcd else None
                if btd in tds:
                    cls['bases'].append(names.get(btd) or demangle_simple(tds[btd]) or tds[btd])
        for v in vts:
            cls['vtables'].append({'address': v['address'], 'offset': v['offset'], 'slots': v['slots']})
            for fn, disp in sorted(stores.get(v['address'], [])):
                cls['stores_vtable'].append([fn, disp])
        sizes = Counter()
        for fn, disp in cls['stores_vtable']:
            if disp == 0:
                sizes.update(new_sizes.get(fn, {}))
        cls['sizes'] = {str(k): v for k, v in sorted(sizes.items())}
        classes.append(cls)
    return {'type_info_vftable': vft, 'types': len(classes), 'classes': classes}


def demangle_simple(mangled):
    m = re.match(r'^\.\?A[VU](.+)@@$', mangled)
    if not m or '?' in m.group(1) or '$' in m.group(1):
        return None
    return '::'.join(reversed(m.group(1).split('@')))


# --- pass: libraries -------------------------------------------------------------------------------------

def sqlite_source(ctx):
    for p in (ctx.home / 'sqlite' / 'sqlite-amalgamation-3071502' / 'sqlite3.c',):
        if p.is_file():
            return p
    return None


def pass_libraries(ctx, img, facts, strings_by_addr):
    from . import csource
    by_entry = {f['entry']: f for f in facts}
    labels = {}

    def label(entry, lib, name=None, by=''):
        cur = labels.get(entry)
        if cur and cur.get('name') and not name:
            return
        labels[entry] = {'library': lib, 'name': name, 'by': by}

    def strings_of(f):
        return [strings_by_addr[a]['text'] for a in f['data_refs'] if a in strings_by_addr]

    # compiler-generated exception-handling funclets, and CRT/STL code named by Ghidra's Function ID
    for f in facts:
        if re.match(r'^(Unwind|Catch|Catch_All)@', f['name']):
            label(f['entry'], 'msvc-eh', None, 'eh-funclet')
        elif f['name_source'] == 'ANALYSIS' and not re.match(r'^(FUN|thunk_FUN|LAB)_', f['name']):
            label(f['entry'], 'stl' if f['name'].startswith(('std::', '_Uninitialized', 'Facet_Register'))
                  else 'crt', f['name'], 'function-id')
        if isinstance(f['thunk_target'], str) and f['thunk_target'].split('!')[0].startswith(('msvcr', 'msvcp')):
            label(f['entry'], 'crt', f['name'], 'import-thunk')

    # STL: functions that use the STL length/position error strings, and functions that only forward to them
    stl = set()
    for f in facts:
        if any(s in STL_ERRORS for s in strings_of(f)):
            stl.add(f['entry'])
    changed = True
    while changed:
        changed = False
        for f in facts:
            if f['entry'] not in stl and f['calls'] and set(f['calls']) <= stl and not f['imports'] \
                    and f['instructions'] <= 12 and not f['data_refs']:
                stl.add(f['entry'])
                changed = True
    for e in stl:
        label(e, 'stl', None, 'stl-error-string')

    # SQLite 3.7.15.2: error-report line numbers and unique strings from the amalgamation
    src = sqlite_source(ctx)
    sqlite_named = {}
    notes = []
    if src is None:
        notes.append('SQLite amalgamation 3.7.15.2 not found in $KMAP_HOME/sqlite/: SQLite functions not named')
    else:
        text = src.read_text(encoding='latin-1')
        funcs = csource.functions(text)
        line_owner = {}
        for fn in funcs:
            for L in range(fn['start'], fn['end'] + 1):
                line_owner[L] = fn['name']
        bk = set(csource.bkpt_lines(text))
        # the report functions are the call targets that receive the most known line numbers
        per_target = Counter()
        for f in facts:
            for target, pushes in f['call_seq']:
                if isinstance(target, int) and pushes and pushes[-1] in bk:
                    per_target[target] += 1
        reporters = {t for t, n in per_target.items() if n >= 3}
        claims = defaultdict(set)          # binary function -> source functions whose lines it contains
        for f in facts:
            for target, pushes in f['call_seq']:
                if target in reporters and pushes and pushes[-1] in bk:
                    claims[f['entry']].add(line_owner.get(pushes[-1]))
        calls = {fn['name']: set(fn['calls']) for fn in funcs}

        def closure(n, seen=None):
            seen = seen or set()
            for c in calls.get(n, ()):
                if c not in seen:
                    seen.add(c)
                    closure(c, seen)
            return seen
        for e, owners in claims.items():
            owners.discard(None)
            tops = [o for o in owners if owners - {o} <= closure(o)]
            if len(tops) == 1:
                sqlite_named.setdefault(tops[0], set()).add(e)
        # unique string literals
        str_owner = defaultdict(set)
        for fn in funcs:
            for s in fn['strings']:
                if len(s) >= 6:
                    str_owner[s].add(fn['name'])
        bin_users = defaultdict(set)
        for f in facts:
            for s in strings_of(f):
                bin_users[s].add(f['entry'])
        for s, owners in str_owner.items():
            if len(owners) == 1 and len(bin_users.get(s, ())) == 1:
                sqlite_named.setdefault(next(iter(owners)), set()).add(next(iter(bin_users[s])))
        # names claimed by more than one binary function were inlined: keep only unambiguous ones
        named = {n: next(iter(es)) for n, es in sqlite_named.items() if len(es) == 1}
        taken = Counter(named.values())
        named = {n: e for n, e in named.items() if taken[e] == 1}
        # one-call wrappers: `return X(args)` with constant arguments told apart by what they push
        wraps = csource.wrappers(funcs)
        for _ in range(3):
            rev = {e: n for n, e in named.items()}
            for wname, (callee, consts) in sorted(wraps.items()):
                if wname in named or callee not in named:
                    continue
                target = named[callee]
                cands = [f['entry'] for f in facts if f['calls'] == [target] and len(f['call_seq']) == 1
                         and sorted(f['call_seq'][0][1]) == sorted(consts) and f['instructions'] <= 16
                         and f['entry'] not in rev]
                if len(cands) == 1:
                    named[wname] = cands[0]
        for n, e in named.items():
            label(e, 'sqlite3', n, 'sqlite-source')
        if named:
            lo, hi = min(named.values()), max(named.values())
            for f in facts:
                if lo <= f['entry'] <= hi and f['entry'] not in labels:
                    label(f['entry'], 'sqlite3', None, 'sqlite-region')
        notes.append(f'SQLite: {len(named)} functions named from the amalgamation, {len(reporters)} report function(s)')

    # Ogg: the capture-pattern check, as a string reference or as the `OggS` dword compared inline
    # (Cube.exe: `cmp dword [ebx], 'OggS'` after checking for a 27-byte page header)
    text_va, text = img.section('.text')
    ogg_at = [text_va + m.start() for m in re.finditer(b'OggS', text)]
    for f in facts:
        if 'OggS' in strings_of(f) or any(a <= x < b for x in ogg_at for a, b in f['ranges']):
            label(f['entry'], 'ogg', None, 'OggS')
    out = {str(e): v for e, v in sorted(labels.items())}
    return {'functions': out, 'counts': dict(sorted(Counter(v['library'] for v in labels.values()).items())),
            'notes': notes}


# --- pass: strings and imports -----------------------------------------------------------------------------

HINTS = [(r'\.cub\b', 'rendering'), (r'\.plx\b', 'ui'), (r'\.(ogg|wav)\b', 'audio'), (r'(^|/)save/|\.db\b', 'saves'),
         (r'shader|direct3d|d3d|texture|\.fx\b', 'rendering'), (r'widget|button|menu|window', 'ui'),
         (r'server|client|connect|packet|socket', 'network'), (r'error|fail|warning|assert|invalid|%', 'log')]


def scan_strings(img):
    out = []
    for name, start, d in img.sections:
        if name not in ('.rdata', '.data'):
            continue
        for m in re.finditer(rb'[\x20-\x7E\t\r\n]{4,}(?=\x00)', d):
            out.append({'address': start + m.start(), 'text': m.group(0).decode('latin-1'), 'encoding': 'ascii'})
        for m in re.finditer(rb'(?:[\x20-\x7E]\x00){4,}(?=\x00\x00)', d):
            if (start + m.start()) % 2 == 0:
                out.append({'address': start + m.start(), 'text': m.group(0).decode('utf-16-le'), 'encoding': 'utf16'})
    out.sort(key=lambda s: (s['address'], s['encoding']))
    return out


def hint(text):
    t = text.lower()
    return sorted({h for rx, h in HINTS if re.search(rx, t)})


def pass_strings(img, facts):
    strings = scan_strings(img)
    by_addr = {s['address']: s for s in strings}
    refs = defaultdict(set)
    for f in facts:
        for a in f['data_refs']:
            if a in by_addr:
                refs[a].add(f['entry'])
    for s in strings:
        s['functions'] = sorted(refs.get(s['address'], ()))
        s['hints'] = hint(s['text'])
    thunks = {f['entry']: f['thunk_target'] for f in facts if isinstance(f['thunk_target'], str)}
    imports = defaultdict(set)
    for f in facts:
        for imp in f['imports']:
            if f['entry'] not in thunks:
                imports[imp].add(f['entry'])
        for c in f['calls']:
            if c in thunks:
                imports[thunks[c]].add(f['entry'])
    return ({'strings': strings},
            {'imports': {k: sorted(v) for k, v in sorted(imports.items())}})


# --- pass: correlation ---------------------------------------------------------------------------------------

def pass_correlation(ctx, facts_a, facts_b, strings_a, strings_b):
    """facts_a: Cube.exe, facts_b: Server.exe. Exact matchers first, then similarity."""
    A = {f['entry']: f for f in facts_a}
    B = {f['entry']: f for f in facts_b}
    matches = {}
    used_b = set()

    def add(a, b, matcher, score):
        if a in matches or b in used_b:
            return False
        matches[a] = (b, matcher, score)
        used_b.add(b)
        return True

    for key, matcher, score in (('bytes_sha1', 'exact-bytes', 1.0), ('mnemonics_sha1', 'exact-instructions', 0.9)):
        ha = defaultdict(list)
        hb = defaultdict(list)
        for f in facts_a:
            if f['entry'] not in matches and f['instructions'] >= 4:
                ha[f[key]].append(f['entry'])
        for f in facts_b:
            if f['entry'] not in used_b and f['instructions'] >= 4:
                hb[f[key]].append(f['entry'])
        for h, la in ha.items():
            lb = hb.get(h, [])
            if len(la) == 1 and len(lb) == 1:
                add(la[0], lb[0], matcher, score)

    def text_refs(f, sb):
        return {sb[a]['text'] for a in f['data_refs'] if a in sb and len(sb[a]['text']) >= 6}
    sa = {s['address']: s for s in strings_a}
    sb = {s['address']: s for s in strings_b}
    users_a, users_b = defaultdict(set), defaultdict(set)
    for f in facts_a:
        for t in text_refs(f, sa):
            users_a[t].add(f['entry'])
    for f in facts_b:
        for t in text_refs(f, sb):
            users_b[t].add(f['entry'])
    for t in sorted(set(users_a) & set(users_b)):
        if len(users_a[t]) == 1 and len(users_b[t]) == 1:
            a, b = next(iter(users_a[t])), next(iter(users_b[t]))
            fa, fb = A[a], B[b]
            ratio = min(fa['instructions'], fb['instructions']) / max(fa['instructions'], fb['instructions'], 1)
            if ratio >= 0.6:
                add(a, b, 'unique-string', round(0.5 + 0.4 * ratio, 3))

    def similar(fa, fb):
        r = min(fa['instructions'], fb['instructions']) / max(fa['instructions'], fb['instructions'], 1)
        return r if len(fa['calls']) == len(fb['calls']) or r >= 0.9 else 0

    for _ in range(6):     # call-graph propagation from matched pairs
        new = 0
        for a, (b, _, _) in sorted(matches.items()):
            ca = [c for c, _ in A[a]['call_seq'] if isinstance(c, int) and c in A]
            cb = [c for c, _ in B[b]['call_seq'] if isinstance(c, int) and c in B]
            if len(ca) != len(cb):
                continue
            for x, y in zip(ca, cb):
                if x in matches or y in used_b:
                    continue
                r = similar(A[x], B[y])
                if r >= 0.75 and add(x, y, 'call-graph', round(0.4 + 0.4 * r, 3)):
                    new += 1
        if not new:
            break
    rows = [{'cube': a, 'server': b, 'matcher': m, 'score': s} for a, (b, m, s) in sorted(matches.items())]
    return {'matches': rows, 'counts': dict(sorted(Counter(r['matcher'] for r in rows).items())),
            'server_functions': len(facts_b), 'cube_functions': len(facts_a)}


def correlation_proposals(ctx, db, corr, cube='cube-0x51D2CE9C', server='server-0x51D2CE9B'):
    """A name mapped in one build and missing at the matched address of the other -> candidate proposal."""
    at = {bid: {e['address']: e for e in db.functions(bid)} for bid in (cube, server)}
    props = []
    for m in corr['matches']:
        for src, dst, sa, da in ((cube, server, m['cube'], m['server']), (server, cube, m['server'], m['cube'])):
            e = at[src].get(sa)
            if e and dbmod.exported(e) and da not in at[dst] and e.get('kind', 'function') == 'function':
                props.append({'build': dst, 'address': da, 'name': e['name'], 'from': src, 'matcher': m['matcher'],
                              'score': m['score'], 'status': 'candidate', 'evidence': f'correlated:{src}:{e["name"]}',
                              'subsystem': e['subsystem'], 'convention': e['convention'],
                              'prototype': e.get('prototype')})
    return sorted(props, key=lambda p: (p['build'], p['address']))


# --- pass: coverage ------------------------------------------------------------------------------------------

def pass_coverage(ctx, db, bid, facts, libs, rtti):
    lib = {int(e) for e in libs['functions']}
    game = [f for f in facts if f['entry'] not in lib]
    mapped = {e['address']: e for e in db.functions(bid)}
    typed = {a for a, e in mapped.items() if e.get('prototype')}
    per_sub = defaultdict(Counter)
    for e in db.symbols(bid):
        per_sub[e['subsystem']]['entries'] += 1
        per_sub[e['subsystem']][e['status']] += 1
        if e['_kind'] == 'function' and e.get('kind', 'function') == 'function':
            per_sub[e['subsystem']]['functions'] += 1
            if e.get('prototype'):
                per_sub[e['subsystem']]['typed'] += 1
    classes = db.classes(bid)
    type_by = {t['name']: t for t in db.types}
    return {
        'functions': {'total': len(facts), 'library': len(lib & {f['entry'] for f in facts}),
                      'game': len(game), 'named': sum(1 for f in facts if f['entry'] in mapped),
                      'typed': sum(1 for f in facts if f['entry'] in typed)},
        'classes': {'rtti': rtti['types'], 'with_vtables': sum(1 for c in rtti['classes'] if c['vtables']),
                    'mapped': len(classes),
                    'with_sizes': sum(1 for c in classes if (type_by.get(c['name']) or {}).get('size') or c.get('size')),
                    'with_fields': sum(1 for c in classes if (type_by.get(c['name']) or {}).get('field'))},
        'subsystems': {s: dict(sorted(per_sub[s].items())) for s in sorted(per_sub)},
    }


# --- the command -----------------------------------------------------------------------------------------------

def cmd_survey(ctx, passes=None, builds=None):
    todo = list(passes or PASSES)
    for p in todo:
        if p not in PASSES:
            raise KmapError(f'unknown pass {p!r}; passes: {", ".join(PASSES)}')
    todo = [p for p in PASSES if p in todo]
    ids = builds or list(ctx.ws.builds)
    ghidra.require_ghidra(ctx)
    db = dbmod.load(ctx)
    if 'facts' in todo:
        if not (ctx.project_dir / 'kyubu.gpr').is_file():
            raise KmapError('no Ghidra project: run `kmap init --game <folder>` first')
        say('facts: reading every function from the Ghidra project')
        ghidra.run_worker(ctx, 'survey', {'project': str(ctx.project_dir), 'builds': ids,
                                          'out': str(ctx.survey_dir)}, quiet=True)
    data = {}
    for bid in ids:
        build = ctx.ws.build(bid)
        img = Image(load_input(ctx, build), build)
        facts = load_facts(ctx, bid)
        d = data[bid] = {'img': img, 'facts': facts}
        gsym = load_json(survey_path(ctx, bid, 'ghidra-rtti.json'))
        if 'rtti' in todo:
            d['rtti'] = pass_rtti(img, facts, gsym)
            save(survey_path(ctx, bid, 'rtti.json'), d['rtti'])
            say(f'rtti: {bid}: {d["rtti"]["types"]} RTTI types, '
                f'{sum(1 for c in d["rtti"]["classes"] if c["vtables"])} with vtables')
        if 'strings' in todo or 'libraries' in todo:
            strings, imports = pass_strings(img, facts)
            d['strings'] = strings['strings']
        if 'libraries' in todo:
            by_addr = {s['address']: s for s in d['strings']}
            d['libraries'] = pass_libraries(ctx, img, facts, by_addr)
            save(survey_path(ctx, bid, 'libraries.json'), d['libraries'])
            say(f'libraries: {bid}: {d["libraries"]["counts"]}; ' + '; '.join(d['libraries']['notes']))
        if 'strings' in todo:
            save(survey_path(ctx, bid, 'strings.json'), strings)
            save(survey_path(ctx, bid, 'imports.json'), imports)
            say(f'strings: {bid}: {len(strings["strings"])} strings, {len(imports["imports"])} imports')
    pair = [next((i for i in ids if ctx.ws.build(i).file == f), None) for f in ('Cube.exe', 'Server.exe')]
    if 'correlation' in todo and len(ids) == 2 and None not in pair:
        a, b = pair            # the surveyed Cube.exe and Server.exe
        sa = data[a].get('strings') or load_json(survey_path(ctx, a, 'strings.json'))['strings']
        sb = data[b].get('strings') or load_json(survey_path(ctx, b, 'strings.json'))['strings']
        corr = pass_correlation(ctx, data[a]['facts'], data[b]['facts'], sa, sb)
        save(ctx.survey_dir / 'correlation.json', corr)
        props = correlation_proposals(ctx, db, corr, a, b)
        save(ctx.survey_dir / 'correlation-proposals.json', props)
        say(f'correlation: {len(corr["matches"])} of {corr["server_functions"]} Server.exe functions matched '
            f'{corr["counts"]}; {len(props)} name proposal(s)')
    if 'cuwo' in todo:
        from . import cuwo
        cuwo.survey_pass(ctx)
    if 'coverage' in todo:
        for bid in ids:
            d = data[bid]
            libs = d.get('libraries') or load_json(survey_path(ctx, bid, 'libraries.json'))
            rtti = d.get('rtti') or load_json(survey_path(ctx, bid, 'rtti.json'))
            cov = pass_coverage(ctx, db, bid, d['facts'], libs, rtti)
            save(survey_path(ctx, bid, 'coverage.json'), cov)
            say(f'coverage: {bid}: {cov["functions"]}')
    return 0


# --- queries --------------------------------------------------------------------------------------------------

def _namer(ctx, bid):
    db = dbmod.load(ctx)
    facts = {f['entry']: f for f in load_facts(ctx, bid)}
    mapped = {e['address']: e['name'] for e in db.symbols(bid)}

    def name(a):
        if a in mapped:
            return f'0x{a:X} {mapped[a]}'
        f = facts.get(a)
        return f'0x{a:X} {f["name"]}' if f else f'0x{a:X}'
    return name, facts


def cmd_query(ctx, args):
    bid = args.build
    ctx.ws.build(bid)
    name, facts = _namer(ctx, bid)
    if args.string is not None:
        strings = load_json(survey_path(ctx, bid, 'strings.json'))['strings']
        hits = [s for s in strings if args.string in s['text']]
        for s in hits[:200]:
            print(f'0x{s["address"]:X} {s["encoding"]:5} {s["text"][:100]!r}  hints: {",".join(s["hints"]) or "-"}')
            for fn in s['functions']:
                print(f'    referenced by {name(fn)}')
        say(f'{len(hits)} string(s)')
        return 0 if hits else 1
    if args.imp is not None:
        imports = load_json(survey_path(ctx, bid, 'imports.json'))['imports']
        key = args.imp.lower() if '!' not in args.imp else args.imp.split('!')[0].lower() + '!' + args.imp.split('!')[1]
        hits = {k: v for k, v in imports.items() if k.lower() == key.lower()}
        for k, callers in hits.items():
            print(k)
            for c in callers:
                print(f'    called by {name(c)}')
        say(f'{sum(len(v) for v in hits.values())} caller(s)')
        return 0 if hits else 1
    a = int(args.function, 0)
    f = facts.get(a)
    if f is None:
        raise KmapError(f'no function starts at 0x{a:X} in {bid}')
    callers = [e for e, g in facts.items() if a in g['calls']]
    print(f'{name(a)}: {f["size"]} bytes, {f["instructions"]} instructions')
    print('  called by: ' + ', '.join(name(c) for c in sorted(callers)))
    print('  calls:     ' + ', '.join(name(c) for c in f['calls']))
    print('  imports:   ' + ', '.join(f['imports']))
    return 0


# --- commit: reviewed survey results into mapping/ -------------------------------------------------------------

SUBSYSTEM_BY_NAME = [(r'Widget$|^plasma::', 'ui'), (r'Behavior$', 'ai'), (r'XAudio2|Audio|Music|Sound|Speech$', 'audio'),
                     (r'Shader|Sprite|Voxel', 'rendering'), (r'^cube::(World|Zone|ZoneTile|Region|Chunk|ChunkBuffer|'
                                                            r'LandscapeTile|Field|House|Dungeon|WorldInfo)$', 'world'),
                     (r'WorldMap|MapOverlay', 'travel'), (r'Creature|Spawn', 'creatures'), (r'Quest', 'core'),
                     (r'Controller$', 'input'), (r'^cube::Server$|Connection', 'network'), (r'^std::|^Concurrency::|'
                                                                                           r'^abstr::|type_info|lambda', 'library')]


def subsystem_for(name):
    for rx, sub in SUBSYSTEM_BY_NAME:
        if re.search(rx, name):
            return sub
    return 'core'


def classes_toml(bid, rtti):
    lines = [f'# {bid}: every RTTI class with its vtables (generated by `kmap survey commit`, then reviewed).',
             '# Slots are function addresses in slot order; constructor/destructor candidates stay in the survey.', '']
    for c in rtti['classes']:
        lines += ['[[class]]', f'name = {toml_str(c["name"])}', f'rtti = {toml_str(c["mangled"])}',
                  f'type_descriptor = 0x{c["type_descriptor"]:X}']
        if c['bases']:
            lines.append('bases = [' + ', '.join(toml_str(b) for b in c['bases']) + ']')
        lines += [f'subsystem = "{subsystem_for(c["name"])}"', 'status = "verified"',
                  f'evidence = [{toml_str("rtti:" + c["name"][:74])}]', 'source = "rtti"', '']
        for v in c['vtables']:
            lines += ['[[class.vtable]]', f'address = 0x{v["address"]:X}']
            if v['offset']:
                lines.append(f'offset = 0x{v["offset"]:X}')
            slots = ', '.join(f'0x{s:X}' for s in v['slots'])
            lines += [f'slots = [{slots}]', '']
    return '\n'.join(lines)


def library_entries(ctx, db, bid, libs):
    existing = {e['address'] for e in db.functions(bid)}
    names = {e['name'] for e in db.functions(bid)} | {a for e in db.functions(bid) for a in e.get('aliases', [])}
    out = []
    for entry, v in sorted(libs['functions'].items(), key=lambda kv: int(kv[0])):
        a = int(entry)
        if v['library'] != 'sqlite3' or not v['name'] or a in existing or v['name'] in names:
            continue
        out.append('\n'.join(['[[function]]', f'name = {toml_str(v["name"])}', f'address = 0x{a:X}',
                              'convention = "cdecl"', 'subsystem = "library"', 'status = "verified"',
                              'evidence = ["library:sqlite3", "manual:named from SQLite 3.7.15.2 source anchors"]',
                              'source = "survey"']))
    return out


def cmd_commit(ctx, write=False):
    db = dbmod.load(ctx)
    files = {}
    for bid in ctx.ws.builds:
        rtti = load_json(survey_path(ctx, bid, 'rtti.json'))
        files[f'mapping/{bid}/classes.toml'] = classes_toml(bid, rtti)
        libs = load_json(survey_path(ctx, bid, 'libraries.json'))
        entries = library_entries(ctx, db, bid, libs)
        rel = f'mapping/{bid}/functions/library.toml'
        cur = (ctx.repo / rel).read_text(encoding='utf-8') if (ctx.repo / rel).is_file() else ''
        if entries:
            files[rel] = cur.rstrip('\n') + '\n\n' + '\n\n'.join(entries) + '\n'
        say(f'{bid}: {len(rtti["classes"])} classes, {len(entries)} new library function(s)')
    for rel, text in sorted(files.items()):
        if write:
            if write_if_changed(ctx.repo / rel, text):
                say(f'wrote {rel}')
        else:
            say(f'would write {rel} ({text.count(chr(10))} lines)')
    if not write:
        say('run `kmap survey commit --write` to write them, then review the diff and `kmap check`')
    return 0
