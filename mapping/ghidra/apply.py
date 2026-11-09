# Kyubu: names your own Ghidra project of Cube World Alpha with Kyubu's mapping.
#
# Applies mappings.json (generated from Kyubu's mapping database by `kmap export`) to the open program: every
# exported function name, prototype, label, global and data type (under the /kyubu category). Before changing
# anything it checks the SHA-256 that Ghidra recorded when the program was imported; for any other binary it
# applies nothing and prints the hashes it expects. It sets only what differs, so running it a second time with
# the same mappings changes nothing. It overwrites names, prototypes and plate comments at mapped addresses.
#
# Needs Ghidra 12.1 with PyGhidra (the default Python in Ghidra 12). Auto-analysis first is optional: the
# script disassembles and creates any mapped function that is missing.
#
#   GUI: open Cube.exe (or Server.exe), add this folder in the Script Manager and run apply.py. It reads
#        mappings.json from its own folder, or asks for the file.
#   Headless:
#        <ghidra>/support/pyghidraRun --headless <project folder> <project name> -import Cube.exe
#            -scriptPath <folder of apply.py> -postScript apply.py [path/to/mappings.json]
#
# @category Kyubu
# @runtime PyGhidra
import hashlib
import json
import os

FORMAT = 1
CATEGORY = '/kyubu'
PREFIX = 'kyubu: '


# --- plain Python (tested without Ghidra) -----------------------------------------------------------------

def load(path):
    with open(path, encoding='utf-8') as f:
        data = json.load(f)
    if data.get('format') != FORMAT:
        raise ValueError(f'{path} has format {data.get("format")!r}; this apply.py reads format {FORMAT}. Use the '
                         'apply.py that came with it.')
    return data


def match_build(mappings, sha256):
    """(build id, build) whose SHA-256 is `sha256`, or (None, None)."""
    for bid, b in sorted(mappings['builds'].items()):
        if sha256 and b['sha256'].lower() == sha256.lower():
            return bid, b
    return None, None


def wrong_binary(mappings, sha256):
    lines = [f'nothing applied: this program (SHA-256 {sha256 or "unknown"}) is not a build these mappings are for.',
             'Import an unmodified copy of one of these files:']
    lines += [f'  {b["file"]} ({bid}): SHA-256 {b["sha256"]}' for bid, b in sorted(mappings['builds'].items())]
    return '\n'.join(PREFIX + line for line in lines)


def summary(build_id, build, counts, changes, warnings):
    applied = (f'{counts["functions"]} functions ({counts["prototypes"]} with prototypes), {counts["labels"]} '
               f'labels, {counts["globals"]} globals and {counts["types"]} types')
    what = f'{changes} change(s)' if changes else '0 change(s), already applied'
    return f'{PREFIX}applied {applied} to {build["file"]} ({build_id}): {what}, {len(warnings)} warning(s)'


# --- inside Ghidra ----------------------------------------------------------------------------------------

def _prims():
    from ghidra.program.model.data import (BooleanDataType, ByteDataType, CharDataType, DoubleDataType,
                                           FloatDataType, IntegerDataType, LongLongDataType, ShortDataType,
                                           SignedByteDataType, UnsignedIntegerDataType, UnsignedLongLongDataType,
                                           UnsignedShortDataType, VoidDataType, WideChar16DataType)
    return {'int8_t': SignedByteDataType.dataType, 'uint8_t': ByteDataType.dataType,
            'int16_t': ShortDataType.dataType, 'uint16_t': UnsignedShortDataType.dataType,
            'int32_t': IntegerDataType.dataType, 'uint32_t': UnsignedIntegerDataType.dataType,
            'int64_t': LongLongDataType.dataType, 'uint64_t': UnsignedLongLongDataType.dataType,
            'float': FloatDataType.dataType, 'double': DoubleDataType.dataType, 'bool': BooleanDataType.dataType,
            'char': CharDataType.dataType, 'wchar_t': WideChar16DataType.dataType, 'void': VoidDataType.dataType,
            'int': IntegerDataType.dataType}


def split_path(name):
    from ghidra.app.util import SymbolPath
    sp = SymbolPath(name)
    parent = sp.getParent()
    return (str(parent.getPath()) if parent else None), str(sp.getName())


