"""kmap probe: the record format, `kmap probe new` and `list`, the check rules, and the replay runner
(add-mapping-workspace tasks 5.5 and 5.6). The Missing-probe and Wrong-build scenarios are in test_kmap_db.py."""
import datetime
import hashlib
import os
import textwrap
from pathlib import Path

import pytest

from conftest import function_entry
from kmap.util import KmapError

CUBE = 'cube-0x51D2CE9C'

RECORD = '''
id = "{pid}"
build = "cube-0x51D2CE9C"
kyubu = "1.0.1"
date = "2026-09-27"
steps = ["started the game at the menu, traced two functions for ten seconds"]
observations = ["update ran every frame"]

[[confirms]]
entry = "cube::Thing::update"
observed = "called every frame"

[[confirms]]
entry = "cube::Thing::draw"
observed = "called every frame"
{replay}'''

REPLAY = '''
[replay]
host = "Cube.exe"
setup = "menu"
seconds = 30
script = """
trace add cube::Thing::update
trace add cube::Thing::draw
trace start
wait 5
trace stop
expect cube::Thing::update calls cube::Thing::update > 0
expect cube::Thing::draw calls cube::Thing::draw > 0
expect - hooks == 0
"""
'''


def mapped(kmap_repo):
    kmap_repo.write(f'mapping/{CUBE}/functions/core.toml',
                    function_entry('cube::Thing::update', '0x4A1230') + '\n' + function_entry('cube::Thing::draw', '0x4A1300'))


def test_probe_new(kmap_repo):
    mapped(kmap_repo)
    kmap_repo.write('CMakeLists.txt', 'project(kyubu VERSION 1.2.3 LANGUAGES C CXX)\n')
    code, out, err = kmap_repo.run('probe', 'new', 'thing-update')
    assert code == 0, out + err
    path = kmap_repo.mapping / 'probes' / 'thing-update.toml'
    text = path.read_text(encoding='utf-8')
    assert 'id = "thing-update"' in text and 'build = "cube-0x51D2CE9C"' in text
    assert 'kyubu = "1.2.3"' in text and f'date = "{datetime.date.today().isoformat()}"' in text
    code, out, err = kmap_repo.check()                       # a template fails until it is filled in
    assert code == 1
    assert 'probe thing-update: needs at least one [[confirms]] table' in out
    assert 'probe thing-update: `steps` is empty' in out
    code, out, err = kmap_repo.run('probe', 'new', 'thing-update')
    assert code != 0 and 'already exists' in out + err
    code, out, err = kmap_repo.run('probe', 'new', 'Thing_Update')
    assert code != 0 and 'lower-case words joined by hyphens' in out + err
    code, out, err = kmap_repo.run('probe', 'new', 'x', '--build', 'nope')
    assert code != 0 and 'unknown build' in out + err
    # Filling it in makes it pass, and the server build is a choice too.
    kmap_repo.write('mapping/probes/thing-update.toml', RECORD.format(pid='thing-update', replay=''))
    code, out, err = kmap_repo.check()
    assert code == 0, out
    code, out, err = kmap_repo.run('probe', 'new', 'server-side', '--build', 'server-0x51D2CE9B')
    assert 'build = "server-0x51D2CE9B"' in (kmap_repo.mapping / 'probes' / 'server-side.toml').read_text()


def test_probe_list(kmap_repo):
    mapped(kmap_repo)
    kmap_repo.write('mapping/probes/thing-update.toml', RECORD.format(pid='thing-update', replay=REPLAY))
    code, out, err = kmap_repo.run('probe', 'list')
    assert code == 0
    assert 'thing-update' in out and 'cube-0x51D2CE9C' in out and '2 entries, replayable' in out


def test_record_rules(kmap_repo):
    mapped(kmap_repo)
    bad = RECORD.format(pid='other', replay=REPLAY).replace('date = "2026-09-27"', 'date = "27.09.2026"')
    bad = bad.replace('setup = "menu"', 'setup = "sideways"').replace('seconds = 30', 'seconds = 5')
    bad = bad.replace('expect - hooks == 0', 'expect cube::Other::thing calls x > 0') + 'input = ["dance"]\n'
    kmap_repo.write('mapping/probes/walk.toml', bad)
    code, out, err = kmap_repo.check()
    assert code == 1
    for text in ("probe walk: id = 'other' does not match the file name walk.toml",
                 'probe walk: date must be YYYY-MM-DD',
                 'probe walk: [replay] setup must be one of menu, new-world, existing-world',
                 'probe walk: [replay] seconds must be a whole number from 10 to 3600',
                 'probe walk: [replay] input must be a list of',
                 "probe walk: the replay checks 'cube::Other::thing' (expect cube::Other::thing calls x > 0), which the "
                 'record does not confirm'):
        assert text in out, out
    kmap_repo.write('mapping/probes/walk.toml', RECORD.format(pid='walk', replay='\n[replay]\nscript = "trace start"\n'))
    code, out, err = kmap_repo.check()
    assert 'the replay script has no `expect` line' in out
    # a joining client exists only beside a Server.exe that stays at its menu
    kmap_repo.write('mapping/probes/walk.toml', RECORD.format(pid='walk', replay=REPLAY) + 'client = "join"\n')
    code, out, err = kmap_repo.check()
    assert 'probe walk: [replay] client = "join" needs host = "Server.exe" and setup = "menu"' in out, out
    kmap_repo.write('mapping/probes/walk.toml', RECORD.format(pid='walk', replay=REPLAY) + 'client = "spectate"\n')
    code, out, err = kmap_repo.check()
    assert 'probe walk: [replay] client must be one of join' in out, out
    # and a local server only beside a Cube.exe that joins it from its menu
    walk_server = RECORD.format(pid='walk', replay=REPLAY).replace('host = "Cube.exe"', 'host = "Server.exe"')
    kmap_repo.write('mapping/probes/walk.toml', walk_server + 'server = "local"\n')
    code, out, err = kmap_repo.check()
    assert 'probe walk: [replay] server = "local" needs host = "Cube.exe" and setup = "menu"' in out, out
    kmap_repo.write('mapping/probes/walk.toml', RECORD.format(pid='walk', replay=REPLAY) + 'server = "remote"\n')
    code, out, err = kmap_repo.check()
    assert 'probe walk: [replay] server must be one of local' in out, out


