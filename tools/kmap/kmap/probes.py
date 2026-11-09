"""kmap probe: probe records in mapping/probes/ (live-probes, "Probe records" and "Replayable probes").

  kmap probe new <id> [--build B]     writes mapping/probes/<id>.toml, a template with the build, the Kyubu version
                                      and today's date filled in
  kmap probe list                     the records: build, Kyubu version, date, entries confirmed, replayable or not
  kmap probe replay <id> --game DIR --kyubu DIR [--probe DIR] [--out DIR] [--keep]
                                      runs the record's [replay] unattended and reports, per confirmed entry,
                                      whether the observation held. It never changes mapping/.
  kmap probe report <id> --log FILE [--out DIR]
                                      the same report for a session played by hand: the record's script ran as
                                      probe.cfg, and FILE is the game's kyubu/logs/latest.log. It never changes
                                      mapping/ either.

A replay copies the game into $KMAP_HOME/tmp/replay-<id>/, installs Kyubu and the kyubu_probe mod there with the
record's script as probe.cfg, and starts the game in dev mode. The record's `mods` (repository mod folders) are
installed next to kyubu_probe (a native mod's DLLs come from the --kyubu folder), and into the second host's sandbox
too (without the probe), unless `mods_on` names only some of the games. The probe runs the script once the game is ready and
logs one line per `expect`; the runner reads them back from the log, which it keeps next to the report. The game is
closed 2 s after the script has finished (under Wine, once the input is done too), or after the record's `seconds`.
  - Windows: the game runs headless (`--kyubu-dev --kyubu-headless`) in a sandbox built by `build_sandbox`, and
    scripts/gamepilot.py's native backend drives the menus (`setup`) and the keyboard (`input`) with inbox files.
    `--kyubu` is a release folder or a CMake build folder.
  - Linux: the game runs under Wine through scripts/wine_sandbox.sh, and scripts/gamepilot.py drives the menus
    (`setup`) and the keyboard (`input`). With host = "Server.exe" and client = "join", a plain Kyubu-loaded Cube.exe
    (no probe) runs beside the server in the same Wine prefix, joins 127.0.0.1 with a new character, and takes the
    input; its log is kept as client.log. With host = "Cube.exe" and server = "local", it is the other way round: a
    plain Kyubu-loaded Server.exe runs beside the probed Cube.exe, which joins it; the server's log is kept as
    server.log.
  - Two clients (Windows only): with client = "join" and watcher = true, a third game, the watcher, a plain Kyubu-loaded
    Cube.exe, joins the same server as "Watcher" after the client has joined. The input still drives the client; the
    steps `shot watcher <name>`, `until watcher <text>` and `console watcher <command>` act on the watcher instead.
    Its log is kept as watcher.log. `log_expect = { client = [...], watcher = [...], server = [...] }` lists text
    that must appear in a second game's log: each is a row of the report, and a missing one fails the replay.
    `latest` names the probed host's own log (latest.log), on any replay.
"""
import datetime
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
import zipfile
from pathlib import Path

from . import db as dbmod
from .util import KmapError, say, toml_str

ID = re.compile(r'^[a-z0-9]+(?:-[a-z0-9]+)*$')
SETUPS = ('menu', 'new-world', 'existing-world')
HOSTS = ('Cube.exe', 'Server.exe')
CLIENTS = ('join',)
SERVERS = ('local',)
INPUT = re.compile(r'^(wait \d+(\.\d+)?|key \S+|hold \S+ \d+(\.\d+)?|console .+|type .+|click \d+ \d+|move \d+ \d+|until .+|wheel (up|down) \d+|'
                   r'shot [A-Za-z0-9_-]+|shot watcher [A-Za-z0-9_-]+|until watcher .+|console watcher .+|modfile [a-z][a-z0-9_]* [A-Za-z0-9_./-]+ [A-Za-z0-9_./-]+|'
                   r'inbox [a-z][a-z0-9_]* [A-Za-z0-9_./-]+ [A-Za-z0-9_-]+\.txt|'
                   r'dropmod [a-z][a-z0-9_]* [A-Za-z0-9_./-]+ [a-z][a-z0-9_]*)$')
EXPECT_LINE = re.compile(r'\[kyubu_probe\] \[[^\]]*\] expect (\S+): (held|FAILED) \((.*)\)$')
FINISHED = re.compile(r'\[kyubu_probe\] \[[^\]]*\] script finished: (\d+) expectation')
STARTED = re.compile(r'\[kyubu_probe\] \[[^\]]*\] script started: ')
HOST_BUILD = re.compile(r'\[kyubu\] \[[^\]]*\] Host build (\S+) verified')


def project_version(ctx):
    cmake = ctx.repo / 'CMakeLists.txt'
    m = re.search(r'project\(kyubu VERSION (\S+)', cmake.read_text(encoding='utf-8')) if cmake.is_file() else None
    return m.group(1) if m else '0.0.0'


def script_expectations(script):
    """[(entry, line)] of the `expect` lines of a replay script."""
    out = []
    for line in script.splitlines():
        line = line.strip()
        if line.startswith('kyubu_probe:'):
            line = line[len('kyubu_probe:'):]
        parts = line.split()
        if len(parts) >= 3 and parts[0] == 'expect':
            out.append((parts[1], line))
    return out


# Mods that live in their own repositories. Probe records name them by their folder in the development tree; a
# checkout without the folder (the public Kyubu repository) accepts them. Replaying such a probe needs the mod.
EXTERNAL_MODS = ('mods-gameplay/emtweaks', 'mods-gameplay/level_rework', 'mods-gameplay/skills_unleashed',
                 'mods-gameplay/title_plus')


def mods_problems(pid, mods, repo=None):
    """Problems of [replay] mods: repository mod folders, each installed as mods/<folder name>. With `repo`, each
    folder must exist there and hold a mod.toml."""
    if not isinstance(mods, list) or not all(isinstance(m, str) and m for m in mods):
        return [f'probe {pid}: [replay] mods must be a list of repository mod folders, e.g. "mods-examples/hello_lua"']
    out, names = [], {}
    for m in mods:
        parts = m.split('/')
        if '\\' in m or m.startswith('/') or ':' in m or '..' in parts or '.' in parts or '' in parts:
            out.append(f'probe {pid}: [replay] mods: {m!r} must be a folder of the repository, written with / '
                       '(e.g. "mods-examples/hello_lua")')
            continue
        if parts[-1] == 'kyubu_probe':
            out.append(f'probe {pid}: [replay] mods: {m!r} is kyubu_probe, which the replay installs itself')
            continue
        if parts[-1] in names:
            out.append(f'probe {pid}: [replay] mods: {names[parts[-1]]!r} and {m!r} would both be installed as '
                       f'mods/{parts[-1]}')
        names.setdefault(parts[-1], m)
        if repo is not None and not (Path(repo) / m / 'mod.toml').is_file() and not (
                m in EXTERNAL_MODS and not (Path(repo) / m).exists()):
            out.append(f'probe {pid}: [replay] mods: {m} is not a mod folder in the repository (no {m}/mod.toml)')
    return out


def replay_games(replay):
    """The games a replay runs: the probed host, and the second host with client = "join" or server = "local"."""
    host = replay.get('host', 'Cube.exe')
    if replay.get('client') == 'join':
        return [host, 'Cube.exe', 'Cube.exe'] if replay.get('watcher') is True else [host, 'Cube.exe']
    if replay.get('server') == 'local':
        return [host, 'Server.exe']
    return [host]


def mods_for(replay, game, mods):
    """The staged mods that go into `game`'s sandbox: all of them, unless [replay] mods_on leaves the game out."""
    return list(mods) if game in replay.get('mods_on', HOSTS) else []


def mods_on_problems(pid, r):
    """Problems of [replay] mods_on: the games of the replay that get the mods (default: every game)."""
    on = r['mods_on']
    if not isinstance(on, list) or not on or not all(g in HOSTS for g in on) or len(set(on)) != len(on):
        return [f'probe {pid}: [replay] mods_on must be a list of the games that get the mods '
                f'({", ".join(HOSTS)}), e.g. ["Cube.exe"]']
    if 'mods' not in r:
        return [f'probe {pid}: [replay] mods_on says where the mods go, but the record has no mods']
    return [f'probe {pid}: [replay] mods_on: this replay runs no {g}' for g in on if g not in replay_games(r)]


