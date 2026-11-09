"""kmap export, reference docs in docs/game/ (add-mapping-workspace task 4.5), checked with tools/check_docs.py."""
import importlib.util
import os

from conftest import ROOT, function_entry
from test_kmap_headers import CUBE, FIXTURE_TYPES

CLASSES = '''
[[class]]
name = "cube::Creature"
rtti = ".?AVCreature@cube@@"
type_descriptor = 0x7639DC
subsystem = "creatures"
status = "verified"
evidence = ["rtti:cube::Creature"]
source = "rtti"

[[class.vtable]]
address = 0x6FC8CC
slots = [0x4A2000, 0x4A2100]

[[class]]
name = "std::exception"
rtti = ".?AVexception@std@@"
subsystem = "library"
status = "verified"
evidence = ["rtti:std::exception"]
source = "rtti"
'''

PROBE = '''
build = "cube-0x51D2CE9C"
kyubu = "1.0.1"
date = "2026-09-26"
steps = ["hit a creature"]
observations = ["its hp dropped"]

[[confirms]]
entry = "cube::Creature::update"
observed = "ran once per frame for every creature"

[[confirms]]
entry = "cube::Thing.hp"
observed = "dropped by the damage dealt"
'''

ATLAS = '''# Creatures

Every creature in the world is a [`cube::Creature`]({target}); its data is a `cube::CreatureData`.
'''