def test_replay_mods_rules(kmap_repo):
    # [replay] mods (add-entity-spawner 2.1): repository mod folders; kmap check fails on a missing one
    mapped(kmap_repo)
    record = RECORD.format(pid='walk', replay=REPLAY)
    kmap_repo.write('mapping/probes/walk.toml', record + 'mods = ["mods-examples/hello_lua"]\n')
    code, out, err = kmap_repo.check()
    assert code == 1
    assert ('probe walk: [replay] mods: mods-examples/hello_lua is not a mod folder in the repository '
            '(no mods-examples/hello_lua/mod.toml)') in out, out
    kmap_repo.write('mods-examples/hello_lua/scripts/common.lua', 'kyubu.info("hello")\n')   # a folder, no mod.toml
    code, out, err = kmap_repo.check()
    assert code == 1 and 'no mods-examples/hello_lua/mod.toml' in out, out
    kmap_repo.write('mods-examples/hello_lua/mod.toml', 'id = "hello_lua"\n')
    code, out, err = kmap_repo.check()
    assert code == 0, out
    for mods, text in (('"mods-examples/hello_lua"', 'mods must be a list of repository mod folders'),
                       ('[1]', 'mods must be a list of repository mod folders'),
                       ('["../hello_lua"]', "mods: '../hello_lua' must be a folder of the repository"),
                       ('["C:/mods/hello_lua"]', "mods: 'C:/mods/hello_lua' must be a folder of the repository"),
                       ('["mods-examples\\\\hello_lua"]',
                        "mods: 'mods-examples\\\\hello_lua' must be a folder of the repository, written with /"),
                       ('["dev-mods/kyubu_probe"]',
                        "mods: 'dev-mods/kyubu_probe' is kyubu_probe, which the replay installs itself"),
                       ('["mods-examples/hello_lua", "other/hello_lua"]',
                        "mods: 'mods-examples/hello_lua' and 'other/hello_lua' would both be installed as "
                        'mods/hello_lua')):
        kmap_repo.write('mapping/probes/walk.toml', record + f'mods = {mods}\n')
        code, out, err = kmap_repo.check()
        assert code == 1 and f'probe walk: [replay] {text}' in out, (mods, out)


def test_replay_mods_on_rules(kmap_repo):
    # [replay] mods_on (add-entity-spawner 5.2): the games of the replay that get the mods, e.g. only the Cube.exe
    # that joins a local server whose Server.exe runs without them
    mapped(kmap_repo)
    kmap_repo.write('mods-examples/hello_lua/mod.toml', 'id = "hello_lua"\n')
    record = RECORD.format(pid='walk', replay=REPLAY) + 'mods = ["mods-examples/hello_lua"]\n'
    kmap_repo.write('mapping/probes/walk.toml', record + 'server = "local"\nmods_on = ["Cube.exe"]\n')
    code, out, err = kmap_repo.check()
    assert code == 0, out
    for extra, text in (('mods_on = "Cube.exe"', 'mods_on must be a list of the games that get the mods'),
                        ('mods_on = []', 'mods_on must be a list of the games that get the mods'),
                        ('mods_on = ["cube.exe"]', 'mods_on must be a list of the games that get the mods'),
                        ('mods_on = ["Cube.exe", "Cube.exe"]', 'mods_on must be a list of the games that get the mods'),
                        ('mods_on = ["Server.exe"]', 'mods_on: this replay runs no Server.exe')):
        kmap_repo.write('mapping/probes/walk.toml', record + extra + '\n')
        code, out, err = kmap_repo.check()
        assert code == 1 and f'probe walk: [replay] {text}' in out, (extra, out)
    kmap_repo.write('mapping/probes/walk.toml', RECORD.format(pid='walk', replay=REPLAY) + 'mods_on = ["Cube.exe"]\n')
    code, out, err = kmap_repo.check()
    assert code == 1 and 'probe walk: [replay] mods_on says where the mods go, but the record has no mods' in out, out


# --- replay ---------------------------------------------------------------------------------------------------

LOG = '''\
2026-09-27 10:00:00.000 INFO  [kyubu] [-] phase: game_ready
2026-09-27 10:00:00.100 INFO  [kyubu_probe] [client] script started: 8 line(s)
2026-09-27 10:00:05.200 INFO  [kyubu_probe] [client] expect cube::Thing::update: held (cube::Thing::update: 312 call(s), expected > 0)
2026-09-27 10:00:05.200 INFO  [kyubu_probe] [client] expect cube::Thing::draw: FAILED (cube::Thing::draw: 0 call(s), expected > 0)
2026-09-27 10:00:05.200 INFO  [kyubu_probe] [client] expect -: held (hooks owned by kyubu_probe: 0, expected == 0)
2026-09-27 10:00:05.200 INFO  [kyubu_probe] [client] script finished: 3 expectation(s), 2 held, 1 failed
'''


def test_log_verdicts():
    from kmap import probes
    import tomllib
    record = tomllib.loads(RECORD.format(pid='p', replay=REPLAY))
    results, finished = probes.parse_log(LOG)
    assert finished and len(results) == 3
    verdict, ok = probes.verdicts(record, results, finished)
    assert not ok
    assert verdict['cube::Thing::update'] == ('held', ['cube::Thing::update: 312 call(s), expected > 0'])
    assert verdict['cube::Thing::draw'][0] == 'FAILED'
    assert verdict['-'][0] == 'held'
    # The script stopped before its last expectation: not reached, and the replay fails.
    cut = LOG.split('\n')[:3]
    verdict, ok = probes.verdicts(record, *probes.parse_log('\n'.join(cut)))
    assert not ok and verdict['cube::Thing::draw'][0] == 'not reached'
    # An entry the script does not check is reported as such.
    record['replay']['script'] = 'expect cube::Thing::update calls cube::Thing::update > 0\n'
    verdict, ok = probes.verdicts(record, *probes.parse_log(LOG))
    assert verdict['cube::Thing::draw'][0] == 'no expectation' and ok


def test_wine_waits_for_game_ready(tmp_path, monkeypatch):
    # Gamepilot's first click is lost when it comes before the start menu takes input (task 8.1, under Wine).
    from kmap import probes
    monkeypatch.setattr(probes.time, 'sleep', lambda s: None)

    class Running:
        def poll(self):
            return None

    log = tmp_path / 'latest.log'
    log.write_text('INFO [kyubu] [-] phase: preinit\n')
    assert not probes.wait_game_ready(log, 0, Running(), timeout=0)      # timed out without game_ready
    log.write_text('INFO [kyubu] [-] phase: preinit\nINFO [kyubu] [-] phase: game_ready\n')
    started = log.stat().st_mtime
    assert probes.wait_game_ready(log, started, Running())
    # A log older than this run (a kept sandbox's) doesn't count.
    assert not probes.wait_game_ready(log, started + 3600, Running(), timeout=0)


def test_input_until_and_shot(tmp_path, monkeypatch):
    # `until` lines gamepilot up with the script's `log` lines, and `shot` takes a screenshot there (task 8.2: the
    # health bar after the inspector halved hp).
    from kmap import probes
    monkeypatch.setattr(probes.time, 'sleep', lambda s: None)
    for ok in ('until hp set to half', 'shot hp-half', 'shot hud_2'):
        assert probes.INPUT.match(ok), ok
    for bad in ('until', 'shot', 'shot ../hud', 'shot a b'):
        assert not probes.INPUT.match(bad), bad

    class Running:
        def poll(self):
            return None

    class Ended:
        def poll(self):
            return 0

    log = tmp_path / 'latest.log'
    log.write_text('INFO [kyubu_probe] [client] > log hp set to half\n')
    assert probes.wait_log(log, 'hp set to half', 0, Running(), timeout=0)
    assert not probes.wait_log(log, 'level set', 0, Running(), timeout=0)
    assert not probes.wait_log(log, 'hp set to half', 0, Ended(), timeout=60)     # the game has closed