WATCHER_STEP = re.compile(r'^(shot|until|console) watcher ')


def second_logs(replay):
    """The names of the logs a replay keeps besides the probed host's: 'client' (client = "join"), 'watcher'
    (watcher = true), 'server' (server = "local")."""
    if replay.get('client') == 'join':
        return ['client', 'watcher'] if replay.get('watcher') is True else ['client']
    return ['server'] if replay.get('server') == 'local' else []


def log_expect_problems(pid, r):
    """Problems of [replay] log_expect: a table of log names (`latest`, the probed host's, or a second game's), each
    a list of texts to find in that log."""
    t = r['log_expect']
    names = ['latest'] + second_logs(r)
    if not isinstance(t, dict) or not t:
        return [f'probe {pid}: [replay] log_expect must be a table such as {{ watcher = ["text"] }}']
    out = []
    for name, texts in t.items():
        if name not in names:
            out.append(f'probe {pid}: [replay] log_expect.{name}: this replay keeps no {name}.log '
                       f'(it keeps {", ".join(n + ".log" for n in names) or "none"})')
        if not isinstance(texts, list) or not texts or not all(isinstance(x, str) and x.strip() for x in texts):
            out.append(f'probe {pid}: [replay] log_expect.{name} must be a list of texts to find in {name}.log')
    return out


def log_expect_results(replay, logs):
    """[(name, text, found)] for each text of [replay] log_expect; `logs` maps a log name to its text (None: no log)."""
    out = []
    for name, texts in (replay.get('log_expect') or {}).items():
        body = logs.get(name) or ''
        for x in texts:
            out.append((name, x, x in body))
    return out


def replay_problems(pid, p, repo=None):
    """Problems of a record's [replay] table (used by kmap check). With `repo`, the `mods` folders must exist."""
    r = p.get('replay')
    if r is None:
        return []
    if not isinstance(r, dict):
        return [f'probe {pid}: `replay` must be a table']
    out = []
    script = r.get('script')
    if not isinstance(script, str) or not script.strip():
        out.append(f'probe {pid}: [replay] needs `script`, the kyubu_probe commands to run (docs/probes.md)')
        script = ''
    if r.get('host', 'Cube.exe') not in HOSTS:
        out.append(f'probe {pid}: [replay] host must be one of {", ".join(HOSTS)}')
    if r.get('setup', 'menu') not in SETUPS:
        out.append(f'probe {pid}: [replay] setup must be one of {", ".join(SETUPS)}')
    if 'client' in r:
        if r['client'] not in CLIENTS:
            out.append(f'probe {pid}: [replay] client must be one of {", ".join(CLIENTS)}')
        elif r.get('host', 'Cube.exe') != 'Server.exe' or r.get('setup', 'menu') != 'menu':
            out.append(f'probe {pid}: [replay] client = "join" needs host = "Server.exe" and setup = "menu"')
    if 'server' in r:
        if r['server'] not in SERVERS:
            out.append(f'probe {pid}: [replay] server must be one of {", ".join(SERVERS)}')
        elif r.get('host', 'Cube.exe') != 'Cube.exe' or r.get('setup', 'menu') != 'menu':
            out.append(f'probe {pid}: [replay] server = "local" needs host = "Cube.exe" and setup = "menu"')
    if 'fast_join' in r:
        if not isinstance(r['fast_join'], bool):
            out.append(f'probe {pid}: [replay] fast_join must be true or false')
        elif r['fast_join'] and r.get('client') != 'join':
            out.append(f'probe {pid}: [replay] fast_join = true needs client = "join" (it skips the joining clients\' menus)')
    if 'watcher' in r:
        if not isinstance(r['watcher'], bool):
            out.append(f'probe {pid}: [replay] watcher must be true or false')
        elif r['watcher'] and r.get('client') != 'join':
            out.append(f'probe {pid}: [replay] watcher = true needs client = "join" (the watcher joins the same server)')
    watching = r.get('watcher') is True
    for step in r.get('input', []) if isinstance(r.get('input', []), list) else []:
        if isinstance(step, str) and WATCHER_STEP.match(step) and not watching:
            out.append(f'probe {pid}: [replay] input "{step}" acts on the watcher, but the record has no watcher = true')
    if 'log_expect' in r:
        out.extend(log_expect_problems(pid, r))
    rd = r.get('render_distance')
    if rd is not None and not (isinstance(rd, int) and not isinstance(rd, bool) and 5 <= rd <= 100):
        out.append(f'probe {pid}: [replay] render_distance must be a whole number from 5 to 100')
    secs = r.get('seconds', 120)
    if not isinstance(secs, int) or isinstance(secs, bool) or not 10 <= secs <= 3600:
        out.append(f'probe {pid}: [replay] seconds must be a whole number from 10 to 3600')
    inputs = r.get('input', [])
    if not isinstance(inputs, list) or not all(isinstance(i, str) and INPUT.match(i) for i in inputs):
        out.append(f'probe {pid}: [replay] input must be a list of "wait <s>", "key <key>", "hold <key> <s>", '
                   '"console <command>", "type <text>", "click <x> <y>", "move <x> <y>", "wheel up|down <n>", "until <log text>", "shot <name>", "shot watcher <name>", "until watcher <text>", "console watcher <command>", "modfile <mod> <from> <to>", "inbox <mod> <from> <name>.txt" or "dropmod <mod> <dir> <name>"')
    if 'mods' in r:
        out.extend(mods_problems(pid, r['mods'], repo))
    if 'mods_on' in r:
        out.extend(mods_on_problems(pid, r))
    confirmed = {c.get('entry') for c in p.get('confirms', []) if isinstance(c, dict)}
    expects = script_expectations(script)
    for entry, line in expects:
        if entry != '-' and entry not in confirmed:
            out.append(f'probe {pid}: the replay checks {entry!r} ({line}), which the record does not confirm')
    if script and not expects:
        out.append(f'probe {pid}: the replay script has no `expect` line, so a replay could not report anything')
    return out


def record_problems(pid, p, repo=None):
    """Format rules beyond the field types that rules.check_probes checks (`repo`: see replay_problems)."""
    out = []
    if not ID.match(pid):
        out.append(f'probe {pid}: the file name must be lower-case words joined by hyphens (e.g. creature-hp)')
    if p.get('id') is not None and p.get('id') != pid:
        out.append(f'probe {pid}: id = {p.get("id")!r} does not match the file name {pid}.toml')
    d = p.get('date')
    if d is not None and not (isinstance(d, (str, datetime.date)) and re.fullmatch(r'\d{4}-\d\d-\d\d', str(d))):
        out.append(f'probe {pid}: date must be YYYY-MM-DD')
    for k in ('steps', 'observations'):
        if isinstance(p.get(k), list) and not p[k]:
            out.append(f'probe {pid}: `{k}` is empty: say what was done and what was seen')
    return out + replay_problems(pid, p, repo)


# --- new and list --------------------------------------------------------------------------------------------

