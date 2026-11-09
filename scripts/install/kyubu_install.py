#!/usr/bin/env python3
"""Kyubu installer for Cube World Alpha (v0.1.1).

    python kyubu_install.py install   <game_dir> [--force]
    python kyubu_install.py uninstall <game_dir> [--purge] [--force]
    python kyubu_install.py status    <game_dir>

install   renames zlib1.dll to zlib1_vanilla.dll, copies the Kyubu proxy in as zlib1.dll, and adds
          kyubu/ (symbol maps, default config) and mods/ with the mods the release carries (ModManager).
          Running it again upgrades the proxy, the symbol maps and those mods in place. It refuses if Cube.exe/Server.exe or zlib1.dll are not
          the known v0.1.1 files (--force skips the executable check; the loader then runs in
          passthrough mode on an unknown build).
uninstall deletes the proxy and renames zlib1_vanilla.dll back to zlib1.dll. kyubu/ and mods/ are
          left in place (they do nothing without the proxy); --purge also deletes kyubu/.

No other game file is written, renamed or deleted. The script needs only the Python standard library.
"""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import sys

KNOWN_HOSTS = {
    'Cube.exe': '84a7a132a84d4282338e7ea45a1940d32066d64e39cbafed8f2418d5a6dc30bf',     # the 2013-07-20 build
    'Server.exe': '1ffbcbce251e1506a63c9ce18ab0009d5553fcbb9e6202be2bc3a7d870d34fb1',
}
VANILLA_ZLIB = 'e96b397b499d9eaa3f52eaf496ca8941e80c0ad1544879ccadf02bf2c6a1ecfc'
PROXY_MARKER = b'Kyubu crash report'   # string present in every Kyubu proxy build

HERE = os.path.dirname(os.path.abspath(__file__))


