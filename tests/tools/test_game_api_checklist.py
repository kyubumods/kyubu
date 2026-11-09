"""The adding-a-function checklist of docs/game-api.md, walked for a test-only function (openspec add-game-api,
game-api "A new function"; task 4.1).

A scratch copy of the files the checklist touches gets `creature_example`, the one function of a new primitive
`example`, step by step. Then the entries test, the side-rules test, the LuaLS test and the docs check pass on it, and
each fails, naming the function or its primitive, when its step is skipped. The real files are not changed.

The side-rules check here compares names: every KyubuApi member has one row in kSideRules and every row names a member.
tests/unit/test_api.cpp ("side rules cover the whole API table") checks the same by size in the loader's build.
"""
import importlib.util
import os
import re
import subprocess

import pytest

import test_game_api_entries as entries_test
import test_lua_stubs as stubs_test
from conftest import ROOT

FILES = {   # the files the checklist touches (steps 2 to 7), and what the four checks read
    'primitives': 'mapping/atlas/primitives.md',
    'entries': 'loader/src/game/entries.inc',
    'header': 'sdk/include/kyubu_api.h',
    'side_rules': 'loader/src/api/api.cpp',
    'prelude': 'loader/src/lua/prelude.lua',
    'stub': 'sdk/lua/kyubu.lua',
    'template_stub': 'sdk/templates/lua-mod/types/kyubu.lua',
    'doc': 'docs/game-api.md',
}
FN = 'creature_example'


def texts():
    return {k: open(os.path.join(ROOT, rel), encoding='utf-8').read() for k, rel in FILES.items()}


def replace_once(text, old, new):
    assert text.count(old) == 1, f'the checklist test no longer finds {old!r}'
    return text.replace(old, new)


# ---- the checklist's steps for creature_example (steps 0, 1 and 4 need no file here: the entry it uses,
# cube::Creature.id, is verified in both builds, and the loader code is checked by building it)

def step_primitives(t):   # 2. the entries under the primitive's section of primitives.md (a new section)
    t['primitives'] += ('\n## Example\n\nSlice: test only.\n\n| Entry | Status | Use |\n|---|---|---|\n'
                        '| `cube::Creature.id` | verified | the id creature_example returns |\n\nGaps: none.\n')


def step_entries(t):   # 3. the primitive, its entries and the function in entries.inc
    t['entries'] = replace_once(t['entries'], 'GAME_API(game_available, none)\n',
                                'GAME_PRIMITIVE(example, "Example")\n'
                                'GAME_ENTRY(example, "cube::Creature.id", field, BOTH)\n'
                                'GAME_API(game_available, none)\n'
                                f'GAME_API({FN}, example)\n')


def step_member(t):   # 5. the member, appended to the game section of KyubuApi
    last = 'KyubuResult (KYUBU_CALL* game_available)(const char* primitive, char* why, uint32_t cap);\n'
    t['header'] = replace_once(t['header'], last, last + f'    /* test only */\n    KyubuResult (KYUBU_CALL* {FN})'
                                                          '(KyubuModHandle self, KyubuCreature c, uint64_t* out_id);\n')


def step_side_rule(t):   # 5. its row in kSideRules
    t['side_rules'] = replace_once(t['side_rules'], '{"game_available", KY_SIDE_BOTH},',
                                   f'{{"game_available", KY_SIDE_BOTH}}, {{"{FN}", KY_SIDE_BOTH}},')


def step_lua(t):   # 6. the kyubu.game binding, its stub, and the template's copy of the stubs
    t['prelude'] = replace_once(t['prelude'], '      available = K.game_available,',
                                f'      {FN} = K.{FN},\n      available = K.game_available,')
    t['stub'] = replace_once(t['stub'], 'function game.available(primitive) end\n',
                             'function game.available(primitive) end\n'
                             '---Test only: the game id of a creature.\n'
                             '---@param creature integer a creature handle\n'
                             '---@return integer? id nil when the call fails\n'
                             '---@return string? why the reason\n'
                             f'function game.{FN}(creature) end\n')
    t['template_stub'] = t['stub']


def step_docs(t):   # 7. the function's entry in docs/game-api.md, and its primitive's row
    t['doc'] = re.sub(r'(^\| `handles` \|.*\n)', lambda m: m.group(1) + f'| `example` | Example | yes | yes | test only | `{FN}` |\n',
                      t['doc'], count=1, flags=re.M)
    t['doc'] = replace_once(t['doc'], '\n## Adding a function\n',
                            f'\n### `{FN}`\n\nTest only: the game id of a creature.\n\n- **Side:** both.\n'
                            '- **Errors:** the guard\'s.\n\n## Adding a function\n')


STEPS = [('primitives.md', step_primitives), ('entries.inc', step_entries), ('member', step_member),
         ('side rule', step_side_rule), ('Lua', step_lua), ('docs', step_docs)]


# ---- the four checks, on the scratch copy