def test_wine_run_ends_when_the_script_finishes(tmp_path, monkeypatch):
    # Under Wine as on Windows, the game is closed 2 s after this run's log says the script has finished: the
    # prefix's wineserver is stopped (which ends a second host too), and wine_sandbox.sh returns when its game has.
    from kmap import probes
    monkeypatch.setattr(probes.time, 'sleep', lambda s: None)
    killed = []
    monkeypatch.setattr(probes.subprocess, 'run', lambda cmd, **k: killed.append((cmd, k['env']['WINEPREFIX'])))

    class Proc:
        def __init__(self, polls):
            self.polls, self.waited = polls, False

        def poll(self):
            return self.polls.pop(0) if self.polls else 0

        def wait(self):
            self.waited = True
            return 0

    log = tmp_path / 'latest.log'
    log.write_text('INFO [kyubu_probe] [client] script finished: 3 expectation(s), 3 held, 0 failed\n')
    monkeypatch.setenv('WINEPREFIX', '/home/k/.wine32')
    proc = Proc([None] * 100)
    probes.wait_finished(log, 0, proc)
    assert killed == [(['wineserver', '-k'], '/home/k/.wine32')] and proc.waited
    # A log older than this run (a kept sandbox's) doesn't count: the run goes on until the game has closed.
    killed.clear()
    proc = Proc([None] * 3)
    probes.wait_finished(log, log.stat().st_mtime + 3600, proc)
    assert killed == [] and proc.waited and proc.polls == []
    # No finished line yet (the script still runs, or has none): nothing is stopped.
    log.write_text('INFO [kyubu_probe] [client] script started: 3 line(s)\n')
    proc = Proc([None] * 3)
    probes.wait_finished(log, 0, proc)
    assert killed == [] and proc.waited


def test_wine_input_steps(tmp_path, monkeypatch):
    # The input steps as gamepilot receives them: `hold mouse1` holds a mouse button (a click is over before a frame
    # reads it at 4 fps), `shot` saves into the sandbox's shots/, and an `until` that never comes ends the input.
    import sys
    import types
    from kmap import probes
    calls = []
    pilot = types.SimpleNamespace(fit_window=lambda: None,
                                  hold_keys=lambda k, secs: calls.append(('hold', k, secs)),
                                  wheel=lambda d, n: calls.append(('wheel', d, n)),
                                  key=lambda k: calls.append(('key', k)),
                                  shot=lambda path: calls.append(('shot', Path(path).relative_to(tmp_path).as_posix())))
    monkeypatch.setitem(sys.modules, 'gamepilot', pilot)

    class Proc:
        def poll(self):
            return None

        def wait(self):
            return 0

    monkeypatch.setattr(probes.subprocess, 'Popen', lambda *a, **k: Proc())
    monkeypatch.setattr(probes, 'wait_game_ready', lambda *a: True)
    monkeypatch.setattr(probes, 'wait_log', lambda log, text, *a: text == 'hp halved')
    monkeypatch.setattr(probes.time, 'sleep', lambda s: None)
    monkeypatch.setattr(probes, 'wait_finished', lambda *a: None)
    replay = {'setup': 'menu', 'input': ['hold mouse1 1', 'hold w 2', 'hold w+mouse2 1', 'wheel down 2', 'until hp halved',
                                         'modfile fx_m edits/b.txt assets/a.txt',
                                         'shot hp', 'until never', 'key x']}
    # `modfile` copies a file of an installed mod over another one
    (tmp_path / 'wine' / 'mods' / 'fx_m' / 'edits').mkdir(parents=True)
    (tmp_path / 'wine' / 'mods' / 'fx_m' / 'edits' / 'b.txt').write_text('new')
    (tmp_path / 'wine' / 'mods' / 'x.txt').write_text('outside')
    probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu',
                    tmp_path / 'mod', 'Cube.exe', replay)
    assert calls == [('hold', 'mouse1', 1.0), ('hold', 'w', 2.0), ('hold', 'w+mouse2', 1.0), ('wheel', 'down', 2),
                     ('shot', 'wine/shots/hp.png')]
    assert (tmp_path / 'wine' / 'mods' / 'fx_m' / 'assets' / 'a.txt').read_text() == 'new'
    # a path that leaves the mod copies nothing and ends the input
    calls.clear()
    probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu',
                    tmp_path / 'mod', 'Cube.exe', {'setup': 'menu', 'input': ['modfile fx_m ../x.txt assets/a.txt', 'key x']})
    assert calls == [] and (tmp_path / 'wine' / 'mods' / 'fx_m' / 'assets' / 'a.txt').read_text() == 'new'
    assert probes.INPUT.match('modfile fx_m edits/b.txt assets/a.txt') and not probes.INPUT.match('modfile fx_m edits/b.txt')
    # `inbox` drops a file of an installed mod in the game's kyubu/inbox/ and leaves no temporary file
    calls.clear()
    (tmp_path / 'wine' / 'mods' / 'fx_m' / 'edits' / 'req.txt').write_text('anim rig\n')
    probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu',
                    tmp_path / 'mod', 'Cube.exe', {'setup': 'menu', 'input': ['inbox fx_m edits/req.txt rig-1.txt']})
    assert [p.name for p in (tmp_path / 'wine' / 'kyubu' / 'inbox').iterdir()] == ['rig-1.txt']
    assert (tmp_path / 'wine' / 'kyubu' / 'inbox' / 'rig-1.txt').read_text() == 'anim rig\n'
    # a source outside the mod drops nothing and ends the input
    probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu',
                    tmp_path / 'mod', 'Cube.exe', {'setup': 'menu', 'input': ['inbox fx_m ../x.txt out-1.txt', 'key x']})
    assert not (tmp_path / 'wine' / 'kyubu' / 'inbox' / 'out-1.txt').exists() and calls == []
    assert probes.INPUT.match('inbox fx_m edits/req.txt rig-1.txt') and not probes.INPUT.match('inbox fx_m edits/req.txt ../rig.txt')
    # `dropmod` copies a folder of an installed mod into mods/<name>, once
    (tmp_path / 'wine' / 'mods' / 'fx_m' / 'drop').mkdir()
    (tmp_path / 'wine' / 'mods' / 'fx_m' / 'drop' / 'mod.toml').write_text('id = "fx_d"')
    probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu',
                    tmp_path / 'mod', 'Cube.exe', {'setup': 'menu', 'input': ['dropmod fx_m drop fx_d']})
    assert (tmp_path / 'wine' / 'mods' / 'fx_d' / 'mod.toml').read_text() == 'id = "fx_d"'
    assert probes.INPUT.match('dropmod fx_m drop fx_d') and not probes.INPUT.match('dropmod fx_m ../drop Fx_d')


