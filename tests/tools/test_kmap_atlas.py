"""The atlas lint in kmap check: page set, sections, Depth line and references (add-mapping-workspace task 6.1)."""
import os
import re
import shutil

import pytest

from conftest import ROOT, function_entry

CUBE = 'cube-0x51D2CE9C'
ATLAS = os.path.join(ROOT, 'mapping', 'atlas')
SECTIONS_EXPECTED = ('Purpose, Key classes, Entry points, Data flow, Client and server, Hook points of interest, '
                     'Open questions, Depth')
DEPTH_RULE = ('the Depth section starts with `not started`, `reconnaissance, reviewed YYYY-MM-DD` or '
              '`deep dive, reviewed YYYY-MM-DD`')


PAGES = ['core', 'world', 'creatures', 'combat', 'items', 'ai', 'ui', 'rendering', 'audio', 'input', 'network',
         'saves', 'travel']


def stub_page(stem):
    body = ''.join(f'## {s}\n\nNot written yet.\n\n' for s in SECTIONS_EXPECTED.split(', ')[:-1])
    return f'# {stem.capitalize()}\n\n{body}## Depth\n\nnot started\n'


def with_atlas(kmap_repo):
    """The committed template and 13 unwritten pages (the committed pages cite the real mapping), plus one mapped
    function with an alias."""
    atlas = kmap_repo.mapping / 'atlas'
    atlas.mkdir(parents=True)
    shutil.copy2(os.path.join(ATLAS, '_template.md'), atlas)
    for stem in PAGES:
        (atlas / f'{stem}.md').write_text(stub_page(stem), encoding='utf-8')
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml',
                    function_entry('cube::Creature::update', 0x4A2000, extra='aliases = ["Creature::update"]'))
    return atlas


def fails(kmap_repo):
    code, out, err = kmap_repo.check()
    return code, [l[5:] for l in out.splitlines() if l.startswith('FAIL ')]


def edit(path, old, new):
    text = path.read_text(encoding='utf-8')
    assert old in text, old
    path.write_text(text.replace(old, new, 1), encoding='utf-8')


def line_of(path, needle):
    return next(n for n, l in enumerate(path.read_text(encoding='utf-8').splitlines(), 1) if needle in l)


def set_depth(page, value):
    lines = page.read_text(encoding='utf-8').splitlines()
    lines[-1] = value
    page.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def test_unwritten_atlas_passes(kmap_repo):
    with_atlas(kmap_repo)
    code, out, err = kmap_repo.check()
    assert code == 0, out + err
    assert '13 atlas pages' in out
    assert sorted(p[:-3] for p in os.listdir(ATLAS) if not p.startswith('_')) == sorted(PAGES + ['primitives'])


def test_unmapped_reference(kmap_repo):
    # covers: game-atlas/Unmapped reference
    atlas = with_atlas(kmap_repo)
    page = atlas / 'combat.md'
    edit(page, '## Entry points\n\nNot written yet.',
         '## Entry points\n\n- `cube::Creature::update()` runs every frame (alias `Creature::update`).\n'
         '- Damage is applied by `cube::Creature::applyDamage`.\n'
         '- Plain text and fenced blocks hold no references: cube::Creature::heal\n\n'
         '```cpp\ncube::Creature::kill(c);   // `cube::Creature::kill`\n```')
    code, problems = fails(kmap_repo)
    assert code == 1
    assert problems == [f'mapping/atlas/combat.md:{line_of(page, "applyDamage")}: `cube::Creature::applyDamage` '
                        'is not the name or alias of any mapping entry']


def test_missing_section(kmap_repo):
    # covers: game-atlas/Missing section
    atlas = with_atlas(kmap_repo)
    edit(atlas / 'ui.md', '## Open questions\n\nNot written yet.\n\n', '')
    code, problems = fails(kmap_repo)
    assert code == 1
    assert problems == ['mapping/atlas/ui.md: missing section "## Open questions"']


def test_extra_sections_and_order(kmap_repo):
    atlas = with_atlas(kmap_repo)
    page = atlas / 'core.md'
    edit(page, '## Depth', '## Design note\n\nExtra sections are fine.\n\n## Depth')
    assert fails(kmap_repo) == (0, [])
    a, b = '## Data flow\n\nNot written yet.\n\n', '## Client and server\n\nNot written yet.\n\n'
    edit(page, a + b, b + a)
    code, problems = fails(kmap_repo)
    assert code == 1
    assert problems == [f'mapping/atlas/core.md: sections out of order; expected {SECTIONS_EXPECTED}']


