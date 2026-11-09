"""Function prototypes as written in mapping/: `<ret> __<conv> <name>(<type> <param>, ...)`."""
import re

PROTO = re.compile(r'^\s*(?P<ret>.+?)\s+__(?P<conv>cdecl|stdcall|thiscall|fastcall)\s+(?P<name>[^\s(]+)\s*'
                   r'\((?P<params>.*)\)\s*$')
PARAM = re.compile(r'^(?P<type>.*?[\w*>\]])\s*\b(?P<name>[A-Za-z_]\w*)$')


def split_top(s, sep=','):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '<([':
            depth += 1
        elif ch in '>)]':
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    out.append(cur)
    return [x.strip() for x in out]


def parse(text):
    """{'ret', 'conv', 'name', 'params': [[type, name], ...]}; raises ValueError."""
    m = PROTO.match(text)
    if not m:
        raise ValueError('expected "<return type> __<convention> <name>(<parameters>)"')
    params = []
    raw = m.group('params').strip()
    if raw and raw != 'void':
        for i, p in enumerate(split_top(raw)):
            pm = PARAM.match(p)
            if not pm or not pm.group('type').strip() or pm.group('type').strip() in ('const', 'unsigned'):
                raise ValueError(f'parameter {i + 1} ({p!r}) needs a type and a name')
            params.append([pm.group('type').strip(), pm.group('name')])
    return {'ret': m.group('ret').strip(), 'conv': m.group('conv'), 'name': m.group('name'), 'params': params}


def render(p):
    params = ', '.join(f'{t} {n}' for t, n in p['params']) or 'void'
    return f'{p["ret"]} __{p["conv"]} {p["name"]}({params})'


def base_type(t):
    """'const std::string*' -> 'std::string'; 'uint8_t[16]' -> 'uint8_t'."""
    t = t.replace('const ', '').strip().rstrip('*').strip()
    return re.sub(r'\[[^\]]*\]$', '', t).strip()