TEMPLATE = '''# Probe record (live-probes). Fill in the steps, the observations and what each confirms, then cite it from the
# entries as evidence "probe:{id}". docs/probes.md explains the format; kmap check checks it.
id = {id_q}
build = {build_q}
kyubu = {kyubu_q}
date = {date_q}
steps = [
    # "Started a new world, opened the inspector on the local creature",
]
observations = [
    # "hp went from 250 to 230 when a wolf hit the player",
]

# One table per mapping entry the probe confirms (a function, global, type, field as Type.field, or enum value).
# [[confirms]]
# entry = "cube::CreatureData.hp"
# observed = "decreased by the damage shown on screen, twice"

# Optional: a script that repeats the observation unattended (kmap probe replay {id}).
# [replay]
# host = "Cube.exe"           # or "Server.exe"
# setup = "menu"              # "new-world" or "existing-world" need gamepilot (Windows native, or Linux Wine)
# client = "join"             # Server.exe only: a Cube.exe joins 127.0.0.1 and takes the input (Windows native, or Linux Wine)
# server = "local"            # Cube.exe only: a Server.exe runs beside it, and the host joins 127.0.0.1 (Windows native, or Linux Wine)
# watcher = true              # with client = "join": a second Cube.exe joins too, as "Watcher" (Windows native only); its log: watcher.log
# fast_join = true            # with client = "join": the clients join by mods-dev/auto_enter with a saved character, both at once (Windows native)
# log_expect = {{ watcher = ["text"] }}   # texts that must appear in a second game's log (client, watcher, server)
# seconds = 120               # the game is closed after this, or once the script has finished
# render_distance = 40        # Wine: the options' renderDistance (5 to 100; default 40, or KMAP_RENDER_DISTANCE): lower is faster
# mods = []                   # repository mod folders installed next to kyubu_probe, e.g. "mods-examples/hello_lua"
# mods_on = ["Cube.exe"]      # only these games get the mods (default: every game, the second host's too)
# input = []                 # gamepilot: "wait 5", "key w", "hold w 10", "console <command>", "click <x> <y>",
#                             # "move <x> <y>" (the cursor alone, a hover), "until <log text>" (waits for the script's `log <text>`), "shot <name>" (a screenshot);
#                             # "hold mouse1 1" holds a mouse button, "hold w+mouse2 1" several at once, "wheel down 8" turns the mouse wheel (camera zoom);
#                             # "modfile <mod> <from> <to>" copies a file of an installed mod over another while the game runs (hot reload);
#                             # "inbox <mod> <from> <name>.txt" drops a file of an installed mod in the game's kyubu/inbox/ (the dev-mode file inbox);
#                             # "dropmod <mod> <dir> <name>" copies a folder of an installed mod into mods/<name> while the game runs (a mod dropped in by hand)
# script = """
# trace add cube::GameController::update
# trace start
# wait 10
# trace stop
# expect cube::GameController::update calls cube::GameController::update > 0
# """
'''


def cmd_new(ctx, pid, build):
    if not ID.match(pid):
        raise KmapError(f'probe id {pid!r}: use lower-case words joined by hyphens, e.g. creature-hp')
    if build not in ctx.ws.builds:
        raise KmapError(f'unknown build {build!r} (one of {", ".join(ctx.ws.builds)})')
    path = ctx.mapping / 'probes' / f'{pid}.toml'
    if path.exists():
        raise KmapError(f'{path} already exists')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(TEMPLATE.format(id=pid, id_q=toml_str(pid), build_q=toml_str(build),
                                    kyubu_q=toml_str(project_version(ctx)),
                                    date_q=toml_str(datetime.date.today().isoformat())), encoding='utf-8', newline='\n')
    say(f'wrote {path.relative_to(ctx.repo).as_posix()}: fill it in; kmap check fails until it confirms an entry')
    return 0


def cmd_list(ctx):
    db = dbmod.load(ctx)
    if not db.probes:
        say('no probe records in mapping/probes/')
        return 0
    for pid, p in sorted(db.probes.items()):
        confirms = p.get('confirms', [])
        say(f'{pid:32} {p.get("build", "?"):18} Kyubu {p.get("kyubu", "?"):8} {str(p.get("date", "")):10} '
            f'{len(confirms)} entr{"y" if len(confirms) == 1 else "ies"}'
            f'{", replayable" if isinstance(p.get("replay"), dict) else ""}')
    return 0


# --- replay --------------------------------------------------------------------------------------------------

def parse_log(text):
    """(results [(entry, held, detail)], finished) from a Kyubu log."""
    results, finished = [], False
    for line in text.splitlines():
        m = EXPECT_LINE.search(line)
        if m:
            results.append((m.group(1), m.group(2) == 'held', m.group(3)))
        elif FINISHED.search(line):
            finished = True
    return results, finished


def verdicts(record, results, finished):
    """{entry: (verdict, [details])} for every confirmed entry, and whether the replay passed. Verdicts: held,
    FAILED, not reached (the script did not get to its expectation), no expectation (nothing to replay)."""
    expected = {}
    for entry, _ in script_expectations(record['replay']['script']):
        expected[entry] = expected.get(entry, 0) + 1
    got = {}
    for entry, held, detail in results:
        got.setdefault(entry, []).append((held, detail))
    out, ok = {}, finished
    entries = [c['entry'] for c in record.get('confirms', [])] + (['-'] if '-' in expected else [])
    for entry in entries:
        rs = got.get(entry, [])
        details = [d for _, d in rs]
        if not expected.get(entry):
            out[entry] = ('no expectation', details)
        elif any(not h for h, _ in rs):
            out[entry], ok = ('FAILED', details), False
        elif len(rs) < expected[entry]:
            out[entry], ok = ('not reached', details), False
        else:
            out[entry] = ('held', details)
    return out, ok


def write_report(out_dir, pid, record, verdict, ok, finished, log_name, how, title='Replay', log_checks=()):
    lines = [f'# {title} of probe {pid}', '',
             f'- Record: `mapping/probes/{pid}.toml` (build {record.get("build")}, recorded with Kyubu '
             f'{record.get("kyubu")} on {record.get("date")})',
             f'- Run: {how}',
             f'- Script finished: {"yes" if finished else "no"}',
             f'- Result: **{"passed" if ok else "FAILED"}**. No status was changed: a person looks at the entries '
             'before any status changes.',
             f'- Log: [{log_name}]({log_name})', '',
             '| Entry | Result | Observed |', '|---|---|---|']
    for entry, (v, details) in verdict.items():
        obs = '; '.join(details).replace('|', '\\|') or '-'
        lines.append(f'| `{entry}` | {v} | {obs} |')
    if log_checks:
        lines += ['', '| Log | Text | Result |', '|---|---|---|']
        for name, text, found in log_checks:
            lines.append(f'| [{name}.log]({name}.log) | `{text.replace("|", chr(92) + "|")}` | {"found" if found else "MISSING"} |')
    (out_dir / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def find_payload(kyubu):
    """('release', dir) for a player release (zlib1.dll, kyubu/, install/), ('build', dir) for a CMake build folder."""
    kyubu = Path(kyubu)
    if (kyubu / 'zlib1.dll').is_file() and (kyubu / 'kyubu').is_dir():
        return 'release', kyubu
    if (kyubu / 'loader' / 'zlib1.dll').is_file():
        return 'build', kyubu
    raise KmapError(f'{kyubu} is neither a Kyubu release folder (zlib1.dll and kyubu/) nor a build folder '
                    '(loader/zlib1.dll)')


def find_probe(kyubu, probe):
    if probe:
        p = Path(probe)
    else:
        cands = [Path(kyubu) / 'dev-mods' / 'kyubu_probe']
        p = next((c for c in cands if c.is_dir()), cands[0])
    if not (p / 'mod.toml').is_file():
        raise KmapError(f'{p} is not the kyubu_probe mod (no mod.toml): pass --probe <built mod folder>')
    if not list(p.rglob('kyubu_probe.dll')):
        raise KmapError(f'{p} has no kyubu_probe.dll: build it first (the kyubu_probe target)')
    return p


def stage_probe(probe, dest, script):
    """The mod folder as the game loads it: mod.toml, bin/kyubu_probe.dll (whatever the build's layout) and the
    replay script as probe.cfg."""
    shutil.rmtree(dest, ignore_errors=True)
    (dest / 'bin').mkdir(parents=True)
    shutil.copy2(probe / 'mod.toml', dest / 'mod.toml')
    shutil.copy2(next(probe.rglob('kyubu_probe.dll')), dest / 'bin' / 'kyubu_probe.dll')
    (dest / 'probe.cfg').write_text(script, encoding='utf-8', newline='\n')


def built_dlls(src, kyubu):
    """{path in the mod: built DLL} for the `[native]` entries of the mod folder `src`. The build (or release) folder
    holds the mod's built folder as <kyubu>/<root>/<id>/ (kyubu_native_mod's staging: `mods/`, `gameplay-mods/`;
    the release: `example-mods/`, `gameplay-mods/`); the DLL is found in it by name, whatever the build's layout."""
    manifest = tomllib.loads((src / 'mod.toml').read_text(encoding='utf-8'))
    out = {}
    for rel in (manifest.get('native') or {}).values():
        found = next((d for root in sorted(Path(kyubu).iterdir()) if (root / manifest['id']).is_dir()
                      for d in (root / manifest['id']).rglob(Path(rel).name)), None)
        if found is None:
            raise KmapError(f'{src.name}: {rel} is not built: {kyubu} has no <folder>/{manifest["id"]}/**/'
                            f'{Path(rel).name} (build the {manifest["id"]} target)')
        out[rel] = found
    return out


def stage_mods(repo, mods, dest, kyubu=None):
    """The record's `mods`, copied as they are from the repository into dest/<folder name> (next to the staged
    kyubu_probe, whose path has no spaces under $KMAP_HOME, unlike the checkout's). Returns the staged folders.
    A native mod's sources hold no DLL: with `kyubu` (the build or release folder), each DLL its mod.toml names under
    `[native]` is copied in from the build, and a DLL that is not built stops the replay."""
    out = []
    for m in mods:
        d = dest / m.split('/')[-1]
        shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(Path(repo) / m, d, ignore=shutil.ignore_patterns('__pycache__'))
        if kyubu is not None:
            for rel, dll in built_dlls(d, kyubu).items():
                (d / rel).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dll, d / rel)
        out.append(d)
    return out


