"""kmap export, C headers: sdk/include/cubeworld/*.h for native mods (mapping-exports, "C headers").

  base.h       calling-convention and static-assertion macros
  types.h      one struct per exported type (explicit _unk_ padding, CUBEWORLD_CANDIDATE markers, static
               assertions on verified offsets and known sizes), sized enums, and opaque structs for the RTTI
               classes that prototypes point to
  functions.h  a name macro (for resolve_symbol) and a function-pointer typedef per exported function
  cubeworld.h  includes them all
  layout.h     the structs and enums as data (field names, offsets, sizes, kinds, statuses), for tools that show
               game memory by field, such as kyubu_probe's inspector; not included by cubeworld.h

No game addresses: those come only from the symbol maps, through resolve_symbol. __thiscall functions are
typed as __fastcall with an unused EDX parameter, which MinGW gcc, clang and MSVC call the same way.
Fields are laid out in offset order and padded forward only, so a field that is too big for its space
moves the next ones, and the static assertion of the next verified field fails to compile.
Old names (`aliases`) stay usable and marked deprecated (mapping-exports, "Name stability"): a typedef for a type
or enum, a union member of the same type for a field, an enumerator for an enum value, and a name macro (plus a
typedef) for a function.
"""
import re

from . import db as dbmod
from . import proto
from .export import GENERATED_NOTE
from .rules import PRIMITIVES, POINTER_SIZE, type_size
from .util import KmapError

DIR = 'sdk/include/cubeworld'
DIMS = re.compile(r'^(?P<elem>.*?)(?P<dims>(?:\[(?:0x[0-9A-Fa-f]+|\d+)\])*)$')
INT_TYPES = {(1, False): 'uint8_t', (2, False): 'uint16_t', (4, False): 'uint32_t', (8, False): 'uint64_t',
             (1, True): 'int8_t', (2, True): 'int16_t', (4, True): 'int32_t', (8, True): 'int64_t'}
CONV_MACRO = {'cdecl': 'CW_CDECL', 'stdcall': 'CW_STDCALL', 'fastcall': 'CW_FASTCALL', 'thiscall': 'CW_FASTCALL'}
KEYWORDS = frozenset('''
    alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class
    compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype
    default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline
    int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public
    register reinterpret_cast requires restrict return short signed sizeof static static_assert static_cast
    struct switch template this thread_local throw true try typedef typeid typename union unsigned using virtual
    void volatile wchar_t while xor xor_eq _Bool'''.split())

BASE_H = f'''/* cubeworld/base.h: macros shared by the generated game headers.
   {GENERATED_NOTE} */
#ifndef CUBEWORLD_BASE_H
#define CUBEWORLD_BASE_H

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#if !defined(_M_IX86) && !defined(__i386__)
#error "the cubeworld headers describe the 32-bit x86 game: compile for 32-bit x86"
#endif

/* Calling conventions. __thiscall functions are typed as CW_FASTCALL with an unused second parameter (EDX). */
#if defined(_MSC_VER)
#define CW_CDECL __cdecl
#define CW_STDCALL __stdcall
#define CW_FASTCALL __fastcall
#else
#define CW_CDECL __attribute__((__cdecl__))
#define CW_STDCALL __attribute__((__stdcall__))
#define CW_FASTCALL __attribute__((__fastcall__))
#endif

/* Compile-time layout checks; C99 has no _Static_assert, so it gets a negative array size instead. */
#if defined(__cplusplus)
#define CW_STATIC_ASSERT(cond, id, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define CW_STATIC_ASSERT(cond, id, msg) _Static_assert(cond, msg)
#else
#define CW_STATIC_ASSERT(cond, id, msg) typedef char cw_static_assert_##id[(cond) ? 1 : -1]
#endif

/* Old names. A renamed type, function, field or enum value keeps its old name for the mods that use it
   (mapping/exported-names.lock), and using the old name warns. CW_DEPRECATED starts a declaration; an enum value
   takes CW_DEPRECATED_VALUE after its name. MSVC warns about old field and enum value names only in C++. */
#if defined(__cplusplus) && (__cplusplus >= 201402L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201402L))
#define CW_DEPRECATED(msg) [[deprecated(msg)]]
#define CW_DEPRECATED_VALUE(msg) [[deprecated(msg)]]
#elif defined(__GNUC__) || defined(__clang__)
#define CW_DEPRECATED(msg) __attribute__((__deprecated__(msg)))
#define CW_DEPRECATED_VALUE(msg) __attribute__((__deprecated__(msg)))
#elif defined(_MSC_VER)
#define CW_DEPRECATED(msg) __declspec(deprecated(msg))
#define CW_DEPRECATED_VALUE(msg)
#else
#define CW_DEPRECATED(msg)
#define CW_DEPRECATED_VALUE(msg)
#endif

/* A renamed field keeps its old name as a second member of an unnamed union, which is standard in C11 and C++.
   A struct that has one is wrapped in CW_ALIASES_BEGIN and CW_ALIASES_END, so C99 accepts it too. */
#if defined(__cplusplus)
#define CW_ALIASES_BEGIN
#define CW_ALIASES_END
#elif defined(__GNUC__) || defined(__clang__)
#define CW_ALIASES_BEGIN __extension__
#define CW_ALIASES_END
#elif defined(_MSC_VER)
#define CW_ALIASES_BEGIN __pragma(warning(push)) __pragma(warning(disable: 4201))
#define CW_ALIASES_END __pragma(warning(pop))
#else
#define CW_ALIASES_BEGIN
#define CW_ALIASES_END
#endif

#endif
'''