def test_depth_line(kmap_repo):
    atlas = with_atlas(kmap_repo)
    page = atlas / 'world.md'
    for ok in ('reconnaissance, reviewed 2026-09-27', 'deep dive, reviewed 2026-10-01.'):
        set_depth(page, ok)
        assert fails(kmap_repo) == (0, []), ok
    for bad in ('reconnaissance', 'deep dive, reviewed 2026-13-01', 'done'):
        set_depth(page, bad)
        code, problems = fails(kmap_repo)
        assert code == 1, bad
        assert problems == [f'mapping/atlas/world.md:{line_of(page, "## Depth")}: {DEPTH_RULE}'], bad


def test_page_set_template_and_primitives(kmap_repo):
    atlas = with_atlas(kmap_repo)
    (atlas / 'saves.md').unlink()
    shutil.copy2(atlas / 'core.md', atlas / 'misc.md')
    (atlas / 'primitives.md').write_text('# Primitives\n\n- entity state: `cube::Creature::update`\n',
                                         encoding='utf-8')
    code, problems = fails(kmap_repo)
    assert code == 1
    assert problems == [
        'mapping/atlas/saves.md: missing: every subsystem has an atlas page (copy mapping/atlas/_template.md)',
        'mapping/atlas/misc.md: not an atlas page: the pages are the subsystems in workspace.toml and primitives.md']
    (atlas / 'primitives.md').write_text('# Primitives\n\n- entity state: `cube::Creature::getHp`\n',
                                         encoding='utf-8')
    (atlas / '_template.md').unlink()
    _, problems = fails(kmap_repo)
    assert 'mapping/atlas/_template.md: missing: the atlas page template' in problems
    assert ('mapping/atlas/primitives.md:3: `cube::Creature::getHp` is not the name or alias of any mapping entry'
            in problems)