def type_name(dt):
    from ghidra.program.model.data import Array, Pointer
    if dt is None:
        return 'void'
    if isinstance(dt, Pointer):
        return type_name(dt.getDataType()) + '*'
    if isinstance(dt, Array):
        return f'{type_name(dt.getDataType())}[{dt.getNumElements()}]'
    return str(dt.getPathName())


def fingerprint(dt):
    """What decides whether a type needs applying again."""
    from ghidra.program.model.data import Enum, Structure, Union
    if dt is None:
        return None
    if isinstance(dt, Enum):
        return ['enum', int(dt.getLength()), sorted([str(n), int(dt.getValue(n))] for n in dt.getNames())]
    if isinstance(dt, (Structure, Union)):
        return ['union' if isinstance(dt, Union) else 'struct', 0 if dt.isNotYetDefined() else int(dt.getLength()),
                [[int(c.getOffset()), str(c.getFieldName() or ''), type_name(c.getDataType()), int(c.getLength()),
                  str(c.getComment() or '')] for c in dt.getDefinedComponents()]]
    return [str(type(dt).__name__)]


class Applier:
    def __init__(self, program, build, monitor):
        self.program = program
        self.monitor = monitor
        self.dtm = program.getDataTypeManager()
        self.prims = _prims()
        self.delta = int(program.getImageBase().getOffset()) - int(build['image_base'], 16)   # a rebased program
        self.changes = 0
        self.warnings = []
        self.counts = {'functions': 0, 'prototypes': 0, 'labels': 0, 'globals': 0, 'types': 0}

    def warn(self, text):
        self.warnings.append(text)

    # data types

    def category(self, name):
        from ghidra.program.model.data import CategoryPath
        parent, simple = split_path(name)
        return CategoryPath(CATEGORY + ('/' + parent.replace('::', '/') if parent else '')), simple

    def named(self, name, create=True):
        from ghidra.program.model.data import DataTypeConflictHandler, StructureDataType
        cat, simple = self.category(name)
        dt = self.dtm.getDataType(cat, simple)
        if dt is None and create:       # a type the mapping names but does not define yet: an empty placeholder
            dt = self.dtm.addDataType(StructureDataType(cat, simple, 0), DataTypeConflictHandler.KEEP_HANDLER)
            self.changes += 1
        return dt

    def resolve(self, t):
        from ghidra.program.model.data import ArrayDataType, PointerDataType
        t = t.replace('const ', '').strip()
        if t.endswith('*'):
            return PointerDataType(self.resolve(t[:-1]), 4, self.dtm)
        if t.endswith(']') and '[' in t:
            elem, n = t[:-1].rsplit('[', 1)
            e = self.resolve(elem)
            return ArrayDataType(e, int(n, 0), max(1, e.getLength()), self.dtm)
        if t in self.prims:
            return self.prims[t]
        return self.named(t)

    def build_type(self, item):
        from ghidra.program.model.data import EnumDataType, StructureDataType, UnionDataType
        cat, simple = self.category(item['name'])
        if item['kind'] == 'enum':
            en = EnumDataType(cat, simple, item['size'])
            for n, v in item['values']:
                en.add(n, v)
            return en
        fields = sorted(item['fields'], key=lambda f: f['offset'])
        ends = [f['offset'] + max(1, self.resolve(f['type']).getLength()) for f in fields]
        size = item['size'] or (max(ends) if ends else 0)
        if item['kind'] == 'union':
            st = UnionDataType(cat, simple)
            for f in fields:
                st.add(self.resolve(f['type']), f['name'], f.get('comment'))
            return st
        st = StructureDataType(cat, simple, size)
        if item.get('base'):
            b = self.named(item['base'], create=False)
            if b is not None and 0 < b.getLength() <= size:
                st.replaceAtOffset(0, b, b.getLength(), 'base', item['base'])
        for f in fields:
            dt = self.resolve(f['type'])
            length = dt.getLength() if dt.getLength() > 0 else 1
            if f['offset'] + length <= size:
                st.replaceAtOffset(f['offset'], dt, length, f['name'], f.get('comment'))
            else:
                self.warn(f'{item["name"]}.{f["name"]} does not fit in {size} bytes; not applied')
        return st

    def apply_type(self, item):
        from ghidra.program.model.data import DataTypeConflictHandler
        new = self.build_type(item)
        if fingerprint(self.named(item['name'], create=False)) != fingerprint(new):
            self.dtm.addDataType(new, DataTypeConflictHandler.REPLACE_HANDLER)
            self.changes += 1
        self.counts['types'] += 1

    # symbols

    def address(self, item):
        a = self.program.getAddressFactory().getDefaultAddressSpace().getAddress(int(item['address'], 16) + self.delta)
        if not self.program.getMemory().contains(a):
            self.warn(f'{item["name"]}: {a} is not in the program')
            return None
        return a

    def namespace(self, path):
        from ghidra.app.util import NamespaceUtils
        from ghidra.program.model.symbol import SourceType
        if not path:
            return self.program.getGlobalNamespace()
        found = NamespaceUtils.getNamespaceByPath(self.program, None, path)
        found = list(found) if found is not None and not hasattr(found, 'getName') else ([found] if found else [])
        found = [n for n in found if str(n.getSymbol().getSymbolType()) in ('Class', 'Namespace')]
        if found:
            return next((n for n in found if str(n.getSymbol().getSymbolType()) == 'Class'), found[0])
        self.changes += 1
        return NamespaceUtils.createNamespaceHierarchy(path, None, self.program, SourceType.USER_DEFINED)

    def signature(self, item):
        from ghidra.program.model.data import FunctionDefinitionDataType, ParameterDefinitionImpl
        p = item['prototype']
        fd = FunctionDefinitionDataType(split_path(item['name'])[1])
        fd.setReturnType(self.resolve(p['ret']))
        params = p['params'][1:] if p['conv'] == 'thiscall' else p['params']   # Ghidra adds `this` itself
        fd.setArguments([ParameterDefinitionImpl(n, self.resolve(t), None) for t, n in params])
        fd.setCallingConvention('__' + p['conv'])
        return fd

    def apply_function(self, item, a):
        from ghidra.app.cmd.disassemble import DisassembleCommand
        from ghidra.app.cmd.function import ApplyFunctionSignatureCmd, CreateFunctionCmd
        from ghidra.app.util import NamespaceUtils
        from ghidra.program.model.symbol import SourceType
        fm = self.program.getFunctionManager()
        f = fm.getFunctionAt(a)
        if f is None:
            if self.program.getListing().getInstructionAt(a) is None:
                DisassembleCommand(a, None, True).applyTo(self.program, self.monitor)
            CreateFunctionCmd(a).applyTo(self.program, self.monitor)
            f = fm.getFunctionAt(a)
            if f is None:
                self.warn(f'{item["name"]}: Ghidra could not create a function at {a}')
                return
            self.changes += 1
        if str(f.getName(True)) != item['name']:
            parent, simple = split_path(item['name'])
            f.setParentNamespace(self.namespace(parent))
            f.setName(simple, SourceType.USER_DEFINED)
            self.changes += 1
        ns = f.getParentNamespace()
        if (item.get('prototype') or {}).get('conv', item.get('convention')) == 'thiscall' and not ns.isGlobal() \
                and str(ns.getSymbol().getSymbolType()) == 'Namespace':
            NamespaceUtils.convertNamespaceToClass(ns)       # a method: Ghidra then types `this` as the class
            self.changes += 1
        self.counts['functions'] += 1
        if item.get('prototype'):
            fd = self.signature(item)
            if f.getSignatureSource() != SourceType.USER_DEFINED or \
                    str(f.getSignature(True).getPrototypeString(True)) != str(fd.getPrototypeString(True)):
                if ApplyFunctionSignatureCmd(a, fd, SourceType.USER_DEFINED).applyTo(self.program):
                    self.changes += 1
                else:
                    self.warn(f'{item["name"]}: the prototype could not be applied')
            self.counts['prototypes'] += 1
        elif item.get('convention') and item['convention'] != 'usercall' and \
                str(f.getCallingConventionName()) != '__' + item['convention']:
            f.setCallingConvention('__' + item['convention'])
            self.changes += 1

    def apply_label(self, item, a):
        from ghidra.program.model.data import DataUtilities
        from ghidra.program.model.symbol import SourceType
        st = self.program.getSymbolTable()
        mine = [s for s in st.getSymbols(a) if str(s.getName(True)) == item['name']]
        if not mine:
            parent, simple = split_path(item['name'])
            st.createLabel(a, simple, self.namespace(parent), SourceType.USER_DEFINED).setPrimary()
            self.changes += 1
        elif not mine[0].isPrimary() and mine[0].setPrimary():
            self.changes += 1
        if item['kind'] == 'global':
            self.counts['globals'] += 1
            dt = self.resolve(item['type']) if item.get('type') else None
            if dt is not None and dt.getLength() > 0:
                d = self.program.getListing().getDataAt(a)
                if d is None or not d.getDataType().isEquivalent(dt):
                    DataUtilities.createData(self.program, a, dt, -1,
                                             DataUtilities.ClearDataMode.CLEAR_ALL_UNDEFINED_CONFLICT_DATA)
                    self.changes += 1
        else:
            self.counts['labels'] += 1

    def apply_symbol(self, item):
        from ghidra.program.model.listing import CommentType
        from ghidra.program.model.symbol import SourceType
        a = self.address(item)
        if a is None:
            return
        if item['kind'] == 'function':
            self.apply_function(item, a)
        else:
            self.apply_label(item, a)
        st = self.program.getSymbolTable()
        for alias in item.get('aliases', []):
            if not any(str(s.getName(True)) == alias for s in st.getSymbols(a)):
                parent, simple = split_path(alias)
                st.createLabel(a, simple, self.namespace(parent), SourceType.USER_DEFINED)
                self.changes += 1
        listing = self.program.getListing()
        now = listing.getComment(CommentType.PLATE, a)
        if str(now or '') != (item.get('comment') or ''):
            listing.setComment(a, CommentType.PLATE, item.get('comment') or None)
            self.changes += 1

    def run(self, build):
        for item in build['types']:
            try:
                self.apply_type(item)
            except Exception as e:      # one bad entry must not stop the rest
                self.warn(f'type {item["name"]}: {e}')
        for item in build['symbols']:
            try:
                self.apply_symbol(item)
            except Exception as e:
                self.warn(f'{item["name"]}: {e}')


