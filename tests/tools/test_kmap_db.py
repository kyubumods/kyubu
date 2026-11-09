"""kmap check on the mapping database: schema, status and evidence, provenance, type layout, naming and
probe references (add-mapping-workspace tasks 2.1-2.5, 5.5)."""
import os
import tomllib

from conftest import ROOT, function_entry, resolved_101

CUBE = 'cube-0x51D2CE9C'
SERVER = 'server-0x51D2CE9B'


def fails(kmap_repo):
    code, out, err = kmap_repo.check()
    assert code == 1, out + err
    return out


def test_empty_database_passes(kmap_repo):
    code, out, err = kmap_repo.check()
    assert code == 0, out + err


def test_duplicate_name(kmap_repo):
    # covers: mapping-database/Duplicate name
    kmap_repo.write(f'mapping/{CUBE}/functions/creatures.toml', function_entry('cube::Creature::update', '0x4A0000'))
    kmap_repo.write(f'mapping/{CUBE}/functions/combat.toml',
                    '# combat\n\n' + function_entry('cube::Creature::update', '0x4B0000'))
    out = fails(kmap_repo)
    line = next(l for l in out.splitlines() if "'cube::Creature::update' is already used" in l)
    assert f'mapping/{CUBE}/functions/combat.toml:4' in line
    assert f'mapping/{CUBE}/functions/creatures.toml:2' in line


def test_address_outside_the_image(kmap_repo):
    # covers: mapping-database/Address outside the image
    kmap_repo.write(f'mapping/{CUBE}/globals.toml', '''
        [[global]]
        name = "cube::far_away"
        address = 0x01000000
        type = "int32_t"
        subsystem = "core"
        status = "candidate"
        evidence = ["manual:test"]
        source = "kyubu"
        ''')
    out = fails(kmap_repo)
    assert 'global cube::far_away: address 0x1000000 is outside the cube-0x51D2CE9C image (0x400000-0x7B9FFF)' in out


def test_verified_on_one_string_alone(kmap_repo):
    # covers: mapping-database/Verified on one string alone
    kmap_repo.write(f'mapping/{CUBE}/functions/combat.toml',
                    function_entry('cube::Creature::teleport', '0x4A0000', 'verified', ['string:"Teleport"']))
    out = fails(kmap_repo)
    assert 'function cube::Creature::teleport: marked verified' in out
    assert 'one strong item' in out and 'two weak items with different tags' in out


def test_two_weak_tags_of_the_same_kind_are_not_enough(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/functions/combat.toml',
                    function_entry('cube::Creature::teleport', '0x4A0000', 'verified',
                                   ['string:"Teleport"', 'string:"Teleport %d"']))
    assert 'marked verified' in fails(kmap_repo)
    kmap_repo.write(f'mapping/{CUBE}/functions/combat.toml',
                    function_entry('cube::Creature::teleport', '0x4A0000', 'verified',
                                   ['string:"Teleport"', 'manual:only caller of the teleport effect']))
    assert kmap_repo.check()[0] == 0


TYPE_HEAD = '''
    [[type]]
    name = "cube::CreatureData"
    kind = "struct"
    size = 0x0
    status = "candidate"
    evidence = ["cuwo:cuwo/entity.pyx"]
    source = "cuwo"
    '''


def test_missing_source(kmap_repo):
    # covers: mapping-database/Missing source
    kmap_repo.write('mapping/types/creature.toml', TYPE_HEAD + '''
    [[type.field]]
    offset = 0x16C
    name = "hp"
    type = "float"
    status = "candidate"
    evidence = ["cuwo:cuwo/entity.pyx"]
    ''')
    out = fails(kmap_repo)
    assert 'field cube::CreatureData.hp: missing `source`' in out


def test_pasted_decompiler_output(kmap_repo):
    # covers: mapping-database/Pasted decompiler output
    notes = 'x' * 1200
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml',
                    function_entry('cube::World::update', '0x4A0000', extra=f'notes = "{notes}"'))
    out = fails(kmap_repo)
    assert 'notes are 1200 characters (limit 300)' in out
    assert 'decompiler output must never be committed' in out


def test_code_in_notes_is_rejected(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml',
                    function_entry('cube::World::update', '0x4A0000',
                                   extra='notes = "iVar1 = FUN_0049a2f0(param_1); return iVar1;"'))
    assert 'look like code or decompiler output' in fails(kmap_repo)


