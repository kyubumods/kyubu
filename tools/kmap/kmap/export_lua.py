"""kmap export, Lua: the LuaJIT FFI module and the LuaLS stubs (mapping-exports, "Lua types").

  loader/lua/cubeworld.lua   ffi.cdef of the same structs as sdk/include/cubeworld/types.h, the function-pointer
                             typedefs, and the mapping as Lua tables (types with their fields, enums, functions).
                             Shipped as kyubu/lua/cubeworld.lua; require("cubeworld") in full-trust mods.
  sdk/lua/cubeworld.lua      LuaLS stubs: one class per struct, every field with its type, offset and status.

The layout comes from export_headers (Headers.definitions), so C and Lua always agree; the module's check()
compares LuaJIT's offsets and sizes with the mapping at run time. Old names of renamed types, fields, enum values
and functions stay usable, as in the headers (mapping-exports, "Name stability").
"""
import re

from . import db as dbmod
from .export import GENERATED_NOTE
from .export_headers import DIMS, Headers, aliases, ident, member
from .rules import PRIMITIVES

MODULE = 'loader/lua/cubeworld.lua'
STUBS = 'sdk/lua/cubeworld.lua'
CONVENTIONS = {'CW_CDECL': '__cdecl', 'CW_STDCALL': '__stdcall', 'CW_FASTCALL': '__fastcall'}
DEPRECATED = re.compile(r'CW_DEPRECATED(?:_VALUE)?\("(?:[^"\\]|\\.)*"\)\s*')
LUA_PRIMITIVES = {'float': 'number', 'double': 'number', 'bool': 'boolean', 'int64_t': 'ffi.cdata*',
                  'uint64_t': 'ffi.cdata*', 'void': 'nil'}

MODULE_HEAD = f'''-- cubeworld: the game's types for LuaJIT's FFI. Full-trust mods only:
--   local cubeworld = require("cubeworld")
--   local creature = cubeworld.cast("cube_CreatureData", address)   -- or the mapped name "cube::CreatureData"
--   print(creature.hp)
-- The structs are the ones in sdk/include/cubeworld/types.h: unknown ranges are _unk_<offset> padding, and
-- cubeworld.types says which fields are only candidates. No addresses: use kyubu.resolve_symbol.
-- {GENERATED_NOTE}
local ffi = require("ffi")

ffi.cdef[[
'''

MODULE_TAIL = '''
local pointer = {}

---A pointer to the game type `name` (C, mapped or old name) at `address` (integer, lightuserdata or cdata).
function M.cast(name, address)
    local t = M.types[name] or M.types[M.aliases[name]]
    local cname = t and t.cname or name
    pointer[cname] = pointer[cname] or ffi.typeof(cname .. "*")
    return ffi.cast(pointer[cname], address)
end

---Where LuaJIT's layout differs from the mapping (verified field offsets and known sizes); empty when it agrees.
function M.check()
    local problems = {}
    for name, t in pairs(M.types) do
        if t.size and ffi.sizeof(t.cname) ~= t.size then
            problems[#problems + 1] = string.format("%s is 0x%X bytes, LuaJIT says 0x%X", name, t.size,
                                                    ffi.sizeof(t.cname) or 0)
        end
        for _, f in ipairs(t.fields) do
            for _, member in ipairs(f.status == "verified" and {f.name, unpack(f.aliases or {})} or {}) do
                if ffi.offsetof(t.cname, member) ~= f.offset then
                    problems[#problems + 1] = string.format("%s.%s is at 0x%X, LuaJIT says 0x%X", name, member, f.offset,
                                                            ffi.offsetof(t.cname, member) or -1)
                end
            end
        end
    end
    table.sort(problems)
    return problems
end

return M
'''

