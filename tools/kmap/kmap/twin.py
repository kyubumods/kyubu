"""kmap twin: a debug build of Infinite-Cubes matched to Cube.exe (openspec change add-twin-matching).

  kmap twin init [--variant <id>] [--rebuild]   import the twin's Cube.exe with its PDB and analyse it
  kmap twin survey [--variant <id>]             facts, PDB names, source files, RTTI, strings and struct types
  kmap twin match [--variant <id>]              pair twin functions with Cube.exe's: matches.json, names.tsv
  kmap twin match --combine                     also vote over the variants by twin name: twin/combined/

The twin is described by $KMAP_HOME/twin/build.toml (written when it was built). Its Ghidra project is
$KMAP_HOME/twin/ghidra/, one program per variant id; kmap's own project and mapping/ are never touched.
Output goes to $KMAP_HOME/twin/<variant id>/.
"""
import json
import re
import tomllib
from collections import Counter, defaultdict
from dataclasses import dataclass

from . import ghidra
from . import survey as sv
from .util import KmapError, dump_json, say, sha256_file, write_if_changed

# Folders of the Infinite-Cubes alpha/ tree that hold game code (the rest is third-party code or platform shims)
GAME_DIRS = ('src/', 'include/', 'plasma/')
# The game's namespaces: a function without line information (a compiler-generated destructor or operator=) whose
# PDB name is in one of these is game code
GAME_NAMESPACES = ('cube::', 'plasma::', 'abstr::')


def twin_dir(ctx):
    return ctx.home / 'twin'


def project_dir(ctx):
    return twin_dir(ctx) / 'ghidra'


def marker_path(ctx):
    return project_dir(ctx) / 'twin-project.json'


def load_record(ctx):
    p = twin_dir(ctx) / 'build.toml'
    if not p.is_file():
        raise KmapError(f'{p} not found: build the twin first (add-twin-matching task 1.1)')
    rec = tomllib.loads(p.read_text(encoding='utf-8'))
    if not rec.get('variant'):
        raise KmapError(f'{p} lists no [[variant]]')
    return rec


def pick_variants(rec, wanted):
    known = {v['id']: v for v in rec['variant']}
    if not wanted:
        return list(known.values())
    unknown = [w for w in wanted if w not in known]
    if unknown:
        raise KmapError(f'unknown variant(s) {", ".join(unknown)}; build.toml has {", ".join(known)}')
    return [known[w] for w in wanted]


def variant_files(ctx, v):
    """The variant's Cube.exe and Cube.pdb, checked against the hashes in build.toml."""
    folder = twin_dir(ctx) / v['folder']
    out = []
    for name, key in (('Cube.exe', 'cube_exe_sha256'), ('Cube.pdb', 'cube_pdb_sha256')):
        p = folder / name
        if not p.is_file():
            raise KmapError(f'{p} not found')
        if sha256_file(p) != v[key]:
            raise KmapError(f'{p}: SHA-256 differs from build.toml ({key}); a rebuild needs a new build.toml entry')
        out.append(p)
    return out


def alpha_root(ctx, rec):
    src = rec['source']
    return ctx.home / src['path'] / src['folder']


def normalise_path(path):
    """Lower case, forward slashes, no leading slash before a drive letter (Ghidra writes /D:/...)."""
    p = str(path).replace('\\', '/').lower()
    return p[1:] if re.match(r'^/[a-z]:/', p) else p


def game_name(name):
    """A PDB name in one of the game's namespaces (local statics are written `cube::f'::`2'::...)."""
    return bool(name) and name.lstrip('`').startswith(GAME_NAMESPACES)


def part_of(source_file, root, pdb_name=None):
    """'game', 'library' or 'none' for a function, by its source file, else by its PDB name's namespace."""
    if not source_file:
        return 'game' if game_name(pdb_name) else 'none'
    p = normalise_path(source_file)
    r = normalise_path(root).rstrip('/') + '/'
    if p.startswith(r) and p[len(r):].startswith(GAME_DIRS):
        return 'game'
    return 'library'


@dataclass(frozen=True)
class TwinImage:
    """The few build fields survey.Image and its passes use, read from the twin's PE header."""
    image_base: int
    image_size: int
    text_start: int
    text_end: int

    def in_image(self, a):
        return self.image_base <= a < self.image_base + self.image_size

    def in_text(self, a):
        return self.text_start <= a < self.text_end