CUBEWORLD_H = f'''/* cubeworld/cubeworld.h: every generated game header.
   {GENERATED_NOTE} */
#ifndef CUBEWORLD_CUBEWORLD_H
#define CUBEWORLD_CUBEWORLD_H

#include "base.h"
#include "types.h"
#include "functions.h"

#endif
'''


def ident(name):
    """C identifier for a mapped name: cube::Database::get -> cube_Database_get; constructors end in _ctor and
    destructors in _dtor (cube::Database::~Database -> cube_Database_dtor)."""
    parts = name.split('::')
    if len(parts) > 1:
        if parts[-1].startswith('~'):
            parts[-1] = 'dtor'
        elif parts[-1] == re.sub(r'<.*', '', parts[-2]):
            parts[-1] = 'ctor'
    return re.sub(r'[^A-Za-z0-9_]+', '_', '_'.join(parts)).strip('_')


def member(name):
    n = re.sub(r'[^A-Za-z0-9_]', '_', name)
    return n + '_' if n in KEYWORDS else n


def c_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def pad(offset, n):
    return (offset, n, 1, f'uint8_t _unk_{offset:x}[0x{n:X}];', None)


def aliases(e):
    """The old names an entry keeps (its `aliases`), in mapping order."""
    return [a for a in e.get('aliases', []) if isinstance(a, str)]


def deprecated(new):
    return f'CW_DEPRECATED({c_string(f"use {new}")})'


