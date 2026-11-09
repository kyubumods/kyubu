#!/usr/bin/env python3
"""cwmod - content pipeline for Cube World Alpha (v0.1.1, July 2013 build).

  cwmod.py extract <game_dir> <workspace>
      Unpacks every asset into <workspace>/vanilla/{models,vox,sounds,icons,text}
      and writes <workspace>/vanilla/catalog.csv (name, loader slot, size, colours, used-by-exe).

  cwmod.py build <game_dir> <mod_dir> <out_dir>
      Validates a mod folder and writes patched copies of data1-4.db into <out_dir>.
      Mod folder layout (every sub-folder optional):
        models/  *.vox or *.cub   -> data1.db   (file name = asset key, e.g. iron-sword1.vox)
        sounds/  *.wav            -> data2.db
        icons/   *.png            -> data3.db
        text/    dict_en.xml / dict_de.xml   -> data4.db (entries are merged by key)

  cwmod.py install <game_dir> <out_dir>     copy built DBs into the game (backs up originals once)
  cwmod.py restore <game_dir>               put the original DBs back

Launch Cube.exe directly after installing. CubeLauncher.exe is the original auto-updater
and could overwrite modified files if an update server ever answers.
"""
import csv, io, json, os, re, shutil, sqlite3, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from cwdb import decode, encode           # noqa: E402
import cubvox                              # noqa: E402

DBS = {'models': 'data1.db', 'sounds': 'data2.db', 'icons': 'data3.db', 'text': 'data4.db'}
EXT = {'models': ('.cub', '.vox'), 'sounds': ('.wav',), 'icons': ('.png',), 'text': ('.xml',)}
SLOTS = json.load(open(os.path.join(HERE, 'model_slots.json')))


def blobs(db):
    con = sqlite3.connect(db)
    return {k: decode(v) for k, v in con.execute('SELECT key, value FROM blobs')}


def cub_bytes_to_model(b):
    sx, sy, sz = struct.unpack('<3i', b[:12])
    return (sx, sy, sz)