def test_wine_client_joins_the_server(tmp_path, monkeypatch):
    # client = "join": once the server is ready, a second sandbox runs Cube.exe in the same prefix, without the probe
    # and without killing the server's wineserver; gamepilot joins with it, and the input goes to it
    import sys
    import types
    from kmap import probes
    calls, runs, ready = [], [], []
    pilot = types.SimpleNamespace(fit_window=lambda: calls.append(('fit',)), join_server=lambda: calls.append(('join',)),
                                  hold_keys=lambda k, secs: calls.append(('hold', k, secs)))
    monkeypatch.setitem(sys.modules, 'gamepilot', pilot)

    class Proc:
        def poll(self):
            return None

        def wait(self):
            return 0

    def popen(cmd, env):
        runs.append((cmd[3:], env.get('MODS'), env.get('NOKILL')))
        return Proc()
    monkeypatch.setattr(probes.subprocess, 'Popen', popen)
    monkeypatch.setattr(probes, 'wait_game_ready', lambda log, *a: ready.append(log.relative_to(tmp_path).as_posix()))
    monkeypatch.setattr(probes.time, 'sleep', lambda s: None)
    monkeypatch.setattr(probes, 'wait_finished', lambda *a: None)
    replay = {'host': 'Server.exe', 'setup': 'menu', 'client': 'join', 'seconds': 300, 'input': ['hold w 2']}
    sb = probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu',
                         tmp_path / 'mod', 'Server.exe', replay)
    (server, smods, skill), (client, cmods, ckill) = runs
    assert server[1:] == [str(tmp_path / 'wine'), '300', 'Server.exe', '--kyubu-dev'] and smods and skill is None
    assert client[1] == str(tmp_path / 'client') and client[3:] == ['Cube.exe', '--kyubu-dev'] and 280 <= int(client[2]) <= 295
    assert cmods is None and ckill == '1'
    assert ready == ['wine/kyubu/logs/latest.log', 'client/kyubu/logs/latest.log']
    assert calls == [('fit',), ('join',), ('hold', 'w', 2.0)]
    assert probes.client_log(sb) is None
    (tmp_path / 'client' / 'kyubu' / 'logs').mkdir(parents=True)
    (tmp_path / 'client' / 'kyubu' / 'logs' / 'latest.log').write_text('connected')
    assert probes.client_log(sb) == 'connected'


def test_wine_host_joins_a_local_server(tmp_path, monkeypatch):
    # server = "local": the probed Cube.exe starts first (its run ends the prefix), then a plain Server.exe in a second
    # sandbox, without the probe and without killing the wineserver; gamepilot joins it from the host's menu
    import sys
    import types
    from kmap import probes
    calls, runs, ready = [], [], []
    pilot = types.SimpleNamespace(fit_window=lambda: calls.append(('fit',)), join_server=lambda: calls.append(('join',)),
                                  hold_keys=lambda k, secs: calls.append(('hold', k, secs)))
    monkeypatch.setitem(sys.modules, 'gamepilot', pilot)

    class Proc:
        def poll(self):
            return None

        def wait(self):
            return 0

    def popen(cmd, env):
        runs.append((cmd[3:], env.get('MODS'), env.get('NOKILL')))
        return Proc()
    monkeypatch.setattr(probes.subprocess, 'Popen', popen)
    monkeypatch.setattr(probes, 'wait_game_ready', lambda log, *a: ready.append(log.relative_to(tmp_path).as_posix()))
    monkeypatch.setattr(probes.time, 'sleep', lambda s: None)
    monkeypatch.setattr(probes, 'wait_finished', lambda *a: None)
    replay = {'host': 'Cube.exe', 'setup': 'menu', 'server': 'local', 'seconds': 300, 'input': ['hold w 2']}
    sb = probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu',
                         tmp_path / 'mod', 'Cube.exe', replay)
    (host, hmods, hkill), (server, smods, skill) = runs
    assert host[1:] == [str(tmp_path / 'wine'), '300', 'Cube.exe', '--kyubu-dev'] and hmods and hkill is None
    assert server[1] == str(tmp_path / 'server') and server[3:] == ['Server.exe', '--kyubu-dev'] and 280 <= int(server[2]) <= 295
    assert smods is None and skill == '1'
    assert ready == ['wine/kyubu/logs/latest.log', 'server/kyubu/logs/latest.log']
    assert calls == [('fit',), ('join',), ('hold', 'w', 2.0)]
    (tmp_path / 'server' / 'kyubu' / 'logs').mkdir(parents=True)
    (tmp_path / 'server' / 'kyubu' / 'logs' / 'latest.log').write_text('listening')
    assert probes.client_log(sb, 'server') == 'listening'


def test_wine_installs_mods(tmp_path, monkeypatch):
    # [replay] mods go into every host's sandbox (wine_sandbox.sh's MODS): beside the probe in the probed host's, and
    # without the probe in the second host's (server = "local" here, client = "join" alike)
    import sys
    import types
    from kmap import probes
    runs = []
    pilot = types.SimpleNamespace(fit_window=lambda: None, join_server=lambda: None)
    monkeypatch.setitem(sys.modules, 'gamepilot', pilot)

    class Proc:
        def poll(self):
            return None

        def wait(self):
            return 0

    def popen(cmd, env):
        runs.append(env.get('MODS'))
        return Proc()
    monkeypatch.setattr(probes.subprocess, 'Popen', popen)
    monkeypatch.setattr(probes, 'wait_game_ready', lambda *a: True)
    monkeypatch.setattr(probes.time, 'sleep', lambda s: None)
    monkeypatch.setattr(probes, 'wait_finished', lambda *a: None)
    probe, hello = tmp_path / 'mods' / 'kyubu_probe', tmp_path / 'mods' / 'hello_lua'
    replay = {'host': 'Cube.exe', 'setup': 'menu', 'server': 'local', 'seconds': 300}
    probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu', probe,
                    'Cube.exe', replay, [hello])
    assert runs == [f'{probe} {hello}', str(hello)]
    runs.clear()
    replay = {'host': 'Server.exe', 'setup': 'menu', 'client': 'join', 'seconds': 300}
    probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu', probe,
                    'Server.exe', replay, [hello])
    assert runs == [f'{probe} {hello}', str(hello)]
    runs.clear()
    probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu', probe,
                    'Cube.exe', {'setup': 'menu'})
    assert runs == [str(probe)]                                      # no mods: the probe alone, as before
    # mods_on: only the games it lists get them (5.2: a joined Cube.exe with the mod, a Server.exe without)
    runs.clear()
    replay = {'host': 'Cube.exe', 'setup': 'menu', 'server': 'local', 'seconds': 300, 'mods_on': ['Cube.exe']}
    probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu', probe,
                    'Cube.exe', replay, [hello])
    assert runs == [f'{probe} {hello}', None]
    runs.clear()
    replay = {'host': 'Server.exe', 'setup': 'menu', 'client': 'join', 'seconds': 300, 'mods_on': ['Cube.exe']}
    probes.run_wine(types.SimpleNamespace(repo=tmp_path), tmp_path, tmp_path / 'game', tmp_path / 'kyubu', probe,
                    'Server.exe', replay, [hello])
    assert runs == [str(probe), str(hello)]