def check_docs():
    spec = importlib.util.spec_from_file_location('check_docs', os.path.join(ROOT, 'tools', 'check_docs.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def creatures(entry):
    return entry.replace('subsystem = "core"', 'subsystem = "creatures"')


def write_fixture(repo):
    repo.write('mapping/types/things.toml', FIXTURE_TYPES)
    repo.write(f'mapping/{CUBE}/classes.toml', CLASSES)
    repo.write(f'mapping/{CUBE}/functions/creatures.toml',
               creatures(function_entry('cube::Creature::update', '0x4A2000', 'verified', ['vtable:cube::Creature:0'],
                                        'prototype = "void __thiscall cube::Creature::update(cube::Creature* this, '
                                        'int32_t dt_ms)"\naliases = ["Creature::update"]')) + '\n' +
               creatures(function_entry('cube::Creature::secretGuess', '0x4A2100', 'guess')))
    repo.write(f'mapping/{CUBE}/functions/core.toml',
               function_entry('cube::helper', '0x4A3000', 'candidate', ['probe:hit', 'manual:test'],
                              'notes = "a | pipe in the notes"'))
    repo.write('mapping/probes/hit.toml', PROBE)
    for doc in ('README.md', 'hooks.md', 'lua.md'):                          # what the index links to in docs/
        repo.write(f'docs/{doc}', f'# {doc}\n')


def export(repo):
    code, out, err = repo.run('export', '--offline')
    assert code == 0, out + err
    return out


def page(repo, rel):
    return (repo.root / 'docs' / 'game' / rel).read_text(encoding='utf-8')


def test_reference_pages(kmap_repo):
    write_fixture(kmap_repo)
    export(kmap_repo)
    assert '0 changed' in export(kmap_repo)                                   # deterministic
    from kmap.config import Context
    for sub in Context(kmap_repo.root).ws.subsystems:                         # one page per subsystem
        assert (kmap_repo.root / 'docs' / 'game' / f'{sub}.md').is_file(), sub
    # one page per exported type: fields with offset, type and status, and the probes that confirm them
    thing = page(kmap_repo, 'types/cube_Thing.md')
    assert '- **Base:** [`cube::Base`](cube_Base.md)' in thing
    assert '| 0x8 | `hp` | `float` | verified | [hit](../probes.md#hit) |' in thing
    assert '| 0xC | `pos` | [`cube::Vec`](cube_Vec.md) | candidate |  |' in thing
    assert '`secret`' not in thing                                            # a guess field
    assert '- **C:** `cube_Thing` in `cubeworld/types.h`' in thing
    mode = page(kmap_repo, 'types/cube_Mode.md')
    assert '| `ATTACK` | 2 |' in mode
    # classes: methods with prototype and status, RTTI, mapped vtable slots
    creature = page(kmap_repo, 'types/cube_Creature.md')
    assert ('| `cube::Creature::update`<br>aliases: `Creature::update` | `void __thiscall cube::Creature::update('
            'cube::Creature* this, int32_t dt_ms)` | 0x4A2000 | — | verified | [hit](../probes.md#hit) |') in creature
    assert '| Cube.exe | `.?AVCreature@cube@@` | 0x7639DC | — |' in creature
    assert '| Cube.exe | 0x6FC8CC | 0x0 | 2 | 0: [`cube::Creature::update`](cube_Creature.md) |' in creature
    subsystem = page(kmap_repo, 'creatures.md')
    assert '| [`cube::Creature`](types/cube_Creature.md) | class (RTTI) | — | verified |' in subsystem
    assert '[`cube::Creature::update`](types/cube_Creature.md)' in subsystem
    core = page(kmap_repo, 'core.md')
    assert '| `cube::helper` | — | 0x4A3000 | — | candidate | [hit](probes.md#hit) | a \\| pipe in the notes |' in core
    # library classes are listed on library.md, without a page each
    assert '| `std::exception` | Cube.exe | — | verified |' in page(kmap_repo, 'library.md')
    assert not (kmap_repo.root / 'docs' / 'game' / 'types' / 'std_exception.md').exists()
    probes = page(kmap_repo, 'probes.md')
    assert '## hit' in probes and '1. hit a creature' in probes
    assert '| [`cube::Creature::update`](types/cube_Creature.md) | ran once per frame for every creature |' in probes
    assert '| [`cube::Thing.hp`](types/cube_Thing.md) | dropped by the damage dealt |' in probes
    index = page(kmap_repo, 'README.md')
    assert '| [creatures](creatures.md) | 1 | 1 | 0 |' in index
    assert '[`cube::Thing`](types/cube_Thing.md)' in index
    for p in (kmap_repo.root / 'docs' / 'game').rglob('*.md'):              # guesses stay private
        assert 'secretGuess' not in p.read_text(encoding='utf-8'), p
    problems = []
    assert check_docs().check_links(problems, root=str(kmap_repo.root)) > 30
    assert problems == []


def test_linked_from_the_atlas(kmap_repo):
    # covers: mapping-exports/Linked from the atlas
    write_fixture(kmap_repo)
    export(kmap_repo)
    target = '../../docs/game/types/cube_Creature.md'
    kmap_repo.write('mapping/atlas/creatures.md', ATLAS.format(target=target))
    resolved = (kmap_repo.root / 'mapping' / 'atlas' / target).resolve()
    assert resolved.read_text(encoding='utf-8').splitlines()[1] == '# `cube::Creature`'   # the generated page
    cd = check_docs()
    problems = []
    cd.check_links(problems, root=str(kmap_repo.root))
    assert problems == []
    code, out, err = kmap_repo.check()        # the atlas page changes nothing generated (the fixture has no signatures)
    assert '1 atlas pages' in out and 'docs/game' not in out, out + err
    # control: a link to a page that is not generated is reported
    kmap_repo.write('mapping/atlas/creatures.md', ATLAS.format(target='../../docs/game/types/cube_Creatur.md'))
    cd.check_links(problems, root=str(kmap_repo.root))
    assert problems == ['mapping/atlas/creatures.md: broken link to ../../docs/game/types/cube_Creatur.md']


def test_committed_pages_pass_check_docs():
    cd = check_docs()
    problems = []
    links = cd.check_links(problems, root=ROOT)
    assert problems == []
    assert links > 500                                                        # docs/game is included
    assert (os.path.isfile(os.path.join(ROOT, 'docs', 'game', 'types', 'cube_Creature.md')))


def test_long_and_clashing_names():
    from kmap.export_docs import MAX_SLUG, slug
    long = 'Concurrency::details::_MallocaListHolder<' + 'std::allocator<plasma::QuadVertex*>,' * 8 + '>'
    assert len(slug(long)) <= MAX_SLUG and slug(long) != slug(long.replace('List', 'Array'))
    assert slug('cube::Creature') == 'cube_Creature'