def copy_game(ctx, game, dest, host):
    """A throw-away copy of the game (the original is only read). Server.exe comes out of Server.zip if needed."""
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(game, dest, ignore=shutil.ignore_patterns('Save'))
    if host == 'Server.exe' and not (dest / 'Server.exe').is_file():
        for b in ctx.ws.builds.values():
            if b.file == 'Server.exe' and b.archive and (dest / b.archive).is_file():
                with zipfile.ZipFile(dest / b.archive) as z:
                    name = next(n for n in z.namelist() if n.replace('\\', '/').split('/')[-1] == 'Server.exe')
                    (dest / 'Server.exe').write_bytes(z.read(name))


def build_of(ctx, sandbox, host):
    from .util import sha256_file
    digest = sha256_file(sandbox / host)
    for bid, b in ctx.ws.builds.items():
        if b.file == host and b.sha256 == digest:
            return bid
    raise KmapError(f'{host} in the game folder is not a pinned build (SHA-256 {digest})')


def options_cfg(replay):
    """The sandbox's options.cfg, as scripts/wine_sandbox.sh writes it: the render distance pinned (the record's
    `render_distance`, else $KMAP_RENDER_DISTANCE, else 40: a percentage of the game's chunk mesh budget), sound off."""
    rd = render_env(replay).get('RENDER_DISTANCE', '40')
    lines = ['fullscreen 0', 'resolutionX 1280', 'resolutionY 720', 'antiAliasing 0', f'renderDistance {rd}',
             'soundVolume 0', 'musicVolume 0', 'cameraSpeed 80', 'cameraSmoothness 0', 'language 0']
    return ''.join(l + '\r\n' for l in lines)


def build_sandbox(ctx, game, kind, kyubu, dest, host, mods, replay, keep=None):
    """The throw-away game folder of one host, with the copy rules of scripts/wine_sandbox.sh: the game copied (no
    `Save/`, which starts empty and its own), `options.cfg` pinned, the Kyubu loader in (the vanilla zlib1.dll kept as
    zlib1_vanilla.dll), this checkout's symbols and loader/lua, the `mods` (staged folders), and $CONFIG as
    kyubu/config.toml. `kyubu` is a release folder (`kind` 'release': its installer does the loader's part) or a CMake
    build folder ('build': loader/zlib1.dll). With $KEEP=1 (or keep=True) an existing `dest` is kept as it is: the game
    copy, `options.cfg` and `Save/` stay, and the loader, symbols, lua and mods are put in again. Returns `dest`."""
    keep = os.environ.get('KEEP') == '1' if keep is None else keep
    if not (keep and dest.is_dir()):
        copy_game(ctx, game, dest, host)
        (dest / 'Save').mkdir(exist_ok=True)
        (dest / 'options.cfg').write_bytes(options_cfg(replay).encode('ascii'))
    if kind == 'release':
        r = subprocess.run([sys.executable, str(kyubu / 'install' / 'kyubu_install.py'), 'install', str(dest)],
                           capture_output=True, text=True)
        if r.returncode:
            raise KmapError('the Kyubu installer failed:\n' + r.stdout + r.stderr)
    else:
        if (dest / 'zlib1.dll').is_file() and not (dest / 'zlib1_vanilla.dll').is_file():
            os.replace(dest / 'zlib1.dll', dest / 'zlib1_vanilla.dll')
        shutil.copy2(kyubu / 'loader' / 'zlib1.dll', dest / 'zlib1.dll')
    (dest / 'kyubu' / 'symbols').mkdir(parents=True, exist_ok=True)
    for toml in (ctx.repo / 'symbols').glob('*.toml'):
        shutil.copy2(toml, dest / 'kyubu' / 'symbols' / toml.name)
    (dest / 'kyubu' / 'lua').mkdir(exist_ok=True)
    for lua in (ctx.repo / 'loader' / 'lua').glob('*.lua'):    # require("cubeworld") for full-trust mods
        shutil.copy2(lua, dest / 'kyubu' / 'lua' / lua.name)
    (dest / 'mods').mkdir(exist_ok=True)
    for m in mods:
        shutil.rmtree(dest / 'mods' / m.name, ignore_errors=True)
        shutil.copytree(m, dest / 'mods' / m.name)
    if os.environ.get('CONFIG'):
        shutil.copy2(os.environ['CONFIG'], dest / 'kyubu' / 'config.toml')
    return dest


def close_game(proc, grace=10):
    """Closes the window as a player would, then ends the process if it has not gone after `grace` seconds.
    A console host (Server.exe) has no window to close: give it a `grace` of 0."""
    if proc.poll() is None:
        if grace:
            subprocess.run(['powershell', '-NoProfile', '-Command',
                            f'(Get-Process -Id {proc.pid}).CloseMainWindow() | Out-Null'], capture_output=True)
        try:
            proc.wait(grace)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    return proc.returncode


class JobAlive:
    """Stands in for a host's Popen where the run must go on while any process of the job lives: a host that restarts
    itself (game_restart) ends its first process, and the second one carries on in the same sandbox."""

    def __init__(self, job, proc):
        self.job, self.proc = job, proc
        self.pid = proc.pid

    def poll(self):
        return None if self.job.active() else (self.proc.poll() or 0)


def start_host(job, sandbox, exe):
    """Starts `exe` of a sandbox with `--kyubu-dev` (and `--kyubu-headless` for Cube.exe) and puts it in the job."""
    args = ['--kyubu-dev'] + (['--kyubu-headless'] if exe == 'Cube.exe' else [])
    proc = subprocess.Popen([str(sandbox / exe), *args], cwd=sandbox)
    job.add(proc)
    return proc


FAST_JOIN_SAVE = ('tmp', 'replay-seed-testworld', 'game', 'Save', 'characters.db')
FAST_JOIN_HOST = '127.0.0.1'


def fast_join_save(ctx):
    """The saved character the fast-join clients use: $KMAP_FAST_JOIN_SAVE, else the fast replay's seed save under
    $KMAP_HOME (a game file: it never enters the repository)."""
    env = os.environ.get('KMAP_FAST_JOIN_SAVE')
    path = Path(env) if env else Path(ctx.home).joinpath(*FAST_JOIN_SAVE)
    if not path.is_file():
        raise KmapError(f'fast_join needs a saved character at {path} (characters.db of a save; see RECIPES, "Fast native '
                        'replay"), or set KMAP_FAST_JOIN_SAVE')
    return path


def prepare_fast_join(ctx, sandbox, kyubu, save):
    """A joining client's sandbox for fast_join: the saved character in Save/, mods-dev/auto_enter staged with its option
    join set, so the client joins the server by itself once its title screen is up."""
    shutil.copy2(save, sandbox / 'Save' / 'characters.db')
    staged = stage_mods(ctx.repo, ['mods-dev/auto_enter'], sandbox / 'kyubu' / 'fast-join', kyubu)[0]
    shutil.rmtree(sandbox / 'mods' / staged.name, ignore_errors=True)
    shutil.copytree(staged, sandbox / 'mods' / staged.name)
    cfg = sandbox / 'kyubu' / 'config'
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / 'auto_enter.toml').write_text(f'enabled = true\njoin = "{FAST_JOIN_HOST}"\n', encoding='utf-8')


