"""Small helpers shared by every kmap module: errors, hashing, deterministic JSON and TOML output."""
import hashlib
import json
import os
import re
import sys
from pathlib import Path


class KmapError(Exception):
    """A user-facing failure: printed without a traceback, exit code 1."""


def say(*parts):
    print(*parts, flush=True)


def warn(*parts):
    print(*parts, file=sys.stderr, flush=True)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def write_if_changed(path, data):
    """Writes text or bytes only when the content differs; returns True when the file changed."""
    path = Path(path)
    raw = data.encode('utf-8') if isinstance(data, str) else data
    if path.is_file() and path.read_bytes() == raw:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_bytes(raw)
    os.replace(tmp, path)
    return True


def dump_json(obj):
    """Deterministic JSON: sorted keys, fixed separators, trailing newline (survey and project records)."""
    return json.dumps(obj, sort_keys=True, indent=1, ensure_ascii=False) + '\n'


# --- TOML output ------------------------------------------------------------------------------------
# Only what the mapping files use: strings, ints (hex where the key is an address/offset/size), bools,
# floats, lists of scalars, and arrays of tables nested one or two levels.

HEX_KEYS = {'address', 'offset', 'size', 'vtable', 'image_base', 'image_size', 'text_start', 'text_end',
            'timestamp', 'type_descriptor', 'value_hex'}
_BARE_KEY = re.compile(r'^[A-Za-z0-9_-]+$')


def toml_str(s):
    out = ['"']
    for ch in s:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append(f'\\u{ord(ch):04X}')
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


def toml_key(k):
    return k if _BARE_KEY.match(k) else toml_str(k)


def toml_value(key, v):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, int):
        if key in HEX_KEYS and v >= 0:
            return f'0x{v:X}'
        return str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, str):
        return toml_str(v)
    if isinstance(v, (list, tuple)):
        items = [toml_value(key, x) for x in v]
        one = '[' + ', '.join(items) + ']'
        if len(one) <= 100 or not items:
            return one
        return '[\n' + ''.join(f'    {x},\n' for x in items) + ']'
    if isinstance(v, dict):
        return '{ ' + ', '.join(f'{toml_key(k)} = {toml_value(k, x)}' for k, x in v.items()) + ' }'
    raise TypeError(f'cannot write {type(v).__name__} as TOML ({key})')


def toml_table(header, d, order=()):
    """One [[header]] table; keys in `order` first, then the rest sorted. Sub-lists of dicts become
    [[header.key]] tables after the scalar keys."""
    lines = [f'[[{header}]]']
    keys = [k for k in order if k in d] + sorted(k for k in d if k not in order)
    subs = []
    for k in keys:
        v = d[k]
        if v is None:
            continue
        if isinstance(v, list) and v and all(isinstance(x, dict) for x in v):
            subs.append(k)
            continue
        lines.append(f'{toml_key(k)} = {toml_value(k, v)}')
    for k in subs:
        for item in d[k]:
            lines.append('')
            lines.extend(toml_table(f'{header}.{k}', item, order).splitlines())
    return '\n'.join(lines)
