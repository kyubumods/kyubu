"""kmap report and the survey-summary staleness check (add-mapping-workspace task 3.7).

Runs on a small made-up survey in the throw-away workspace, so it needs neither the game nor Ghidra.
"""
import json

from conftest import function_entry

CUBE = 'cube-0x51D2CE9C'
SERVER = 'server-0x51D2CE9B'
COLUMNS = ('subsystem', 'entries', 'verified', 'candidate', 'guess', 'functions', 'typed', 'classes')


def fake_survey(home, cube_functions=12):
    for bid, n in ((CUBE, cube_functions), (SERVER, 6)):
        d = home / 'survey' / bid
        d.mkdir(parents=True, exist_ok=True)
        (d / 'functions.json').write_text(json.dumps([{'entry': 0x401000 + i} for i in range(n)]), encoding='utf-8')
        (d / 'libraries.json').write_text(json.dumps({'counts': {'msvc-eh': 3, 'sqlite3': 1, 'stl': 2},
                                                      'functions': {}}), encoding='utf-8')
        (d / 'rtti.json').write_text(json.dumps({'types': 2, 'classes': [{'vtables': [0x700000], 'sizes': {'8': 1}},
                                                                         {'vtables': [], 'sizes': {}}]}),
                                     encoding='utf-8')
    (home / 'survey' / 'correlation.json').write_text(
        json.dumps({'matches': [{}], 'server_functions': 6, 'counts': {'exact-bytes': 1}}), encoding='utf-8')


def section(text, bid):
    return text.split(f'## {bid} ')[1].split('\n## ')[0]


def row(text, bid, subsystem):
    line = next(l for l in section(text, bid).splitlines() if l.startswith(f'| {subsystem} |'))
    return dict(zip(COLUMNS, (c.strip() for c in line.strip('|').split('|'))))


def report(repo):
    code, out, err = repo.run('report')
    assert code == 0, out + err
    return out, (repo.mapping / 'survey-summary.md').read_text(encoding='utf-8')


def test_summary_follows_the_database(kmap_repo):
    # covers: binary-survey/Summary follows the database
    fake_survey(kmap_repo.home)
    _, text = report(kmap_repo)
    before = row(text, CUBE, 'combat')
    code, out, err = kmap_repo.check()
    assert code == 0, out + err

    kmap_repo.write(f'mapping/{CUBE}/functions/combat.toml', '# combat\n\n' + function_entry(
        'cube::Creature::hit', '0x4A0000', 'verified', ['string:"Hit"', 'manual:test']
    ).replace('subsystem = "core"', 'subsystem = "combat"'))
    code, out, err = kmap_repo.check()
    assert code == 1
    assert 'mapping/survey-summary.md' in out and 'is stale' in out

    _, text = report(kmap_repo)
    after = row(text, CUBE, 'combat')
    assert int(after['verified']) == int(before['verified']) + 1
    assert int(after['entries']) == int(before['entries']) + 1
    code, out, err = kmap_repo.check()
    assert code == 0, out + err


def test_report_is_deterministic(kmap_repo):
    fake_survey(kmap_repo.home)
    _, first = report(kmap_repo)
    out, second = report(kmap_repo)
    assert 'unchanged' in out and first == second


def test_eh_funclets_apart_from_library_code(kmap_repo):
    fake_survey(kmap_repo.home)
    _, text = report(kmap_repo)
    cube = section(text, CUBE)
    assert '| library code (sqlite3 1, stl 2) | 3 |' in cube
    assert '| exception-handling funclets (`msvc-eh`, compiler-generated) | 3 |' in cube
    assert '| game code | 6 |' in cube
    assert '| RTTI types with a size from `operator new` | 1 |' in cube
    assert '1 of 6 `Server.exe` functions are matched' in text


def test_full_check_uses_the_fresh_survey(kmap_repo):
    from kmap import db as dbmod
    from kmap import report as reportmod
    from kmap.config import Context
    fake_survey(kmap_repo.home)
    report(kmap_repo)
    fake_survey(kmap_repo.home, cube_functions=13)   # the survey re-ran and found one more function
    ctx = Context(str(kmap_repo.root))
    db = dbmod.load(ctx)
    offline, full = [], []
    reportmod.check_summary(ctx, db, offline, offline=True)
    reportmod.check_summary(ctx, db, full, offline=False)
    assert offline == []
    assert len(full) == 1 and 'is stale' in str(full[0])


def test_no_survey(kmap_repo):
    code, out, err = kmap_repo.run('report')
    assert code != 0
    assert 'run `kmap survey` first' in out + err