def run_native(ctx, sandbox, host, replay, second=None, watcher=None):
    """Windows: the host runs in its sandbox with `--kyubu-dev --kyubu-headless` (Cube.exe: no focus needed), and
    gamepilot's native backend drives it with inbox files: the menus (`setup`), then the record's `input` steps.
    Every host is in one Job Object (kill on close), so a game that a host starts itself ends with the run.
    `second` is (sandbox, exe) of the other host: client = "join" starts a Cube.exe that joins the Server.exe host,
    server = "local" starts a Server.exe that the Cube.exe host joins. `watcher` is the sandbox of a third game, a Cube.exe
    that joins the same server after the client (watcher = true). Ends 2 s after the script has finished and the
    input is done, or at the record's `seconds`; then each host gets its window closed, and the job is closed after
    10 s. Returns (exit code of the host, whether a host had ended by itself with a non-zero code before the end)."""
    seconds = replay.get('seconds', 120)
    setup = replay.get('setup', 'menu')
    sys.path.insert(0, str(ctx.repo / 'scripts'))
    import gamepilot
    from .winjob import Job
    log = sandbox / 'kyubu' / 'logs' / 'latest.log'
    started = time.time()
    procs, job = [], Job()
    try:
        proc = start_host(job, sandbox, host)
        procs.append((proc, host))
        input_sb = sandbox
        if second and host == 'Cube.exe':                      # server = "local"
            gamepilot.configure(sandbox, 'native')
            gamepilot.WORLD_LOG = str(log)
            wait_game_ready(log, started, proc)
            sproc = start_host(job, second[0], second[1])
            procs.append((sproc, second[1]))
            wait_game_ready(second[0] / 'kyubu' / 'logs' / 'latest.log', started, sproc)
            gamepilot.fit_window()
            gamepilot.join_server()
        elif second and replay.get('fast_join') is True:      # client = "join", both clients join by auto_enter
            wait_game_ready(log, started, proc)
            joiners = [second[0]] + ([watcher] if watcher else [])
            for sb in joiners:
                procs.append((start_host(job, sb, 'Cube.exe'), 'Cube.exe'))
            for sb, (p, _) in zip(joiners, procs[1:]):
                if not wait_log(sb / 'kyubu' / 'logs' / 'latest.log', '] connected to ', started, p, 240):
                    say(f'fast_join: {sb.name} did not join (no "connected to" in its log)')
            input_sb = second[0]
            gamepilot.configure(input_sb, 'native')
            gamepilot.WORLD_LOG = str(input_sb / 'kyubu' / 'logs' / 'latest.log')
        elif second:                                           # client = "join"
            wait_game_ready(log, started, proc)
            cproc = start_host(job, second[0], second[1])
            procs.append((cproc, second[1]))
            input_sb = second[0]
            gamepilot.configure(input_sb, 'native')
            gamepilot.WORLD_LOG = str(input_sb / 'kyubu' / 'logs' / 'latest.log')
            wait_game_ready(input_sb / 'kyubu' / 'logs' / 'latest.log', started, cproc)
            gamepilot.fit_window()
            gamepilot.join_server()
            if watcher:
                wproc = start_host(job, watcher, 'Cube.exe')
                procs.append((wproc, 'Cube.exe'))
                gamepilot.configure(watcher, 'native')
                gamepilot.WORLD_LOG = str(watcher / 'kyubu' / 'logs' / 'latest.log')
                wait_game_ready(watcher / 'kyubu' / 'logs' / 'latest.log', started, wproc)
                gamepilot.fit_window()
                gamepilot.join_server(name='Watcher')
                gamepilot.configure(input_sb, 'native')     # the input drives the client again
                gamepilot.WORLD_LOG = str(input_sb / 'kyubu' / 'logs' / 'latest.log')
        else:
            gamepilot.configure(sandbox, 'native')
            gamepilot.WORLD_LOG = str(log)
            if host == 'Cube.exe':
                wait_game_ready(log, started, proc)
                gamepilot.fit_window()
                if setup == 'new-world':
                    gamepilot.new_character_and_world()
                elif setup == 'existing-world':
                    gamepilot.existing_world()
        alive = JobAlive(job, proc)
        if host == 'Cube.exe' or second:
            drive_input(sandbox, input_sb, replay, log, started, alive, [], watcher)
        deadline, finished_at = started + seconds, None
        while time.time() < deadline and alive.poll() is None:
            text = log.read_text(encoding='utf-8-sig', errors='replace') if log.is_file() else ''
            if FINISHED.search(text):
                finished_at = finished_at or time.time()
                if time.time() - finished_at > 2:
                    break
            time.sleep(1)
    finally:
        died = any(p.poll() not in (None, 0) for p, _ in procs)      # ended by itself with an error code
        code = None
        for p, exe in procs:
            c = close_game(p, grace=10 if exe == 'Cube.exe' else 0)
            code = c if p is procs[0][0] else code
        job.wait_empty(10)
        job.close()
    return code, died


def wait_game_ready(log, started, proc, timeout=240):
    """Waits until this run's log says `phase: game_ready`. The game window appears seconds before the start menu
    takes input (longer under software rendering), and gamepilot's clicks before that are lost."""
    return wait_log(log, 'phase: game_ready', started, proc, timeout, settle=2)


def wait_log(log, text, started, proc, timeout, settle=0):
    """Waits until this run's log holds `text` (the input step `until`, which lines gamepilot up with the script's
    `log` lines), while the game runs and for at most `timeout` seconds."""
    deadline = time.time() + timeout
    while proc.poll() is None:
        try:
            if log.stat().st_mtime >= started and text in log.read_text(errors='replace'):
                time.sleep(settle)
                return True
        except OSError:
            pass
        if time.time() >= deadline:
            break
        time.sleep(1)
    return False


def render_env(replay):
    """wine_sandbox.sh's RENDER_DISTANCE: the record's render_distance, else KMAP_RENDER_DISTANCE, else its default."""
    rd = replay.get('render_distance', os.environ.get('KMAP_RENDER_DISTANCE'))
    return {'RENDER_DISTANCE': str(rd)} if rd is not None else {}


def run_wine(ctx, sandbox_parent, game, kyubu, staged_mod, host, replay, mods=()):
    """Linux: scripts/wine_sandbox.sh builds its own sandbox and runs the host; gamepilot drives it meanwhile.
    With client = "join", a second sandbox runs Cube.exe in the same prefix (NOKILL: the server's run ends both).
    With server = "local", the second sandbox runs Server.exe instead, and the host Cube.exe joins it. `mods` (the
    staged [replay] mods) go into every sandbox (wine_sandbox.sh's MODS), or only into those of the games `mods_on`
    lists; the probe only into the host's."""
    sb = sandbox_parent / 'wine'
    env = dict(os.environ, MODS=' '.join(str(m) for m in (staged_mod, *mods_for(replay, host, mods))))
    env.update(render_env(replay))
    started = time.time()
    seconds = replay.get('seconds', 120)
    script = str(ctx.repo / 'scripts' / 'wine_sandbox.sh')
    proc = subprocess.Popen(['bash', script, str(game), str(kyubu), str(sb), str(seconds), host, '--kyubu-dev'],
                            env=env)
    sys.path.insert(0, str(ctx.repo / 'scripts'))
    import gamepilot
    setup = replay.get('setup', 'menu')
    log = sb / 'kyubu' / 'logs' / 'latest.log'
    # the log that says when the joining game has entered its world: the second host's when it is the client
    input_sb = sb.parent / 'client' if replay.get('client') == 'join' else sb   # the game that takes the input
    gamepilot.WORLD_LOG = str(input_sb / 'kyubu' / 'logs' / 'latest.log')
    sent = []
    if host == 'Cube.exe' and replay.get('server') == 'local':
        wait_game_ready(log, started, proc)
        second(ctx, sandbox_parent, game, kyubu, 'server', 'Server.exe', started, seconds,
               mods_for(replay, 'Server.exe', mods), render_env(replay))
        gamepilot.fit_window()
        gamepilot.join_server()
    elif host == 'Cube.exe':
        wait_game_ready(log, started, proc)
        gamepilot.fit_window()
        if setup == 'new-world':
            gamepilot.new_character_and_world()
        elif setup == 'existing-world':
            gamepilot.existing_world()
    elif replay.get('client') == 'join':
        wait_game_ready(log, started, proc)
        second(ctx, sandbox_parent, game, kyubu, 'client', 'Cube.exe', started, seconds,
               mods_for(replay, 'Cube.exe', mods), render_env(replay))
        gamepilot.fit_window()
        gamepilot.join_server()
    drive_input(sb, input_sb, replay, log, started, proc, sent)
    wait_finished(log, started, proc)
    return sb


