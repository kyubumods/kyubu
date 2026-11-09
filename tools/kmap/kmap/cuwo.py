"""kmap import-cuwo: candidate types and enums from a cuwo checkout at the pinned commit.

Only facts are taken: field names, types and sizes from the serialisation order in cuwo/entity.pyx, and
constant names and values from cuwo/constants.py and cuwo/strings.py. cuwo is GPL-3.0; none of its code is
copied. Every entry is a `candidate` with `cuwo:<path>` evidence: the offsets describe the network format and
must be confirmed in memory by probes. A probe that confirms an entry raises it in the committed file, and a
re-import keeps that (`mapped`).
"""
import re
import subprocess
import tomllib
from pathlib import Path

from .util import KmapError, dump_json, say, toml_str, write_if_changed

READERS = {'int8': ('int8_t', 1), 'uint8': ('uint8_t', 1), 'int16': ('int16_t', 2), 'uint16': ('uint16_t', 2),
           'int32': ('int32_t', 4), 'uint32': ('uint32_t', 4), 'int64': ('int64_t', 8), 'uint64': ('uint64_t', 8),
           'float': ('float', 4), 'double': ('double', 8), 'vec3': ('cube::FloatVector3', 12),
           'ivec3': ('cube::IntVector3', 12), 'qvec3': ('cube::LongVector3', 24)}
TYPES = {'ItemUpgrade': ('cube::ItemUpgrade', []), 'ItemData': ('cube::ItemData', []),
         'AppearanceData': ('cube::AppearanceData', []), 'EntityData': ('cube::CreatureData', ['EntityData'])}
VECTORS = [('cube::FloatVector3', 'float', 4, 'vec3'), ('cube::IntVector3', 'int32_t', 4, 'ivec3'),
           ('cube::LongVector3', 'int64_t', 8, 'qvec3')]
ENTITY = 'cuwo/entity.pyx'


