"""mapping/ghidra/mappings.json: what mapping/ghidra/apply.py applies to a modder's own Ghidra project.

Per supported build: the file name, SHA-256 and image base the script checks, every exported type and enum
(enums, bases and by-value field types before the types that use them), and every exported function, label
and global, with its aliases, prototype and plate comment. It is the exported view of what `kmap push` applies
to the private project: guesses stay there. One item per line, so a change to mapping/ is a small diff.
"""
import json

from . import db as dbmod
from .export import GENERATED_NOTE
from .proto import base_type
from .roundtrip import desired

PATH = 'mapping/ghidra/mappings.json'
FORMAT = 1          # apply.py refuses a file of another format


def ordered(types):
    """Enums first, then every type after its base and its by-value field types (pointers need nothing)."""
    by_name = {t['name']: t for t in types}
    out, done = [], set()

    def visit(t, stack):
        if t['name'] in done or t['name'] in stack:
            return
        deps = [t.get('base')] + [f['type'] for f in t.get('fields', []) if '*' not in f['type']]
        for d in deps:
            if d and base_type(d) in by_name:
                visit(by_name[base_type(d)], stack | {t['name']})
        done.add(t['name'])
        out.append(t)

    for t in sorted(types, key=lambda t: (t['kind'] != 'enum', t['name'])):
        visit(t, frozenset())
    return out


def item(x):
    x = {k: v for k, v in x.items() if k != 'key'}
    if 'address' in x:
        x['address'] = f'0x{x["address"]:X}'
    return x


def build_entry(ctx, db, bid):
    b = ctx.ws.build(bid)
    want = desired(db, bid, keep=dbmod.exported)
    return {'file': b.file, 'sha256': b.sha256.lower(), 'image_base': f'0x{b.image_base:X}',
            'types': [item(t) for t in ordered(want['types'])],
            'symbols': [item(s) for s in sorted(want['symbols'], key=lambda s: (s['address'], s['kind'], s['name']))]}


def render(doc):
    """JSON with one type or symbol per line."""
    def one(v):
        return json.dumps(v, sort_keys=True, ensure_ascii=False)

    def items(key, xs, last):
        return ([f'   {one(key)}: ['] + [f'    {one(x)}' + (',' if i < len(xs) - 1 else '') for i, x in enumerate(xs)]
                + ['   ]' + ('' if last else ',')])

    lines = ['{', f' "_": {one(doc["_"])},', f' "format": {doc["format"]},', f' "ghidra": {one(doc["ghidra"])},',
             ' "builds": {']
    for i, (bid, b) in enumerate(doc['builds'].items()):
        lines.append(f'  {one(bid)}: {{')
        lines += [f'   {one(k)}: {one(b[k])},' for k in ('file', 'sha256', 'image_base')]
        lines += items('types', b['types'], False) + items('symbols', b['symbols'], True)
        lines.append('  }' + ('' if i == len(doc['builds']) - 1 else ','))
    return '\n'.join(lines + [' }', '}']) + '\n'


def generate(ctx, db):
    doc = {'_': GENERATED_NOTE + ' Applied to your own Ghidra project by apply.py (next to this file).',
           'format': FORMAT, 'ghidra': ctx.ws.ghidra.version,
           'builds': {bid: build_entry(ctx, db, bid) for bid in ctx.ws.builds}}
    return {PATH: render(doc)}
