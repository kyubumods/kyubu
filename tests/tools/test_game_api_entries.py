"""The game API's entry list (loader/src/game/entries.inc) against the mapping, primitives.md and kyubu_api.h
(openspec add-game-api, design D1; task 3.2).

It fails when an entry is not listed under its primitive's section of mapping/atlas/primitives.md, when it is not
`verified` for the builds its line names, when a `call` or `detour` entry has no `prototype`, when a member after
the game marker in kyubu_api.h has no GAME_API line (or a GAME_API line names no member), and (openspec
add-entity-spawner, D7) when a GAME_BUILDS line is malformed or an entry of a limited primitive names a build the
primitive is not for (`field` and `type` entries are exempt: layouts are shared by both builds).

An entry that is not `verified` passes only when primitives.md's row for it states the same status (openspec
add-ability-api, task 4.1: `ability_stun` and `ability_ranks` list candidate entries on purpose and fall back at run
time). Anywhere else a candidate entry fails.
"""
import os
import re

import pytest

from conftest import ROOT

ENTRIES = os.path.join(ROOT, 'loader', 'src', 'game', 'entries.inc')
PRIMITIVES = os.path.join(ROOT, 'mapping', 'atlas', 'primitives.md')
API_HEADER = os.path.join(ROOT, 'sdk', 'include', 'kyubu_api.h')

KINDS = ('field', 'type', 'global', 'call', 'detour', 'site')
HOSTS = {'CLIENT': ('Cube.exe',), 'SERVER': ('Server.exe',), 'BOTH': ('Cube.exe', 'Server.exe')}
GAME_MARKER = '/* ---- game API ----'
LINE = re.compile(r'^\s*GAME_(PRIMITIVE|BUILDS|ENTRY|API)\((.*)\)\s*$')


def read(path):
    return open(path, encoding='utf-8').read()


def parse_entries(text):
    """(primitives {id: section}, entries [(primitive, name, kind, builds, line)], apis [(member, primitive, line)],
    limits {primitive: (builds, line)}, problems). Comments and preprocessor lines are skipped, so the list's own
    documentation doesn't count."""
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    prims, entries, apis, limits, problems = {}, [], [], {}, []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        m = LINE.match(line)
        if not m:
            problems.append(f'entries.inc:{n}: not a GAME_PRIMITIVE, GAME_ENTRY or GAME_API line: {line.strip()}')
            continue
        args = [a.strip() for a in re.findall(r'"[^"]*"|[^,]+', m.group(2))]
        args = [a[1:-1] if a.startswith('"') else a for a in args]
        what = m.group(1)
        if what == 'PRIMITIVE' and len(args) == 2:
            prims[args[0]] = args[1]
        elif what == 'BUILDS' and len(args) == 2:
            limits[args[0]] = (args[1], n)
        elif what == 'ENTRY' and len(args) == 4:
            entries.append((*args, n))
        elif what == 'API' and len(args) == 2:
            apis.append((*args, n))
        else:
            problems.append(f'entries.inc:{n}: GAME_{what} with {len(args)} argument(s)')
    return prims, entries, apis, limits, problems


def game_members(header):
    """The KyubuApi members after the game marker, up to the next section (e.g. the mod options) or the end, and the members of every later section marked the same way."""
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


def mapping_db():
    from kmap import config, db
    return db.load(config.Context(ROOT))


def find_entry(loaded, bid, name, kind):
    """The mapping entry `name` of `kind` as build `bid` sees it, and the entries whose status also counts, or (None, []).
    A field counts alone, as in the loader's field_problem: a verified offset stands even when its struct is a candidate
    (cube::AppearanceData.scale)."""
    if kind == 'field':
        parent, _, field = name.rpartition('.')
        t = loaded.find_type(parent)
        if not t or (t.get('builds') and bid not in t['builds']):
            return None, []
        f = next((f for f in t.get('field', []) if f.get('name') == field or field in f.get('aliases', [])), None)
        return f, []
    if kind == 'type':
        t = loaded.find_type(name) or loaded.find_enum(name)
        if t and t.get('builds') and bid not in t['builds']:
            return None, []
        return t, []
    e = loaded.find_function(bid, name)
    if e and (e.get('_kind') == 'global') != (kind == 'global'):
        return None, []
    return e, []