STUBS_HEAD = f'''---@meta cubeworld
-- LuaLS type stubs for require("cubeworld"), the game's types for LuaJIT's FFI (full-trust mods only).
-- Add this folder to "workspace.library" in your .luarc.json, like kyubu.lua. Nothing here runs in the game.
-- Fields read and write game memory. A field's description gives its C type, offset and mapping status:
-- "candidate" fields are not confirmed yet.
-- {GENERATED_NOTE}

---@class cubeworld.Field
---@field name string C member name
---@field offset integer
---@field type string mapped type
---@field status "verified"|"candidate"
---@field aliases? string[] old member names of a renamed field (deprecated), at the same offset

---@class cubeworld.Type
---@field cname string C name, for ffi.typeof and cubeworld.cast
---@field size? integer
---@field status "verified"|"candidate"
---@field fields cubeworld.Field[] the fields the struct declares, in offset order

---@class cubeworld.Enum
---@field cname string
---@field status "verified"|"candidate"
---@field values table<string, integer>

---@class cubeworld
---@field types table<string, cubeworld.Type> by mapped name, e.g. "cube::CreatureData"
---@field enums table<string, cubeworld.Enum> by mapped name
---@field functions table<string, string> mapped function name (or old name) -> function-pointer C type, for ffi.cast
---@field aliases table<string, string> old name of a renamed type or enum (deprecated) -> its mapped name
local cubeworld = {{}}

---A pointer to the game type `name` at `address`. With the C name (`"cube_CreatureData"`) the editor knows the
---type; the mapped name (`"cube::CreatureData"`) and old names work too.
---@generic T
---@param name `T`
---@param address integer|lightuserdata|ffi.cdata*
---@return T
function cubeworld.cast(name, address) end

---Where LuaJIT's layout differs from the mapping (verified field offsets and known sizes); empty when it agrees.
---@return string[]
function cubeworld.check() end
'''


def lua_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


LUA_KEYWORDS = frozenset('and break do else elseif end false for function goto if in local nil not or repeat return '
                         'then true until while'.split())


def lua_key(s):
    return s if re.fullmatch(r'[A-Za-z_]\w*', s) and s not in LUA_KEYWORDS else f'[{lua_str(s)}]'


def plain(lines):
    """Header struct lines as LuaJIT's cdef takes them: without the deprecation and C99 macros of old names."""
    return [DEPRECATED.sub('', line.replace('CW_ALIASES_BEGIN ', '')) for line in lines if line != 'CW_ALIASES_END']