def test_cuwo_needs_a_path(kmap_repo):
    kmap_repo.write('mapping/types/creature.toml', TYPE_HEAD.replace('"cuwo:cuwo/entity.pyx"', '"cuwo:EntityData"'))
    assert 'must name a file in the cuwo repository' in fails(kmap_repo)


def test_long_evidence_is_rejected(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml',
                    function_entry('cube::World::update', '0x4A0000', evidence=['manual:' + 'y' * 90]))
    assert 'limit 80' in fails(kmap_repo)


def test_overlapping_fields(kmap_repo):
    # covers: mapping-database/Overlapping fields
    kmap_repo.write('mapping/types/creature.toml', TYPE_HEAD + '''
    [[type.field]]
    offset = 0x16C
    name = "hp"
    type = "float"
    status = "candidate"
    evidence = ["cuwo:cuwo/entity.pyx"]
    source = "cuwo"

    [[type.field]]
    offset = 0x16E
    name = "mp"
    type = "float"
    status = "candidate"
    evidence = ["cuwo:cuwo/entity.pyx"]
    source = "cuwo"
    ''')
    out = fails(kmap_repo)
    assert 'field cube::CreatureData.mp overlaps field cube::CreatureData.hp' in out
    assert 'bytes 0x16E-0x16F' in out


def test_field_past_the_size_and_unknown_type(kmap_repo):
    kmap_repo.write('mapping/types/creature.toml', TYPE_HEAD.replace('size = 0x0', 'size = 0x10') + '''
    [[type.field]]
    offset = 0xE
    name = "flags"
    type = "uint32_t"
    status = "candidate"
    evidence = ["cuwo:cuwo/entity.pyx"]
    source = "cuwo"

    [[type.field]]
    offset = 0x4
    name = "thing"
    type = "cube::NoSuchType"
    status = "candidate"
    evidence = ["cuwo:cuwo/entity.pyx"]
    source = "cuwo"
    ''')
    out = fails(kmap_repo)
    assert 'ends past the size' in out
    assert "type 'cube::NoSuchType' is not a primitive" in out


def test_base_occupies_offset_zero(kmap_repo):
    kmap_repo.write('mapping/types/widgets.toml', '''
    [[type]]
    name = "plasma::Widget"
    kind = "class"
    size = 0x20
    status = "candidate"
    evidence = ["rtti:plasma::Widget"]
    source = "rtti"

    [[type]]
    name = "cube::ChatWidget"
    kind = "class"
    size = 0x0
    base = "plasma::Widget"
    status = "candidate"
    evidence = ["rtti:cube::ChatWidget"]
    source = "rtti"

    [[type.field]]
    offset = 0x10
    name = "lines"
    type = "void*"
    status = "candidate"
    evidence = ["manual:test"]
    source = "kyubu"
    ''')
    assert 'lies inside the base plasma::Widget' in fails(kmap_repo)


def test_default_name_committed(kmap_repo):
    # covers: mapping-database/Default name committed
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml', function_entry('FUN_004a1230', '0x4A1230'))
    out = fails(kmap_repo)
    assert "'FUN_004a1230' is a Ghidra default name" in out
    assert 'give it a real name' in out and 'remove the entry' in out


def test_missing_namespace(kmap_repo):
    # covers: mapping-database/Missing namespace
    kmap_repo.write(f'mapping/{CUBE}/classes.toml', '''
    [[class]]
    name = "cube::Creature"
    rtti = ".?AVCreature@cube@@"
    status = "verified"
    evidence = ["rtti:cube::Creature"]
    source = "rtti"
    ''')
    kmap_repo.write('mapping/types/creature.toml', '''
    [[type]]
    name = "Creature"
    kind = "class"
    size = 0x0
    status = "candidate"
    evidence = ["rtti:cube::Creature"]
    source = "rtti"
    ''')
    out = fails(kmap_repo)
    assert "type Creature: type names carry their namespace; RTTI names this class cube::Creature: use 'cube::Creature'" in out