def problems(entries_text, primitives_text, header_text, loaded):
    """Every way the entry list disagrees with primitives.md, the mapping and kyubu_api.h, as messages that name the
    primitive and the entry (or the API member)."""
    from kmap import atlas
    prims, entries, apis, limits, out = parse_entries(entries_text)
    sections = atlas.sections(primitives_text)
    listed = {s: {m.group(1): m.group(2) for line in body
                  for m in [re.match(r'\|\s*`([^`]+)`\s*\|\s*(\w+)\s*\|', line)] if m}
              for s, (_, body) in sections.items()}
    if 'world' not in prims:
        out.append('entries.inc: no GAME_PRIMITIVE(world, ...): every primitive needs it')
    for p, section in prims.items():
        if section not in sections:
            out.append(f'primitive {p}: primitives.md has no section "## {section}"')
    for prim, (builds, n) in limits.items():
        if prim not in prims:
            out.append(f'entries.inc:{n}: GAME_BUILDS({prim}, ...): no GAME_PRIMITIVE({prim}, ...)')
        elif builds not in HOSTS:
            out.append(f'entries.inc:{n}: GAME_BUILDS({prim}, {builds}): builds must be CLIENT, SERVER or BOTH')
    builds_of = {}
    for bid, b in loaded.ws.targets.items():     # what the loader runs on; a retired build is only a record
        builds_of.setdefault(b.file, []).append(bid)
    for prim, name, kind, builds, n in entries:
        where = f'entries.inc:{n}: primitive {prim}, entry {name}'
        if prim not in prims:
            out.append(f'{where}: no GAME_PRIMITIVE({prim}, ...)')
            continue
        if kind not in KINDS or builds not in HOSTS:
            out.append(f'{where}: kind must be one of {", ".join(KINDS)} and builds CLIENT, SERVER or BOTH')
            continue
        if name not in listed.get(prims[prim], ()):
            out.append(f'{where}: not listed under "## {prims[prim]}" in primitives.md')
        limit = limits.get(prim, ('BOTH', 0))[0]
        if kind not in ('field', 'type') and limit in HOSTS and not set(HOSTS[builds]) <= set(HOSTS[limit]):
            out.append(f'{where}: names {builds}, but GAME_BUILDS({prim}, {limit}) limits the primitive to '
                       f'{" and ".join(HOSTS[limit])}')
        for exe in HOSTS[builds]:
            for bid in builds_of.get(exe, []):
                e, also = find_entry(loaded, bid, name, kind)
                if e is None:
                    out.append(f'{where}: no {kind} of that name in the mapping of {bid}')
                    continue
                stated = listed.get(prims[prim], {}).get(name)
                bad = [x for x in [e] + also if x.get('status') != 'verified' and x.get('status') != stated]
                if bad:
                    out.append(f'{where}: not verified on {bid} ({bad[0].get("name")} is {bad[0].get("status")})')
                if kind in ('call', 'detour') and not e.get('prototype'):
                    out.append(f'{where}: no prototype on {bid} (a {kind} needs one)')
    members = game_members(header_text)
    if members is None:
        out.append('kyubu_api.h: no game marker ("' + GAME_MARKER + '") in KyubuApi')
        members = []
    api_names = {m for m, _, _ in apis}
    for m in members:
        if m not in api_names:
            out.append(f'kyubu_api.h: game-API member {m} has no GAME_API line in entries.inc')
    for m, prim, n in apis:
        if m not in members:
            out.append(f'entries.inc:{n}: GAME_API({m}, ...) names no member after the game marker in kyubu_api.h')
        if prim != 'none' and prim not in prims:
            out.append(f'entries.inc:{n}: GAME_API({m}, {prim}): no GAME_PRIMITIVE({prim}, ...)')
    return out


@pytest.fixture(scope='module')
def loaded():
    return mapping_db()


def repo_problems(loaded, entries=None, header=None):
    return problems(entries if entries is not None else read(ENTRIES), read(PRIMITIVES),
                    header if header is not None else read(API_HEADER), loaded)


