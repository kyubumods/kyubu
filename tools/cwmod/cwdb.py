#!/usr/bin/env python3
"""cwdb - read/write Cube World Alpha asset databases (data1-4.db).

Each dataN.db is SQLite with one table: blobs(key TEXT PRIMARY KEY, value BLOB).
Every value is obfuscated. Algorithm reversed from Cube.exe (function @0x449c20,
key table @0x6fea68):

  decode: for i = n-1 .. 0: swap(d[i], d[(KEY[i % 44] + i) % n]);  then d[i] = 0xFF - d[i]
  encode: the exact inverse (invert bytes, then swap for i = 0 .. n-1)

Usage:
  cwdb.py list   <db>
  cwdb.py unpack <db> <out_dir>
  cwdb.py pack   <db> <in_dir> [--only NAME ...]   (insert/replace files from in_dir into db)
  cwdb.py verify <db>                               (decode->encode round-trip on every blob)
"""
import os, sqlite3, sys

KEY = [4242, 9551, 840, 84800, 9242, 9846, 127, 9, 9483, 394, 123, 4834, 32444,
       24355, 2433, 17, 34234, 42342, 4243, 14, 184934, 1987, 3094, 1901, 89409,
       4813, 37, 143, 3490, 19483, 1343, 432, 84732, 9184, 9612, 1233, 3434, 1839,
       2984, 1993, 2984, 4895, 816583, 13]


def decode(data: bytes) -> bytes:
    d = bytearray(data)
    n = len(d)
    for i in range(n - 1, -1, -1):
        j = (KEY[i % 44] + i) % n
        d[i], d[j] = d[j], d[i]
    return bytes(0xFF - b for b in d)


def encode(data: bytes) -> bytes:
    d = bytearray(0xFF - b for b in data)
    n = len(d)
    for i in range(n):
        j = (KEY[i % 44] + i) % n
        d[i], d[j] = d[j], d[i]
    return bytes(d)


def main(argv):
    if len(argv) < 3:
        print(__doc__); return 1
    cmd, db = argv[1], argv[2]
    con = sqlite3.connect(db)
    if cmd == 'list':
        for k, n in con.execute('SELECT key, length(value) FROM blobs ORDER BY key'):
            print(f'{n:>10}  {k}')
    elif cmd == 'unpack':
        out = argv[3]; os.makedirs(out, exist_ok=True)
        c = 0
        for k, v in con.execute('SELECT key, value FROM blobs'):
            with open(os.path.join(out, k), 'wb') as f:
                f.write(decode(v)); c += 1
        print(f'unpacked {c} files -> {out}')
    elif cmd == 'pack':
        src = argv[3]
        only = argv[argv.index('--only') + 1:] if '--only' in argv else None
        names = only or sorted(os.listdir(src))
        for name in names:
            raw = open(os.path.join(src, name), 'rb').read()
            con.execute('INSERT OR REPLACE INTO blobs(key, value) VALUES(?, ?)',
                        (name, encode(raw)))
            print('packed', name, len(raw))
        con.commit()
    elif cmd == 'verify':
        bad = 0
        for k, v in con.execute('SELECT key, value FROM blobs'):
            if encode(decode(v)) != v: bad += 1; print('MISMATCH', k)
        print('round-trip OK' if not bad else f'{bad} mismatches')
    else:
        print(__doc__); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