class Lua:
    def __init__(self, ctx, db):
        self.h = Headers(ctx, db)
        self.sections = self.h.functions()        # first: collects the opaque classes
        self.fwd, self.enums, self.structs = self.h.definitions()
        self.typed = {name: (fid, td) for sub in self.sections.values() for name, fid, _, _, td, _ in sub if td}

    def renamed(self):
        """[(old name, mapped name, C name)] of the renamed types and enums."""
        ts = [self.h.types[n] for n in sorted(self.h.types)] + [en for en, _ in self.enums]
        return [(a, t['name'], ident(t['name'])) for t in ts for a in aliases(t)]

    def old_functions(self):
        """[(old name, its function's name, function-pointer C type)] of renamed functions with a typedef."""
        return [(a, owners[0], f'cw_{self.typed[owners[0]][0]}_fn') for a, owners in self.h.function_aliases.items()
                if len(owners) == 1 and owners[0] in self.typed]

    # --- the module -------------------------------------------------------------------------------------------
    def cdef(self):
        lines = self.fwd + ['']
        lines += [self.h.enum_typedef(en) for en, _ in self.enums] + ['']
        for _, struct, _ in self.structs:
            lines += plain(struct) + ['']
        olds = [f'typedef {cname} {ident(a)};' for a, _, cname in self.renamed()]
        lines += olds + ([''] if olds else [])
        for sub in sorted(self.sections):
            for name, fid, where, text, td, why in self.sections[sub]:
                if td:
                    lines.append(re.sub(r'\bCW_\w+\b', lambda m: CONVENTIONS[m.group(0)], td))
        lines += [f'typedef {fn} cw_{ident(a)}_fn;' for a, _, fn in self.old_functions()]
        return lines

    def tables(self):
        out = ['local M = {}', '', '-- mapped name -> C name, size, status and the declared fields', 'M.types = {']
        for name in sorted(self.h.types):
            ty = self.h.types[name]
            head = f'    [{lua_str(name)}] = {{cname = {lua_str(ident(name))}, '
            head += (f'size = 0x{ty["size"]:X}, ' if ty.get('size') else '') + f'status = {lua_str(ty["status"])}, fields = {{'
            fields = [f'        {{name = {lua_str(m)}, offset = 0x{off:X}, type = {lua_str(f["type"])}, '
                      f'status = {lua_str(f["status"])}' +
                      (f', aliases = {{{", ".join(lua_str(member(a)) for a in aliases(f))}}}' if aliases(f) else '') +
                      '},' for m, off, f in self.h.declared.get(name, [])]
            out += [head] + fields + ['    }},'] if fields else [head + '}},']
        out += ['}', '', '-- mapped name -> C name, status and values (old names of renamed values too)', 'M.enums = {']
        for en, _ in self.enums:
            values = ', '.join(f'{lua_key(n)} = {v["value"]}' for v in self.h.enum_values(en) for n in [v['name']] + aliases(v))
            out.append(f'    [{lua_str(en["name"])}] = {{cname = {lua_str(ident(en["name"]))}, '
                       f'status = {lua_str(en["status"])}, values = {{{values}}}}},')
        out += ['}', '', '-- mapped function name -> function-pointer C type (for ffi.cast), when it has a prototype',
                'M.functions = {']
        for sub in sorted(self.sections):
            for name, fid, where, text, td, why in self.sections[sub]:
                if td:
                    out.append(f'    [{lua_str(name)}] = {lua_str(f"cw_{fid}_fn")},')
        out += [f'    [{lua_str(a)}] = {lua_str(fn)},  -- old name of {name}' for a, name, fn in self.old_functions()]
        out += ['}', '', '-- old name of a renamed type or enum (deprecated) -> its mapped name', 'M.aliases = {']
        out += [f'    [{lua_str(a)}] = {lua_str(name)},' for a, name, _ in self.renamed()]
        return out + ['}']

    def module(self):
        return MODULE_HEAD + '\n'.join(self.cdef()).rstrip() + '\n]]\n\n' + '\n'.join(self.tables()) + '\n' + MODULE_TAIL

    # --- the stubs --------------------------------------------------------------------------------------------
    def lua_type(self, t):
        """LuaLS type of a mapped field type (pointers to mapped structs read like the struct: LuaJIT dereferences)."""
        m = DIMS.match(t.strip())
        elem, dims = m.group('elem').strip(), re.findall(r'\[(\w+)\]', m.group('dims'))
        if dims and elem in ('char', 'wchar_t'):
            return 'ffi.cdata*'
        if elem.endswith('*'):
            target = self.h.lookup(elem[:-1].strip())
            base = ident(target['name']) if target is not None and target['_kind'] == 'type' else 'ffi.cdata*'
        elif elem in PRIMITIVES:
            base = LUA_PRIMITIVES.get(elem, 'integer')
        else:
            target = self.h.lookup(elem)
            base = ident(target['name']) if target is not None else 'ffi.cdata*'
        return base + '[]' * len(dims)

    def stubs(self):
        out = [STUBS_HEAD]
        old = {}
        for a, name, cname in self.renamed():
            old.setdefault(name, []).append(f'---Old name of {name} (deprecated): use {cname}.\n---@alias {ident(a)} {cname}\n')
        for en, _ in self.enums:
            out.append(f'---{en["name"]}' + (' (candidate)' if en['status'] == 'candidate' else '') +
                       f': a {en["size"]}-byte integer; values in cubeworld.enums[{lua_str(en["name"])}].values')
            out.append(f'---@alias {ident(en["name"])} integer\n')
            out += old.get(en['name'], [])
        for ty, _, _ in self.structs:
            name = ty['name']
            about = ', '.join(x for x in (f'0x{ty["size"]:X} bytes' if ty.get('size') else '', ty['status'],
                                          f'from {ty["source"]}' if ty.get('source') else '') if x)
            out.append(f'---{name} ({about}). Get one with cubeworld.cast({lua_str(ident(name))}, address).')
            out.append(f'---@class {ident(name)}')
            base = self.h.lookup(ty['base']) if ty.get('base') else None
            if base is not None and base['_kind'] == 'type' and base.get('size') and ty.get('kind') != 'union':
                out.append(f'---@field _base {ident(base["name"])} {base["name"]} at 0x0 (the base class)')
            for m, off, f in self.h.declared[name]:
                label = f['type'] + ('' if m == f['name'] else f', mapped name {f["name"]}')
                out.append(f'---@field {m} {self.lua_type(f["type"])} {label} at 0x{off:X} ({f["status"]})')
                out += [f'---@field {member(a)} {self.lua_type(f["type"])} old name of {m} (deprecated): {f["type"]} '
                        f'at 0x{off:X} ({f["status"]})' for a in aliases(f)]
            out.append('')
            out += old.get(name, [])
        out.append('return cubeworld')
        return '\n'.join(out) + '\n'


def generate(ctx, db):
    lua = Lua(ctx, db)
    return {MODULE: lua.module(), STUBS: lua.stubs()}
