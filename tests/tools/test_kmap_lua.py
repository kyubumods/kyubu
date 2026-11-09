"""kmap export, Lua: the LuaJIT FFI module loader/lua/cubeworld.lua and the LuaLS stubs sdk/lua/cubeworld.lua
(add-mapping-workspace task 4.4).

The LuaJIT tests need a 32-bit LuaJIT (KYUBU_LUAJIT, or luajit on PATH), built from third_party/luajit for Windows so
that struct layout follows the game's ABI. The editor tests need lua-language-server (KYUBU_LUALS, or on PATH).
Both are skipped otherwise. tests/unit/test_cubeworld_lua.cpp checks the committed module in the loader's LuaJIT.
"""
import json
import os
import queue
import shutil
import subprocess
import threading
import time
from pathlib import Path

import pytest

from conftest import ROOT, function_entry
from test_kmap_headers import CUBE, FIXTURE_TYPES, field, type_head

LUAJIT = os.environ.get('KYUBU_LUAJIT') or shutil.which('luajit')
LUALS = os.environ.get('KYUBU_LUALS') or shutil.which('lua-language-server')
MODULE, STUBS = 'loader/lua/cubeworld.lua', 'sdk/lua/cubeworld.lua'


def game_abi_luajit():
    """True when LUAJIT lays out structs like the game: 32-bit Windows (a 64-bit or Linux LuaJIT does not)."""
    if not LUAJIT:
        return False
    try:
        r = subprocess.run([LUAJIT, '-e', 'local ffi = require("ffi") io.write(tostring(ffi.abi("32bit") and ffi.abi("win")))'],
                           capture_output=True, text=True, timeout=30)
    except OSError:
        return False
    return r.stdout == 'true'


needs_luajit = pytest.mark.skipif(not game_abi_luajit(), reason='no 32-bit Windows LuaJIT (KYUBU_LUAJIT)')
needs_luals = pytest.mark.skipif(not LUALS, reason='lua-language-server not available')


def generate(repo):
    from kmap import db as dbmod
    from kmap import export_lua
    from kmap.config import Context
    ctx = Context(repo.root)
    return export_lua.generate(ctx, dbmod.load(ctx))


def write_fixture(repo):
    repo.write('mapping/types/things.toml', FIXTURE_TYPES +
               # a verified field at an offset C doesn't align naturally: the struct is packed
               type_head('cube::Packed', '0x6') + field('0x0', 'tag', 'uint16_t') + field('0x2', 'value', 'uint32_t', 'verified'))
    repo.write(f'mapping/{CUBE}/functions/core.toml',
               function_entry('cube::Thing::update', '0x4A1230', 'verified', ['rtti:.?AVThing@cube@@'],
                              'prototype = "void __thiscall cube::Thing::update(cube::Thing* this, int32_t dt)"'))


def test_lua_exports(kmap_repo):
    write_fixture(kmap_repo)
    gen = generate(kmap_repo)
    assert sorted(gen) == [MODULE, STUBS]
    assert gen == generate(kmap_repo)                                        # deterministic
    module, stubs = gen[MODULE], gen[STUBS]
    cdef = module[module.index('ffi.cdef[['):module.index(']]')]
    assert 'struct cube_Thing {' in cdef and 'cube_Base _base;' in cdef and 'uint8_t _unk_4[0x4];' in cdef
    assert '#pragma pack(push, 1)' in cdef                                  # cube::Packed
    assert 'typedef uint8_t cube_Mode;' in cdef
    assert 'typedef void (__fastcall *cw_cube_Thing_update_fn)(cube_Thing* self, void* edx, int32_t dt);' in cdef
    assert 'CW_' not in cdef                                                 # no header macros in the cdef
    assert ('["cube::Thing"] = {cname = "cube_Thing", size = 0x20, status = "candidate", fields = {\n'
            '        {name = "hp", offset = 0x8, type = "float", status = "verified"},') in module
    assert '["cube::Mode"] = {cname = "cube_Mode", status = "candidate", values = {IDLE = 0, ATTACK = 2}},' in module
    assert '["cube::Thing::update"] = "cw_cube_Thing_update_fn",' in module
    assert stubs.startswith('---@meta cubeworld\n') and stubs.endswith('return cubeworld\n')
    assert '---@class cube_Thing\n---@field _base cube_Base cube::Base at 0x0 (the base class)\n' in stubs
    assert '---@field hp number float at 0x8 (verified)' in stubs
    assert '---@field pos cube_Vec cube::Vec at 0xC (candidate)' in stubs
    assert '---@field label ffi.cdata* std::string* at 0x18 (candidate)' in stubs
    assert '---@alias cube_Mode integer' in stubs
    for text in gen.values():
        assert 'secret' not in text                                          # guesses stay private
        assert '4A1230' not in text.upper()                                  # no addresses