def drive_input(sb, input_sb, replay, log, started, proc, sent, watcher=None):
    """The record's `input` steps, for both paths: `sb` is the probed host's sandbox (its mods/, shots/, kyubu/inbox/),
    `input_sb` the sandbox of the game that takes the input (the joining client's, else `sb`), `log` the probed host's
    log. gamepilot's backend (xdotool under Wine, inbox files natively) does the keys and the mouse. `sent` collects the
    console lines. A step that cannot run ends the input (it says why). `watcher` is the watcher's sandbox (watcher = true):
    `shot watcher`, `until watcher` and `console watcher` act on it."""
    import gamepilot
    for step in replay.get('input', []):
        verb, _, rest = step.partition(' ')
        if WATCHER_STEP.match(step):
            if not watcher:
                say(f'input: "{step}" needs a watcher (watcher = true, Windows); the remaining input steps are skipped')
                break
            rest = rest[len('watcher '):]
            if verb == 'shot':
                (sb / 'shots').mkdir(parents=True, exist_ok=True)
                was = gamepilot.SANDBOX
                gamepilot.SANDBOX = str(watcher)
                try:
                    gamepilot.shot(str(sb / 'shots' / f'watcher-{rest}.png'))
                finally:
                    gamepilot.SANDBOX = was
            elif verb == 'until':
                if not wait_log(watcher / 'kyubu' / 'logs' / 'latest.log', rest, started, proc, replay.get('seconds', 120)):
                    say(f'input: "{rest}" never appeared in the watcher\'s log; the remaining input steps are skipped')
                    break
            else:
                console_step(watcher, rest, f'{len(sent):03d}-watcher')
                sent.append(rest)
            continue
        if verb == 'wait':
            time.sleep(float(rest))
        elif verb == 'key':
            gamepilot.key(rest)
        elif verb == 'hold':
            # mouse1..mouse3 hold a mouse button; `w+mouse2` holds several keys and buttons at once: all go down in
            # order, and come up in reverse order (gamepilot.hold_keys, for either backend)
            k, secs = rest.split()
            gamepilot.hold_keys(k, float(secs))
        elif verb == 'wheel':
            # "wheel down 8": the mouse wheel, a tick every 0.5 s, over the game's window (camera zoom)
            direction, ticks = rest.split()
            gamepilot.wheel(direction, int(ticks))
        elif verb == 'type':
            gamepilot.type_text(rest, 300)   # text into whatever has the keyboard (an open console); `key Return` sends it
        elif verb == 'console':
            console_step(input_sb, rest, f'{len(sent):03d}-input')
            sent.append(rest)
        elif verb == 'click':
            x, y = rest.split()
            gamepilot.click((int(x), int(y)))
        elif verb == 'move':
            # the cursor alone, for a hover (an item's information box); window coordinates, as click
            x, y = rest.split()
            gamepilot.move(int(x), int(y))
        elif verb == 'until':
            if not wait_log(log, rest, started, proc, replay.get('seconds', 120)):
                say(f'input: "{rest}" never appeared in the log; the remaining input steps are skipped')
                break
        elif verb == 'shot':
            (sb / 'shots').mkdir(parents=True, exist_ok=True)
            gamepilot.shot(str(sb / 'shots' / f'{rest}.png'))
        elif verb == 'modfile':
            # "modfile <mod> <from> <to>": copies a file of an installed mod over another of its files while the game
            # runs (a dev-mode hot reload: the mod ships the edited text beside the file the game reads)
            mod, src, dst = rest.split()
            root = (sb / 'mods' / mod).resolve()
            a, b = (root / src).resolve(), (root / dst).resolve()
            if not (a.is_file() and root in a.parents and root in b.parents):
                say(f'input: "{step}" needs two paths inside mods/{mod}, the first an existing file; the remaining '
                    'input steps are skipped')
                break
            b.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(a, b)
            os.utime(b)   # the copy's own time, so the game sees a change
        elif verb == 'dropmod':
            # "dropmod <mod> <dir> <name>": a folder shipped inside an installed mod is copied to mods/<name>, as a player drops a mod
            # into mods\ while the game runs (the Mods page rescan, add-mod-manager 6.5)
            mod, src, name = rest.split()
            root = (sb / 'mods' / mod).resolve()
            a = (root / src).resolve()
            dest = sb / 'mods' / name
            if not (a.is_dir() and root in a.parents) or dest.exists():
                say(f'input: "{step}" needs an existing folder inside mods/{mod} and no mods/{name}; the remaining input steps are skipped')
                break
            shutil.copytree(a, dest)
        elif verb == 'inbox':
            # "inbox <mod> <from> <name>.txt": a file of an installed mod becomes a request in <game>/kyubu/inbox/ (the capture mod's
            # dev-mode file inbox, add-ability-animation 7.6). Written under a temporary name and renamed, as a dropper should.
            mod, src, name = rest.split()
            root = (sb / 'mods' / mod).resolve()
            a = (root / src).resolve()
            if not (a.is_file() and root in a.parents):
                say(f'input: "{step}" needs an existing file inside mods/{mod}; the remaining input steps are skipped')
                break
            inbox = sb / 'kyubu' / 'inbox'
            inbox.mkdir(parents=True, exist_ok=True)
            tmp = inbox / (name + '.tmp')
            shutil.copyfile(a, tmp)
            os.replace(tmp, inbox / name)


def console_step(sandbox, line, name, limit=15):
    """The input step `console <command>`: the line goes to <sandbox>/kyubu/inbox/<name>.cmd, which a dev-mode loader runs as
    if typed (at most 250 ms later, on a frame) and removes; its output is in the log as `inbox <name>.cmd: ...`. A loader
    without the inbox leaves the file: after `limit` seconds it is removed and the line is typed into the console instead."""
    import gamepilot
    inbox = sandbox / 'kyubu' / 'inbox'
    inbox.mkdir(parents=True, exist_ok=True)
    tmp, dest = inbox / f'{name}.cmd.tmp', inbox / f'{name}.cmd'
    tmp.write_text(line + '\n', encoding='utf-8')
    os.replace(tmp, dest)
    deadline = time.time() + limit
    while dest.exists() and time.time() < deadline:
        time.sleep(0.25)
    if dest.exists():
        dest.unlink(missing_ok=True)
        say(f'input: the loader did not take console {line!r} from its inbox; typing it instead')
        gamepilot.console([line])
    else:
        time.sleep(1)   # the command's own work on the next frames


def wait_finished(log, started, proc, grace=2):
    """Waits for the Wine run to end, as run_windows does: once this run's log says the script has finished (and the
    input is done), it stops the prefix's wineserver after `grace` seconds, which closes a second host (client =
    "join", server = "local") too. Otherwise wine_sandbox.sh's own limit, the record's `seconds`, ends the run."""
    while proc.poll() is None:
        try:
            finished = log.stat().st_mtime >= started and FINISHED.search(log.read_text(errors='replace'))
        except OSError:
            finished = False
        if finished:
            time.sleep(grace)
            say('the script has finished: closing the game')
            env = dict(os.environ)
            env.setdefault('WINEPREFIX', str(Path.home() / '.wine32'))      # wine_sandbox.sh's default
            subprocess.run(['wineserver', '-k'], env=env, capture_output=True)
            break
        time.sleep(1)
    proc.wait()


