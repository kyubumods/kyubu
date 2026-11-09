#!/usr/bin/env python3
"""cubvox - convert Cube World .cub models <-> MagicaVoxel .vox, and render previews.

.cub format (verified against all 2,857 models in data1.db):
  int32 sx, int32 sy, int32 sz            (little-endian)
  sx*sy*sz * (R,G,B) bytes, X fastest, then Y, then Z (Z is up)
  RGB (0,0,0) = empty voxel

Every shipped model uses <= 57 colours, so .vox (255-colour palette) is lossless.
Note: pure black (0,0,0) cannot be a solid colour in .cub; the converter nudges it to (1,1,1).

Usage:
  cubvox.py cub2vox in.cub out.vox
  cubvox.py vox2cub in.vox out.cub
  cubvox.py preview in.cub out.png [scale]
"""
import struct, sys


def read_cub(path):
    d = open(path, 'rb').read()
    sx, sy, sz = struct.unpack('<3i', d[:12])
    if len(d) != 12 + sx * sy * sz * 3:
        raise ValueError(f'{path}: size mismatch')
    vox = {}
    i = 12
    for z in range(sz):
        for y in range(sy):
            for x in range(sx):
                c = d[i:i + 3]; i += 3
                if c != b'\0\0\0':
                    vox[(x, y, z)] = tuple(c)
    return (sx, sy, sz), vox


def write_cub(path, size, vox):
    sx, sy, sz = size
    out = bytearray(struct.pack('<3i', sx, sy, sz))
    for z in range(sz):
        for y in range(sy):
            for x in range(sx):
                c = vox.get((x, y, z))
                if c is None:
                    out += b'\0\0\0'
                else:
                    if c == (0, 0, 0): c = (1, 1, 1)
                    out += bytes(c)
    open(path, 'wb').write(out)


def _chunk(cid, content, children=b''):
    return cid + struct.pack('<ii', len(content), len(children)) + content + children


def write_vox(path, size, vox):
    sx, sy, sz = size
    if max(size) > 256: raise ValueError('.vox models are limited to 256 per axis')
    palette = sorted(set(vox.values()))
    if len(palette) > 255: raise ValueError(f'{len(palette)} colours; .vox allows 255')
    idx = {c: i + 1 for i, c in enumerate(palette)}
    xyzi = struct.pack('<i', len(vox)) + b''.join(
        struct.pack('<4B', x, y, z, idx[c]) for (x, y, z), c in sorted(vox.items()))
    rgba = b''.join(struct.pack('<4B', *c, 255) for c in palette)
    rgba += b'\0\0\0\xff' * (256 - len(palette))
    children = (_chunk(b'SIZE', struct.pack('<3i', sx, sy, sz)) +
                _chunk(b'XYZI', xyzi) + _chunk(b'RGBA', rgba))
    open(path, 'wb').write(b'VOX ' + struct.pack('<i', 150) + _chunk(b'MAIN', b'', children))


def read_vox(path):
    d = open(path, 'rb').read()
    if d[:4] != b'VOX ': raise ValueError('not a .vox file')
    pos = 8 + 12  # skip header + MAIN chunk header
    size = None; voxels = []; palette = None
    while pos < len(d):
        cid = d[pos:pos + 4]; n, m = struct.unpack('<ii', d[pos + 4:pos + 12])
        body = d[pos + 12:pos + 12 + n]
        if cid == b'SIZE' and size is None:
            size = struct.unpack('<3i', body)
        elif cid == b'XYZI' and not voxels:
            cnt = struct.unpack('<i', body[:4])[0]
            voxels = [struct.unpack('<4B', body[4 + 4 * k:8 + 4 * k]) for k in range(cnt)]
        elif cid == b'RGBA':
            palette = [tuple(body[4 * k:4 * k + 3]) for k in range(256)]
        pos += 12 + n  # children of non-MAIN chunks are skipped (only first model is used)
    if palette is None: raise ValueError('vox has no RGBA chunk (default palette not supported)')
    vox = {(x, y, z): palette[c - 1] for x, y, z, c in voxels}
    return tuple(size), vox


def preview(path, out, scale=8):
    """Isometric-ish preview: top view + front view side by side."""
    from PIL import Image
    (sx, sy, sz), vox = read_cub(path)
    top = Image.new('RGB', (sx, sy), (40, 40, 48)); front = Image.new('RGB', (sx, sz), (40, 40, 48))
    for (x, y, z), c in sorted(vox.items(), key=lambda kv: kv[0][2]):
        top.putpixel((x, sy - 1 - y), c)
    for (x, y, z), c in sorted(vox.items(), key=lambda kv: -kv[0][1]):
        front.putpixel((x, sz - 1 - z), c)
    W = (sx * 2 + 1) * scale; H = max(sy, sz) * scale
    img = Image.new('RGB', (W, H), (40, 40, 48))
    img.paste(top.resize((sx * scale, sy * scale), Image.NEAREST), (0, 0))
    img.paste(front.resize((sx * scale, sz * scale), Image.NEAREST), ((sx + 1) * scale, 0))
    img.save(out)


if __name__ == '__main__':
    a = sys.argv
    if len(a) < 4: print(__doc__); sys.exit(1)
    if a[1] == 'cub2vox': write_vox(a[3], *read_cub(a[2]))
    elif a[1] == 'vox2cub': write_cub(a[3], *read_vox(a[2]))
    elif a[1] == 'preview': preview(a[2], a[3], int(a[4]) if len(a) > 4 else 8)
    else: print(__doc__); sys.exit(1)