def test_no_atlas_yet(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml', function_entry('cube::Thing::update', 0x4A2000))
    assert fails(kmap_repo) == (0, [])


def test_committed_atlas_in_repository():
    from kmap import atlas, config, db
    ctx = config.Context(ROOT)
    problems = []
    atlas.check_atlas(ctx, db.load(ctx), problems)
    assert [str(p) for p in problems] == []


@pytest.mark.parametrize('stem', [
    'core', 'world', 'saves',        # task 6.2
    'creatures', 'combat', 'ai',     # task 6.3
    'ui',                            # task 6.4
    'items', 'travel',               # task 6.5
    'rendering', 'audio', 'input', 'network',  # task 6.6
])
def test_reconnaissance_pages(stem):
    """Tasks 6.2 to 6.6: the page is at reconnaissance depth, every section is written, and the subsystem has
    candidate or better entries (key classes and entry points) that the page names."""
    from kmap import atlas, config, db
    ctx = config.Context(ROOT)
    loaded = db.load(ctx)
    text = loaded.atlas[stem].read_text(encoding='utf-8')
    found = atlas.sections(text)
    depth = atlas.page_depth(found)
    assert depth and depth[0] in ('reconnaissance', 'deep dive'), stem
    assert 'Not written yet.' not in text, stem
    for section in SECTIONS_EXPECTED.split(', '):
        assert any(l.strip() for l in found[section][1]), (stem, section)
    mapped = [f['name'] for bid in loaded.ws.builds for f in loaded.functions(bid)
              if f.get('subsystem') == stem and f.get('status') in ('candidate', 'verified')]
    assert len(mapped) >= 3, (stem, mapped)
    # the entry points the page names are mapped entries: qualified names as references, unqualified ones
    # as links to their docs/game/ page (the convention in mapping/conventions.md)
    refs = {name for _, name in atlas.references(text)}
    refs |= set(re.findall(r'\[(\w+)\]\(\.\./\.\./docs/game/', text))
    assert refs & set(mapped), stem


def test_ui_reconnaissance_names_the_framework():
    # covers: game-atlas/UI reconnaissance names the framework
    """Task 6.4: the ui page names the plasma:: widget base class, how a cube::*Widget is created and attached, and
    how the .plx files are loaded, each as a reference to a mapping entry."""
    from kmap import atlas, config, db
    ctx = config.Context(ROOT)
    loaded = db.load(ctx)
    text = loaded.atlas['ui'].read_text(encoding='utf-8')
    depth = atlas.page_depth(atlas.sections(text))
    assert depth and depth[0] in ('reconnaissance', 'deep dive')
    refs = {name for _, name in atlas.references(text)}
    cube_exe = next(b for b in loaded.ws.builds if b.startswith('cube-'))
    classes = {c['name']: c for c in loaded.classes(cube_exe)}
    functions = {f['name']: f for f in loaded.functions(cube_exe) if f.get('status') in ('candidate', 'verified')}
    widgets = sorted(n for n in classes if n.startswith('cube::') and n.endswith('Widget'))
    assert widgets
    # the widget base class: a plasma:: class every cube::*Widget derives from, with its constructor
    base = {n for n in refs if n.startswith('plasma::') and n in classes
            and all(n in classes[w].get('bases', []) for w in widgets)}
    assert 'plasma::Widget' in base
    assert 'plasma::Widget::Widget' in refs and 'plasma::Widget::Widget' in functions
    # created: widget constructors named on the page are mapped, one per cube::*Widget class
    ctors = {f'{w}::{w.split("::")[-1]}' for w in widgets}
    assert ctors <= set(functions)
    assert ctors & refs
    # attached and loaded: the page names the mapped entries it describes
    for name in ('plasma::set_node_widget', 'plasma::create_node', 'plasma::load_plx', 'plasma::read_plx'):
        assert name in refs and name in functions, name
    assert any('.plx' in line for line in atlas.sections(text)['Data flow'][1])


def summary_counts(text):
    """{subsystem: verified + candidate entries, summed over the builds} from survey-summary.md's subsystem
    tables."""
    counts = {}
    header = None
    for line in text.splitlines():
        cells = [c.strip() for c in line.strip().strip('|').split('|')] if line.startswith('|') else None
        if not cells:
            header = None
        elif cells[0] == 'Subsystem':
            header = cells
        elif header and not set(cells[0]) <= set('-:'):
            row = dict(zip(header, cells))
            counts[row['Subsystem']] = counts.get(row['Subsystem'], 0) + int(row['verified']) + int(row['candidate'])
    return counts


def test_recon_complete():
    # covers: game-atlas/Recon complete
    """Task 6.7, the reconnaissance milestone: every one of the 13 pages is at reconnaissance depth or deeper, and
    the committed survey-summary.md (current with mapping/) shows a candidate or better entry in each subsystem."""
    from kmap import atlas, config, db, report
    ctx = config.Context(ROOT)
    loaded = db.load(ctx)
    assert set(PAGES) <= set(loaded.atlas)
    for stem in PAGES:
        depth = atlas.page_depth(atlas.sections(loaded.atlas[stem].read_text(encoding='utf-8')))
        assert depth and depth[0] in ('reconnaissance', 'deep dive'), stem
    problems = []
    report.check_summary(ctx, loaded, problems)
    assert [str(p) for p in problems] == []
    counts = summary_counts((ctx.repo / report.SUMMARY).read_text(encoding='utf-8'))
    assert [s for s in PAGES if counts.get(s, 0) < 1] == [], counts


OLD_CANDIDATES = ['World::openSave', 'Map::openSave', 'Server::simLoop', 'Server::tickSite']


def test_old_candidates_resolved():
    # covers: game-atlas/Old candidates resolved
    """Task 7.5: the foundation deep dive is declared done (core and world at deep-dive depth), and none of the four
    1.0.1 candidates is still a candidate. Each still resolves (the names shipped, see exported-names.lock) to a
    verified entry whose notes record the resolution and whose evidence meets the promotion rule."""
    from kmap import atlas, config, db, rules
    ctx = config.Context(ROOT)
    loaded = db.load(ctx)
    for stem in ('core', 'world'):
        depth = atlas.page_depth(atlas.sections(loaded.atlas[stem].read_text(encoding='utf-8')))
        assert depth and depth[0] == 'deep dive', stem
    for old in OLD_CANDIDATES:
        hits = [(bid, f) for bid in loaded.ws.builds for f in loaded.functions(bid)
                if old == f['name'] or old in f.get('aliases', [])]
        assert hits, f'{old} no longer resolves: it shipped in 1.0.1'
        for bid, f in hits:
            assert f['status'] == 'verified', (bid, old, f['name'], f['status'])
            assert 'resolved in 7.5' in f.get('notes', ''), (bid, old, f['name'])
            tags = {i.split(':', 1)[0] for i in f['evidence']}
            assert tags & set(rules.STRONG) or len(tags & set(rules.WEAK)) >= 2, (bid, old, f['evidence'])
    problems = []
    rules.check_status_evidence(loaded, problems)
    assert [str(p) for p in problems] == []


RESOLVER_SITES = {0x45A0E0: 'cube::GameController::GameController', 0x4890A0: 'ModelResolverB',
                  0x5E3280: 'ModelResolverC'}


def test_resolvers_classified():
    # covers: game-atlas/Resolvers classified
    """Task 8.4: each of the three sites 1.0.1 listed as model resolvers has a recorded role or rejection (in its
    entry's notes and on the creatures page), the old names still resolve, and add-mod-loader task 7.7 is checked
    with a reference to the atlas. The scenario's WHEN (creatures at deep-dive depth) is declared by task 8.5."""
    from kmap import config, db
    ctx = config.Context(ROOT)
    loaded = db.load(ctx)
    page = loaded.atlas['creatures'].read_text(encoding='utf-8')
    for address, name in RESOLVER_SITES.items():
        hits = [f for f in loaded.functions(CUBE) if f['address'] == address]
        assert len(hits) == 1, hex(address)
        f = hits[0]
        assert name == f['name'] or name in f.get('aliases', []), (hex(address), f['name'])
        assert 'task 8.4' in f.get('notes', ''), (hex(address), f['name'])
        assert 'resolver' not in f['name'].lower(), f['name']          # the role, not the old guess, is the name
        assert f'0x{address:X}' in page, f'creatures.md does not classify 0x{address:X}'
    tasks = os.path.join(ROOT, 'openspec', 'changes', 'add-mod-loader', 'tasks.md')
    if not os.path.isfile(tasks):   # the design notes are not in a public checkout
        return
    text = open(tasks, encoding='utf-8').read()
    item = text[text.index('- [x] 7.7 '):]
    item = item[:item.index('\n- [')]
    assert 'mapping/atlas/creatures.md' in item


DESIGN_NOTE = 'Design note: hook points for a new ability id'


def test_hook_points_for_a_new_ability():
    # covers: game-atlas/Hook points for a new ability
    """The combat page has the design note, a table of hook points. Every name in a row's first
    column resolves to a verified entry, and the row says which side (client, server or both) the hook runs on."""
    from kmap import atlas, config, db
    ctx = config.Context(ROOT)
    loaded = db.load(ctx)
    found = atlas.sections(loaded.atlas['combat'].read_text(encoding='utf-8'))
    assert DESIGN_NOTE in found
    statuses = {}
    for e in loaded.all_entries():
        names = [e.get('name')] + e.get('aliases', [])
        if e.get('_kind') in ('field', 'value'):
            names = [f'{e.get("_parent")}.{n}' for n in names]
        for n in names:
            statuses.setdefault(n, set()).add(e.get('status'))
    rows = [line for line in found[DESIGN_NOTE][1] if line.startswith('| `')]
    assert len(rows) >= 5
    for row in rows:
        cells = [c.strip() for c in row.strip('|').split('|')]
        refs = [name for _, name in atlas.references(cells[0])]
        assert refs, row
        for name in refs:
            assert 'verified' in statuses.get(name, ()), (name, statuses.get(name))
        assert cells[3].split()[0] in ('client', 'server', 'both'), row


PRIMITIVES = ('Entity state', 'Entity list', 'Spawn', 'Animation and mode', 'Damage', 'Items', 'Camera', 'Drawing',
              'UI widgets', 'World travel', 'Abilities')
FIRST_SLICES = ('Entity state', 'Entity list', 'Spawn', 'Animation and mode', 'Damage')   # creatures, combat
FOUNDATION = 'Foundation'   # add-game-api's base layer, before the primitives


def test_ready_for_the_api_spec():
    # covers: game-atlas/Ready for the API spec
    """Task 10.1: primitives.md has a section per planned primitive, each with a table of the entries it builds on
    and their statuses. Every entry names a mapping entry (qualified or not) whose status matches the one stated.
    The first two slices' primitives cite only verified entries; each other primitive cites at least one candidate
    or better entry, or lists its gaps. add-game-api 2.3: a Foundation section comes first and, like the first
    slices, cites only verified entries."""
    from kmap import atlas, config, db
    ctx = config.Context(ROOT)
    loaded = db.load(ctx)
    statuses = {}
    for e in loaded.all_entries():
        names = [e.get('name')] + e.get('aliases', [])
        if e.get('_kind') in ('field', 'value'):
            names = [f'{e.get("_parent")}.{n}' for n in names]
        for n in names:
            statuses.setdefault(n, set()).add(e.get('status'))
    found = atlas.sections(loaded.atlas['primitives'].read_text(encoding='utf-8'))
    assert [s for s in found if s in PRIMITIVES] == list(PRIMITIVES)
    assert [s for s in found if s in (FOUNDATION,) + PRIMITIVES][0] == FOUNDATION
    for primitive in (FOUNDATION,) + PRIMITIVES:
        body = found[primitive][1]
        slice_line = next(l for l in body if l.startswith('Slice: '))
        assert (primitive in FIRST_SLICES + (FOUNDATION,)) == (not slice_line.startswith('Slice: later')), slice_line
        rows = [[c.strip() for c in l.strip().strip('|').split('|')] for l in body if l.startswith('| `')]
        assert rows, primitive
        cited = set()
        for cells in rows:
            m = re.fullmatch(r'`([^`]+)`', cells[0])
            assert m, cells
            name, status = m.group(1), cells[1]
            assert status in statuses.get(name, ()), (primitive, name, status, statuses.get(name))
            cited.add(status)
        gaps = next((l for l in body if l.startswith('Gaps: ')), '')
        if primitive in FIRST_SLICES + (FOUNDATION,):
            assert cited == {'verified'}, (primitive, cited)
        else:
            assert cited & {'verified', 'candidate'} or len(gaps) > len('Gaps: '), primitive
        assert gaps, f'{primitive}: no Gaps line'