def test_method_on_an_rtti_class_needs_the_namespace(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/classes.toml', '''
    [[class]]
    name = "cube::Creature"
    rtti = ".?AVCreature@cube@@"
    status = "verified"
    evidence = ["rtti:cube::Creature"]
    source = "rtti"
    ''')
    kmap_repo.write(f'mapping/{CUBE}/functions/creatures.toml', function_entry('Creature::update', '0x4A0000'))
    assert 'use cube::Creature::update' in fails(kmap_repo)


def test_case_rules(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml',
                    function_entry('cube::World::Update', '0x4A0000') + '\n' +
                    function_entry('DoThings', '0x4A0010') + '\n' +
                    function_entry('sqlite3_prepare_v2', '0x4A0020', evidence=['library:sqlite3']) + '\n' +
                    function_entry('cube::read_packet', '0x4A0030', extra='aliases = ["ReadPacket"]'))
    out = fails(kmap_repo)
    assert "methods are lowerCamel ('Update')" in out
    assert "a free function is snake_case" in out and "'DoThings'" in out
    assert 'sqlite3_prepare_v2' not in out and 'read_packet' not in out


def test_missing_probe(kmap_repo):
    # covers: live-probes/Missing probe
    kmap_repo.write('mapping/types/creature.toml', TYPE_HEAD + '''
    [[type.field]]
    offset = 0x16C
    name = "hp"
    type = "float"
    status = "verified"
    evidence = ["probe:creature-hp"]
    source = "kyubu"
    ''')
    out = fails(kmap_repo)
    assert 'field cube::CreatureData.hp: cites probe:creature-hp, but mapping/probes/creature-hp.toml does not exist' in out


PROBE = '''
    build = "cube-0x51D2CE9C"
    kyubu = "1.0.1"
    steps = ["walked forward"]
    observations = ["position changed"]

    [[confirms]]
    entry = "{entry}"
    observed = "changes every frame"
    '''


def test_wrong_build(kmap_repo):
    # covers: live-probes/Wrong build
    kmap_repo.write('mapping/probes/walk.toml', PROBE.format(entry='cube::World::update'))
    kmap_repo.write(f'mapping/{SERVER}/functions/core.toml',
                    function_entry('cube::World::update', '0x4A0000', 'verified', ['probe:walk']))
    out = fails(kmap_repo)
    assert 'an entry of server-0x51D2CE9B cites probe:walk, which was recorded on cube-0x51D2CE9C' in out


def test_probe_record_needs_confirmations(kmap_repo):
    kmap_repo.write('mapping/probes/walk.toml', PROBE.format(entry='cube::Nothing::here'))
    assert "confirms 'cube::Nothing::here', which is not mapped" in fails(kmap_repo)


def test_vtable_evidence_must_match_classes(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/classes.toml', '''
    [[class]]
    name = "cube::GameController"
    rtti = ".?AVGameController@cube@@"
    status = "verified"
    evidence = ["rtti:cube::GameController"]
    source = "rtti"

    [[class.vtable]]
    address = 0x6FEDC8
    slots = [0x401000, 0x401010]
    ''')
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml',
                    function_entry('cube::GameController::update', '0x489130', 'verified',
                                   ['vtable:cube::GameController:1']))
    assert 'disagrees with cube-0x51D2CE9C/classes.toml (slot 1 is 0x401010)' in fails(kmap_repo)


def test_unknown_key_and_bad_subsystem(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml',
                    function_entry('cube::World::update', '0x4A0000', extra='adress = 1').replace(
                        'subsystem = "core"', 'subsystem = "physics"'))
    out = fails(kmap_repo)
    assert 'unknown key `adress`' in out and "subsystem 'physics' is not one of" in out


# --- the migrated database (task 2.5) ---------------------------------------------------------------------

def test_migrated_database_checks_clean(monkeypatch, tmp_path, capsys):
    from kmap.cli import main
    monkeypatch.setenv('KMAP_HOME', str(tmp_path / 'home'))
    monkeypatch.delenv('KMAP_GHIDRA', raising=False)
    code = main(['--repo', ROOT, 'check', '--offline'])
    out = capsys.readouterr().out
    assert code == 0, out


FIXTURE_101 = os.path.join(ROOT, 'tests', 'tools', 'fixtures', 'symbols-1.0.1')


def test_every_1_0_1_name_is_still_mapped(monkeypatch, tmp_path):
    # the 1.0.1 symbol maps as released: every name resolves to the same address, convention and status, apart
    # from the four old candidates that task 7.5 resolved (conftest.RESOLVED_101)
    from kmap import db as dbmod
    from kmap.config import Context
    monkeypatch.setenv('KMAP_HOME', str(tmp_path / 'home'))
    db = dbmod.load(Context(ROOT))
    for build, count in ((CUBE, 24), (SERVER, 14)):
        old = tomllib.load(open(os.path.join(FIXTURE_101, f'{build}.toml'), 'rb'))['symbol']
        assert len(old) == count
        for s in old:
            e = db.find_function(build, s['name'])
            assert e is not None, s['name']
            assert (e['address'], e['convention'], e['status']) == (s['address'], *resolved_101(s)), s['name']