def program_sha256(program):
    sha = program.getExecutableSHA256()
    if sha:
        return str(sha).lower()
    path = program.getExecutablePath()        # imported by a Ghidra that did not record it: hash the file
    if path and os.path.isfile(str(path)):
        with open(str(path), 'rb') as f:
            return hashlib.sha256(f.read()).hexdigest()
    return None


def mappings_path():
    args = list(getScriptArgs())                                        # noqa: F821 (GhidraScript)
    if args:
        return str(args[0])
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mappings.json')
    if os.path.isfile(here):
        return here
    if isRunningHeadless():                                             # noqa: F821
        raise FileNotFoundError(f'{here} not found: pass the path of mappings.json as the script argument')
    return str(askFile('Kyubu mappings.json', 'Apply'))                 # noqa: F821


def main():
    try:
        program = currentProgram                                        # noqa: F821
    except NameError:
        print('apply.py is a Ghidra script: run it from Ghidra (Script Manager, or pyghidraRun --headless).')
        return 2
    if program is None:
        printerr(PREFIX + 'open Cube.exe or Server.exe first')          # noqa: F821
        return 2
    path = mappings_path()
    mappings = load(path)
    sha = program_sha256(program)
    bid, build = match_build(mappings, sha)
    if bid is None:
        printerr(wrong_binary(mappings, sha))                           # noqa: F821
        return 1
    from ghidra.framework import Application
    version = str(Application.getApplicationVersion())
    if version != mappings['ghidra']:
        print(f'{PREFIX}warning: these mappings were made for Ghidra {mappings["ghidra"]}; this is {version}')
    print(f'{PREFIX}{build["file"]} matches {bid}; applying {path}')
    applier = Applier(program, build, monitor)                          # noqa: F821
    tx = program.startTransaction('Kyubu: apply mappings')
    ok = False
    try:
        applier.run(build)
        ok = True
    finally:
        program.endTransaction(tx, ok)
    for w in applier.warnings:
        print(f'{PREFIX}warning: {w}')
    print(summary(bid, build, applier.counts, applier.changes, applier.warnings))
    return 0


if __name__ == '__main__':
    main()