def head_commit(checkout):
    r = subprocess.run(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if r.returncode:
        raise KmapError(f'{checkout} is not a git checkout of cuwo ({r.stderr.strip()})')
    return r.stdout.strip()


def parse_reads(text):
    """class name -> [(field, type, size, count)] in read order; skips as (None, None, n, 1)."""
    out = {}
    for m in re.finditer(r'^cdef class (\w+):.*?^    cpdef read\(self, ByteReader reader\):\n(.*?)(?=^    \S|^\S)',
                         text, re.M | re.S):
        rows = []
        lines = m.group(2).splitlines()
        i = 0
        while i < len(lines):
            l = lines[i].strip()
            i += 1
            if not l:
                continue
            loop = re.match(r'for \w+ in range\((\d+)\):', l)
            count = 1
            if loop:
                count = int(loop.group(1))
                l = lines[i].strip()
                i += 1
            r = re.match(r'self\.(\w+)(?:\[\w+\])? = reader\.read_(\w+)\((\d*)\)', l)
            if r:
                name, kind, arg = r.groups()
                if kind == 'ascii':
                    rows.append((name, 'char', 1, int(arg) * count))
                elif kind in READERS:
                    t, n = READERS[kind]
                    rows.append((name, t, n, count))
                else:
                    raise KmapError(f'{ENTITY}: unknown reader read_{kind} in {m.group(1)}')
                continue
            r = re.match(r'reader\.skip\((\d+)\)', l)
            if r:
                rows.append((None, None, int(r.group(1)) * count, 1))
                continue
            r = re.match(r'(?:\(<(\w+)>)?self\.(\w+)(?:\[\w+\]\))?\.read\(reader\)', l)
            if r:
                rows.append((r.group(2), ('nested', r.group(1)), None, count))
                continue
            raise KmapError(f'{ENTITY}: cannot read {l!r} in {m.group(1)}')
        out[m.group(1)] = rows
    return out


def build_types(reads, field_types):
    """[type dict] with offsets from the read order; nested structs resolved by their computed size."""
    sizes = {}
    types = []
    order = ['ItemUpgrade', 'ItemData', 'AppearanceData', 'EntityData']
    for cls in order:
        rows = reads.get(cls)
        if rows is None:
            raise KmapError(f'{ENTITY}: class {cls} not found')
        name, aliases = TYPES[cls]
        off = 0
        fields = []
        for fname, t, size, count in rows:
            if fname is None:
                off += size
                continue
            if isinstance(t, tuple):
                sub = t[1] or field_types.get((cls, fname))
                if sub not in TYPES:
                    raise KmapError(f'{ENTITY}: {cls}.{fname} reads an unknown class {sub!r}')
                tname = TYPES[sub][0]
                size = sizes[sub]
            else:
                tname = t
            ftype = tname if count == 1 else f'{tname}[{count}]'
            fields.append({'offset': off, 'name': fname, 'type': ftype})
            off += size * count
        sizes[cls] = off
        types.append({'name': name, 'aliases': aliases, 'size': off, 'fields': fields, 'cuwo': cls})
    return types


def field_types_from_decls(text):
    """(class, field) -> class of a nested struct, from `self.x = Foo.__new__(Foo)` in __cinit__."""
    out = {}
    for m in re.finditer(r'^cdef class (\w+):(.*?)(?=^cdef class|\Z)', text, re.M | re.S):
        for f in re.finditer(r'self\.(\w+) = (\w+)\.__new__\(\2\)', m.group(2)):
            out[(m.group(1), f.group(1))] = f.group(2)
    return out


def upper_snake(s):
    s = re.sub(r'[^A-Za-z0-9]+', '_', s)
    s = re.sub(r'(?<=[a-z0-9])(?=[A-Z])', '_', s)
    s = s.strip('_').upper() or 'UNNAMED'
    return s if not s[0].isdigit() else '_' + s


def literal_dict(text, name):
    m = re.search(rf'^{name} = \{{(.*?)^\}}', text, re.M | re.S)
    if not m:
        raise KmapError(f'cuwo: {name} not found')
    out = []
    for k, v in re.findall(r"^\s*(\([^)]*\)|\w+):\s*'([^']*)'", m.group(1), re.M):
        out.append((k, v))
    return out


def build_enums(constants, strings):
    consts = dict((k, int(eval(v, {}, {}))) for k, v in re.findall(r'^([A-Z_0-9]+) = ([0-9 <x+*A-Fa-f]+?)\s*(?:#.*)?$',
                                                                    constants, re.M)
                  if re.fullmatch(r'[0-9 <x+*A-Fa-f]+', v))
    enums = []

    def enum(name, values, source, size=4, notes=None):
        seen = set()
        vals = []
        for vname, v in values:
            vname = upper_snake(vname)
            while vname in seen:
                vname += '_2'
            seen.add(vname)
            vals.append((vname, v))
        enums.append({'name': name, 'size': size, 'values': vals, 'source': source, 'notes': notes})
    enum('cube::HostileType', [(k[:-5], consts[k]) for k in ('FRIENDLY_PLAYER_TYPE', 'HOSTILE_TYPE', 'FRIENDLY_TYPE',
                                                           'NAMED_FRIENDLY_TYPE', 'TARGET_TYPE') if k in consts],
         'cuwo/constants.py', 1)
    enum('cube::ClassType', [(k[:-6], consts[k]) for k in ('WARRIOR_CLASS', 'RANGER_CLASS', 'MAGE_CLASS', 'ROGUE_CLASS')
                             if k in consts], 'cuwo/constants.py', 1)
    enum('cube::Specialization', [(k, consts[k]) for k in ('BERSERKER', 'GUARDIAN', 'SNIPER', 'SCOUT', 'FIRE_MAGE',
                                                          'WATER_MAGE', 'ASSASSIN', 'NINJA') if k in consts],
         'cuwo/constants.py', 1, 'values depend on the class: 0 and 1 for each')
    enum('cube::EntityType', [(v, int(k)) for k, v in literal_dict(strings, 'ENTITY_NAMES')], 'cuwo/strings.py')
    enum('cube::AbilityId', [(v, int(k)) for k, v in literal_dict(strings, 'ABILITY_NAMES')], 'cuwo/strings.py', 1)
    enum('cube::SkillId', [(v, int(k)) for k, v in literal_dict(strings, 'SKILL_NAMES')], 'cuwo/strings.py')
    items = []
    for k, v in literal_dict(strings, 'ITEM_NAMES'):
        t, s = (int(x) for x in k.strip('()').split(','))
        items.append((v, t * 256 + s))
    enum('cube::ItemId', items, 'cuwo/strings.py', 4, 'value = type * 256 + sub_type (cuwo keys items by both)')
    return enums


def proposal(checkout):
    checkout = Path(checkout)
    entity = (checkout / ENTITY).read_text(encoding='utf-8')
    types = build_types(parse_reads(entity), field_types_from_decls(entity))
    enums = build_enums((checkout / 'cuwo/constants.py').read_text(encoding='utf-8'),
                        (checkout / 'cuwo/strings.py').read_text(encoding='utf-8'))
    return types, enums


def mapped(path):
    """What mapping work added to a committed cuwo-entity.toml: {(type, field or None): (status, evidence, notes)} for
    each type or field whose evidence cites a probe, and each field with notes (the import writes none). A re-import
    writes these back unchanged, so it never lowers a verified field to candidate or drops a finding."""
    path = Path(path)
    if not path.is_file():
        return {}
    out = {}
    for t in tomllib.loads(path.read_text(encoding='utf-8')).get('type', []):
        for key, e in [((t['name'], None), t)] + [((t['name'], f['name']), f) for f in t.get('field', [])]:
            if any(str(x).startswith('probe:') for x in e.get('evidence', [])) or (key[1] and e.get('notes')):
                out[key] = (e.get('status'), e.get('evidence'), e.get('notes'))
    return out


def enums_elsewhere(types_dir):
    """Enum names that a hand-written file in mapping/types/ defines (task 9.2 moved cube::AbilityId to combat.toml,
    with the game's own names). The import leaves those out of cuwo-constants.toml, so they are not defined twice."""
    names = set()
    for p in sorted(Path(types_dir).glob('*.toml')):
        if p.name not in ('cuwo-entity.toml', 'cuwo-constants.toml'):
            names |= {e.get('name') for e in tomllib.loads(p.read_text(encoding='utf-8')).get('enum', [])}
    return names


def _entry(kept, key, evidence, notes=None):
    status, evidence, notes = kept.get(key, ('candidate', [evidence], notes))
    lines = [f'status = {toml_str(status)}', 'evidence = [' + ', '.join(toml_str(e) for e in evidence) + ']',
             'source = "cuwo"']
    return lines + ([f'notes = {toml_str(notes)}'] if notes else []) + ['']


def types_toml(types, kept=None):
    """The committed file for the proposal's types; `kept` (from `mapped`) keeps what mapping work added."""
    kept = kept or {}
    lines = ['# Candidate layouts imported from cuwo (kmap import-cuwo): names, types and serialisation offsets from',
             '# cuwo/entity.pyx at the pinned commit. The offsets describe the network format; probes confirm them.', '']
    for name, elem, size, reader in VECTORS:
        lines += ['[[type]]', f'name = "{name}"', 'kind = "struct"', f'size = 0x{3 * size:X}']
        lines += _entry(kept, (name, None), 'cuwo:cuwo/bytes.pyx', f'cuwo reads it with read_{reader}')
        for i, axis in enumerate('xyz'):
            lines += ['[[type.field]]', f'offset = 0x{i * size:X}', f'name = "{axis}"', f'type = "{elem}"']
            lines += _entry(kept, (name, axis), 'cuwo:cuwo/bytes.pyx')
    for t in types:
        lines += ['[[type]]', f'name = {toml_str(t["name"])}', 'kind = "struct"', f'size = 0x{t["size"]:X}']
        if t['aliases']:
            lines.append('aliases = [' + ', '.join(toml_str(a) for a in t['aliases']) + ']')
        lines += _entry(kept, (t['name'], None), f'cuwo:{ENTITY}',
                        f'cuwo class {t["cuwo"]}; serialisation layout, not yet confirmed in memory')
        for f in t['fields']:
            lines += ['[[type.field]]', f'offset = 0x{f["offset"]:X}', f'name = {toml_str(f["name"])}',
                      f'type = {toml_str(f["type"])}']
            lines += _entry(kept, (t['name'], f['name']), f'cuwo:{ENTITY}')
    return '\n'.join(lines)


def enums_toml(enums):
    lines = ['# Candidate enums imported from cuwo (kmap import-cuwo): constant names and values at the pinned commit.', '']
    for e in enums:
        lines += ['[[enum]]', f'name = {toml_str(e["name"])}', f'size = {e["size"]}', 'status = "candidate"',
                  f'evidence = ["cuwo:{e["source"]}"]', 'source = "cuwo"']
        if e['notes']:
            lines.append(f'notes = {toml_str(e["notes"])}')
        lines.append('')
        for n, v in e['values']:
            lines += ['[[enum.value]]', f'name = {toml_str(n)}', f'value = {v}', '']
    return '\n'.join(lines)


def check_commit(ctx, checkout, override=None):
    want = override or ctx.ws.cuwo['commit']
    have = head_commit(checkout)
    if not have.startswith(want) and not want.startswith(have):
        raise KmapError(f'the cuwo checkout {checkout} is at {have}, but mapping/workspace.toml pins {want}; '
                        'check out the pinned commit (or pass --commit to override the pin)')
    return have


def cmd_import(ctx, checkout, commit=None, write=False):
    have = check_commit(ctx, checkout, commit)
    types, enums = proposal(checkout)
    say(f'cuwo {have[:12]}: {len(types)} types ({", ".join(t["name"] for t in types)}), {len(enums)} enums '
        f'({sum(len(e["values"]) for e in enums)} values); all candidates')
    for t in types:
        say(f'  {t["name"]}: {len(t["fields"])} fields, 0x{t["size"]:X} bytes serialised')
    kept = mapped(ctx.repo / 'mapping/types/cuwo-entity.toml')
    if kept:
        say(f'  keeping {len(kept)} entr{"y" if len(kept) == 1 else "ies"} that mapping work changed in cuwo-entity.toml')
    elsewhere = enums_elsewhere(ctx.repo / 'mapping/types')
    if elsewhere & {e['name'] for e in enums}:
        say(f'  leaving out {", ".join(sorted(elsewhere & {e["name"] for e in enums}))}: mapped in another types file')
    files = {'mapping/types/cuwo-entity.toml': types_toml(types, kept),
             'mapping/types/cuwo-constants.toml': enums_toml([e for e in enums if e['name'] not in elsewhere])}
    for rel, text in files.items():
        if write:
            if write_if_changed(ctx.repo / rel, text):
                say(f'wrote {rel}')
        else:
            say(f'would write {rel}; run with --write')
    return 0


def find_checkout(ctx):
    import os
    for p in (os.environ.get('KMAP_CUWO'), ctx.home / 'cuwo'):
        if p and (Path(p) / ENTITY).is_file():
            return Path(p)
    return None


def survey_pass(ctx):
    checkout = find_checkout(ctx)
    if checkout is None:
        say('cuwo: no checkout (KMAP_CUWO or $KMAP_HOME/cuwo); skipped')
        return
    check_commit(ctx, checkout)
    types, enums = proposal(checkout)
    write_if_changed(ctx.survey_dir / 'cuwo.json', dump_json({'types': types, 'enums': enums}))
    say(f'cuwo: {len(types)} types and {len(enums)} enums proposed (survey/cuwo.json; `kmap import-cuwo --write`)')
