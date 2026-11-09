"""pytest suite for tools/kcat (add-mod-manager, D12), with a throwaway key."""
import hashlib
import json
import os
import sys
import zipfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'kcat'))

import kcat  # noqa: E402

YES = lambda q: True   # noqa: E731
NO = lambda q: False   # noqa: E731


def make_mod(root, mid, version, kyubu='^1.1', deps=None, extra=None):
    d = os.path.join(root, '%s-%s' % (mid, version))
    os.makedirs(os.path.join(d, 'scripts'), exist_ok=True)
    lines = ['id = "%s"' % mid, 'name = "%s"' % mid.title(), 'version = "%s"' % version,
             'kyubu = "%s"' % kyubu, 'sides = ["client"]', 'authors = ["tester"]', 'description = "Test mod."']
    if deps:
        lines.append('[dependencies]')
        lines += ['%s = "%s"' % kv for kv in deps.items()]
    open(os.path.join(d, 'mod.toml'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    open(os.path.join(d, 'scripts', 'main.lua'), 'w').write('-- %s %s\n' % (mid, version))
    open(os.path.join(d, 'config.toml'), 'w').write('[ring_alpha]\ndefault = 0.5\n')
    for name, data in (extra or {}).items():
        open(os.path.join(d, name), 'wb').write(data)
    return d


@pytest.fixture
def env(tmp_path):
    key = str(tmp_path / 'keys' / 'catalog.key')
    pub = kcat.keygen(key)
    cat = kcat.Catalog(str(tmp_path / 'catalog'))
    return {'tmp': str(tmp_path), 'key': key, 'pub': pub, 'cat': cat, 'src': str(tmp_path / 'src')}


def publish(env, mid, version, **kw):
    folder = make_mod(env['src'], mid, version, **kw)
    assert kcat.cmd_add(env['cat'], folder, YES, out=lambda s: None)


def write_pack(env, text, name='pack.toml'):
    p = os.path.join(env['tmp'], name)
    open(p, 'w').write(text)
    return p


def sign_and_check(env):
    kcat.cmd_sign(env['cat'], kcat.load_key(env['key']), out=lambda s: None)
    return kcat.cmd_check(env['cat'], env['pub'])


# ---------------------------------------------------------------- hash vector shared with the loader
def test_folder_hash_matches_golden_vector():
    canon = os.path.join(ROOT, 'tests', 'fixtures', 'manage', 'canonical')
    text = open(os.path.join(ROOT, 'tests', 'fixtures', 'manage', 'golden.txt')).read().replace('\r\n', '\n')
    listing = text.split('listing:\n')[1].split('folder:\n')[0]
    folder = text.split('folder:\n')[1].strip()
    assert kcat.folder_listing(canon) == listing
    assert kcat.package_hash(canon) == folder


def test_zip_hash_is_file_hash(tmp_path):
    z = tmp_path / 'a.zip'
    z.write_bytes(b'not really a zip')
    assert kcat.package_hash(str(z)) == hashlib.sha256(b'not really a zip').hexdigest()


# ---------------------------------------------------------------- versions
def test_requirements():
    v = kcat.Version.parse
    r = kcat.VersionReq
    assert r('^1.2').matches(v('1.9.0')) and not r('^1.2').matches(v('2.0.0')) and not r('^1.2').matches(v('1.1.9'))
    assert r('^0.3').matches(v('0.3.5')) and not r('^0.3').matches(v('0.4.0'))
    assert r('~1.2').matches(v('1.2.9')) and not r('~1.2').matches(v('1.3.0'))
    assert r('>=1.0, <1.5').matches(v('1.4.9')) and not r('>=1.0, <1.5').matches(v('1.5.0'))
    assert r('=1.4.2').matches(v('1.4.2')) and not r('=1.4.2').matches(v('1.4.3'))
    assert r('*').matches(v('9.9.9'))
    assert v('1.0.0-beta') < v('1.0.0') < v('1.10.0')
    assert r('^1.2').min_version() == v('1.2.0')


# ---------------------------------------------------------------- keys, add, sign, check
def test_keygen_refuses_overwrite_and_git_tree(env, tmp_path):
    with pytest.raises(kcat.KcatError, match='refusing to overwrite'):
        kcat.keygen(env['key'])
    repo = tmp_path / 'repo'
    (repo / '.git').mkdir(parents=True)
    with pytest.raises(kcat.KcatError, match='inside a git repository'):
        kcat.keygen(str(repo / 'k.key'))


def test_add_sign_check_round_trip(env):
    publish(env, 'emtweaks', '1.3.0')
    assert sign_and_check(env) == []
    idx = env['cat'].load()
    v = idx['mods'][0]['versions'][0]
    assert v['url'] == 'mods/emtweaks/emtweaks-1.3.0.zip' and v['size'] > 0
    assert v['sha256'] == kcat.package_hash(env['cat'].zip_path(v['url']))
    assert len(open(env['cat'].index_path + '.sig', 'rb').read()) == 64


def test_zip_is_reproducible(env):
    folder = make_mod(env['src'], 'emtweaks', '1.3.0')
    assert kcat.make_zip(folder) == kcat.make_zip(folder)


def test_add_declined_writes_nothing(env):
    folder = make_mod(env['src'], 'emtweaks', '1.3.0')
    assert kcat.cmd_add(env['cat'], folder, NO, out=lambda s: None) is False
    assert not os.path.exists(env['cat'].index_path)


def test_add_refuses_game_file_and_bad_manifest(env):
    folder = make_mod(env['src'], 'bad_game', '1.0.0', extra={'Cube.exe': b'MZ'})
    with pytest.raises(kcat.KcatError, match='game file'):
        kcat.cmd_add(env['cat'], folder, YES, out=lambda s: None)
    folder = make_mod(env['src'], 'bad_ver', 'one')
    with pytest.raises(kcat.KcatError, match="key 'version'"):
        kcat.cmd_add(env['cat'], folder, YES, out=lambda s: None)


def test_add_refuses_path_escape_zip(env, tmp_path):
    z = tmp_path / 'evil.zip'
    with zipfile.ZipFile(z, 'w') as zf:
        zf.writestr('mod.toml', open(make_mod(env['src'], 'evil_one', '1.0.0') + '/mod.toml').read())
        zf.writestr('../zlib1.dll', b'x')
    with pytest.raises(kcat.KcatError, match='escaping path'):
        kcat.cmd_add(env['cat'], str(z), YES, out=lambda s: None)


def test_add_same_version_other_content_refused(env):
    publish(env, 'emtweaks', '1.3.0')
    folder = make_mod(env['src'], 'emtweaks', '1.3.0', extra={'scripts/extra.lua': b'-- changed\n'})
    with pytest.raises(kcat.KcatError, match='already published'):
        kcat.cmd_add(env['cat'], folder, YES, out=lambda s: None)


def test_check_catches_replaced_zip(env):
    publish(env, 'emtweaks', '1.3.0')
    assert sign_and_check(env) == []
    path = env['cat'].zip_path('mods/emtweaks/emtweaks-1.3.0.zip')
    old = kcat.sha256_file(path)
    open(path, 'ab').write(b'x')
    problems = kcat.cmd_check(env['cat'], env['pub'])
    assert len(problems) == 1
    assert 'emtweaks-1.3.0.zip' in problems[0] and old in problems[0] and kcat.sha256_file(path) in problems[0]


def test_check_catches_tampered_index_and_wrong_key(env, tmp_path):
    publish(env, 'emtweaks', '1.3.0')
    assert sign_and_check(env) == []
    other = kcat.keygen(str(tmp_path / 'other' / 'k.key'))
    assert any('not a valid signature' in p for p in kcat.cmd_check(env['cat'], other))
    raw = open(env['cat'].index_path, 'rb').read()
    open(env['cat'].index_path, 'wb').write(raw.replace(b'Test mod.', b'Test mod!'))
    assert any('not a valid signature' in p for p in kcat.cmd_check(env['cat'], env['pub']))


def test_check_missing_signature(env):
    publish(env, 'emtweaks', '1.3.0')
    assert kcat.cmd_check(env['cat'], env['pub']) == ['index.json.sig is missing']


# ---------------------------------------------------------------- packs
def test_pack_refuses_range(env):
    publish(env, 'emtweaks', '1.3.0')
    p = write_pack(env, 'id = "p1"\nname = "P"\nversion = "1.0.0"\nkyubu = "^1.2"\n[mods]\nemtweaks = "^1.3"\n')
    with pytest.raises(kcat.KcatError, match='exact version'):
        kcat.cmd_pack(env['cat'], p, YES, out=lambda s: None)


def test_pack_refuses_unknown_mod_and_version(env):
    publish(env, 'emtweaks', '1.3.0')
    p = write_pack(env, 'id = "p1"\nname = "P"\nversion = "1.0.0"\nkyubu = "^1.2"\n[mods]\nunknown_mod = "1.0.0"\n')
    with pytest.raises(kcat.KcatError, match='unknown_mod'):
        kcat.cmd_pack(env['cat'], p, YES, out=lambda s: None)
    p = write_pack(env, 'id = "p1"\nname = "P"\nversion = "1.0.0"\nkyubu = "^1.2"\n[mods]\nemtweaks = "1.9.9"\n')
    with pytest.raises(kcat.KcatError, match='1.9.9'):
        kcat.cmd_pack(env['cat'], p, YES, out=lambda s: None)


def test_pack_adds_dependency_with_exact_version(env):
    publish(env, 'libx', '0.4.0')
    publish(env, 'libx', '0.4.2')
    publish(env, 'libx', '0.5.0')
    publish(env, 'emtweaks', '1.3.0', deps={'libx': '^0.4'})
    asked = []
    p = write_pack(env, 'id = "p1"\nname = "P"\nversion = "1.0.0"\nkyubu = "^1.2"\n[mods]\nemtweaks = "1.3.0"\n'
                        '[config.emtweaks]\nring_alpha = 0.6\n')
    kcat.cmd_pack(env['cat'], p, lambda q: asked.append(q) or True, out=lambda s: None)
    pack = env['cat'].load()['packs'][0]
    assert pack['mods'] == {'emtweaks': '1.3.0', 'libx': '0.4.2'}   # the highest that fits
    assert pack['reset_unlisted'] is True and pack['config'] == {'emtweaks': {'ring_alpha': 0.6}}
    assert len(asked) == 1 and 'libx 0.4.2' in asked[0]
    assert sign_and_check(env) == []


def test_pack_dependency_declined_or_unsatisfied(env):
    publish(env, 'libx', '0.5.0')
    publish(env, 'emtweaks', '1.3.0', deps={'libx': '^0.4'})
    p = write_pack(env, 'id = "p1"\nname = "P"\nversion = "1.0.0"\nkyubu = "^1.2"\n[mods]\nemtweaks = "1.3.0"\n')
    with pytest.raises(kcat.KcatError, match='no version of libx'):
        kcat.cmd_pack(env['cat'], p, YES, out=lambda s: None)
    publish(env, 'libx', '0.4.1')
    with pytest.raises(kcat.KcatError, match='not added'):
        kcat.cmd_pack(env['cat'], p, NO, out=lambda s: None)
    p = write_pack(env, 'id = "p1"\nname = "P"\nversion = "1.0.0"\nkyubu = "^1.2"\n[mods]\n'
                        'emtweaks = "1.3.0"\nlibx = "0.5.0"\n')
    with pytest.raises(kcat.KcatError, match='pack pins 0.5.0'):
        kcat.cmd_pack(env['cat'], p, YES, out=lambda s: None)


def test_pack_kyubu_must_cover_mod_requirement(env):
    publish(env, 'emtweaks', '1.3.0', kyubu='^1.2')
    p = write_pack(env, 'id = "p1"\nname = "P"\nversion = "1.0.0"\nkyubu = "^1.1"\n[mods]\nemtweaks = "1.3.0"\n')
    with pytest.raises(kcat.KcatError, match='does not guarantee'):
        kcat.cmd_pack(env['cat'], p, YES, out=lambda s: None)


def test_remove_refuses_pinned_version_and_check_fails_when_missing(env):
    publish(env, 'emtweaks', '1.2.0')
    publish(env, 'emtweaks', '1.3.0')
    p = write_pack(env, 'id = "p1"\nname = "P"\nversion = "1.0.0"\nkyubu = "^1.2"\n[mods]\nemtweaks = "1.3.0"\n')
    kcat.cmd_pack(env['cat'], p, YES, out=lambda s: None)
    with pytest.raises(kcat.KcatError, match='pinned by the pack p1'):
        kcat.cmd_remove(env['cat'], 'emtweaks', '1.3.0', out=lambda s: None)
    with pytest.raises(kcat.KcatError, match='p1'):
        kcat.cmd_remove(env['cat'], 'emtweaks', out=lambda s: None)
    kcat.cmd_remove(env['cat'], 'emtweaks', '1.2.0', out=lambda s: None)   # not pinned: allowed
    idx = env['cat'].load()
    assert [v['version'] for v in idx['mods'][0]['versions']] == ['1.3.0']
    assert not os.path.exists(env['cat'].zip_path('mods/emtweaks/emtweaks-1.2.0.zip'))
    # a pinned version gone by hand makes check fail
    idx['mods'][0]['versions'] = []
    json.dump(idx, open(env['cat'].index_path, 'w'))
    assert any('pinned emtweaks 1.3.0 is missing' in x for x in sign_and_check(env))


def test_command_line(env, capsys):
    folder = make_mod(env['src'], 'emtweaks', '1.3.0')
    base = ['--catalog', env['cat'].folder, '--key', env['key'], '--yes']
    assert kcat.main(base + ['add', folder]) == 0
    assert kcat.main(base + ['sign']) == 0
    assert kcat.main(base + ['check']) == 0
    assert 'Catalog ok.' in capsys.readouterr().out
    assert kcat.main(base + ['remove', 'nope']) == 1


def test_fixture_catalog_is_valid():
    fx = os.path.join(ROOT, 'tests', 'fixtures')
    cat = kcat.Catalog(os.path.join(fx, 'catalog'))
    key = kcat.load_key(os.path.join(fx, 'catalog-src', 'test.key'))
    assert kcat.cmd_check(cat, kcat.public_hex(key)) == []
    idx = cat.load()
    assert sorted(m['id'] for m in idx['mods']) == ['cat_alpha', 'cat_beta', 'cat_lib']
    assert len(find_alpha(idx)['versions']) == 2
    assert idx['packs'][0]['mods']['cat_alpha'] == '1.0.0'


def find_alpha(idx):
    return kcat.find_mod(idx, 'cat_alpha')


# ---------------------------------------------------------------- kcat build (D16)
class FakeRun:
    """Stands in for subprocess.run: ninja writes the DLL, strip copies its first 4 bytes."""
    def __init__(self, build_dir, root='gameplay-mods'):
        self.calls, self.build_dir, self.root = [], build_dir, root

    def __call__(self, cmd, **kw):
        import types
        self.calls.append(cmd)
        name = os.path.basename(cmd[0]).lower()
        if name.startswith('ninja'):
            d = os.path.join(self.build_dir, self.root, cmd[-1], 'bin')
            os.makedirs(d, exist_ok=True)
            open(os.path.join(d, cmd[-1] + '.dll'), 'wb').write(b'MZ-unstripped')
        elif name.startswith('strip'):
            open(cmd[2], 'wb').write(open(cmd[3], 'rb').read()[:4])
        return types.SimpleNamespace(returncode=0, stdout='', stderr='')


def native_source(tmp, mid='nat', version='0.2.0'):
    d = os.path.join(tmp, 'mods', mid)
    os.makedirs(os.path.join(d, 'src'))
    os.makedirs(os.path.join(d, 'assets'))
    open(os.path.join(d, 'mod.toml'), 'w').write(
        'id = "%s"\nname = "N"\nversion = "%s"\nkyubu = "^1.1"\nsides = ["client"]\n[native]\nclient = "bin/%s.dll"\n'
        % (mid, version, mid))
    open(os.path.join(d, 'config.toml'), 'w').write('[a]\ndefault = 1\n')
    open(os.path.join(d, 'assets', 'x.txt'), 'w').write('x')
    open(os.path.join(d, 'src', 'main.cpp'), 'w').write('// source')
    return d


def quiet(_):
    pass


def test_build_native_strips_and_leaves_source(tmp_path):
    src = native_source(str(tmp_path))
    bd = str(tmp_path / 'build')
    os.makedirs(bd)
    run = FakeRun(bd)
    out = kcat.cmd_build(src, str(tmp_path / 'out'), bd, str(tmp_path / 'tc'), runner=run, say=quiet)
    files = sorted(os.path.relpath(os.path.join(r, f), out).replace(os.sep, '/') for r, _, fs in os.walk(out) for f in fs)
    assert files == ['assets/x.txt', 'bin/nat.dll', 'config.toml', 'mod.toml']
    assert open(os.path.join(out, 'bin', 'nat.dll'), 'rb').read() == b'MZ-u'   # the stripped copy
    assert [os.path.basename(c[0]).lower() for c in run.calls] == ['ninja', 'strip']
    kcat.inspect_folder(out)


def test_build_is_repeatable(tmp_path):
    src = native_source(str(tmp_path))
    bd = str(tmp_path / 'build')
    os.makedirs(bd)
    a = kcat.cmd_build(src, str(tmp_path / 'o1'), bd, str(tmp_path / 'tc'), runner=FakeRun(bd), say=quiet)
    b = kcat.cmd_build(src, str(tmp_path / 'o2'), bd, str(tmp_path / 'tc'), runner=FakeRun(bd), say=quiet)
    assert kcat.make_zip(a) == kcat.make_zip(b)
    assert kcat.package_hash(a) == kcat.package_hash(b)
    kcat.cmd_build(src, str(tmp_path / 'o1'), bd, str(tmp_path / 'tc'), runner=FakeRun(bd), say=quiet)   # over its own output


def test_build_refuses_foreign_output_and_wrong_folder(tmp_path):
    src = native_source(str(tmp_path))
    bd = str(tmp_path / 'build')
    os.makedirs(bd)
    os.makedirs(tmp_path / 'other')
    (tmp_path / 'other' / 'keep.txt').write_text('mine')
    with pytest.raises(kcat.KcatError, match='not an earlier build'):
        kcat.cmd_build(src, str(tmp_path / 'other'), bd, 'tc', runner=FakeRun(bd), say=quiet)
    assert (tmp_path / 'other' / 'keep.txt').exists()
    os.rename(src, str(tmp_path / 'mods' / 'renamed'))
    with pytest.raises(kcat.KcatError, match='mod.toml says id'):
        kcat.cmd_build(str(tmp_path / 'mods' / 'renamed'), str(tmp_path / 'o'), bd, 'tc', runner=FakeRun(bd), say=quiet)


def test_build_native_needs_toolchain_and_dll(tmp_path, monkeypatch):
    import types
    src = native_source(str(tmp_path))
    monkeypatch.setattr(kcat, 'LOCAL_CONFIG', str(tmp_path / 'none.toml'))
    with pytest.raises(kcat.KcatError, match='needs build_dir'):
        kcat.cmd_build(src, str(tmp_path / 'o'), None, None, say=quiet)
    bd = str(tmp_path / 'build')
    os.makedirs(bd)
    silent = lambda cmd, **kw: types.SimpleNamespace(returncode=0, stdout='', stderr='')   # noqa: E731
    with pytest.raises(kcat.KcatError, match='made no'):
        kcat.cmd_build(src, str(tmp_path / 'o'), bd, 'tc', runner=silent, say=quiet)


def test_build_ninja_failure_is_reported(tmp_path):
    import types
    src = native_source(str(tmp_path))
    bd = str(tmp_path / 'build')
    os.makedirs(bd)
    bad = lambda cmd, **kw: types.SimpleNamespace(returncode=1, stdout='', stderr='error: boom')   # noqa: E731
    with pytest.raises(kcat.KcatError, match='boom'):
        kcat.cmd_build(src, str(tmp_path / 'o'), bd, 'tc', runner=bad, say=quiet)


def test_build_lua_mod_and_game_file(tmp_path):
    src = make_mod(str(tmp_path / 'mods'), 'luam', '1.0.0')
    os.rename(src, str(tmp_path / 'mods' / 'luam'))
    src = str(tmp_path / 'mods' / 'luam')
    os.makedirs(os.path.join(src, 'icons-src'))
    open(os.path.join(src, 'icons-src', 'a.png'), 'w').write('x')
    out = kcat.cmd_build(src, str(tmp_path / 'o'), runner=None, say=quiet)   # no native side: no process starts
    assert sorted(os.listdir(out)) == ['config.toml', 'mod.toml', 'scripts']
    os.makedirs(os.path.join(src, 'assets'))
    open(os.path.join(src, 'assets', 'Cube.exe'), 'w').write('x')
    with pytest.raises(kcat.KcatError, match='game file'):
        kcat.cmd_build(src, str(tmp_path / 'o2'), runner=None, say=quiet)


# ---------------------------------------------------------------- kcat verify and the hook (D16)
KEYS_H = '''#pragma once
inline std::vector<Key> trusted() {
    std::vector<Key> k;
#ifdef KYUBU_MM_TEST_KEY
    k.push_back(Key{%(test)s});
#endif
%(real)s    return k;
}
'''


def c_bytes(pub_hex):
    return ', '.join('0x%02x' % b for b in bytes.fromhex(pub_hex))


def keys_h(tmp, real=(), test='aa', name='keys.h'):
    real_lines = ''.join('    k.push_back(Key{%s});\n' % c_bytes(r) for r in real)
    p = os.path.join(tmp, name)
    open(p, 'w').write(KEYS_H % {'test': c_bytes(test * 32), 'real': real_lines})
    return p


def test_trusted_keys_skips_the_test_block(env):
    kh = keys_h(env['tmp'], real=[env['pub']])
    assert kcat.trusted_keys(kh) == [env['pub']]
    assert kcat.trusted_keys(keys_h(env['tmp'], name='k5.h')) == []


def test_verify_good_tampered_wrong_key_and_unsigned(env):
    publish(env, 'alpha', '1.0.0')
    assert sign_and_check(env) == []
    kh = keys_h(env['tmp'], real=[env['pub']])
    assert kcat.cmd_verify(env['cat'], kh) == []
    zp = os.path.join(env['cat'].folder, 'mods', 'alpha', 'alpha-1.0.0.zip')
    data = bytearray(open(zp, 'rb').read())
    data[-1] ^= 1
    open(zp, 'wb').write(bytes(data))
    assert any('was replaced' in p for p in kcat.cmd_verify(env['cat'], kh))
    data[-1] ^= 1
    open(zp, 'wb').write(bytes(data))
    other = kcat.public_hex(kcat.Ed25519PrivateKey.generate())
    assert any('not a valid signature' in p for p in kcat.cmd_verify(env['cat'], keys_h(env['tmp'], real=[other], name='k2.h')))
    assert kcat.cmd_verify(env['cat'], keys_h(env['tmp'], real=[other, env['pub']], name='k3.h')) == []   # any listed key
    assert any('no real public key' in p for p in kcat.cmd_verify(env['cat'], keys_h(env['tmp'], name='k4.h')))
    publish(env, 'beta', '1.0.0')   # the index changed after signing
    assert any('not a valid signature' in p for p in kcat.cmd_verify(env['cat'], kh))


def test_hook_refuses_an_unsigned_index_and_a_foreign_hook(env):
    import subprocess
    git = shutil_which('git')
    if not git:
        pytest.skip('git not found')
    publish(env, 'alpha', '1.0.0')
    sign_and_check(env)
    kh = keys_h(env['tmp'], real=[env['pub']])
    folder = env['cat'].folder
    subprocess.run([git, 'init', '-q'], cwd=folder, check=True)
    kcat.cmd_hook(env['cat'], kh, say=lambda s: None)
    kcat.cmd_hook(env['cat'], kh, say=lambda s: None)   # again: its own hook is replaced
    git_cfg = ['-c', 'user.name=t', '-c', 'user.email=t@t']
    subprocess.run([git, 'add', '-A'], cwd=folder, check=True)
    ok = subprocess.run([git] + git_cfg + ['commit', '-q', '-m', 'one'], cwd=folder, capture_output=True, text=True)
    assert ok.returncode == 0, ok.stdout + ok.stderr
    publish(env, 'beta', '1.0.0')   # no sign
    subprocess.run([git, 'add', '-A'], cwd=folder, check=True)
    bad = subprocess.run([git] + git_cfg + ['commit', '-q', '-m', 'two'], cwd=folder, capture_output=True, text=True)
    assert bad.returncode != 0 and 'not a valid signature' in bad.stdout + bad.stderr
    hook = os.path.join(folder, '.git', 'hooks', 'pre-commit')
    open(hook, 'w').write('#!/bin/sh\nexit 0\n')
    with pytest.raises(kcat.KcatError, match='not a kcat hook'):
        kcat.cmd_hook(env['cat'], kh, say=lambda s: None)


def test_hook_needs_a_git_clone(env):
    os.makedirs(env['cat'].folder, exist_ok=True)
    with pytest.raises(kcat.KcatError, match='not a git clone'):
        kcat.cmd_hook(env['cat'], 'keys.h', say=lambda s: None)


def shutil_which(name):
    import shutil
    return shutil.which(name)


# ---------------------------------------------------------------- kcat release and tag (D16)
class GitRunner:
    """subprocess.run, except `gh`, which is recorded (gh_rc 0 = ok, None = not installed)."""
    def __init__(self, gh_rc=0, fail_gh_create=False):
        self.gh, self.gh_rc, self.fail_create = [], gh_rc, fail_gh_create

    def __call__(self, cmd, **kw):
        import subprocess
        import types
        if cmd[0] == 'gh':
            if self.gh_rc is None:
                raise FileNotFoundError('gh')
            self.gh.append(cmd)
            rc = self.gh_rc
            if cmd[1:3] == ['release', 'view']:
                rc = 1
            if cmd[1:3] == ['release', 'create'] and self.fail_create:
                rc = 1
            return types.SimpleNamespace(returncode=rc, stdout='', stderr='gh says no' if rc else '')
        return subprocess.run(cmd, **kw)


def git(cwd, *args):
    import subprocess
    r = subprocess.run(['git'] + list(args), cwd=cwd, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


@pytest.fixture
def rel(env, monkeypatch):
    if not shutil_which('git'):
        pytest.skip('git not found')
    for k, v in (('NAME', 't'), ('EMAIL', 't@t')):
        monkeypatch.setenv('GIT_AUTHOR_' + k, v)
        monkeypatch.setenv('GIT_COMMITTER_' + k, v)
    tmp = env['tmp']
    monkeypatch.setattr(kcat, 'LOCAL_CONFIG', os.path.join(tmp, 'none.toml'))
    cat_remote, mod_remote = os.path.join(tmp, 'cat.git'), os.path.join(tmp, 'mod.git')
    for r in (cat_remote, mod_remote):
        git(tmp, 'init', '-q', '--bare', r)
    folder = env['cat'].folder
    os.makedirs(folder, exist_ok=True)
    git(folder, 'init', '-q')
    git(folder, 'remote', 'add', 'origin', cat_remote)
    open(os.path.join(folder, 'README'), 'w').write('catalog\n')
    git(folder, 'add', '-A')
    git(folder, 'commit', '-q', '-m', 'start')
    src = make_mod(os.path.join(tmp, 'mods'), 'luam', '1.0.0')
    mod = os.path.join(tmp, 'mods', 'luam')
    os.rename(src, mod)
    git(mod, 'init', '-q')
    git(mod, 'remote', 'add', 'origin', mod_remote)
    git(mod, 'add', '-A')
    git(mod, 'commit', '-q', '-m', 'luam 1.0.0')
    env.update(mod=mod, cat_remote=cat_remote, mod_remote=mod_remote, kh=keys_h(tmp, real=[env['pub']]),
               signing_key=kcat.load_key(env['key']))
    return env


def release(rel, **kw):
    args = dict(bump=None, changes='', push=True, runner=GitRunner(), say=lambda s: None)
    args.update(kw)
    return kcat.cmd_release(rel['cat'], rel['mod'], rel['signing_key'], rel['kh'], **args)


def test_release_bump_publishes_tags_and_makes_the_release(rel):
    gh = GitRunner()
    release(rel, bump='minor', changes='Faster.', runner=gh)
    idx = rel['cat'].load()
    v = kcat.find_version(kcat.find_mod(idx, 'luam'), '1.1.0')
    assert v and v['changes'] == 'Faster.'
    assert kcat.cmd_verify(rel['cat'], rel['kh']) == []
    assert 'luam 1.1.0' in git(rel['cat_remote'], 'log', '--all', '--format=%s')
    assert 'version = "1.1.0"' in open(os.path.join(rel['mod'], 'mod.toml')).read()
    assert 'v1.1.0' in git(rel['mod_remote'], 'tag')
    assert git(rel['mod'], 'status', '--porcelain').strip() == ''
    create = [c for c in gh.gh if c[1:3] == ['release', 'create']][0]
    assert create[3] == 'v1.1.0' and create[4].endswith('luam-1.1.0.zip') and os.path.exists(create[4])


def test_release_refuses_a_published_version_and_a_dirty_repository(rel):
    release(rel)   # 1.0.0, no bump
    with pytest.raises(kcat.KcatError, match='already published'):
        release(rel)
    open(os.path.join(rel['mod'], 'scripts', 'main.lua'), 'a').write('-- edit\n')
    with pytest.raises(kcat.KcatError, match='uncommitted'):
        release(rel, bump='patch')


def test_release_needs_gh_before_it_changes_anything(rel):
    with pytest.raises(kcat.KcatError, match='gh is not installed'):
        release(rel, bump='patch', runner=GitRunner(gh_rc=None))
    assert not os.path.exists(rel['cat'].index_path)
    with pytest.raises(kcat.KcatError, match='gh is not installed'):
        release(rel, bump='patch', runner=GitRunner(gh_rc=1))
    release(rel, bump='patch', push=False, runner=GitRunner(gh_rc=None))   # no push: gh is not needed


def restore_check(rel, text_before):
    assert not os.path.exists(rel['cat'].index_path)
    assert not os.path.exists(rel['cat'].index_path + '.sig')
    assert not os.path.exists(os.path.join(rel['cat'].folder, 'mods', 'luam', 'luam-1.1.0.zip'))
    assert open(os.path.join(rel['mod'], 'mod.toml')).read() == text_before


def test_release_stops_on_the_test_key_and_restores(rel):
    text = open(os.path.join(rel['mod'], 'mod.toml')).read()
    os.makedirs(os.path.join(rel['mod'], 'assets'))
    open(os.path.join(rel['mod'], 'assets', 'x.dll'), 'wb').write(b'MZ' + bytes.fromhex('aa' * 32) + b'tail')
    git(rel['mod'], 'add', '-A')
    git(rel['mod'], 'commit', '-q', '-m', 'dll')
    with pytest.raises(kcat.KcatError, match='test public key'):
        release(rel, bump='minor')
    restore_check(rel, text)


def test_release_restores_when_verify_fails(rel):
    text = open(os.path.join(rel['mod'], 'mod.toml')).read()
    other = kcat.public_hex(kcat.Ed25519PrivateKey.generate())
    rel['kh'] = keys_h(rel['tmp'], real=[other], name='other.h')   # signed with a key players do not trust
    with pytest.raises(kcat.KcatError, match='verify failed'):
        release(rel, bump='minor')
    restore_check(rel, text)
    assert git(rel['cat'].folder, 'status', '--porcelain').strip() == ''


def test_release_no_push_commits_nothing(rel):
    release(rel, bump='minor', push=False)
    assert kcat.cmd_verify(rel['cat'], rel['kh']) == []
    assert git(rel['cat'].folder, 'log', '--format=%s').split() == ['start']
    assert git(rel['mod_remote'], 'tag').strip() == ''


def test_release_gh_failure_leaves_the_catalog_published_and_tag_retries(rel):
    said = []
    release(rel, bump='minor', runner=GitRunner(fail_gh_create=True), say=said.append)
    assert any('Retry with: kcat tag' in s for s in said)
    assert 'luam 1.1.0' in git(rel['cat'].folder, 'log', '--format=%s')
    gh = GitRunner()
    kcat.cmd_tag(rel['cat'], rel['mod'], runner=gh, say=lambda s: None)
    assert [c[3] for c in gh.gh if c[1:3] == ['release', 'create']] == ['v1.1.0']


def test_release_of_mod_manager_runs_its_unit_tests(rel):
    new = os.path.join(rel['tmp'], 'mods', 'mod_manager')
    os.rename(rel['mod'], new)
    rel['mod'] = new
    text = open(os.path.join(new, 'mod.toml')).read().replace('id = "luam"', 'id = "mod_manager"')
    open(os.path.join(new, 'mod.toml'), 'w').write(text)
    git(new, 'commit', '-q', '-am', 'id')
    with pytest.raises(kcat.KcatError, match='mod_manager_tests'):
        release(rel, push=False)
    failing = [sys.executable, '-c', 'raise SystemExit(1)']
    with pytest.raises(kcat.KcatError, match='failed'):
        release(rel, push=False, unit_tests=failing)
    assert not os.path.exists(rel['cat'].index_path)
    release(rel, push=False, unit_tests=[sys.executable, '-c', 'pass'])


def test_bump_version():
    assert kcat.bump_version('1.2.3', 'patch') == '1.2.4'
    assert kcat.bump_version('1.2.3', 'minor') == '1.3.0'
    assert kcat.bump_version('1.2.3', 'major') == '2.0.0'
    with pytest.raises(kcat.KcatError):
        kcat.bump_version('1.2.3', 'huge')


def test_finish_after_remove_signs_verifies_and_pushes(rel):
    release(rel)
    release(rel, bump='minor')
    kcat.cmd_remove(rel['cat'], 'luam', '1.1.0', out=lambda s: None)
    kcat.cmd_finish(rel['cat'], rel['signing_key'], rel['kh'], 'remove luam 1.1.0', runner=GitRunner(),
                    say=lambda s: None)
    assert kcat.cmd_verify(rel['cat'], rel['kh']) == []
    assert git(rel['cat'].folder, 'log', '--format=%s').split('\n')[0] == 'remove luam 1.1.0'


# ---------------------------------------------------------------- kcat release with a source repository ([source])
SYNC = ('import os,shutil,sys; m,r=sys.argv[1],sys.argv[2]; d=os.path.join(r,os.path.basename(m)); '
        'shutil.rmtree(d,ignore_errors=True); shutil.copytree(m,d,ignore=shutil.ignore_patterns(".git"))')


@pytest.fixture
def src(rel):
    """rel plus a public source repository: the mod folder is the development tree (it need not be committed), the
    sync copies it into <source>/luam/, and kcat commits, tags and releases there."""
    repo, remote = os.path.join(rel['tmp'], 'luam-public'), os.path.join(rel['tmp'], 'luam-public.git')
    git(rel['tmp'], 'init', '-q', '--bare', remote)
    os.makedirs(repo)
    git(repo, 'init', '-q')
    git(repo, 'remote', 'add', 'origin', remote)
    open(os.path.join(repo, 'README.md'), 'w').write('luam\n')
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', 'start')
    rel.update(src=repo, src_remote=remote)
    write_source_config(rel)
    return rel


def write_source_config(rel, sync=None, extra=''):
    fwd = lambda p: p.replace('\\', '/')
    sync = sync or [fwd(sys.executable), '-c', SYNC, fwd(rel['mod']), '{repo}']
    items = ', '.join("'%s'" % s for s in sync)
    open(kcat.LOCAL_CONFIG, 'w').write("[source]\nrepo = '%s'\nsync = [%s]\n%s"
                                       % (fwd(os.path.join(rel['tmp'], '{id}-public')), items, extra))


def test_source_release_commits_tags_and_releases_in_the_source_repository(src):
    open(os.path.join(src['mod'], 'scripts', 'main.lua'), 'a').write('-- new\n')   # not committed: the dev tree
    gh = GitRunner()
    release(src, bump='minor', changes='New.', runner=gh)
    assert git(src['src'], 'log', '--format=%s').split('\n')[:2] == ['luam 1.1.0', 'start']
    assert 'version = "1.1.0"' in open(os.path.join(src['src'], 'luam', 'mod.toml')).read()
    assert '-- new' in open(os.path.join(src['src'], 'luam', 'scripts', 'main.lua')).read()
    assert git(src['src'], 'status', '--porcelain').strip() == ''
    assert 'v1.1.0' in git(src['src_remote'], 'tag')
    assert 'luam 1.1.0' in git(src['src_remote'], 'log', '--all', '--format=%s')
    assert git(src['mod_remote'], 'tag').strip() == ''                 # nothing in the mod folder's own repository
    assert 'luam 1.1.0' in git(src['cat_remote'], 'log', '--all', '--format=%s')
    assert [c[3] for c in gh.gh if c[1:3] == ['release', 'create']] == ['v1.1.0']


def test_source_release_refuses_a_dirty_source_repository(src):
    open(os.path.join(src['src'], 'notes.txt'), 'w').write('wip\n')
    with pytest.raises(kcat.KcatError, match='source repository of luam .* uncommitted'):
        release(src, bump='minor')
    assert not os.path.exists(src['cat'].index_path)


def test_source_release_restores_everything_when_the_sync_fails(src):
    text = open(os.path.join(src['mod'], 'mod.toml')).read()
    write_source_config(src, sync=[sys.executable.replace('\\', '/'), '-c',
                                   'open("half.txt","w").write("x"); raise SystemExit(3)'])
    with pytest.raises(kcat.KcatError, match='failed'):
        release(src, bump='minor')
    restore_check(src, text)
    assert git(src['src'], 'status', '--porcelain').strip() == ''
    assert git(src['src'], 'log', '--format=%s').strip() == 'start'
    assert git(src['cat'].folder, 'status', '--porcelain').strip() == ''


def test_source_release_keeps_decompiler_output_private(src):
    text = open(os.path.join(src['mod'], 'mod.toml')).read()
    os.makedirs(os.path.join(src['mod'], 'decomp'))
    open(os.path.join(src['mod'], 'decomp', 'f.c'), 'w').write('int f(void);\n')   # never in the zip, but the sync copies it
    with pytest.raises(kcat.KcatError, match='must not go public'):
        release(src, bump='minor')
    restore_check(src, text)
    assert git(src['src'], 'log', '--format=%s').strip() == 'start'
    assert not os.path.exists(os.path.join(src['src'], 'luam'))


def test_source_release_with_its_own_tag_and_a_tag_retry(src):
    write_source_config(src, extra="[source.tags]\nluam = 'luam-v{version}'\n")
    said = []
    release(src, bump='minor', runner=GitRunner(fail_gh_create=True), say=said.append)
    assert 'luam-v1.1.0' in git(src['src_remote'], 'tag')
    assert any('Retry with: kcat tag' in s for s in said)
    gh = GitRunner()
    kcat.cmd_tag(src['cat'], src['mod'], runner=gh, say=lambda s: None)   # reads [source] itself
    assert [c[3] for c in gh.gh if c[1:3] == ['release', 'create']] == ['luam-v1.1.0']


def test_source_release_needs_a_clone(src):
    os.rename(os.path.join(src['src'], '.git'), os.path.join(src['src'], 'git-off'))   # git's objects are read-only
    with pytest.raises(kcat.KcatError, match='not a git clone'):
        release(src, bump='minor')