class InstallError(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def is_kyubu_proxy(path):
    with open(path, 'rb') as f:
        return PROXY_MARKER in f.read()


def payload_dir():
    """The folder holding zlib1.dll and kyubu/ to install: the release root (this script's folder)."""
    for d in (HERE, os.path.dirname(HERE)):
        if os.path.isfile(os.path.join(d, 'zlib1.dll')) and os.path.isdir(os.path.join(d, 'kyubu')):
            return d
    raise InstallError('cannot find the Kyubu files (zlib1.dll and kyubu/) next to this script')


def state(game):
    """Describes the current install state of a game folder."""
    hosts = {n: sha256(os.path.join(game, n)) for n in KNOWN_HOSTS if os.path.isfile(os.path.join(game, n))}
    zlib = os.path.join(game, 'zlib1.dll')
    van = os.path.join(game, 'zlib1_vanilla.dll')
    s = {
        'hosts': hosts,
        'zlib1': sha256(zlib) if os.path.isfile(zlib) else None,
        'zlib1_is_proxy': os.path.isfile(zlib) and is_kyubu_proxy(zlib),
        'zlib1_vanilla': sha256(van) if os.path.isfile(van) else None,
    }
    if s['zlib1'] == VANILLA_ZLIB and s['zlib1_vanilla'] is None:
        s['state'] = 'vanilla'
    elif s['zlib1_is_proxy'] and s['zlib1_vanilla'] == VANILLA_ZLIB:
        s['state'] = 'installed'
    else:
        s['state'] = 'unknown'
    return s


def data_hashes(game):
    return {n: sha256(os.path.join(game, n)) for n in sorted(os.listdir(game))
            if n.startswith('data') and n.endswith('.db')}


def copy_tree(src, dst, overwrite):
    """Copies src into dst. Existing files are replaced only if overwrite(rel_path) is true."""
    copied = 0
    for root, _, files in os.walk(src):
        rel_root = os.path.relpath(root, src)
        os.makedirs(os.path.join(dst, rel_root), exist_ok=True)
        for f in files:
            rel = os.path.normpath(os.path.join(rel_root, f))
            target = os.path.join(dst, rel)
            if os.path.exists(target) and not overwrite(rel):
                continue
            shutil.copy2(os.path.join(root, f), target)
            copied += 1
    return copied


def install(game, force=False, log=print):
    src = payload_dir()
    s = state(game)
    if not s['hosts']:
        raise InstallError(f'{game} contains neither Cube.exe nor Server.exe')
    for name, h in s['hosts'].items():
        if h != KNOWN_HOSTS[name]:
            msg = f'{name} is not the Cube World Alpha v0.1.1 file Kyubu supports (sha256 {h[:16]}...)'
            if not force:
                raise InstallError(msg + '; use --force to install anyway (Kyubu will stay inactive on that build)')
            log('warning: ' + msg)

    zlib = os.path.join(game, 'zlib1.dll')
    van = os.path.join(game, 'zlib1_vanilla.dll')
    if s['state'] == 'vanilla':
        backup = os.path.join(game, 'kyubu', 'backup')
        os.makedirs(backup, exist_ok=True)
        shutil.copy2(zlib, os.path.join(backup, 'zlib1.dll'))
        os.replace(zlib, van)
        log('renamed zlib1.dll -> zlib1_vanilla.dll (a copy is kept in kyubu/backup/)')
    elif s['state'] == 'installed':
        log('Kyubu is already installed; upgrading')
    else:
        raise InstallError('zlib1.dll is neither the original Cube World file nor a Kyubu proxy '
                           '(another tool may have replaced it); restore the original zlib1.dll first')

    shutil.copy2(os.path.join(src, 'zlib1.dll'), zlib)
    log('installed the Kyubu proxy as zlib1.dll')
    # Symbol maps and Lua modules shipped by the release always win; user files (config, logs, mod data) are kept.
    n = copy_tree(os.path.join(src, 'kyubu'), os.path.join(game, 'kyubu'),
                  overwrite=lambda rel: rel.split(os.sep)[0] in ('symbols', 'lua'))
    cfg = os.path.join(game, 'kyubu', 'config.toml')
    if not os.path.exists(cfg) and os.path.exists(os.path.join(game, 'kyubu', 'config.default.toml')):
        shutil.copy2(os.path.join(game, 'kyubu', 'config.default.toml'), cfg)
    os.makedirs(os.path.join(game, 'mods'), exist_ok=True)
    log(f'copied {n} file(s) into kyubu/, created mods/')
    # The mods the release carries (ModManager): replaced on an upgrade, apart from a player's config.toml.
    bundled = os.path.join(src, 'mods')
    if os.path.isdir(bundled):
        m = copy_tree(bundled, os.path.join(game, 'mods'),
                      overwrite=lambda rel: rel.split(os.sep)[1:] != ['config.toml'])
        log(f'installed {", ".join(sorted(os.listdir(bundled)))} into mods/ ({m} file(s))')

    after = state(game)
    if after['state'] != 'installed':
        raise InstallError('verification failed after install: ' + json.dumps(after))
    with open(os.path.join(game, 'kyubu', 'install.json'), 'w', encoding='utf-8') as f:
        json.dump({'installed_at': datetime.datetime.now().isoformat(timespec='seconds'),
                   'proxy_sha256': after['zlib1'], 'zlib1_vanilla_sha256': after['zlib1_vanilla'],
                   'hosts': after['hosts']}, f, indent=2)
    log('Kyubu installed. Start Cube.exe as usual; hold Shift while it starts for safe mode.')


def uninstall(game, purge=False, force=False, log=print):
    s = state(game)
    zlib = os.path.join(game, 'zlib1.dll')
    van = os.path.join(game, 'zlib1_vanilla.dll')
    if s['state'] == 'vanilla':
        log('Kyubu is not installed (zlib1.dll is the original file)')
    elif s['state'] == 'installed' or (force and s['zlib1_vanilla'] is not None):
        if s['state'] != 'installed':
            log('warning: forcing uninstall of a folder in an unknown state')
        os.remove(zlib)
        os.replace(van, zlib)
        log('removed the proxy and renamed zlib1_vanilla.dll -> zlib1.dll')
    else:
        backup = os.path.join(game, 'kyubu', 'backup', 'zlib1.dll')
        hint = f' A copy of the original is in {backup}.' if os.path.exists(backup) else ''
        raise InstallError('cannot uninstall: zlib1.dll / zlib1_vanilla.dll are not in the state Kyubu left them.' + hint)
    if purge and os.path.isdir(os.path.join(game, 'kyubu')):
        shutil.rmtree(os.path.join(game, 'kyubu'))
        log('deleted kyubu/ (mods/ was kept)')
    after = state(game)
    if after['state'] != 'vanilla':
        raise InstallError('verification failed after uninstall: ' + json.dumps(after))
    log('Cube World is back to vanilla.')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    for name in ('install', 'uninstall', 'status'):
        sp = sub.add_parser(name)
        sp.add_argument('game_dir')
        if name != 'status':
            sp.add_argument('--force', action='store_true')
        if name == 'uninstall':
            sp.add_argument('--purge', action='store_true', help='also delete kyubu/ (logs, config, mod data)')
    a = p.parse_args(argv)
    game = os.path.abspath(a.game_dir)
    try:
        if a.cmd == 'install':
            install(game, a.force)
        elif a.cmd == 'uninstall':
            uninstall(game, a.purge, a.force)
        else:
            s = state(game)
            print(json.dumps(s, indent=2))
    except InstallError as e:
        print('error: ' + str(e), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