def load_image(exe):
    import pefile
    data = exe.read_bytes()
    pe = pefile.PE(data=data, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    info = TwinImage(base, pe.OPTIONAL_HEADER.SizeOfImage, base + text.VirtualAddress,
                     base + text.VirtualAddress + text.Misc_VirtualSize)
    return sv.Image(data, info)


def vtable_classes(rtti):
    return {c['name'] for c in rtti['classes'] if c['vtables']}


def summarise(facts, root, twin_rtti, cube_rtti):
    part = {f['entry']: part_of(f['source_file'], root, f['pdb_name']) for f in facts}
    game = [f for f in facts if part[f['entry']] == 'game']
    tv, cv = vtable_classes(twin_rtti), vtable_classes(cube_rtti)
    gcv = {n for n in cv if game_name(n)}
    return {
        'functions': len(facts),
        'pdb_named': sum(1 for f in facts if f['pdb_name']),
        'parts': dict(sorted(Counter(part.values()).items())),
        'game_pdb_named': sum(1 for f in game if f['pdb_name']),
        'game_by_name_only': sum(1 for f in game if not f['source_file']),
        'rtti': {'twin_vtable_classes': len(tv), 'cube_vtable_classes': len(cv), 'same_name': len(tv & cv),
                 'cube_game_vtable_classes': len(gcv), 'game_same_name': len(gcv & tv),
                 'only_cube': sorted(cv - tv), 'only_twin': sorted(tv - cv)},
    }


# --- commands -----------------------------------------------------------------------------------------------

def read_marker(ctx):
    p = marker_path(ctx)
    return json.loads(p.read_text(encoding='utf-8')) if p.is_file() else None


def cmd_init(ctx, wanted=None, rebuild=False):
    rec = load_record(ctx)
    todo = pick_variants(rec, wanted)
    files = {v['id']: variant_files(ctx, v) for v in todo}
    ghidra.require_ghidra(ctx)
    ws = ctx.ws
    toolchain = {'ghidra': ws.ghidra.version, 'analysis': ws.analysis}
    marker = read_marker(ctx)
    if marker and {k: marker.get(k) for k in toolchain} != toolchain:
        raise KmapError(f'the twin project in {project_dir(ctx)} was made with other toolchain settings; '
                        'delete it and run `kmap twin init` again')
    done = (marker or {}).get('programs', {})
    if not rebuild:
        todo = [v for v in todo if done.get(v['id'], {}).get('exe_sha256') != v['cube_exe_sha256']]
    if not todo:
        say(f'the twin project in {project_dir(ctx)} is up to date; nothing to do (use --rebuild to redo)')
        return 0
    result = ghidra.run_worker(ctx, 'twin_init', {
        'project': str(project_dir(ctx)), 'language': ws.analysis['language'], 'compiler': ws.analysis['compiler'],
        'options': ws.analysis.get('options', {}),
        'programs': [{'id': v['id'], 'exe': str(files[v['id']][0]), 'pdb': str(files[v['id']][1])} for v in todo]})
    marker = dict(toolchain, programs=dict(done))
    for v in todo:
        r = result['programs'][v['id']]
        marker['programs'][v['id']] = {'exe_sha256': v['cube_exe_sha256'], 'pdb_sha256': v['cube_pdb_sha256'],
                                       'functions': r['functions'], 'pdb_named': r['pdb_named'],
                                       'analysis_seconds': r['seconds']}
        say(f'{v["id"]}: {r["functions"]} functions, {r["pdb_named"]} named by the PDB, analysis {r["seconds"]} s')
    write_if_changed(marker_path(ctx), dump_json(marker))
    say('Next: `kmap twin survey`.')
    return 0


def cmd_survey(ctx, wanted=None):
    rec = load_record(ctx)
    todo = pick_variants(rec, wanted)
    marker = read_marker(ctx) or {}
    missing = [v['id'] for v in todo if v['id'] not in marker.get('programs', {})]
    if missing:
        raise KmapError(f'not in the twin project: {", ".join(missing)}; run `kmap twin init` first')
    cube = rec['source']['rebuilds']
    cube_rtti = sv.load_json(sv.survey_path(ctx, cube, 'rtti.json'))
    ghidra.run_worker(ctx, 'twin_survey', {'project': str(project_dir(ctx)), 'programs': [v['id'] for v in todo],
                                           'out': str(twin_dir(ctx))}, quiet=True)
    root = alpha_root(ctx, rec)
    for v in todo:
        out = twin_dir(ctx) / v['id']
        exe, _ = variant_files(ctx, v)
        img = load_image(exe)
        facts = sv.load_json(out / 'functions.json')
        gsym = sv.load_json(out / 'ghidra-rtti.json')
        rtti = sv.pass_rtti(img, facts, gsym)
        sv.save(out / 'rtti.json', rtti)
        strings, imports = sv.pass_strings(img, facts)
        sv.save(out / 'strings.json', strings)
        sv.save(out / 'imports.json', imports)
        summary = summarise(facts, root, rtti, cube_rtti)
        sv.save(out / 'summary.json', summary)
        r = summary['rtti']
        say(f'{v["id"]}: {summary["functions"]} functions, {summary["pdb_named"]} named by the PDB; '
            f'parts {summary["parts"]} ({summary["game_by_name_only"]} game by name only), '
            f'{summary["game_pdb_named"]} game functions named; vtable classes: {r["same_name"]} of '
            f'{r["cube_vtable_classes"]} Cube.exe names also in the twin, game classes {r["game_same_name"]} of '
            f'{r["cube_game_vtable_classes"]} ({r["twin_vtable_classes"]} in the twin); '
            f'{len(strings["strings"])} strings; in {out}')
    return 0


# --- matching -------------------------------------------------------------------------------------------------

SCORES = {'vtable': 0.95, 'import': 0.85, 'string': 0.75, 'constants': 0.65, 'callers': 0.55, 'pairs': 1.0}
CALLGRAPH_FACTOR = 0.85
WEAK_BELOW = 0.5
SIZE_RATIO = {'string': 0.4, 'constants': 0.4, 'callgraph': 0.5}
# The callgraph skips functions shorter than this many bytes on either side. Off (0): in the second hand sample the pairs
# it removed were about as often right as the ones it kept (task 1.4), and they included the lock and unlock helpers.
CALLGRAPH_TINY = 0
STRING_MIN = 6
CONSTANTS_MIN = 3
# A pushed scalar is a "rare immediate" above 0xFFFF and outside the image's address range, held by few functions
RARE_MIN = 0x10000
ADDRESS_RANGE = (0x400000, 0x1000000)
RARE_MAX_USERS = 20
THIRD_PARTY_DLLS = ('zlib', 'freeimage')
MAX_ROUNDS = 50


def func_size(f):
    return sum(b - a for a, b in f['ranges'])


def size_ratio(a, b):
    sa, sb = func_size(a), func_size(b)
    return min(sa, sb) / max(sa, sb, 1)


def twin_eligible(f, root):
    """Game code, plus library code that is an instantiation over game types (STL templates in MSVC headers)."""
    part = part_of(f.get('source_file'), root, f.get('pdb_name'))
    if part == 'game':
        return True
    name = f.get('pdb_name') or ''
    src = normalise_path(f.get('source_file') or '')
    return part == 'library' and '/microsoft visual studio/' in src and any(ns in name for ns in GAME_NAMESPACES)


def data_consts(facts, eligible, img):
    """{entry: set of 32-bit values} read from the read-only data a function refers to. Compilers keep float and double
    constants there (the game's x87 code and the twin's SSE code both load them from memory), so the values are the same
    on both sides. Small values are left to the caller's rarity filter."""
    ro = [(start, start + len(d)) for name, start, d in img.sections if name == '.rdata']
    out = {}
    for f in facts:
        e = f['entry']
        if e not in eligible:
            continue
        vals = set()
        for a in f['data_refs']:
            if any(lo <= a < hi for lo, hi in ro):
                for off in (0, 4):
                    v = img.u32(a + off)
                    if v is not None:
                        vals.add(v)
        out[e] = vals
    return out


def import_key(key):
    """'msvcr110.dll!_foo@8' and 'vcruntime140.dll!foo' are the same import: the name, lower case, no decoration."""
    name = key.rsplit('!', 1)[-1].lstrip('_')
    return re.sub(r'@\d+$', '', name).lower()


def _callers(facts, eligible):
    out = defaultdict(set)
    for f in facts.values():
        if f['entry'] in eligible:
            for c in f['calls']:
                out[c].add(f['entry'])
    return out


class Matcher:
    """One-to-one pairing of the twin's functions with Cube.exe's. cube / twin_side: {'facts': [..], 'eligible': set,
    'rtti': {..}, 'strings': [..], 'imports': {..}}."""

    def __init__(self, cube, twin_side, pairs=(), seeds=()):
        self.A = {f['entry']: f for f in cube['facts']}
        self.B = {f['entry']: f for f in twin_side['facts']}
        self.ea = {e for e in cube['eligible'] if e in self.A}
        self.eb = {e for e in twin_side['eligible'] if e in self.B}
        self.cube, self.twin = cube, twin_side
        self.by_c, self.by_t = {}, {}
        self.conflicts = {}
        self.dirty = None      # parents whose callees may pair up now: None = all, else a set of cube entries
        self.callers_a = _callers(self.A, self.ea)
        self.callers_b = _callers(self.B, self.eb)
        self.shared = set()    # Cube.exe functions that sit in vtable slots of several classes (folded stubs)
        for c, t in pairs:
            if c in self.ea and t in self.eb and c not in self.by_c and t not in self.by_t:
                self._add(c, t, 'pairs', SCORES['pairs'], None, [])
        # Pairs that other variants of the twin found (combine_variants): anchors at the score they had there
        for c, t, matcher, score in seeds:
            if c in self.ea and t in self.eb and c not in self.by_c and t not in self.by_t:
                self._add(c, t, matcher, score, None, [], seed=True)

    # -- bookkeeping
    def _add(self, c, t, matcher, score, parent, also, seed=False):
        row = {'cube': c, 'twin': t, 'twin_name': self.B[t].get('pdb_name') or self.B[t]['name'], 'matcher': matcher,
               'score': round(score, 3), 'weak': score < WEAK_BELOW}
        if seed:
            row['seed'] = True
        if parent:
            row['from'] = {'cube': parent[0], 'twin': parent[1]}
        if also:
            row['also'] = sorted(also)
        self.by_c[c] = self.by_t[t] = row
        if self.dirty is not None:
            self.dirty.add(c)
            self.dirty.update(self.callers_a.get(c, ()))

    def resolve(self, cands):
        """cands: [(cube, twin, matcher, score, parent)]. A function with two different partners is disputed: all its
        pairings are dropped and listed as conflicts. Returns the number of rows added."""
        cands = [x for x in cands if x[0] not in self.by_c and x[1] not in self.by_t]
        partners_c, partners_t = defaultdict(set), defaultdict(set)
        for c, t, *_ in cands:
            partners_c[c].add(t)
            partners_t[t].add(c)
        best = {}
        for c, t, m, sc, par in cands:
            if len(partners_c[c]) > 1 or len(partners_t[t]) > 1:
                self.conflicts[(c, t, m)] = {'cube': c, 'twin': t, 'matcher': m, 'score': round(sc, 3)}
                continue
            if (c, t) not in best:
                best[(c, t)] = [m, sc, par, []]
            else:
                b = best[(c, t)]
                if sc > b[1]:
                    b[3].append(b[0])
                    b[0], b[1], b[2] = m, sc, par
                else:
                    b[3].append(m)
        for (c, t), (m, sc, par, also) in sorted(best.items()):
            self._add(c, t, m, sc, par, [x for x in also if x != m])
        return len(best)

    def ok_size(self, c, t, matcher):
        need = SIZE_RATIO.get(matcher)
        return need is None or size_ratio(self.A[c], self.B[t]) >= need

    def comparable(self, c, t):
        """The callgraph pairs by position among unmatched callees, so it cannot tell apart a trivial function (a getter
        of a few bytes) from another, nor a stub the linker folded into several vtable slots."""
        return c not in self.shared and min(func_size(self.A[c]), func_size(self.B[t])) >= CALLGRAPH_TINY

    # -- anchor matchers
    def m_vtable(self):
        tcls = {}
        for k in self.twin['rtti']['classes']:
            if k['vtables']:
                tcls.setdefault(k['name'], k)
        out = []
        for k in self.cube['rtti']['classes']:
            o = tcls.get(k['name'])
            if not (k['vtables'] and o):
                continue
            tv = {v['offset']: v for v in o['vtables']}
            for v in k['vtables']:
                w = tv.get(v['offset'])
                if not w:
                    continue
                for c, t in zip(v['slots'], w['slots']):
                    if c in self.ea and t in self.eb:
                        out.append((c, t, 'vtable', SCORES['vtable'], None))
        # A function the linker folded into the slots of several classes has several twin partners: it names none of them.
        partners = defaultdict(set)
        for c, t, *_ in out:
            partners[c].add(t)
        self.shared = {c for c, ts in partners.items() if len(ts) > 1}
        return [x for x in out if x[0] not in self.shared]

    def m_import(self):
        def users(imports, eligible):
            by = defaultdict(set)
            for k, callers in imports.items():
                by[import_key(k)].update(x for x in callers if x in eligible)
            return by
        ua, ub = users(self.cube['imports'], self.ea), users(self.twin['imports'], self.eb)
        # the twin links zlib and FreeImage statically: the callers of its function of the same PDB name
        static = defaultdict(set)
        for e, f in self.B.items():
            n = f.get('pdb_name')
            if n and e not in self.eb:
                static[import_key(n)].add(e)
        for k in self.cube['imports']:
            if k.split('!', 1)[0].startswith(THIRD_PARTY_DLLS):
                key = import_key(k)
                if len(static.get(key, ())) == 1:
                    ub[key] |= self.callers_b.get(next(iter(static[key])), set()) & self.eb
        out = []
        for key, ca in ua.items():
            cb = ub.get(key, ())
            if len(ca) == 1 and len(cb) == 1:
                out.append((next(iter(ca)), next(iter(cb)), 'import', SCORES['import'], None))
        return out

    def m_string(self):
        def users(strings, eligible):
            by = defaultdict(set)
            for s in strings:
                if len(s['text']) >= STRING_MIN:
                    by[s['text']].update(x for x in s['functions'] if x in eligible)
            return by
        ua, ub = users(self.cube['strings'], self.ea), users(self.twin['strings'], self.eb)
        out = []
        for text, ca in ua.items():
            cb = ub.get(text, ())
            if len(ca) == 1 and len(cb) == 1:
                c, t = next(iter(ca)), next(iter(cb))
                if self.ok_size(c, t, 'string'):
                    out.append((c, t, 'string', SCORES['string'], None))
        return out

    def m_constants(self):
        def sets(facts, eligible, extra):
            held = {e: {v for _, args in facts[e]['call_seq'] for v in args
                        if v >= RARE_MIN and not ADDRESS_RANGE[0] <= v < ADDRESS_RANGE[1]} | extra.get(e, set())
                    for e in eligible}
            users = Counter(v for vals in held.values() for v in vals)
            by = defaultdict(set)
            for e, vals in held.items():
                rare = frozenset(v for v in vals if users[v] <= RARE_MAX_USERS)
                if len(rare) >= CONSTANTS_MIN:
                    by[rare].add(e)
            return by
        ba = sets(self.A, self.ea, self.cube.get('consts', {}))
        bb = sets(self.B, self.eb, self.twin.get('consts', {}))
        out = []
        for key, ca in ba.items():
            cb = bb.get(key, ())
            if len(ca) == 1 and len(cb) == 1:
                c, t = next(iter(ca)), next(iter(cb))
                if self.ok_size(c, t, 'constants'):
                    out.append((c, t, 'constants', SCORES['constants'], None))
        return out

    # -- propagating matchers
    def m_callgraph(self):
        """The callees of each strong pair, aligned by a longest common subsequence of already matched callees; the
        unmatched callees between two aligned ones pair up in order when both gaps hold the same number."""
        out = []
        todo = self.dirty
        self.dirty = set()
        parents = sorted((r for r in self.by_c.values() if not r['weak'] and (todo is None or r['cube'] in todo)),
                         key=lambda r: (-r['score'], r['cube']))
        for r in parents:
            seq_a = [x for x, _ in self.A[r['cube']]['call_seq'] if isinstance(x, int) and x in self.ea]
            seq_b = [x for x, _ in self.B[r['twin']]['call_seq'] if isinstance(x, int) and x in self.eb]
            n, m = len(seq_a), len(seq_b)

            def eq(x, y):
                return self.by_c.get(x, {}).get('twin') == y
            lcs = [[0] * (m + 1) for _ in range(n + 1)]
            for i in range(n - 1, -1, -1):
                for j in range(m - 1, -1, -1):
                    lcs[i][j] = lcs[i + 1][j + 1] + 1 if eq(seq_a[i], seq_b[j]) else max(lcs[i + 1][j], lcs[i][j + 1])
            i = j = 0
            ga, gb, gaps = [], [], []
            while i < n and j < m:
                if eq(seq_a[i], seq_b[j]):
                    gaps.append((ga, gb))
                    ga, gb = [], []
                    i += 1
                    j += 1
                elif lcs[i + 1][j] >= lcs[i][j + 1]:
                    ga.append(seq_a[i])
                    i += 1
                else:
                    gb.append(seq_b[j])
                    j += 1
            gaps.append((ga + seq_a[i:], gb + seq_b[j:]))
            for ga, gb in gaps:
                ua = list(dict.fromkeys(x for x in ga if x not in self.by_c))
                ub = list(dict.fromkeys(x for x in gb if x not in self.by_t))
                if ua and len(ua) == len(ub):
                    for c, t in zip(ua, ub):
                        if self.ok_size(c, t, 'callgraph') and self.comparable(c, t):
                            out.append((c, t, 'callgraph', r['score'] * CALLGRAPH_FACTOR, (r['cube'], r['twin'])))
        return out

    def m_callers(self):
        """An unmatched function that is the only unmatched caller shared by two matched callees, on both sides."""
        out = []
        for c in sorted(self.ea):
            if c in self.by_c:
                continue
            done = sorted(x for x in set(self.A[c]['calls']) if x in self.by_c and not self.by_c[x]['weak'])[:8]
            for i in range(len(done)):
                for j in range(i + 1, len(done)):
                    ca = {x for x in self.callers_a[done[i]] & self.callers_a[done[j]] if x not in self.by_c}
                    t1, t2 = self.by_c[done[i]]['twin'], self.by_c[done[j]]['twin']
                    cb = {x for x in self.callers_b[t1] & self.callers_b[t2] if x not in self.by_t}
                    if ca == {c} and len(cb) == 1:
                        out.append((c, next(iter(cb)), 'callers', SCORES['callers'], None))
        return out

    def run(self):
        self.resolve(self.m_vtable() + self.m_import() + self.m_string() + self.m_constants())
        for _ in range(MAX_ROUNDS):
            added = 0
            for _ in range(MAX_ROUNDS):
                n = self.resolve(self.m_callgraph())
                added += n
                if not n:
                    break
            added += self.resolve(self.m_callers())
            if not added:
                break
        return self.result()

    def result(self):
        rows = [self.by_c[c] for c in sorted(self.by_c)]
        conflicts = [self.conflicts[k] for k in sorted(self.conflicts)]
        return {'matches': rows, 'conflicts': conflicts,
                'counts': {'by_matcher': dict(sorted(Counter(r['matcher'] for r in rows).items())),
                           'weak': sum(1 for r in rows if r['weak']), 'conflicts': len(conflicts),
                           'cube_eligible': len(self.ea), 'twin_eligible': len(self.eb)}}


def match_functions(cube, twin_side, pairs=(), seeds=()):
    return Matcher(cube, twin_side, pairs, seeds).run()


# --- several variants ------------------------------------------------------------------------------------------
# The variants are one source built with other inlining, so one source function has one PDB name in each. The combined
# table pairs a Cube.exe function with a twin *name*: each variant's pair is a vote, and the best-supported one wins.

COMBINE_ROUNDS = 4


def name_keys(facts, eligible):
    """Twin entry -> the key that identifies its function in every variant: the PDB name, when only one eligible
    function of the variant has it. Overloads share a name, so those get no key and stay within their variant."""
    named = {f['entry']: f.get('pdb_name') for f in facts if f['entry'] in eligible and f.get('pdb_name')}
    count = Counter(named.values())
    return {e: n for e, n in named.items() if count[n] == 1}


def _edge_key(variant, row, keys):
    return keys[variant].get(row['twin']) or f'{row["twin_name"]} @{variant}:0x{row["twin"]:X}'


def combine_variants(evidence, keys, rank=None):
    """evidence: {variant: [rows]} (rows as Matcher writes them; seed rows are skipped, they repeat a vote). keys:
    {variant: name_keys}. Pairs Cube.exe functions with twin keys one to one. Each (function, key) edge takes the best
    score any variant gave it and the list of variants that voted for it; edges are taken best first (score, then
    number of variants). Two free edges that tie on one side are a dispute: both are dropped and listed."""
    edges = {}
    for v, rws in evidence.items():
        for r in rws:
            if r.get('seed'):
                continue
            k = (r['cube'], _edge_key(v, r, keys))
            e = edges.setdefault(k, {'cube': r['cube'], 'key': k[1], 'votes': {}, 'twin': {}})
            old = e['votes'].get(v)
            if old is None or r['score'] > old['score']:
                e['votes'][v] = {'matcher': r['matcher'], 'score': r['score'], 'weak': r['weak']}
                e['twin'][v] = r['twin']
                e['twin_name'] = r['twin_name']
    for e in edges.values():
        best = max(e['votes'].items(), key=lambda kv: (kv[1]['score'], kv[0]))
        e['score'], e['matcher'] = best[1]['score'], best[1]['matcher']
        e['rank'] = rank(e) if rank else (e['score'], len(e['votes']))
    by_c, by_k = defaultdict(list), defaultdict(list)
    for e in edges.values():
        by_c[e['cube']].append(e)
        by_k[e['key']].append(e)
    taken_c, taken_k, rows, conflicts = set(), set(), [], []
    for e in sorted(edges.values(), key=lambda e: tuple(-x for x in e['rank']) + (e['cube'], e['key'])):
        if e['cube'] in taken_c or e['key'] in taken_k:
            continue
        rivals = [x for x in by_c[e['cube']] + by_k[e['key']]
                  if x is not e and x['cube'] not in taken_c and x['key'] not in taken_k]
        if any(x['rank'] == e['rank'] for x in rivals):
            for x in [e] + [x for x in rivals if x['rank'] == e['rank']]:
                conflicts.append({'cube': x['cube'], 'key': x['key'], 'score': x['score'], 'variants': sorted(x['votes'])})
            taken_c.add(e['cube'])
            taken_k.add(e['key'])
            continue
        taken_c.add(e['cube'])
        taken_k.add(e['key'])
        row = {'cube': e['cube'], 'twin_name': e['twin_name'], 'key': e['key'], 'matcher': e['matcher'],
               'score': e['score'], 'weak': e['score'] < WEAK_BELOW, 'variants': sorted(e['votes']),
               'twin': dict(sorted(e['twin'].items())), 'votes': dict(sorted(e['votes'].items()))}
        if rivals:
            row['rivals'] = sorted({x['key'] if x['cube'] == e['cube'] else f'0x{x["cube"]:X}' for x in rivals})
        rows.append(row)
    rows.sort(key=lambda r: r['cube'])
    conflicts.sort(key=lambda c: (c['cube'], c['key']))
    return {'matches': rows, 'conflicts': conflicts,
            'counts': {'by_matcher': dict(sorted(Counter(r['matcher'] for r in rows).items())),
                       'by_variants': dict(sorted(Counter(len(r['variants']) for r in rows).items())),
                       'weak': sum(1 for r in rows if r['weak']), 'conflicts': len(conflicts)}}


def seeds_for(variant, combined, keys):
    """The strong combined pairs as anchors for one variant: (cube, twin entry, matcher, score)."""
    entry_of = {k: e for e, k in keys[variant].items()}
    out = []
    for r in combined['matches']:
        if r['weak']:
            continue
        t = r['twin'].get(variant, entry_of.get(r['key']))
        if t is not None:
            out.append((r['cube'], t, r['matcher'], r['score']))
    return out


def match_variants(cube, sides, pairs=None, rounds=COMBINE_ROUNDS, log=None):
    """sides: {variant: twin side}. Round 1 matches each variant alone; each later round seeds every variant with the
    strong combined pairs, so a pair found in one variant can propagate in another. The votes of all rounds count.
    Returns (combined result, round-1 result of each variant)."""
    pairs = pairs or {}
    keys = {v: name_keys(s['facts'], s['eligible']) for v, s in sides.items()}
    first = {v: match_functions(cube, s, pairs.get(v, ())) for v, s in sides.items()}
    evidence = {v: list(r['matches']) for v, r in first.items()}
    combined = combine_variants(evidence, keys)
    history = [len(combined['matches'])]
    for _ in range(rounds - 1):
        before = {(r['cube'], r['key']) for r in combined['matches'] if not r['weak']}
        for v, s in sides.items():
            res = match_functions(cube, s, pairs.get(v, ()), seeds_for(v, combined, keys))
            evidence[v].extend(r for r in res['matches'] if not r.get('seed'))
        combined = combine_variants(evidence, keys)
        history.append(len(combined['matches']))
        if log:
            log(f'round {len(history)}: {history[-1]} pairs')
        if {(r['cube'], r['key']) for r in combined['matches'] if not r['weak']} == before:
            break
    combined['counts']['rounds'] = history
    return combined, first


def combined_tsv(result, our_names, short):
    """names.tsv with a sixth column: the variants that voted for the pair (short ids)."""
    lines = [f'0x{r["cube"]:X}\t{r["twin_name"]}\t{r["matcher"]}\t{r["score"]:.3f}\t{our_names.get(r["cube"], "-")}\t'
             + ','.join(short(v) for v in r['variants'])
             for r in result['matches']]
    return '\n'.join(lines) + ('\n' if lines else '')


def names_tsv(result, our_names):
    """One line per pair, sorted by address: address, twin name, matcher, score, our name or '-'."""
    lines = [f'0x{r["cube"]:X}\t{r["twin_name"]}\t{r["matcher"]}\t{r["score"]:.3f}\t{our_names.get(r["cube"], "-")}'
             for r in sorted(result['matches'], key=lambda r: r['cube'])]
    return '\n'.join(lines) + ('\n' if lines else '')


def read_pairs(path):
    """pairs.toml: [[pair]] cube = 0x..., twin = 0x... (a person confirmed it)."""
    if not path.is_file():
        return []
    return [(int(p['cube']), int(p['twin'])) for p in tomllib.loads(path.read_text(encoding='utf-8')).get('pair', [])]


def short_ids(ids):
    """Variant ids without their common prefix up to the last '-' ('ic-aeff559-o2' -> 'o2')."""
    ids = list(ids)
    pre = ids[0][:ids[0].rfind('-') + 1] if ids else ''
    if len(ids) < 2 or not all(i.startswith(pre) for i in ids):
        return lambda v: v
    return lambda v: v[len(pre):]


def cmd_match(ctx, wanted=None, combine=False):
    from . import db as dbmod
    rec = load_record(ctx)
    cube_id = rec['source']['rebuilds']
    cfacts = sv.load_facts(ctx, cube_id)
    libs = sv.load_json(sv.survey_path(ctx, cube_id, 'libraries.json'))
    lib = {int(e) for e in libs['functions']}
    cube = {'facts': cfacts, 'eligible': {f['entry'] for f in cfacts if f['entry'] not in lib},
            'rtti': sv.load_json(sv.survey_path(ctx, cube_id, 'rtti.json')),
            'strings': sv.load_json(sv.survey_path(ctx, cube_id, 'strings.json'))['strings'],
            'imports': sv.load_json(sv.survey_path(ctx, cube_id, 'imports.json'))['imports']}
    ours = {e['address']: e['name'] for e in dbmod.load(ctx).symbols(cube_id)}
    cube['consts'] = data_consts(cfacts, cube['eligible'], sv.Image(sv.load_input(ctx, ctx.ws.build(cube_id)), ctx.ws.build(cube_id)))
    root = alpha_root(ctx, rec)
    variants = pick_variants(rec, wanted)
    if combine and len(variants) < 2:
        raise KmapError('--combine needs two variants or more')

    def load_side(v):
        out = twin_dir(ctx) / v['id']
        if not (out / 'functions.json').is_file():
            raise KmapError(f'{out / "functions.json"} not found: run `kmap twin survey` first')
        tfacts = sv.load_json(out / 'functions.json')
        side = {'facts': tfacts, 'eligible': {f['entry'] for f in tfacts if twin_eligible(f, root)},
                'rtti': sv.load_json(out / 'rtti.json'), 'strings': sv.load_json(out / 'strings.json')['strings'],
                'imports': sv.load_json(out / 'imports.json')['imports']}
        side['consts'] = data_consts(tfacts, side['eligible'], load_image(variant_files(ctx, v)[0]))
        return side

    def write_variant(v, result):
        out = twin_dir(ctx) / v['id']
        result['twin'] = v['id']
        sv.save(out / 'matches.json', result)
        write_if_changed(out / 'names.tsv', names_tsv(result, ours))
        c = result['counts']
        say(f'{v["id"]}: {len(result["matches"])} pairs ({len(result["matches"]) - c["weak"]} strong, {c["weak"]} weak) '
            f'of {c["cube_eligible"]} Cube.exe game functions; by matcher {c["by_matcher"]}; '
            f'{c["conflicts"]} conflicts; in {out}')

    if not combine:
        for v in variants:
            write_variant(v, match_functions(cube, load_side(v), read_pairs(twin_dir(ctx) / v['id'] / 'pairs.toml')))
        return 0
    sides = {v['id']: load_side(v) for v in variants}
    pairs = {v['id']: read_pairs(twin_dir(ctx) / v['id'] / 'pairs.toml') for v in variants}
    combined, first = match_variants(cube, sides, pairs, log=say)
    for v in variants:
        write_variant(v, first[v['id']])
    combined['twin'] = [v['id'] for v in variants]
    out = twin_dir(ctx) / 'combined'
    out.mkdir(parents=True, exist_ok=True)
    sv.save(out / 'matches.json', combined)
    write_if_changed(out / 'names.tsv', combined_tsv(combined, ours, short_ids(combined['twin'])))
    c = combined['counts']
    strong = len(combined['matches']) - c['weak']
    say(f'combined ({", ".join(combined["twin"])}): {len(combined["matches"])} pairs ({strong} strong, {c["weak"]} weak); '
        f'by matcher {c["by_matcher"]}; by number of variants {c["by_variants"]}; {c["conflicts"]} conflicts; '
        f'rounds {c["rounds"]}; in {out}')
    return 0