class Headers:
    def __init__(self, ctx, db):
        self.db = db
        self.builds = list(ctx.ws.targets)      # the headers describe the builds the loader runs on
        self.label = {b: ctx.ws.build(b).file for b in self.builds}
        self.types = {t['name']: t for t in db.types if dbmod.exported(t)}
        self.enums = {e['name']: e for e in db.enums if dbmod.exported(e)}
        self.classes = {c['name'] for b in self.builds for c in db.classes(b)
                        if dbmod.exported(c) and re.fullmatch(r'[A-Za-z_]\w*(::[A-Za-z_]\w*)*', c['name'])}
        self.opaque = set()
        self.align = {}
        self.declared = {}   # type name -> [(member, offset, field)] of the fields the struct declares
        self.function_aliases = {}   # old function name -> [the exported functions that keep it] (functions())

    # --- types ---------------------------------------------------------------------------------------------
    def lookup(self, name):
        """The exported type or enum a name (or alias) refers to, or None."""
        t = self.db.find_type(name)
        if t is not None and t['name'] in self.types:
            return t
        e = self.db.find_enum(name)
        if e is not None and e['name'] in self.enums:
            return e
        return None

    def named(self, name, by_value):
        """C spelling of a type name without pointers, or None when C can't use it here."""
        if name in PRIMITIVES:
            return name if name != 'void' or not by_value else None
        t = self.lookup(name)
        if t is not None:
            if by_value and t['_kind'] == 'type' and not t.get('size'):
                return None
            return ident(t['name'])
        if name in self.classes and not by_value:
            self.opaque.add(name)
            return ident(name)
        return None

    def c_type(self, t, ret=False):
        """(C spelling, exact) of a prototype type; pointers to unknown types become void*. None if impossible."""
        t = t.strip()
        const = t.startswith('const ')
        if const:
            t = t[6:].strip()
        depth = 0
        while t.endswith('*'):
            depth += 1
            t = t[:-1].strip()
        base, exact = self.named(t, by_value=not depth and not (ret and t == 'void')), True
        if base is None:
            if not depth:
                return None
            base, exact = 'void', False
        return ('const ' if const else '') + base + '*' * depth, exact

    def field_decl(self, f, name=None):
        """(declaration, size, alignment, exact) of a field (or of its old `name`), or None when C can't express it."""
        m = DIMS.match(f['type'].strip())
        elem, dims = m.group('elem').strip(), [int(d, 0) for d in re.findall(r'\[(\w+)\]', m.group('dims'))]
        ct = self.c_type(elem)
        size = type_size(self.db, elem)
        if ct is None or not size:
            return None
        if elem.endswith('*'):
            align = POINTER_SIZE
        elif elem in PRIMITIVES:
            align = PRIMITIVES[elem]
        else:
            t = self.lookup(elem)
            align = self.align.get(t['name'], 1) if t['_kind'] == 'type' else t['size']
        n = 1
        for d in dims:
            n *= d
        return f'{ct[0]} {member(name or f["name"])}' + ''.join(f'[{d}]' for d in dims), size * n, align, ct[1]

    def value_deps(self, ty):
        deps = set()
        names = [ty.get('base')] if ty.get('base') else []
        names += [DIMS.match(f['type'].strip()).group('elem').strip() for f in ty.get('field', [])
                  if dbmod.exported(f) and isinstance(f.get('type'), str)]
        for n in names:
            if not n.endswith('*'):
                t = self.lookup(n)
                if t is not None and t['_kind'] == 'type' and t['name'] != ty['name']:
                    deps.add(t['name'])
        return deps

    def ordered_types(self):
        order, state = [], {}

        def visit(name, stack):
            if state.get(name) == 2:
                return
            if state.get(name) == 1:
                raise KmapError(f'types contain each other by value: {" -> ".join(stack + [name])}')
            state[name] = 1
            for dep in sorted(self.value_deps(self.types[name])):
                visit(dep, stack + [name])
            state[name] = 2
            order.append(name)

        for name in sorted(self.types):
            visit(name, [])
        return order

    def struct(self, ty):
        """Lines of the definition (empty when there is nothing to define) and the static assertions."""
        name, cname, size = ty['name'], ident(ty['name']), ty.get('size') or 0
        union = ty.get('kind') == 'union'
        members, asserts, cur = [], [], 0    # members: (offset, size, alignment, declaration, comment)
        declared = self.declared[name] = []
        renamed = False
        base = self.lookup(ty['base']) if ty.get('base') else None
        if base is not None and base['_kind'] == 'type' and base.get('size') and not union:
            members.append((0, base['size'], self.align.get(base['name'], 1), f'{ident(base["name"])} _base;',
                            f'base {base["name"]}'))
            cur = base['size']
        for f in sorted(ty.get('field', []), key=lambda f: (f.get('offset', 0), f.get('name', ''))):
            if not dbmod.exported(f):
                continue
            off = f['offset']
            only = [b for b in self.builds if b in (f.get('builds') or self.builds)]
            d = None if only != self.builds or (union and off) else self.field_decl(f)
            if d is None:
                why = (', '.join(self.label[b] for b in only) + ' only' if only != self.builds else
                       'not at offset 0 of a union' if union and off else 'no C type yet')
                members.append((None, 0, 1, None, f'0x{off:X} {f["name"]}: {f["type"]} ({why})'))
                continue
            decl, fsize, falign, exact = d
            if not union and off > cur:
                members.append(pad(cur, off - cur))
                cur = off
            marker = ' CUBEWORLD_CANDIDATE' if f['status'] == 'candidate' else ''
            comment = f'0x{off:X}{marker}' + ('' if exact else f' ({f["type"]})')
            old = aliases(f)
            if old:              # a renamed field: its old names share its bytes in an unnamed union
                renamed = True
                decl = 'union { ' + ' '.join([decl + ';'] + [f'{deprecated(member(f["name"]))} '
                                                             f'{self.field_decl(f, a)[0]};' for a in old]) + ' }'
                comment += f'; old name{"s" if len(old) > 1 else ""} ' + ', '.join(old)
            members.append((cur, fsize, falign, decl + ';', comment))
            declared.append((member(f['name']), off, f))
            if f['status'] == 'verified':
                field, msg = member(f['name']), c_string(f'{name}.{f["name"]} is at 0x{off:X}')
                asserts.append(f'CW_STATIC_ASSERT(offsetof({cname}, {field}) == 0x{off:X}, {cname}_{field}, {msg});')
            if not union:
                cur += fsize
        if size and (union or size > cur):
            members.append(pad(0 if union else cur, size if union else size - cur))
        real = [m for m in members if m[3]]
        if not real:
            self.align[name] = 1
            self.declared[name] = []
            return [], []
        pos, align, packed = 0, 1, False
        for off, msize, malign, _, _ in real:
            align = max(align, malign)
            if not union:
                packed |= pos % malign != 0
                pos += msize
        packed |= bool(size) and size % align != 0
        self.align[name] = 1 if packed else align
        if size:
            asserts.append(f'CW_STATIC_ASSERT(sizeof({cname}) == 0x{size:X}, {cname}_sizeof, '
                           f'{c_string(f"{name} is 0x{size:X} bytes")});')
        head = f'/* {name}' + (' CUBEWORLD_CANDIDATE' if ty['status'] == 'candidate' else '') + \
               (f' (from {ty["source"]})' if ty.get('source') else '') + ' */'
        lines = [head]
        if packed:
            lines += ['/* packed: a mapped offset is not naturally aligned */', '#pragma pack(push, 1)']
        lines.append(('CW_ALIASES_BEGIN ' if renamed else '') + f'{"union" if union else "struct"} {cname} {{')
        for _, _, _, decl, comment in members:
            lines.append(f'    {decl:<40} /* {comment} */' if decl and comment else f'    {decl}' if decl
                         else f'    /* {comment} */')
        lines.append('};')
        if renamed:
            lines.append('CW_ALIASES_END')
        if packed:
            lines.append('#pragma pack(pop)')
        return lines, asserts

    @staticmethod
    def enum_values(en):
        return [v for v in en.get('value', []) if v.get('status', en['status']) in ('verified', 'candidate')]

    def enum_typedef(self, en):
        signed = any(v['value'] < 0 for v in self.enum_values(en))
        return f'typedef {INT_TYPES.get((en["size"], signed), "int32_t" if signed else "uint32_t")} {ident(en["name"])};'

    def enum(self, en):
        cname = ident(en['name'])
        values = self.enum_values(en)
        lines = [f'/* {en["name"]}' + (' CUBEWORLD_CANDIDATE' if en['status'] == 'candidate' else '') + ' */',
                 self.enum_typedef(en)]
        inside = [v for v in values if -2 ** 31 <= v['value'] < 2 ** 31]
        mark = lambda v: ' /* CUBEWORLD_CANDIDATE */' if v.get('status', en['status']) != en['status'] else ''
        if inside:
            lines.append('enum {')
            for v in inside:
                new = f'{cname}_{member(v["name"])}'
                lines.append(f'    {new} = {v["value"]},{mark(v)}')
                lines += [f'    {cname}_{member(a)} CW_DEPRECATED_VALUE({c_string(f"use {new}")}) = {v["value"]},'
                          for a in aliases(v)]
            lines.append('};')
        for v in values:
            if v not in inside:
                new = f'{cname}_{member(v["name"])}'
                lines.append(f'#define {new} (({cname})0x{v["value"]:X}u){mark(v)}')
                lines += [f'#define {cname}_{member(a)} {new} /* old name: use {new} */' for a in aliases(v)]
        return lines + self.alias_typedefs(en)

    def alias_typedefs(self, t):
        """The old names of a renamed type or enum, as deprecated typedefs."""
        return [f'{deprecated(ident(t["name"]))} typedef {ident(t["name"])} {ident(a)};' for a in aliases(t)]

    def definitions(self):
        """(forward declarations, [(enum, lines)], [(type, lines, assertions)]) in declaration order. Shared with
        the Lua exporter; call it once per Headers, after the functions (which collect the opaque classes)."""
        enums = [(self.enums[name], self.enum(self.enums[name])) for name in sorted(self.enums)]
        structs, decls = [], {}
        for name in self.ordered_types():
            ty = self.types[name]
            decls[ident(name)] = 'union' if ty.get('kind') == 'union' else 'struct'
            lines, asserts = self.struct(ty)
            if lines:
                structs.append((ty, lines, asserts))
        for name in self.opaque - set(self.types):
            decls[ident(name)] = 'struct'
        known = {}
        for name in list(self.types) + list(self.enums) + list(self.opaque):
            if known.setdefault(ident(name), name) != name:
                raise KmapError(f'{known[ident(name)]} and {name} both become the C name {ident(name)}')
        for t in list(self.types.values()) + list(self.enums.values()):
            for a in aliases(t):
                if known.setdefault(ident(a), a) != a:
                    raise KmapError(f'{a}, an old name of {t["name"]}, and {known[ident(a)]} both become the C name '
                                    f'{ident(a)}')
        return [f'typedef {kind} {c} {c};' for c, kind in sorted(decls.items())], enums, structs

    def types_h(self):
        fwd, enums, structs = self.defs = self.definitions()
        defined = {ty['name'] for ty, _, _ in structs}
        body = [line for name in sorted(self.types) if name not in defined for line in self.alias_typedefs(self.types[name])]
        body += [''] if body else []
        for _, lines in enums:
            body += lines + ['']
        for ty, lines, asserts in structs:
            body += lines + asserts + self.alias_typedefs(ty) + ['']
        return '\n'.join([
            '/* cubeworld/types.h: the game\'s types. Fields marked CUBEWORLD_CANDIDATE are not confirmed yet;',
            '   unknown ranges are _unk_<offset> padding. Classes without a mapped layout are opaque (pointers only).',
            '   Renamed types, fields and enum values keep their old names, marked deprecated.',
            f'   {GENERATED_NOTE} */',
            '#ifndef CUBEWORLD_TYPES_H', '#define CUBEWORLD_TYPES_H', '', '#include "base.h"', ''] +
            fwd + [''] + body + ['#endif', ''])

    # --- functions ---------------------------------------------------------------------------------------------
    def typedef(self, fid, conv, text):
        """(typedef line, None) or (None, why there is none)."""
        if conv not in CONV_MACRO:
            return None, f'__{conv}: no C calling convention'
        try:
            p = proto.parse(text)
        except ValueError as e:
            return None, f'prototype not understood: {e}'
        ret = self.c_type(p['ret'], ret=True)
        if ret is None:
            return None, f'returns {p["ret"]} by value'
        params = []
        for t, n in p['params']:
            ct = self.c_type(t)
            if ct is None:
                return None, f'takes {t} by value'
            params.append((ct[0], 'self' if n == 'this' else member(n)))
        if conv == 'thiscall':
            if not params:
                return None, 'no this parameter'
            params.insert(1, ('void*', 'edx' if 'edx' not in [n for _, n in params] else '_edx'))
        plist = ', '.join(f'{t} {n}' for t, n in params) or 'void'
        return f'typedef {ret[0]} ({CONV_MACRO[conv]} *cw_{fid}_fn)({plist});', None

    def functions(self):
        """{subsystem: [(name, C id, builds text, prototype or None, typedef or None, why there is none)]}.
        Shared with the Lua exporter; collects the opaque classes that the prototypes point to."""
        by_name = {}
        for b in self.builds:
            for e in self.db.functions(b):
                if dbmod.exported(e):
                    by_name.setdefault(e['name'], []).append((b, e))
        sections, ids = {}, {}
        for name in sorted(by_name):
            ents = by_name[name]
            fid = ident(name)
            if ids.setdefault(fid, name) != name:
                raise KmapError(f'{ids[fid]} and {name} both become the C name {fid}')
            shapes = {(e['convention'], e.get('prototype')) for _, e in ents if e.get('prototype')}
            if len(shapes) > 1:
                raise KmapError(f'{name} has different prototypes in the builds: ' +
                                '; '.join(f'{c}: {p}' for c, p in sorted(shapes)))
            statuses = {e['status'] for _, e in ents}
            if len(statuses) == 1:
                where = ', '.join(self.label[b] for b, _ in ents) + ('; CUBEWORLD_CANDIDATE' if 'candidate' in statuses
                                                                     else '')
            else:
                where = ', '.join(f'{self.label[b]} ' + ('CUBEWORLD_CANDIDATE' if e['status'] == 'candidate'
                                                         else e['status']) for b, e in ents)
            text = td = why = None
            if shapes:
                conv, text = shapes.pop()
                td, why = self.typedef(fid, conv, text)
            sections.setdefault(ents[0][1]['subsystem'], []).append((name, fid, where, text, td, why))
            for _, e in ents:
                for a in aliases(e):
                    owners = self.function_aliases.setdefault(a, [])
                    if name not in owners:
                        owners.append(name)
        for a in list(self.function_aliases):
            if a in by_name:     # still the name of a function in the other build: declared with that one
                del self.function_aliases[a]
            elif ids.setdefault(ident(a), a) != a:
                raise KmapError(f'{a}, an old name of {" and ".join(self.function_aliases[a])}, and {ids[ident(a)]} '
                                f'both become the C name {ident(a)}')
        return sections

    def alias_lines(self, name, fid, td):
        """The deprecated old names of function `name`: a name macro each, and a typedef when the old name belongs
        to this function alone. An old name kept by several (one per build) goes with the first of them."""
        out = []
        for a, owners in self.function_aliases.items():
            if owners[0] != name:
                continue
            aid, use = ident(a), ' or '.join(f'CW_SYM_{ident(o)}' for o in owners)
            out.append(f'#define CW_SYM_{aid} {c_string(a)} /* old name: use {use} */')
            if td and len(owners) == 1:
                out.append(f'{deprecated(f"cw_{fid}_fn")} typedef cw_{fid}_fn cw_{aid}_fn;')
        return out

    def functions_h(self):
        sections = self.functions()
        body = []
        for sub in sorted(sections):
            body += [f'/* --- {sub} --- */', '']
            for name, fid, where, text, td, why in sections[sub]:
                macro = f'#define CW_SYM_{fid} {c_string(name)}'
                if text:
                    lines = [f'/* {text}', f'   {where}' + (f'; {why}' if why else '') + ' */', macro] + \
                            ([td] if td else [])
                else:
                    lines = [f'{macro} /* {where}; no prototype yet */']
                lines += self.alias_lines(name, fid, td)
                if len(lines) > 1 and body[-1] != '':
                    body.append('')
                body += lines + ([''] if len(lines) > 1 else [])
            if body[-1] != '':
                body.append('')
        return '\n'.join([
            '/* cubeworld/functions.h: game functions. Pass CW_SYM_<name> to resolve_symbol (or hook) and use the',
            '   address as cw_<name>_fn. __thiscall functions are typed as __fastcall with an unused second',
            '   parameter (EDX): pass 0 when calling, and pass it on unchanged in a hook. A renamed function keeps its',
            '   old name, which resolves to the same address: its macro says what to use instead, and its typedef is',
            '   marked deprecated.',
            f'   {GENERATED_NOTE} */',
            '#ifndef CUBEWORLD_FUNCTIONS_H', '#define CUBEWORLD_FUNCTIONS_H', '', '#include "types.h"', ''] +
            body + ['#endif', ''])


    # --- layout ----------------------------------------------------------------------------------------------
    def field_layout(self, cname, mem, f):
        """The CwLayoutField initializer of a declared field. A base struct is listed as a field named _base."""
        m = DIMS.match(f['type'].strip())
        elem, dims = m.group('elem').strip(), [int(d, 0) for d in re.findall(r'\[(\w+)\]', m.group('dims'))]
        count = 1
        for d in dims:
            count *= d
        target = 'NULL'
        if elem.endswith('*'):
            kind = 'CW_KIND_POINTER'
            t = self.lookup(elem[:-1].strip())
            if t is not None and t['_kind'] == 'type' and t['name'] in self.laid_out:
                target = c_string(ident(t['name']))
        elif elem in PRIMITIVES:
            kind = ('CW_KIND_FLOAT' if elem in ('float', 'double') else 'CW_KIND_BOOL' if elem == 'bool' else
                    'CW_KIND_INT' if elem == 'char' or elem.startswith('int') else 'CW_KIND_UINT')
        else:
            t = self.lookup(elem)
            kind = 'CW_KIND_ENUM' if t['_kind'] == 'enum' else 'CW_KIND_STRUCT'
            target = c_string(ident(t['name']))
            if kind == 'CW_KIND_ENUM' and any(v['value'] < 0 for v in self.enum_values(t)):
                kind = 'CW_KIND_SIGNED_ENUM'
        return (f'{{{c_string(f["name"])}, {c_string(f["type"])}, (uint32_t)offsetof({cname}, {mem}), '
                f'(uint32_t)sizeof((({cname}*)0)->{mem}), {count}, {kind}, {int(f["status"] == "verified")}, '
                f'{target}}}')

    def layout_h(self):
        """cubeworld/layout.h: every struct of types.h and every enum as data, for tools that show game memory
        (kyubu_probe's inspector). Offsets and sizes are taken from the structs (offsetof, sizeof), so the
        description can't disagree with types.h. Call after types_h."""
        _, enums, structs = self.defs
        self.laid_out = {ty['name'] for ty, _, _ in structs}
        body = []
        listed = [en for en, _ in enums if self.enum_values(en)]
        for en in listed:
            body.append(f'static const CwLayoutValue cw_layout_values_{ident(en["name"])}[] = {{')
            body += [f'    {{{c_string(v["name"])}, {v["value"]}}},' for v in self.enum_values(en)]
            body += ['};', '']
        tables = []
        for ty, _, _ in structs:
            name, cname = ty['name'], ident(ty['name'])
            fields = [self.field_layout(cname, mem, f) for mem, _, f in self.declared.get(name, [])]
            base = self.lookup(ty['base']) if ty.get('base') else None
            if base is not None and base['name'] in self.laid_out and ty.get('kind') != 'union':
                fields.insert(0, f'{{"_base", {c_string(base["name"])}, (uint32_t)offsetof({cname}, _base), '
                                 f'(uint32_t)sizeof((({cname}*)0)->_base), 1, CW_KIND_STRUCT, '
                                 f'{int(ty["status"] == "verified")}, {c_string(ident(base["name"]))}}}')
            if fields:
                body.append(f'static const CwLayoutField cw_layout_fields_{cname}[] = {{')
                body += [f'    {f},' for f in fields]
                body += ['};', '']
            tables.append(f'    {{{c_string(name)}, {c_string(cname)}, (uint32_t)sizeof({cname}), '
                          f'{int(ty["status"] == "verified")}, {len(fields or [])}, '
                          f'{f"cw_layout_fields_{cname}" if fields else "NULL"}}},')
        body += ['static const CwLayoutType cw_layout_types[] = {'] + \
                (tables or ['    {"", "", 0, 0, 0, NULL},']) + ['};', '']
        body.append('static const CwLayoutEnum cw_layout_enums[] = {')
        body += [f'    {{{c_string(en["name"])}, {c_string(ident(en["name"]))}, {en["size"]}, '
                 f'{int(en["status"] == "verified")}, {len(self.enum_values(en))}, '
                 f'cw_layout_values_{ident(en["name"])}}},' for en in listed] or \
                ['    {"", "", 0, 0, 0, NULL},']
        body += ['};', '']
        return '\n'.join([
            '/* cubeworld/layout.h: the structs of types.h and the enums, described as data for tools that show game',
            '   memory by field (kyubu_probe\'s inspector). Offsets and sizes come from the structs themselves, so',
            '   they always agree with types.h. Fields that types.h can\'t declare (no C type yet) are not listed.',
            '   The tables are static: include this header in one source file.',
            f'   {GENERATED_NOTE} */',
            '#ifndef CUBEWORLD_LAYOUT_H', '#define CUBEWORLD_LAYOUT_H', '', '#include "types.h"', '',
            '/* CwLayoutField.kind: what one element of the field is. */',
            '#define CW_KIND_INT 1          /* signed integer (char too) */',
            '#define CW_KIND_UINT 2         /* unsigned integer (wchar_t too) */',
            '#define CW_KIND_FLOAT 3        /* float or double */',
            '#define CW_KIND_BOOL 4',
            '#define CW_KIND_POINTER 5      /* target: the C name of the pointed-to struct when it is described here */',
            '#define CW_KIND_STRUCT 6       /* target: the C name of the struct */',
            '#define CW_KIND_ENUM 7         /* target: the C name of the enum (unsigned values) */',
            '#define CW_KIND_SIGNED_ENUM 8  /* target: the C name of the enum (it has negative values) */',
            '',
            'typedef struct CwLayoutField {',
            '    const char* name;      /* the mapped name */',
            '    const char* type;      /* the mapped type, e.g. "cube::ItemUpgrade[32]" */',
            '    uint32_t offset;',
            '    uint32_t size;         /* of the whole field */',
            '    uint32_t count;        /* elements: 1 unless it is an array */',
            '    int kind;              /* CW_KIND_* of one element */',
            '    int verified;          /* 1 verified, 0 candidate (CUBEWORLD_CANDIDATE) */',
            '    const char* target;    /* see CW_KIND_*; NULL otherwise */',
            '} CwLayoutField;',
            '',
            'typedef struct CwLayoutType {',
            '    const char* name;      /* the mapped name, e.g. "cube::CreatureData" */',
            '    const char* c_name;    /* e.g. "cube_CreatureData" */',
            '    uint32_t size;',
            '    int verified;',
            '    uint32_t field_count;',
            '    const CwLayoutField* fields;   /* in offset order; NULL when there are none */',
            '} CwLayoutType;',
            '',
            'typedef struct CwLayoutValue {',
            '    const char* name;',
            '    int64_t value;',
            '} CwLayoutValue;',
            '',
            'typedef struct CwLayoutEnum {',
            '    const char* name;',
            '    const char* c_name;',
            '    uint32_t size;',
            '    int verified;          /* 1 verified, 0 candidate: its values are not vouched for */',
            '    uint32_t value_count;',
            '    const CwLayoutValue* values;',
            '} CwLayoutEnum;',
            ''] + body + [
            f'#define CW_LAYOUT_TYPE_COUNT {len(tables)}',
            f'#define CW_LAYOUT_ENUM_COUNT {len(listed)}',
            '', '#endif', ''])


def generate(ctx, db):
    h = Headers(ctx, db)
    functions = h.functions_h()      # first: it collects the opaque classes that types.h declares
    types = h.types_h()
    return {f'{DIR}/base.h': BASE_H, f'{DIR}/types.h': types, f'{DIR}/functions.h': functions,
            f'{DIR}/cubeworld.h': CUBEWORLD_H, f'{DIR}/layout.h': h.layout_h()}