def side_rule_problems(header, api_cpp):
    """Every KyubuApi member has exactly one row in kSideRules (loader/src/api/api.cpp), and every row names one."""
    table = header[header.index('typedef struct KyubuApi'):header.index('} KyubuApi;')]
    members = re.findall(r'\(\s*KYUBU_CALL\s*\*\s*(\w+)\s*\)', table)
    rules = re.findall(r'\{\s*"(\w+)"\s*,\s*KY_SIDE_\w+\s*\}',
                       re.search(r'kSideRules\[\]\s*=\s*\{(.*?)\n\};', api_cpp, re.S).group(1))
    out = [f'kyubu_api.h: member {m} has no row in kSideRules (loader/src/api/api.cpp)' for m in members
           if m not in rules]
    out += [f'kSideRules: row {r} names no member of KyubuApi' for r in rules if r not in members]
    out += [f'kSideRules: {r} has {rules.count(r)} rows' for r in sorted(set(rules)) if rules.count(r) > 1]
    return out


def check_docs_module():
    spec = importlib.util.spec_from_file_location('check_docs', os.path.join(ROOT, 'tools', 'check_docs.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run_checks(t, loaded, tmp_path):
    """{check: problems} for the scratch texts."""
    for key in ('header', 'entries', 'doc'):   # check_docs reads a repository tree
        path = tmp_path / FILES[key]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(t[key], encoding='utf-8')
    docs = []
    check_docs_module().check_game_api(docs, root=str(tmp_path))
    lua = stubs_test.game_lua_problems(t['header'], t['prelude'], t['stub'])
    if t['template_stub'] != t['stub']:
        lua.append('sdk/templates/lua-mod/types/kyubu.lua differs from sdk/lua/kyubu.lua')
    return {
        'entries': entries_test.problems(t['entries'], t['primitives'], t['header'], loaded),
        'side rules': side_rule_problems(t['header'], t['side_rules']),
        'LuaLS': lua,
        'docs': docs,
    }


@pytest.fixture(scope='module')
def loaded():
    return entries_test.mapping_db()


def test_the_repository_passes_every_check(loaded, tmp_path):
    assert run_checks(texts(), loaded, tmp_path) == {'entries': [], 'side rules': [], 'LuaLS': [], 'docs': []}


def test_a_new_function(loaded, tmp_path):
    # covers: game-api/A new function (every step done: the four checks pass)
    t = texts()
    for _, step in STEPS:
        step(t)
    assert run_checks(t, loaded, tmp_path) == {'entries': [], 'side rules': [], 'LuaLS': [], 'docs': []}


# the step skipped, and the checks that fail for it
SKIPPED = [('primitives.md', {'entries'}), ('entries.inc', {'entries'}), ('member', {'entries', 'side rules'}),
           ('side rule', {'side rules'}), ('Lua', {'LuaLS'}), ('docs', {'docs'})]


@pytest.mark.parametrize('skipped,failing', SKIPPED, ids=[s for s, _ in SKIPPED])
def test_a_skipped_step_fails_its_check(loaded, tmp_path, skipped, failing):
    # covers: game-api/A new function (each check fails when its step is skipped)
    t = texts()
    for name, step in STEPS:
        if name != skipped:
            step(t)
    found = {check: p for check, p in run_checks(t, loaded, tmp_path).items() if p}
    assert set(found) == failing, found
    for check, p in found.items():
        assert any('example' in line for line in p), (check, p)


def test_the_docs_check_wants_side_and_errors(loaded, tmp_path):
    t = texts()
    for _, step in STEPS:
        step(t)
    t['doc'] = replace_once(t['doc'], "- **Errors:** the guard's.\n", '')
    assert run_checks(t, loaded, tmp_path)['docs'] == [f'docs/game-api.md: the entry of `{FN}` has no **Errors:** line']


@pytest.mark.skipif(not stubs_test.LUALS, reason='lua-language-server not available')
@pytest.mark.parametrize('with_stub', [True, False], ids=['stub', 'no-stub'])
def test_luals_knows_the_new_function_only_from_its_stub(tmp_path, with_stub):
    t = texts()
    for name, step in STEPS:
        if with_stub or name != 'Lua':
            step(t)
    work = tmp_path / 'mod'
    (work / 'types').mkdir(parents=True)
    (work / 'types' / 'kyubu.lua').write_text(t['stub'], encoding='utf-8')
    (work / '.luarc.json').write_text('{"runtime.version": "LuaJIT", "workspace.library": ["types"]}')
    (work / 'client.lua').write_text('local game = kyubu.game\nif game then\n'
                                     f'  local id, why = game.{FN}(0)\n  kyubu.info(id, why)\nend\n')
    r = subprocess.run([stubs_test.LUALS, '--check', str(work), '--checklevel=Warning',
                        f'--logpath={tmp_path / "log"}'], capture_output=True, text=True, timeout=300)
    out = r.stdout + r.stderr
    assert ('no problems found' in out) == with_stub, out