def test_replay_installs_mods(kmap_repo, tmp_path, monkeypatch):
    # A record with mods stages them next to kyubu_probe, copied from the repository (add-entity-spawner 2.1): on
    # Windows into the game's copy, under Wine as run_wine's mods. A missing folder stops the replay before it runs.
    from kmap import probes
    mapped(kmap_repo)
    kmap_repo.write('mapping/probes/thing-update.toml',
                    RECORD.format(pid='thing-update', replay=REPLAY) + 'mods = ["mods-examples/hello_lua"]\n')
    kmap_repo.write('mods-examples/hello_lua/mod.toml', 'id = "hello_lua"\n')
    kmap_repo.write('mods-examples/hello_lua/scripts/common.lua', 'kyubu.info("hello from Lua")\n')
    game = tmp_path / 'game'
    game.mkdir()
    (game / 'Cube.exe').write_bytes(b'MZ not the game')
    kyubu = tmp_path / 'kyubu'
    probe = tmp_path / 'kyubu_probe'
    (probe / 'bin').mkdir(parents=True)
    (probe / 'mod.toml').write_text('id = "kyubu_probe"\n')
    (probe / 'bin' / 'kyubu_probe.dll').write_bytes(b'dll')
    seen = {}

    def files(mods_dir):
        return sorted(p.relative_to(mods_dir).as_posix() for p in mods_dir.rglob('*') if p.is_file())

    def write_log(log):
        log.parent.mkdir(parents=True)
        log.write_text(LOG, encoding='utf-8')

    if os.name == 'nt':
        (kyubu / 'kyubu').mkdir(parents=True)
        (kyubu / 'zlib1.dll').write_bytes(b'proxy')
        (kyubu / 'install').mkdir()
        (kyubu / 'install' / 'kyubu_install.py').write_text(
            'import os, sys\nos.makedirs(os.path.join(sys.argv[2], "kyubu", "symbols"))\n')

        def fake_windows(ctx, sandbox, host, replay):
            seen['mods'] = files(sandbox / 'mods')
            seen['lua'] = (sandbox / 'mods' / 'hello_lua' / 'scripts' / 'common.lua').read_text()
            write_log(sandbox / 'kyubu' / 'logs' / 'latest.log')
            return 0, False
        monkeypatch.setattr(probes, 'run_native', fake_windows)
        monkeypatch.setattr(probes, 'build_of', lambda ctx, sandbox, host: 'cube-0x51D2CE9C')
    else:
        (kyubu / 'loader').mkdir(parents=True)
        (kyubu / 'loader' / 'zlib1.dll').write_bytes(b'proxy')

        def fake_wine(ctx, work, game, kyubu, staged, host, replay, mods=()):
            assert [m.parent for m in mods] == [staged.parent] and [m.name for m in mods] == ['hello_lua']
            seen['mods'] = files(staged.parent)
            seen['lua'] = (mods[0] / 'scripts' / 'common.lua').read_text()
            write_log(work / 'wine' / 'kyubu' / 'logs' / 'latest.log')
            return work / 'wine'
        monkeypatch.setattr(probes, 'run_wine', fake_wine)
    out_dir = tmp_path / 'report'
    code, out, err = kmap_repo.run('probe', 'replay', 'thing-update', '--game', game, '--kyubu', kyubu,
                                   '--probe', probe, '--out', out_dir)
    assert code == 1, out + err                                         # LOG has a failed expectation
    assert seen['mods'] == ['hello_lua/mod.toml', 'hello_lua/scripts/common.lua', 'kyubu_probe/bin/kyubu_probe.dll',
                            'kyubu_probe/mod.toml', 'kyubu_probe/probe.cfg'], seen['mods']
    assert seen['lua'] == 'kyubu.info("hello from Lua")\n'
    assert ', mods hello_lua' in (out_dir / 'report.md').read_text(encoding='utf-8')
    assert files(kmap_repo.root / 'mods-examples') == ['hello_lua/mod.toml', 'hello_lua/scripts/common.lua']
    # The folder gone from the repository: refused before anything runs.
    seen.clear()
    (kmap_repo.root / 'mods-examples' / 'hello_lua' / 'mod.toml').unlink()
    code, out, err = kmap_repo.run('probe', 'replay', 'thing-update', '--game', game, '--kyubu', kyubu,
                                   '--probe', probe, '--out', out_dir)
    assert code != 0 and 'mods-examples/hello_lua is not a mod folder in the repository' in out + err
    assert not seen


def test_build_sandbox_copy_rules(tmp_path):
    # The copy rules of scripts/wine_sandbox.sh in Python (task 4.1): own Save/, pinned options.cfg, the loader in with
    # the vanilla zlib1.dll kept, this checkout's symbols and lua, the mods, and KEEP leaves the game copy alone.
    import types
    from kmap import probes
    repo = tmp_path / 'repo'
    (repo / 'symbols').mkdir(parents=True)
    (repo / 'symbols' / 'a.toml').write_text('sym')
    (repo / 'loader' / 'lua').mkdir(parents=True)
    (repo / 'loader' / 'lua' / 'cubeworld.lua').write_text('lua')
    game = tmp_path / 'game'
    (game / 'Save').mkdir(parents=True)
    (game / 'Save' / 'old.db').write_text('save')
    (game / 'Cube.exe').write_text('exe')
    (game / 'zlib1.dll').write_text('vanilla')
    (game / 'options.cfg').write_text('renderDistance 99')
    build = tmp_path / 'build'
    (build / 'loader').mkdir(parents=True)
    (build / 'loader' / 'zlib1.dll').write_text('proxy')
    mod = tmp_path / 'staged' / 'm1'
    mod.mkdir(parents=True)
    (mod / 'mod.toml').write_text('id = "m1"')
    ctx = types.SimpleNamespace(repo=repo)
    sb = probes.build_sandbox(ctx, game, 'build', build, tmp_path / 'sb', 'Cube.exe', [mod], {'render_distance': 25})
    assert [p.name for p in (sb / 'Save').iterdir()] == []
    assert (sb / 'options.cfg').read_bytes() == probes.options_cfg({'render_distance': 25}).encode()
    assert b'renderDistance 25' in (sb / 'options.cfg').read_bytes()
    assert (sb / 'zlib1.dll').read_text() == 'proxy' and (sb / 'zlib1_vanilla.dll').read_text() == 'vanilla'
    assert (game / 'zlib1.dll').read_text() == 'vanilla'                               # the original is only read
    assert (sb / 'kyubu' / 'symbols' / 'a.toml').read_text() == 'sym'
    assert (sb / 'kyubu' / 'lua' / 'cubeworld.lua').read_text() == 'lua'
    assert (sb / 'mods' / 'm1' / 'mod.toml').is_file()
    # KEEP: the saves and options stay, the mods are put in again
    (sb / 'Save' / 'mine.db').write_text('x')
    (mod / 'mod.toml').write_text('id = "m1b"')
    probes.build_sandbox(ctx, game, 'build', build, sb, 'Cube.exe', [mod], {'render_distance': 25}, keep=True)
    assert (sb / 'Save' / 'mine.db').is_file() and (sb / 'zlib1_vanilla.dll').read_text() == 'vanilla'
    assert (sb / 'mods' / 'm1' / 'mod.toml').read_text() == 'id = "m1b"'