# ---------------------------------------------------------------- extract
def extract(game, ws):
    exe = open(os.path.join(game, 'Cube.exe'), 'rb').read()
    root = os.path.join(ws, 'vanilla')
    for sub in ('models', 'vox', 'sounds', 'icons', 'text'):
        os.makedirs(os.path.join(root, sub), exist_ok=True)
    rows = []
    for kind, db in DBS.items():
        for name, data in sorted(blobs(os.path.join(game, db)).items()):
            open(os.path.join(root, kind, name), 'wb').write(data)
            if kind == 'models':
                size, vox = cubvox.read_cub(os.path.join(root, kind, name))
                cubvox.write_vox(os.path.join(root, 'vox', name[:-4] + '.vox'), size, vox)
                rows.append([name, ';'.join(map(str, SLOTS.get(name, []))), 'x'.join(map(str, size)),
                             len(set(vox.values())), len(vox),
                             'yes' if (name.encode() + b'\0') in exe else 'no'])
    with open(os.path.join(root, 'catalog.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['model', 'loader_slot', 'size_xyz', 'colours', 'voxels', 'referenced_by_exe'])
        w.writerows(rows)
    print(f'extracted to {root}  ({len(rows)} models)')


# ---------------------------------------------------------------- text merge
ENTRY = re.compile(r'(\s*<(name|speech|landscape) key="([^"]*)"[^>]*>.*?</\2>)', re.S)


def merge_dict(base: bytes, patch: bytes):
    b = base.decode('utf-8-sig'); p = patch.decode('utf-8-sig').replace('\r\n', '\n').replace('\n', '\r\n')
    index = {m.group(3): m for m in ENTRY.finditer(b)}
    added, replaced = [], []
    for m in ENTRY.finditer(p):
        key, block = m.group(3), m.group(1)
        if key in index:
            b = b.replace(index[key].group(1).lstrip(), block.lstrip(), 1); replaced.append(key)
        else:
            b = b.replace('</root>', block.lstrip() + '\r\n\r\n</root>'); added.append(key)
        index = {mm.group(3): mm for mm in ENTRY.finditer(b)}
    return '﻿'.encode() + b.encode('utf-8'), added, replaced


# ---------------------------------------------------------------- validation
def check_model(name, path, vanilla):
    warn, err = [], []
    if path.endswith('.vox'):
        size, vox = cubvox.read_vox(path)
    else:
        size, vox = cubvox.read_cub(path)
    tmp = path + '.tmp.cub'
    cubvox.write_cub(tmp, size, vox); data = open(tmp, 'rb').read(); os.remove(tmp)
    if not vox: err.append('model is empty')
    if max(size) > 256: err.append(f'size {size} exceeds 256')
    if name in vanilla:
        old = cub_bytes_to_model(vanilla[name])
        if tuple(size) != old:
            warn.append(f'size {tuple(size)} differs from original {old} - '
                        'attached parts (heads, hands, weapons) may sit off-centre')
    elif name not in SLOTS:
        warn.append('NEW asset: no loader slot references this name, so the game will not '
                    'use it until a code mod (or loader patch) points a slot at it')
    return data, warn, err


def check_sound(path, vanilla, name):
    d = open(path, 'rb').read(); warn, err = [], []
    if d[:4] != b'RIFF' or d[8:12] != b'WAVE': err.append('not a RIFF/WAVE file')
    else:
        i = d.find(b'fmt ')
        fmt, ch, sr, _, _, bits = struct.unpack('<HHIIHH', d[i + 8:i + 24])
        if (fmt, bits) not in ((1, 16), (3, 32)): warn.append(f'format {fmt}/{bits}-bit; vanilla uses PCM16 or float32')
        if sr not in (44100, 48000): warn.append(f'sample rate {sr}; vanilla uses 44100')
    if name not in vanilla: warn.append('NEW sound: nothing in the exe plays it without a code mod')
    return d, warn, err


def check_icon(path, vanilla, name):
    from PIL import Image
    d = open(path, 'rb').read(); warn, err = [], []
    im = Image.open(io.BytesIO(d))
    if im.format != 'PNG': err.append('not a PNG')
    if im.mode != 'RGBA': warn.append(f'mode {im.mode}; vanilla icons are RGBA')
    if name in vanilla:
        old = Image.open(io.BytesIO(vanilla[name])).size
        if im.size != old: warn.append(f'size {im.size} differs from original {old}')
    else:
        warn.append('NEW icon: nothing in the exe shows it without a code mod')
    return d, warn, err


# ---------------------------------------------------------------- build / install
def build(game, mod, out):
    os.makedirs(out, exist_ok=True)
    total_err = 0
    for kind, db in DBS.items():
        src = os.path.join(mod, kind)
        dst = os.path.join(out, db)
        files = sorted(f for f in os.listdir(src) if f.lower().endswith(EXT[kind])) if os.path.isdir(src) else []
        if not files:
            continue
        vanilla = blobs(os.path.join(game, db))
        shutil.copyfile(os.path.join(game, db), dst)
        con = sqlite3.connect(dst)
        print(f'\n[{db}]')
        for f in files:
            path = os.path.join(src, f)
            if kind == 'models':
                name = os.path.splitext(f)[0] + '.cub'
                data, warn, err = check_model(name, path, vanilla)
            elif kind == 'sounds':
                name = f; data, warn, err = check_sound(path, vanilla, name)
            elif kind == 'icons':
                name = f; data, warn, err = check_icon(path, vanilla, name)
            else:
                name = f
                if name not in vanilla:
                    print(f'  SKIP {f}: only dict_en.xml / dict_de.xml exist'); continue
                data, added, replaced = merge_dict(vanilla[name], open(path, 'rb').read())
                warn, err = [f'{len(replaced)} entries replaced, {len(added)} added'], []
                if added:
                    warn.append('added keys are only shown if game code asks for them: ' + ', '.join(added[:5]))
            status = 'ERROR' if err else ('ok' if not warn else 'warn')
            print(f'  {status:5} {name}')
            for m in err + warn: print(f'        - {m}')
            if err: total_err += 1; continue
            con.execute('INSERT OR REPLACE INTO blobs(key, value) VALUES(?, ?)', (name, encode(data)))
        con.commit(); con.close()
        # self-check: every blob we wrote must decode back to what we packed
        chk = blobs(dst)
        print(f'  -> {dst} ({len(chk)} blobs, verified readable)')
    print('\nBUILD FAILED' if total_err else '\nbuild complete')
    return 1 if total_err else 0


def install(game, out):
    for db in DBS.values():
        built = os.path.join(out, db)
        if not os.path.exists(built): continue
        bak = os.path.join(game, db + '.vanilla')
        if not os.path.exists(bak): shutil.copyfile(os.path.join(game, db), bak)
        shutil.copyfile(built, os.path.join(game, db)); print('installed', db)
    print('Copy the same data1.db next to Server.exe if you host a server - it loads models too.')


def restore(game):
    for db in DBS.values():
        bak = os.path.join(game, db + '.vanilla')
        if os.path.exists(bak): shutil.copyfile(bak, os.path.join(game, db)); print('restored', db)


if __name__ == '__main__':
    a = sys.argv
    cmds = {'extract': (extract, 4), 'build': (build, 5), 'install': (install, 4), 'restore': (restore, 3)}
    if len(a) < 2 or a[1] not in cmds or len(a) != cmds[a[1]][1]:
        print(__doc__); sys.exit(1)
    sys.exit(cmds[a[1]][0](*a[2:]) or 0)