def second(ctx, sandbox_parent, game, kyubu, name, exe, started, seconds, mods=(), extra_env=None):
    """The second host beside the probed one (client = "join", server = "local"): a Kyubu-loaded game in its own
    sandbox and the same prefix, with the record's mods but without the probe, and without killing the prefix's
    wineserver (NOKILL), so the probed host's run ends both. Returns once it is ready."""
    left = int(seconds - (time.time() - started)) - 5
    env = {k: v for k, v in os.environ.items() if k != 'MODS'}
    if mods:
        env['MODS'] = ' '.join(str(m) for m in mods)
    sb = sandbox_parent / name
    proc = subprocess.Popen(['bash', str(ctx.repo / 'scripts' / 'wine_sandbox.sh'), str(game), str(kyubu), str(sb),
                             str(max(left, 10)), exe, '--kyubu-dev'],   # dev mode: the loader's console inbox
                            env=dict(env, NOKILL='1', **(extra_env or {})))
    wait_game_ready(sb / 'kyubu' / 'logs' / 'latest.log', started, proc)


def client_log(sandbox, name='client'):
    """The log of the second host (client = "join": 'client', server = "local": 'server'): its sandbox sits next to
    the probed host's."""
    p = sandbox.parent / name / 'kyubu' / 'logs' / 'latest.log'
    return p.read_text(encoding='utf-8-sig', errors='replace') if p.is_file() else None


def cmd_replay(ctx, pid, game, kyubu, probe=None, out=None, keep=False):
    db = dbmod.load(ctx)
    record = db.probes.get(pid)
    if record is None:
        raise KmapError(f'no probe record mapping/probes/{pid}.toml')
    problems = record_problems(pid, record, ctx.repo)
    if problems:
        raise KmapError('\n'.join(problems))
    replay = record.get('replay')
    if not isinstance(replay, dict):
        raise KmapError(f'probe {pid} has no [replay] table, so it can only be repeated by hand')
    host, setup = replay.get('host', 'Cube.exe'), replay.get('setup', 'menu')
    game = Path(game)
    if not (game / 'Cube.exe').is_file():
        raise KmapError(f'{game} is not a Cube World folder (no Cube.exe)')
    kind, kyubu = find_payload(kyubu)
    probe_dir = find_probe(kyubu, probe)
    work = ctx.home / 'tmp' / f'replay-{pid}'
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out_dir = Path(out) if out else ctx.home / 'replays' / f'{pid}-{stamp}'
    out_dir.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    staged = work / 'mods' / 'kyubu_probe'
    stage_probe(probe_dir, staged, replay['script'])
    mods = stage_mods(ctx.repo, replay.get('mods', []), work / 'mods', kyubu)
    with_mods = f', mods {", ".join(m.name for m in mods)}' if mods else ''
    logs = {}                                   # the second games' logs, for log_expect
    if mods and 'mods_on' in replay:
        with_mods += ' on ' + ' and '.join(replay['mods_on'])
    try:
        if os.name == 'nt':
            sandbox = build_sandbox(ctx, game, kind, kyubu, work / 'game', host,
                                    (staged, *mods_for(replay, host, mods)), replay)
            bid = build_of(ctx, sandbox, host)
            if bid != record.get('build'):
                raise KmapError(f'probe {pid} was recorded on {record.get("build")}, but the game is {bid}')
            second = None
            if replay.get('client') or replay.get('server'):
                name, exe = ('client', 'Cube.exe') if replay.get('client') else ('server', 'Server.exe')
                sb2 = build_sandbox(ctx, game, kind, kyubu, work / name, exe, mods_for(replay, exe, mods), replay)
                build_of(ctx, sb2, exe)
                second = (sb2, exe)
            watcher = None
            if replay.get('watcher') is True:
                watcher = build_sandbox(ctx, game, kind, kyubu, work / 'watcher', 'Cube.exe', mods_for(replay, 'Cube.exe', mods), replay)
                build_of(ctx, watcher, 'Cube.exe')
            if replay.get('fast_join') is True and second:
                save = fast_join_save(ctx)
                for sbx in (second[0], watcher):
                    if sbx is not None:
                        prepare_fast_join(ctx, sbx, kyubu, save)
            say(f'replaying {pid}: {host} in {sandbox} (headless), up to {replay.get("seconds", 120)} s')
            if watcher:
                code, died = run_native(ctx, sandbox, host, replay, second, watcher)
            else:
                code, died = run_native(ctx, sandbox, host, replay, second) if second else run_native(ctx, sandbox, host, replay)
            for sbx in (second[0] if second else None, watcher):
                if sbx is None:
                    continue
                ctext = client_log(sandbox, sbx.name)
                logs[sbx.name] = ctext
                if ctext is not None:
                    (out_dir / f'{sbx.name}.log').write_text(ctext, encoding='utf-8')
            crashes = [(sb, c) for sb in (sandbox, second[0] if second else None, watcher) if sb
                       for c in sorted((sb / 'kyubu' / 'logs').glob('crash-*.txt'))]
            for sb, c in crashes:
                shutil.copy2(c, out_dir / (c.name if sb == sandbox else f'{sb.name}-{c.name}'))
            if crashes or died:
                keep = True                         # the sandbox stays for a look at the crash
                say(f'a host crashed or failed: the sandboxes stay in {work}')
            how = (f'Windows, {host} headless in a copy of the game, setup {setup}{with_mods}, '
                   f'{len(replay.get("input", []))} input step(s) (exit code {code})')
            if second:
                how += (', a client joined (its log: client.log)' if second[1] == 'Cube.exe'
                        else ', it joined a local server (its log: server.log)')
            if watcher:
                how += ', and a watcher joined after it (its log: watcher.log)'
        else:
            if kind != 'build':
                raise KmapError('under Wine, pass the CMake build folder as --kyubu (as for scripts/wine_sandbox.sh)')
            if replay.get('watcher') is True:
                raise KmapError(f'probe {pid} has a watcher (a second client): that runs on Windows only')
            say(f'replaying {pid}: {host} under Wine, up to {replay.get("seconds", 120)} s')
            sandbox = run_wine(ctx, work, game, kyubu, staged, host, replay, mods)
            how = f'Wine, {host}, setup {setup}{with_mods}, {len(replay.get("input", []))} input step(s)'
            second_host = 'client' if replay.get('client') else 'server' if replay.get('server') else None
            if second_host:
                how += (', a client joined (its log: client.log)' if second_host == 'client'
                        else ', it joined a local server (its log: server.log)')
                ctext = client_log(sandbox, second_host)
                logs[second_host] = ctext
                if ctext is not None:
                    (out_dir / f'{second_host}.log').write_text(ctext, encoding='utf-8')
        log = sandbox / 'kyubu' / 'logs' / 'latest.log'
        text = log.read_text(encoding='utf-8-sig', errors='replace') if log.is_file() else ''
        (out_dir / 'latest.log').write_text(text, encoding='utf-8')
        for png in sorted((sandbox / 'shots').glob('*.png')):          # the input's `shot` steps
            shutil.copy2(png, out_dir / png.name)
        if (sandbox / 'kyubu' / 'capture').is_dir():                    # what `anim capture` wrote (add-ability-animation 7.3)
            shutil.copytree(sandbox / 'kyubu' / 'capture', out_dir / 'capture', dirs_exist_ok=True)
        replies = sorted((sandbox / 'kyubu' / 'inbox').glob('*.reply'))   # what the capture mod's file inbox answered (7.6)
        if replies:
            (out_dir / 'inbox').mkdir(exist_ok=True)
            for r in replies:
                shutil.copyfile(r, out_dir / 'inbox' / r.name)
    finally:
        if not keep:
            shutil.rmtree(work, ignore_errors=True)
    return report(out_dir, pid, record, text, how, 'Replay', logs)


# --- replay-all -----------------------------------------------------------------------------------------------

FPS_LINE = re.compile(r'headless: ([0-9.]+) fps \(min ([0-9.]+)\)')


def needs_port(replay):
    """The game cannot change its port (12345): a server, a local server or a join allows one such run at a time."""
    return replay.get('host') == 'Server.exe' or replay.get('client') == 'join' or replay.get('server') == 'local'