def tree_hash(root):
    h = hashlib.sha256()
    for p in sorted(Path(root).rglob('*')):
        if p.is_file():
            h.update(str(p.relative_to(root)).encode() + p.read_bytes())
    return h.hexdigest()


@pytest.mark.skipif(os.name != 'nt', reason='the Windows runner')
def test_replay_reports_and_changes_nothing(kmap_repo, tmp_path, monkeypatch):
    # covers: live-probes/Replayable probes (a failed replay is reported and changes no status)
    from kmap import probes
    mapped(kmap_repo)
    kmap_repo.write('mapping/probes/thing-update.toml', RECORD.format(pid='thing-update', replay=REPLAY))
    game = tmp_path / 'game'
    (game / 'Save').mkdir(parents=True)
    (game / 'Cube.exe').write_bytes(b'MZ not the game')
    (game / 'Save' / 'world.db').write_bytes(b'save')
    release = tmp_path / 'kyubu-1.0.1'
    (release / 'kyubu').mkdir(parents=True)
    (release / 'zlib1.dll').write_bytes(b'proxy')
    (release / 'install').mkdir()
    (release / 'install' / 'kyubu_install.py').write_text(textwrap.dedent('''
        import os, sys
        game = sys.argv[2]
        os.makedirs(os.path.join(game, 'kyubu', 'symbols'))
        open(os.path.join(game, 'zlib1.dll'), 'w').write('proxy')
    '''))
    mod = tmp_path / 'kyubu_probe'
    (mod / 'bin' / 'Release').mkdir(parents=True)
    (mod / 'mod.toml').write_text('id = "kyubu_probe"\n')
    (mod / 'bin' / 'Release' / 'kyubu_probe.dll').write_bytes(b'dll')
    seen = {}

    def fake_run(ctx, sandbox, host, replay, second=None):
        log = sandbox / 'kyubu' / 'logs' / 'latest.log'
        seen['options'] = (sandbox / 'options.cfg').read_text()
        seen['sandbox'] = sandbox
        seen['cfg'] = (sandbox / 'mods' / 'kyubu_probe' / 'probe.cfg').read_text()
        seen['dll'] = (sandbox / 'mods' / 'kyubu_probe' / 'bin' / 'kyubu_probe.dll').is_file()
        seen['save'] = [p.name for p in (sandbox / 'Save').iterdir()]
        log.parent.mkdir(parents=True)
        log.write_text(LOG, encoding='utf-8')
        if second:
            seen['second'] = second
            seen['second_mods'] = sorted(p.name for p in (second[0] / 'mods').iterdir())
            (second[0] / 'kyubu' / 'logs').mkdir(parents=True)
            (second[0] / 'kyubu' / 'logs' / 'latest.log').write_text('second host log', encoding='utf-8')
            (second[0] / 'kyubu' / 'logs' / 'crash-1.txt').write_text('boom', encoding='utf-8')
        return 0, False

    monkeypatch.setattr(probes, 'run_native', fake_run)
    monkeypatch.setattr(probes, 'build_of', lambda ctx, sandbox, host: 'cube-0x51D2CE9C')
    before, game_before = tree_hash(kmap_repo.mapping), tree_hash(game)
    out_dir = tmp_path / 'report'
    code, out, err = kmap_repo.run('probe', 'replay', 'thing-update', '--game', game, '--kyubu', release,
                                   '--probe', mod, '--out', out_dir)
    assert code == 1, out + err
    assert 'cube::Thing::update: held' in out and 'cube::Thing::draw: FAILED' in out
    assert 'replay of thing-update: FAILED' in out
    assert 'expect cube::Thing::draw calls cube::Thing::draw > 0' in seen['cfg']      # the script became probe.cfg
    assert seen['dll'] and seen['save'] == []                                          # an empty Save/, none copied
    assert 'renderDistance 40' in seen['options']                                      # the pinned options
    report = (out_dir / 'report.md').read_text(encoding='utf-8')
    assert '| `cube::Thing::draw` | FAILED | cube::Thing::draw: 0 call(s), expected > 0 |' in report
    assert '[latest.log](latest.log)' in report and (out_dir / 'latest.log').read_text() == LOG   # log attached
    assert tree_hash(kmap_repo.mapping) == before                                      # no status changed
    assert tree_hash(game) == game_before                                              # the game was only read
    assert not seen['sandbox'].exists()                                                # the copy is gone

    # A record recorded on another build is refused before anything runs.
    monkeypatch.setattr(probes, 'build_of', lambda ctx, sandbox, host: 'server-0x51D2CE9B')
    code, out, err = kmap_repo.run('probe', 'replay', 'thing-update', '--game', game, '--kyubu', release,
                                   '--probe', mod, '--out', out_dir)
    assert code != 0 and 'was recorded on cube-0x51D2CE9C, but the game is server-0x51D2CE9B' in out + err
    # A second host (task 4.2): a sandbox of its own beside the first, no probe in it; its log and crash report are kept.
    monkeypatch.setattr(probes, 'build_of', lambda ctx, sandbox, host: 'cube-0x51D2CE9C')
    kmap_repo.write('mapping/probes/thing-update.toml',
                    RECORD.format(pid='thing-update', replay=REPLAY.replace('seconds = 30', 'seconds = 30' + chr(10) + 'server = "local"')))
    out2 = tmp_path / 'report2'
    code, out, err = kmap_repo.run('probe', 'replay', 'thing-update', '--game', game, '--kyubu', release,
                                   '--probe', mod, '--out', out2)
    assert seen['second'][0].name == 'server' and seen['second'][1] == 'Server.exe'
    assert 'kyubu_probe' not in seen['second_mods']
    assert (out2 / 'server.log').read_text() == 'second host log'
    assert (out2 / 'server-crash-1.txt').read_text() == 'boom'
    assert seen['second'][0].exists()                                                   # a crash keeps the sandboxes


# --- report on a session played by hand -----------------------------------------------------------------------

HEADER = '''\
2026-09-27 09:59:59.000 INFO  [kyubu] [-] Kyubu 1.0 starting in Cube.exe (C:\\Games\\Cube World\\Cube.exe)
2026-09-27 09:59:59.100 INFO  [kyubu] [-] Host build {build} verified (image base 0x000F0000)
'''