# --- LuaJIT ---------------------------------------------------------------------------------------------------

def luajit(script, cwd):
    r = subprocess.run([LUAJIT, '-'], input=script, capture_output=True, text=True, cwd=cwd, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def expectations(root):
    """(C name, member, offset, verified) of every exported field on every build, and (C name, size), from mapping/."""
    from kmap import db as dbmod
    from kmap.config import Context
    from kmap.export_headers import ident, member
    db = dbmod.load(Context(root))
    fields, sizes = [], []
    for t in db.types:
        if not dbmod.exported(t):
            continue
        if t.get('size'):
            sizes.append((ident(t['name']), t['size']))
        for f in t.get('field', []):
            if dbmod.exported(f) and not f.get('builds'):
                fields.append((ident(t['name']), member(f['name']), f['offset'], f['status'] == 'verified'))
    return fields, sizes


def offsets_script(module_dir, fields, sizes):
    rows = ',\n'.join(f'{{"{c}", "{m}", {o}, {str(v).lower()}}}' for c, m, o, v in fields)
    size_rows = ',\n'.join(f'{{"{c}", {s}}}' for c, s in sizes)
    return f'''
package.path = [[{module_dir}/?.lua;]] .. package.path
local cubeworld = require("cubeworld")
local ffi = require("ffi")
local bad, checked = {{}}, 0
for _, e in ipairs({{{rows}}}) do
    local at = ffi.offsetof(e[1], e[2])
    if at ~= nil or e[4] then                   -- a verified field must be declared; candidates when C can type them
        checked = checked + 1
        if at ~= e[3] then bad[#bad + 1] = string.format("%s.%s at %s, mapped 0x%X", e[1], e[2], tostring(at), e[3]) end
    end
end
for _, e in ipairs({{{size_rows}}}) do
    checked = checked + 1
    if ffi.sizeof(e[1]) ~= e[2] then bad[#bad + 1] = string.format("sizeof %s is %s, mapped 0x%X", e[1], tostring(ffi.sizeof(e[1])), e[2]) end
end
for _, p in ipairs(cubeworld.check()) do bad[#bad + 1] = "check: " .. p end
print(checked)
print(table.concat(bad, "\\n"))
'''


def run_offsets(root, module_dir):
    fields, sizes = expectations(root)
    out = luajit(offsets_script(Path(module_dir).as_posix(), fields, sizes), module_dir).splitlines()
    return int(out[0]), [l for l in out[1:] if l]


@needs_luajit
def test_offsets_agree(kmap_repo):
    # covers: mapping-exports/Offsets agree (fixture types: base, padding, a nested struct, a packed struct)
    write_fixture(kmap_repo)
    kmap_repo.write(MODULE, generate(kmap_repo)[MODULE])
    checked, bad = run_offsets(kmap_repo.root, kmap_repo.root / 'loader' / 'lua')
    assert bad == [] and checked >= 10
    out = luajit('local ffi = require("ffi")\npackage.path = "loader/lua/?.lua;" .. package.path\n'
                 'require("cubeworld")\nprint(ffi.offsetof("cube_Packed", "value"), ffi.sizeof("cube_Packed"))',
                 kmap_repo.root)
    assert out.split() == ['2', '6']


@needs_luajit
def test_offsets_agree_committed_module():
    # covers: mapping-exports/Offsets agree (the real mapping and the committed module)
    checked, bad = run_offsets(Path(ROOT), Path(ROOT) / 'loader' / 'lua')
    assert bad == [] and checked > 100


@needs_luajit
def test_check_reports_a_wrong_layout(kmap_repo):
    # the module's own check() names a verified field that LuaJIT places elsewhere (export --force of an overlap)
    kmap_repo.write('mapping/types/things.toml', type_head('cube::Thing', '0x8') +
                    field('0x4', 'a', 'uint32_t') + field('0x6', 'b', 'uint16_t', 'verified'))
    kmap_repo.write(MODULE, generate(kmap_repo)[MODULE])
    out = luajit('package.path = "loader/lua/?.lua;" .. package.path\n'
                 'print(table.concat(require("cubeworld").check(), "\\n"))', kmap_repo.root)
    assert 'cube::Thing.b is at 0x6, LuaJIT says 0x8' in out
    assert 'cube::Thing is 0x8 bytes, LuaJIT says 0xC' in out


# --- lua-language-server --------------------------------------------------------------------------------------

SAMPLE = '''local cubeworld = require("cubeworld")

local creature = cubeworld.cast("cube_CreatureData", 0x1000)
local hp = creature.hp
print(hp + 1, creature.pos.x, creature.appearance.scale.y, creature.level)
'''


def luals_workspace(tmp_path, script):
    work = tmp_path / 'mod'
    (work / 'types').mkdir(parents=True)
    shutil.copy2(os.path.join(ROOT, STUBS), work / 'types' / 'cubeworld.lua')
    (work / '.luarc.json').write_text('{"runtime.version": "LuaJIT", "workspace.library": ["types"]}')
    (work / 'sample.lua').write_text(script)
    return work


def luals_check(tmp_path, work):
    r = subprocess.run([LUALS, f'--check={work}', '--checklevel=Warning', f'--logpath={tmp_path / "log"}'],
                       capture_output=True, text=True, timeout=300)
    return r.stdout + r.stderr


class Lsp:
    """Just enough of the language server protocol to ask lua-language-server for a hover."""

    def __init__(self, work, logpath):
        self.p = subprocess.Popen([LUALS, f'--logpath={logpath}'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL)
        self.q, self.next_id = queue.Queue(), 0
        threading.Thread(target=self._read, daemon=True).start()
        self.request('initialize', {'processId': None, 'rootUri': work.as_uri(), 'capabilities': {},
                                    'workspaceFolders': [{'uri': work.as_uri(), 'name': 'mod'}]})
        self.notify('initialized', {})

    def _read(self):
        out = self.p.stdout
        while True:
            headers = {}
            while True:
                line = out.readline()
                if not line:
                    return
                if line in (b'\r\n', b'\n'):
                    break
                k, _, v = line.decode().partition(':')
                headers[k.strip().lower()] = v.strip()
            self.q.put(json.loads(out.read(int(headers['content-length']))))

    def _send(self, msg):
        body = json.dumps(msg).encode()
        self.p.stdin.write(b'Content-Length: %d\r\n\r\n' % len(body) + body)
        self.p.stdin.flush()

    def notify(self, method, params):
        self._send({'jsonrpc': '2.0', 'method': method, 'params': params})

    def request(self, method, params, timeout=120):
        self.next_id += 1
        rid = self.next_id
        self._send({'jsonrpc': '2.0', 'id': rid, 'method': method, 'params': params})
        end = time.monotonic() + timeout
        while True:
            msg = self.q.get(timeout=max(0.1, end - time.monotonic()))
            if 'method' in msg and 'id' in msg:              # a request from the server: answer it with nothing
                items = msg.get('params', {}).get('items')
                self._send({'jsonrpc': '2.0', 'id': msg['id'], 'result': [None] * len(items) if items else None})
            elif msg.get('id') == rid:
                return msg.get('result')

    def close(self):
        try:
            self.request('shutdown', None, timeout=10)
            self.notify('exit', None)
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


@needs_luals
def test_editor_completion(tmp_path):
    # covers: mapping-exports/Editor completion
    work = luals_workspace(tmp_path, SAMPLE)
    assert 'no problems found' in luals_check(tmp_path, work)
    typo = luals_workspace(tmp_path / 'typo', SAMPLE.replace('creature.hp', 'creature.hpp'))
    assert 'undefined-field' in luals_check(tmp_path / 'typo', typo)      # the stubs know every field
    lsp = Lsp(work, tmp_path / 'lsp-log')
    try:
        uri = (work / 'sample.lua').as_uri()
        lsp.notify('textDocument/didOpen', {'textDocument': {'uri': uri, 'languageId': 'lua', 'version': 1,
                                                             'text': SAMPLE}})
        line = SAMPLE.splitlines().index('local hp = creature.hp')
        pos = {'line': line, 'character': len('local hp = creature.h')}
        text, end = '', time.monotonic() + 120
        while 'float at 0x15C' not in text and time.monotonic() < end:     # the workspace loads in the background
            hover = lsp.request('textDocument/hover', {'textDocument': {'uri': uri}, 'position': pos})
            text = json.dumps(hover['contents']) if hover else ''
            time.sleep(0.5)
    finally:
        lsp.close()
    assert 'hp: number' in text and 'float at 0x15C (verified)' in text, text     # verified in 8.2