def test_the_entry_list_agrees_with_the_mapping(loaded):
    assert repo_problems(loaded) == []


def test_unverified_entry(loaded):
    # covers: game-api/Unverified entry
    # cube::CreatureData.body_pitch is a candidate field (primitives.md, "Entity state", leaves it out)
    entries = read(ENTRIES).replace('GAME_API(game_available, none)',
                                    'GAME_ENTRY(handles, "cube::CreatureData.body_pitch", field, BOTH)\n'
                                    'GAME_API(game_available, none)')
    found = [p for p in repo_problems(loaded, entries=entries) if 'not verified' in p]
    assert found, 'the test did not fail'
    assert all('primitive handles, entry cube::CreatureData.body_pitch' in p for p in found), found


def test_unlisted_function(loaded):
    # covers: game-api/Unlisted function
    game_available = 'KyubuResult (KYUBU_CALL* game_available)(const char* primitive, char* why, uint32_t cap);\n'
    header = read(API_HEADER).replace(
        game_available, game_available + '    KyubuResult (KYUBU_CALL* creature_frobnicate)(KyubuModHandle self, KyubuCreature c);\n')
    assert 'kyubu_api.h: game-API member creature_frobnicate has no GAME_API line in entries.inc' in \
        repo_problems(loaded, header=header)
    # and the other way round: a GAME_API line for a member that isn't in the header
    entries = read(ENTRIES).replace('GAME_API(game_available, none)',
                                    'GAME_API(game_available, none)\nGAME_API(creature_frobnicate, handles)')
    assert any('GAME_API(creature_frobnicate, ...) names no member' in p for p in repo_problems(loaded, entries=entries))


def test_detour_without_prototype_and_unlisted_entry(loaded):
    # cube::World::applyHit is verified in both builds and has no prototype
    entries = read(ENTRIES).replace('GAME_API(game_available, none)',
                                    'GAME_ENTRY(world, "cube::World::applyHit", call, BOTH)\n'
                                    'GAME_API(game_available, none)')
    found = [p for p in repo_problems(loaded, entries=entries) if 'cube::World::applyHit' in p]
    assert any('not listed under "## Foundation"' in p for p in found), found
    assert any('no prototype on cube-0x51EA955E' in p for p in found), found   # a targeted build
    assert not any('not verified' in p for p in found), found


def test_parser_ignores_the_lists_own_documentation():
    prims, entries, apis, limits, bad = parse_entries(read(ENTRIES))
    assert bad == []
    assert list(prims) == ['world', 'handles', 'entity_state', 'spawn', 'behavior', 'behavior_companion',
                           'behavior_path', 'items', 'ability_state', 'ability_stun', 'ability_step', 'ability_bar',
                           'ability_stats', 'ability_icons', 'ability_ranks', 'ability_use', 'ability_iconfile', 'ability_skilltree', 'animation_pose', 'ui', 'particles', 'lights', 'creature_flags', 'terrain', 'world_step',
                           'creature_root', 'creature_state', 'net_transport', 'creature_body', 'player_shots', 'server_hits'] and prims['world'] == 'Foundation'
    assert {p for p in prims if p.startswith('ability_')} == {p for p, s in prims.items() if s == 'Abilities'}
    assert prims['behavior_companion'] == prims['behavior_path'] == prims['behavior'] == 'Behavior'   # 8.3
    assert limits == {'spawn': ('CLIENT', limits['spawn'][1]),   # the first GAME_BUILDS line (4.3)
                      'behavior': ('CLIENT', limits['behavior'][1]),   # 6.2
                      'behavior_companion': ('CLIENT', limits['behavior_companion'][1]),   # 8.3
                      'behavior_path': ('CLIENT', limits['behavior_path'][1]),
                      'items': ('CLIENT', limits['items'][1]),   # creature_give_item
                      'animation_pose': ('CLIENT', limits['animation_pose'][1]),   # add-ability-animation 3.4
                      'ui': ('CLIENT', limits['ui'][1]),   # the UI scene tree and the render device
                      'particles': ('CLIENT', limits['particles'][1]),   # add-particle-effects: the push is mapped for Cube.exe only
                      'lights': ('CLIENT', limits['lights'][1]),   # add-light-api: Server.exe draws nothing
                      'creature_flags': ('CLIENT', limits['creature_flags'][1]),   # the hit sites are mapped for Cube.exe only
                      'creature_body': ('CLIENT', limits['creature_body'][1]),   # the creature draw sites are Cube.exe only
                      'player_shots': ('SERVER', limits['player_shots'][1]),     # the projectile packet's site is Server.exe only
                      'server_hits': ('SERVER', limits['server_hits'][1]),       # the double push is Server.exe's
                      # ability_step and ability_stats run on both builds: the server runs a mod ability's steps
                      **{p: ('CLIENT', limits[p][1]) for p in ('ability_bar', 'ability_icons', 'ability_ranks',
                                                                'ability_use', 'ability_iconfile', 'ability_skilltree')}}
    assert ('game_available', 'none') in [(m, p) for m, p, _ in apis]
    assert all(kind in KINDS for _, _, kind, _, _ in entries)