def test_session_report(kmap_repo, tmp_path):
    # The same per-entry report as a replay, from the log of a session someone played with the script as probe.cfg.
    mapped(kmap_repo)
    kmap_repo.write('mapping/probes/thing-update.toml', RECORD.format(pid='thing-update', replay=REPLAY))
    before = tree_hash(kmap_repo.mapping)
    log = tmp_path / 'game' / 'kyubu' / 'logs' / 'latest.log'
    log.parent.mkdir(parents=True)
    passing = LOG.replace('draw: FAILED (cube::Thing::draw: 0 call(s)', 'draw: held (cube::Thing::draw: 290 call(s)')
    log.write_text('\ufeff' + HEADER.format(build=CUBE) + passing, encoding='utf-8')
    out_dir = tmp_path / 'report'
    code, out, err = kmap_repo.run('probe', 'report', 'thing-update', '--log', log, '--out', out_dir)
    assert code == 0, out + err
    assert 'cube::Thing::draw: held (cube::Thing::draw: 290 call(s), expected > 0)' in out
    assert 'session report of thing-update: passed' in out
    report = (out_dir / 'report.md').read_text(encoding='utf-8')
    assert report.startswith('# Session report of probe thing-update')
    assert 'a session played by hand' in report and '**passed**' in report
    assert '| `cube::Thing::update` | held | cube::Thing::update: 312 call(s), expected > 0 |' in report
    assert (out_dir / 'latest.log').is_file()
    assert tree_hash(kmap_repo.mapping) == before                                      # no status changed

    # A log with two script runs: only the last counts (here the failing one), and the report says so.
    log.write_text(HEADER.format(build=CUBE) + passing + LOG, encoding='utf-8')
    code, out, err = kmap_repo.run('probe', 'report', 'thing-update', '--log', log, '--out', out_dir)
    assert code == 1 and 'cube::Thing::draw: FAILED' in out and 'cube::Thing::update: held' in out
    assert 'the last of 2 script runs' in (out_dir / 'report.md').read_text(encoding='utf-8')
    # A session still running when the log was copied: the rest is not reached.
    log.write_text(HEADER.format(build=CUBE) + ''.join(passing.splitlines(keepends=True)[:3]), encoding='utf-8')
    code, out, err = kmap_repo.run('probe', 'report', 'thing-update', '--log', log, '--out', out_dir)
    assert code == 1 and 'cube::Thing::draw: not reached' in out

    # Refused: another build, a log without the build line, a log without a script, no such log.
    log.write_text(HEADER.format(build='server-0x51D2CE9B') + passing, encoding='utf-8')
    code, out, err = kmap_repo.run('probe', 'report', 'thing-update', '--log', log)
    assert code != 0 and 'was recorded on cube-0x51D2CE9C, but the log is from server-0x51D2CE9B' in out + err
    log.write_text(passing, encoding='utf-8')
    code, out, err = kmap_repo.run('probe', 'report', 'thing-update', '--log', log)
    assert code != 0 and 'not a Kyubu log' in out + err
    log.write_text(HEADER.format(build=CUBE), encoding='utf-8')
    code, out, err = kmap_repo.run('probe', 'report', 'thing-update', '--log', log)
    assert code != 0 and 'no "script started" line' in out + err
    code, out, err = kmap_repo.run('probe', 'report', 'thing-update', '--log', tmp_path / 'nope.log')
    assert code != 0 and 'no such log' in out + err
    kmap_repo.write('mapping/probes/thing-update.toml', RECORD.format(pid='thing-update', replay=''))
    code, out, err = kmap_repo.run('probe', 'report', 'thing-update', '--log', log)
    assert code != 0 and 'has no [replay] script' in out + err
    assert tree_hash(kmap_repo.mapping) != before and not list((kmap_repo.mapping / 'probes').glob('*.md'))


def test_stage_mods_takes_a_native_mods_dll_from_the_build(kmap_repo, tmp_path):
    # A native mod's sources hold no DLL: stage_mods copies each [native] DLL of mod.toml from <build>/<root>/<id>/,
    # wherever the build put it (MinGW: bin/, MSVC: bin/<config>/); one that is not built stops the replay.
    from kmap import probes
    kmap_repo.write('mods-gameplay/nat/mod.toml',
                    'id = "nat"\n[native]\ncommon = "bin/nat.dll"\nserver = "bin/nat_srv.dll"\n')
    kmap_repo.write('mods-gameplay/nat/src/main.cpp', 'int x;\n')
    kmap_repo.write('mods-examples/lua_only/mod.toml', 'id = "lua_only"\n')
    build = tmp_path / 'build'
    (build / 'gameplay-mods' / 'nat' / 'bin' / 'Release').mkdir(parents=True)
    (build / 'gameplay-mods' / 'nat' / 'bin' / 'Release' / 'nat.dll').write_bytes(b'common dll')
    (build / 'gameplay-mods' / 'nat' / 'bin' / 'nat_srv.dll').write_bytes(b'server dll')
    (build / 'gameplay-mods' / 'other').mkdir()
    (build / 'gameplay-mods' / 'other' / 'nat.dll').write_bytes(b'wrong mod')        # another mod's folder is not read
    staged = probes.stage_mods(kmap_repo.root, ['mods-gameplay/nat', 'mods-examples/lua_only'], tmp_path / 'w', build)
    assert [m.name for m in staged] == ['nat', 'lua_only']
    assert (staged[0] / 'bin' / 'nat.dll').read_bytes() == b'common dll'
    assert (staged[0] / 'bin' / 'nat_srv.dll').read_bytes() == b'server dll'
    assert (staged[0] / 'src' / 'main.cpp').is_file()                                # sources stay, as before
    assert sorted(p.name for p in staged[1].rglob('*') if p.is_file()) == ['mod.toml']   # no [native]: copied as it is
    (build / 'gameplay-mods' / 'nat' / 'bin' / 'nat_srv.dll').unlink()
    with pytest.raises(KmapError, match=r'nat: bin/nat_srv.dll is not built'):
        probes.stage_mods(kmap_repo.root, ['mods-gameplay/nat'], tmp_path / 'w', build)
    # without a build folder nothing is looked up (the record's own tests, and callers that never had DLLs)
    assert probes.stage_mods(kmap_repo.root, ['mods-gameplay/nat'], tmp_path / 'w')[0].name == 'nat'


def test_replay_all_queue_and_fps():
    # covers: add-native-replay/replay-all (port records queue, the others run beside them)
    import threading
    import time
    from kmap import probes
    active, peak, port_peak = set(), [0], [0]
    lock = threading.Lock()

    def run(i):
        with lock:
            active.add(i)
            peak[0] = max(peak[0], len(active))
            port_peak[0] = max(port_peak[0], len(active & {'p1', 'p2', 'p3'}))
        time.sleep(0.05)
        with lock:
            active.discard(i)
        return i.upper()

    jobs = ['p1', 'p2', 'p3', 'a', 'b']
    assert probes.run_queue(jobs, {'p1', 'p2', 'p3'}, 3, run) == {i: i.upper() for i in jobs}
    assert port_peak[0] == 1                      # never two port records at once
    assert peak[0] >= 2                           # the others ran beside a port record
    assert probes.run_queue(['x'], set(), 0, run) == {'x': 'X'}
    assert probes.needs_port({'host': 'Server.exe'}) and probes.needs_port({'client': 'join'})
    assert probes.needs_port({'server': 'local'}) and not probes.needs_port({'host': 'Cube.exe'})
    text = ('headless: 90.0 fps (min 20.0), frame 1\nheadless: 100.0 fps (min 60.5), frame 2\n'
            'headless: 110.0 fps (min 30.0), frame 3\n')
    assert probes.fps_of(text) == (100.0, 20.0)
    assert probes.fps_of('nothing') == (None, None)
    table = probes.summary_table([('a', 'passed', 12.4, 100.0, 20.0), ('b', 'error', 1, None, None)])
    assert '| `a` | passed | 12 | 100.0 | 20.0 |' in table and '| `b` | error | 1 | - | - |' in table


