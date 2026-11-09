"""The LuaLS type stubs describe the Lua API well enough that the example mods check cleanly (task 8.6), and every
game-API function has its `kyubu.game` binding and stub (openspec add-game-api, task 4.1).

The LuaLS runs need lua-language-server on PATH or KYUBU_LUALS pointing at it; they are skipped otherwise.
"""
import os
import re
import shutil
import subprocess

import pytest

from test_game_api_entries import game_members

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LUALS = os.environ.get('KYUBU_LUALS') or shutil.which('lua-language-server')
STUB = os.path.join(ROOT, 'sdk', 'lua', 'kyubu.lua')
TEMPLATE_STUB = os.path.join(ROOT, 'sdk', 'templates', 'lua-mod', 'types', 'kyubu.lua')
PRELUDE = os.path.join(ROOT, 'loader', 'src', 'lua', 'prelude.lua')
API_HEADER = os.path.join(ROOT, 'sdk', 'include', 'kyubu_api.h')
LUA_MODS = [os.path.join(ROOT, 'mods-examples', m)
            for m in ('hello_lua', 'spells_lib', 'frost_spells', 'entity_spawner')] + \
           [os.path.join(ROOT, 'sdk', 'templates', 'lua-mod')]


def read(path):
    return open(path, encoding='utf-8').read()


def game_lua_problems(header, prelude, stub):
    """Every game-API member (after the game marker in kyubu_api.h) has a key in the `api.game` table of prelude.lua,
    named like the member without a leading `game_`; every key has a stub `function game.<key>(...)` in kyubu.lua,
    and every stub a key (docs/game-api.md, "Adding a function", step 6)."""
    block = re.search(r'\bapi\.game\s*=\s*\{(.*?)\n\s*\}', prelude, re.S)
    keys = set(re.findall(r'^\s*(\w+)\s*=', block.group(1), re.M)) if block else set()
    stubs = set(re.findall(r'^function game\.(\w+)\(', stub, re.M))
    out = []
    for m in game_members(header) or []:
        name = m[len('game_'):] if m.startswith('game_') else m
        if name not in keys:
            out.append(f'game-API member {m}: no key {name} in the api.game table of prelude.lua')
    out += [f'kyubu.game.{k}: no stub (function game.{k}) in sdk/lua/kyubu.lua' for k in sorted(keys - stubs)]
    out += [f'kyubu.game.{k}: stubbed in sdk/lua/kyubu.lua but not in the api.game table of prelude.lua'
            for k in sorted(stubs - keys)]
    return out


def test_template_carries_the_same_stub():
    # the Lua template ships a copy of the stubs so editors find them without extra setup
    assert open(TEMPLATE_STUB, 'rb').read() == open(STUB, 'rb').read()


def test_every_game_function_has_a_binding_and_a_stub():
    assert game_lua_problems(read(API_HEADER), read(PRELUDE), read(STUB)) == []


@pytest.mark.skipif(not LUALS, reason='lua-language-server not available')
@pytest.mark.parametrize('mod', LUA_MODS, ids=os.path.basename)
def test_mod_checks_clean_with_stubs(tmp_path, mod):
    work = tmp_path / 'mod'
    shutil.copytree(mod, work)
    (work / 'types').mkdir(exist_ok=True)
    shutil.copy2(STUB, work / 'types' / 'kyubu.lua')
    (work / '.luarc.json').write_text('{"runtime.version": "LuaJIT", "workspace.library": ["types"]}')
    r = subprocess.run([LUALS, '--check', str(work), '--checklevel=Warning', f'--logpath={tmp_path / "log"}'],
                       capture_output=True, text=True, timeout=300)
    assert 'no problems found' in r.stdout + r.stderr, r.stdout + r.stderr


@pytest.mark.skipif(not LUALS, reason='lua-language-server not available')
def test_without_stubs_kyubu_is_unknown(tmp_path):
    # control: the clean result above really comes from the stubs
    work = tmp_path / 'mod'
    shutil.copytree(os.path.join(ROOT, 'mods-examples', 'hello_lua'), work)
    (work / '.luarc.json').write_text('{"runtime.version": "LuaJIT"}')
    r = subprocess.run([LUALS, '--check', str(work), '--checklevel=Warning', f'--logpath={tmp_path / "log"}'],
                       capture_output=True, text=True, timeout=300)
    assert 'no problems found' not in r.stdout + r.stderr