def test_limited_primitive_entries(loaded):
    # covers: game-api/Primitives limited to some builds (the entries test half)
    # cube::World::update is a detour verified in both builds; the primitive is limited to Cube.exe
    id_line = 'GAME_ENTRY(handles, "cube::Creature.id", field, BOTH)\n'
    entries = read(ENTRIES).replace(id_line, id_line + 'GAME_ENTRY(handles, "cube::World::update", detour, BOTH)\n')
    assert not any('limits the primitive' in p for p in repo_problems(loaded, entries=entries))   # no limit yet
    dtor = 'GAME_ENTRY(handles, "cube::Creature::~Creature"'
    limited = entries.replace(dtor, 'GAME_BUILDS(handles, CLIENT)\n' + dtor)
    found = [p for p in repo_problems(loaded, entries=limited) if 'limits the primitive' in p]
    # both detours name BOTH; the field says BOTH too, and is exempt
    assert len(found) == 2 and all('GAME_BUILDS(handles, CLIENT) limits the primitive to Cube.exe' in p for p in found), found
    assert not any('cube::Creature.id' in p for p in found), found
    assert any('entry cube::Creature::~Creature' in p for p in found), found
    # CLIENT entries of a CLIENT primitive are fine
    fixed = limited.replace('"cube::Creature::~Creature", detour, BOTH', '"cube::Creature::~Creature", detour, CLIENT') \
                   .replace('"cube::World::update", detour, BOTH)\n', '"cube::World::update", detour, CLIENT)\n')
    assert not any('limits the primitive' in p for p in repo_problems(loaded, entries=fixed))
    # malformed lines
    bad = entries.replace(dtor, 'GAME_BUILDS(handles, EVERYWHERE)\nGAME_BUILDS(no_such, CLIENT)\n' + dtor)
    out = repo_problems(loaded, entries=bad)
    assert any('GAME_BUILDS(handles, EVERYWHERE): builds must be' in p for p in out), out
    assert any('GAME_BUILDS(no_such, ...): no GAME_PRIMITIVE(no_such' in p for p in out), out


def test_candidate_entry_needs_its_status_in_primitives_md(loaded):
    # covers: add-ability-api task 4.1. Entries may stay candidate on purpose while primitives.md says so (stun_time was raised
    # to verified in task 2.7, save_character with 2.8's table on 2026-10-05, load_character with 5.2's on 2026-10-05; none is left)
    assert repo_problems(loaded) == []
    candidates = ()
    primitives = read(PRIMITIVES)
    for name in candidates:
        assert re.search(r'\|\s*`%s`\s*\|\s*candidate\s*\|' % re.escape(name), primitives), name
        # primitives.md says verified: the same entry now disagrees with the mapping
        stated = re.sub(r'(\|\s*`%s`\s*\|\s*)candidate' % re.escape(name), r'verified', primitives)
        found = problems(read(ENTRIES), stated, read(API_HEADER), loaded)
        assert any(f'entry {name}: not verified' in p for p in found), (name, found)