def test_watcher_rules(kmap_repo):
    # add-skills-unleashed-multiplayer 6.2: a second client, the watcher, joins the same server as the client
    mapped(kmap_repo)
    server = RECORD.format(pid='walk', replay=REPLAY).replace('host = "Cube.exe"', 'host = "Server.exe"')
    kmap_repo.write('mapping/probes/walk.toml', server + 'watcher = true\n')
    code, out, err = kmap_repo.check()
    assert 'probe walk: [replay] watcher = true needs client = "join"' in out, out
    kmap_repo.write('mapping/probes/walk.toml', server + 'client = "join"\nwatcher = "yes"\n')
    code, out, err = kmap_repo.check()
    assert 'probe walk: [replay] watcher must be true or false' in out, out
    kmap_repo.write('mapping/probes/walk.toml', server + 'client = "join"\ninput = ["shot watcher strike"]\n')
    code, out, err = kmap_repo.check()
    assert 'probe walk: [replay] input "shot watcher strike" acts on the watcher, but the record has no watcher = true' in out, out
    kmap_repo.write('mapping/probes/walk.toml', server + 'client = "join"\nlog_expect = { watcher = ["x"], client = [] }\n')
    code, out, err = kmap_repo.check()
    assert 'probe walk: [replay] log_expect.watcher: this replay keeps no watcher.log (it keeps latest.log, client.log)' in out, out
    assert 'probe walk: [replay] log_expect.client must be a list of texts to find in client.log' in out, out
    good = (server + 'client = "join"\nwatcher = true\n'
            'input = ["shot watcher strike", "until watcher (watched) ends", "console watcher config skills_unleashed log_effects true"]\n'
            'log_expect = { watcher = ["(watched) ends `done`"], client = ["ends `done`"] }\n')
    kmap_repo.write('mapping/probes/walk.toml', good)
    code, out, err = kmap_repo.check()
    assert 'probe walk:' not in out, out
    from kmap import probes
    assert probes.replay_games({'host': 'Server.exe', 'client': 'join', 'watcher': True}) == ['Server.exe', 'Cube.exe', 'Cube.exe']
    assert probes.second_logs({'client': 'join', 'watcher': True}) == ['client', 'watcher']
    assert probes.second_logs({'server': 'local'}) == ['server'] and probes.second_logs({}) == []


def test_log_expect_rows_in_the_report(tmp_path):
    from kmap import probes
    import tomllib
    record = tomllib.loads(RECORD.format(pid='p', replay=REPLAY))
    record['replay']['log_expect'] = {'watcher': ['(watched) ends `done`', 'effect shockwave at'], 'client': ['ends `done`']}
    logs = {'watcher': 'x skills_unleashed:berserker (watched) ends `done` on creature 1\n', 'client': None}
    assert probes.log_expect_results(record['replay'], logs) == [
        ('watcher', '(watched) ends `done`', True), ('watcher', 'effect shockwave at', False), ('client', 'ends `done`', False)]
    good = LOG.replace('FAILED', 'held')
    assert probes.report(tmp_path, 'p', record, good, 'test', 'Replay', logs) == 1      # a missing text fails the replay
    text = (tmp_path / 'report.md').read_text(encoding='utf-8')
    assert '| [watcher.log](watcher.log) | `effect shockwave at` | MISSING |' in text, text
    assert '| [watcher.log](watcher.log) | `(watched) ends `done`` | found |' in text, text
    logs = {'watcher': 'effect shockwave at 1 2 3 (watched) ends `done`', 'client': 'ends `done`'}
    assert probes.report(tmp_path, 'p', record, good, 'test', 'Replay', logs) == 0
    # a session played by hand has no second logs: no rows, and the record's log_expect does not fail it
    assert probes.report(tmp_path, 'p', record, good, 'test', 'Session') == 0
    # `latest` is the probed host's own log, the text the report reads
    record['replay']['log_expect'] = {'latest': ['struck', 'never written']}
    assert probes.log_expect_problems('p', record['replay']) == []
    assert probes.report(tmp_path, 'p', record, good + 'x berserker struck\n', 'test', 'Replay', {}) == 1
    text = (tmp_path / 'report.md').read_text(encoding='utf-8')
    assert '| [latest.log](latest.log) | `struck` | found |' in text, text
    assert '| [latest.log](latest.log) | `never written` | MISSING |' in text, text


def test_watcher_input_steps(tmp_path, monkeypatch):
    # `shot watcher`, `until watcher` and `console watcher` act on the watcher's sandbox; the rest on the client's
    import sys
    import types
    from kmap import probes
    calls = []
    pilot = types.SimpleNamespace(SANDBOX='client-sb', key=lambda k: calls.append(('key', k)))
    pilot.shot = lambda path: calls.append(('shot', pilot.SANDBOX, Path(path).name))
    monkeypatch.setitem(sys.modules, 'gamepilot', pilot)
    monkeypatch.setattr(probes.time, 'sleep', lambda s: None)
    waited = []
    monkeypatch.setattr(probes, 'wait_log', lambda log, text, *a: waited.append((Path(log).parts[-4], text)) or text != 'never')
    consoled = []
    monkeypatch.setattr(probes, 'console_step', lambda sb, line, name: consoled.append((Path(sb).name, line, name)))
    sb, watcher = tmp_path / 'game', tmp_path / 'watcher'
    replay = {'input': ['shot watcher strike', 'key 4', 'until watcher (watched) ends', 'console watcher config a b true',
                        'until watcher never', 'key x']}
    probes.drive_input(sb, tmp_path / 'client', replay, sb / 'kyubu' / 'logs' / 'latest.log', 0, None, [], watcher)
    assert calls == [('shot', str(watcher), 'watcher-strike.png'), ('key', '4')]
    assert pilot.SANDBOX == 'client-sb'                     # the client's again after the watcher's shot
    assert waited == [('watcher', '(watched) ends'), ('watcher', 'never')]
    assert consoled == [('watcher', 'config a b true', '000-watcher')]
    # without a watcher, the first watcher step ends the input
    calls.clear()
    probes.drive_input(sb, tmp_path / 'client', {'input': ['shot watcher x', 'key 4']}, sb / 'l', 0, None, [])
    assert calls == []