def fps_of(text):
    """(median of the fps lines, lowest `min`) of a headless log, or (None, None) when it has no fps line."""
    found = [(float(a), float(b)) for a, b in FPS_LINE.findall(text)]
    if not found:
        return None, None
    fps = sorted(a for a, _ in found)
    n = len(fps)
    return (fps[n // 2] if n % 2 else (fps[n // 2 - 1] + fps[n // 2]) / 2), min(b for _, b in found)


def replayable_ids(ctx, ids, everything):
    """The record ids replay-all runs: the named ones, or with --all every replayable record of a build that is not retired."""
    db = dbmod.load(ctx)
    if everything:
        retired = {k for k, b in ctx.ws.builds.items() if b.retired}
        return sorted(k for k, r in db.probes.items()
                      if isinstance(r.get('replay'), dict) and r.get('build') not in retired)
    for i in ids:
        if i not in db.probes:
            raise KmapError(f'no probe record mapping/probes/{i}.toml')
    return list(ids)


def run_queue(jobs, port, workers, run):
    """Run `run(id)` for every id, `workers` at a time. An id in `port` waits while another id of `port` runs; the
    others do not wait for it. Returns {id: result}, in the order of `jobs`."""
    import threading
    cond = threading.Condition()
    todo, results, state = list(jobs), {}, {'port': False}

    def worker():
        while True:
            with cond:
                while True:
                    pick = next((i for i in todo if i not in port or not state['port']), None)
                    if pick is not None or not todo:
                        break
                    cond.wait()
                if pick is None:
                    return
                todo.remove(pick)
                if pick in port:
                    state['port'] = True
            try:
                results[pick] = run(pick)
            finally:
                with cond:
                    if pick in port:
                        state['port'] = False
                    cond.notify_all()

    threads = [threading.Thread(target=worker) for _ in range(max(1, workers))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return {i: results[i] for i in jobs}


def summary_table(rows):
    """Markdown table of (id, verdict, seconds, median fps, lowest fps) rows."""
    def num(v):
        return '-' if v is None else f'{v:.1f}'
    out = ['| Record | Result | Time (s) | Median fps | Lowest fps |', '|---|---|---|---|---|']
    for i, v, t, med, low in rows:
        out.append(f'| `{i}` | {v} | {t:.0f} | {num(med)} | {num(low)} |')
    return '\n'.join(out) + '\n'


def cmd_replay_all(ctx, ids, everything, jobs, game, kyubu, probe=None, out=None, keep=False):
    if bool(ids) == bool(everything):
        raise KmapError('replay-all: name the records, or pass --all (not both)')
    if jobs < 1:
        raise KmapError('replay-all: -j must be 1 or more')
    names = replayable_ids(ctx, ids, everything)
    base = Path(out) if out else ctx.home / 'replays' / ('all-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    base.mkdir(parents=True, exist_ok=True)
    db = dbmod.load(ctx)
    port = {i for i in names if needs_port(db.probes[i].get('replay') or {})}
    if len(port) > 1:
        say(f'replay-all: {len(port)} records use port 12345 and run one at a time')

    def one(pid):
        start = time.time()
        d = base / pid
        d.mkdir(parents=True, exist_ok=True)
        # One process per record: gamepilot keeps its sandbox in module globals, so threads would steer each other's game.
        cmd = [sys.executable, str(Path(__file__).resolve().parents[1]), '--repo', str(ctx.repo), 'probe', 'replay', pid,
               '--game', str(game), '--kyubu', str(kyubu), '--out', str(d)]
        cmd += ['--probe', str(probe)] if probe else []
        cmd += ['--keep'] if keep else []
        with open(d / 'run.log', 'w', encoding='utf-8') as f:
            code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT,
                                  env=dict(os.environ, KMAP_HOME=str(ctx.home))).returncode
        verdict = 'passed' if code == 0 else 'FAILED' if (d / 'report.md').is_file() else 'error'
        say(f'  {pid}: {verdict}')
        med, low = None, None
        for name in ('latest.log', 'client.log', 'server.log'):      # the first log of a Cube.exe that has fps lines
            log = d / name
            if med is None and log.is_file():
                med, low = fps_of(log.read_text(encoding='utf-8', errors='replace'))
        return verdict, time.time() - start, med, low

    results = run_queue(names, port, jobs, one)
    rows = [(i, *results[i]) for i in names]
    table = summary_table(rows)
    (base / 'summary.md').write_text(f'# replay-all, {len(names)} record(s), -j {jobs}\n\n' + table, encoding='utf-8')
    say(table)
    bad = [r[0] for r in rows if r[1] != 'passed']
    say(f'replay-all: {len(rows) - len(bad)} passed, {len(bad)} not; summary in {base / "summary.md"}')
    return 1 if bad else 0


def report(out_dir, pid, record, text, how, title, logs=None):
    results, finished = parse_log(text)
    verdict, ok = verdicts(record, results, finished)
    checks = log_expect_results(record.get('replay') or {}, {**logs, 'latest': text}) if logs is not None else []
    ok = ok and all(found for _, _, found in checks)
    write_report(out_dir, pid, record, verdict, ok, finished, 'latest.log', how, title, checks)
    for entry, (v, details) in verdict.items():
        say(f'  {entry}: {v}' + (f' ({"; ".join(details)})' if details else ''))
    for name, x, found in checks:
        say(f'  {name}.log: {x!r} {"found" if found else "MISSING"}')
    say(f'{title.lower()} of {pid}: {"passed" if ok else "FAILED"}; report and log in {out_dir}')
    return 0 if ok else 1


# --- report on a session played by hand ----------------------------------------------------------------------

def last_run(text):
    """The part of a log from its last `script started` line on (the game runs probe.cfg once per start, but a log
    pasted together from several runs must not mix them), and how many runs the log has."""
    lines = text.splitlines(keepends=True)
    starts = [i for i, l in enumerate(lines) if STARTED.search(l)]
    return (''.join(lines[starts[-1]:]) if starts else ''), len(starts)


def cmd_report(ctx, pid, log, out=None):
    db = dbmod.load(ctx)
    record = db.probes.get(pid)
    if record is None:
        raise KmapError(f'no probe record mapping/probes/{pid}.toml')
    problems = record_problems(pid, record)
    if problems:
        raise KmapError('\n'.join(problems))
    if not isinstance(record.get('replay'), dict):
        raise KmapError(f'probe {pid} has no [replay] script, so there is nothing to check a log against')
    log = Path(log)
    if not log.is_file():
        raise KmapError(f'{log}: no such log (the game writes kyubu/logs/latest.log)')
    text = log.read_text(encoding='utf-8-sig', errors='replace')
    builds = set(HOST_BUILD.findall(text))
    if not builds:
        raise KmapError(f'{log} does not say which build ran (no "Host build ... verified" line): not a Kyubu log')
    if builds != {record.get('build')}:
        raise KmapError(f'probe {pid} was recorded on {record.get("build")}, but the log is from '
                        f'{", ".join(sorted(builds))}')
    run, runs = last_run(text)
    if not runs:
        raise KmapError(f'{log} has no "script started" line: put the record\'s script in '
                        'mods/kyubu_probe/probe.cfg and start the game with --kyubu-dev')
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out_dir = Path(out) if out else ctx.home / 'replays' / f'{pid}-{stamp}-session'
    out_dir.mkdir(parents=True, exist_ok=True)
    kept = out_dir / 'latest.log'
    if not (kept.exists() and kept.samefile(log)):
        kept.write_text(text, encoding='utf-8')
    how = f'a session played by hand, log `{log}`' + (f' (the last of {runs} script runs)' if runs > 1 else '')
    return report(out_dir, pid, record, run, how, 'Session report')


def cmd_probe(ctx, args):
    if args.probe_cmd == 'new':
        return cmd_new(ctx, args.id, args.build)
    if args.probe_cmd == 'list':
        return cmd_list(ctx)
    if args.probe_cmd == 'replay':
        return cmd_replay(ctx, args.id, args.game, args.kyubu, args.probe, args.out, args.keep)
    if args.probe_cmd == 'replay-all':
        return cmd_replay_all(ctx, args.ids, args.all, args.jobs, args.game, args.kyubu, args.probe,
                              args.out, args.keep)
    if args.probe_cmd == 'report':
        return cmd_report(ctx, args.id, args.log, args.out)
    raise KmapError(f'unknown probe command {args.probe_cmd}')
