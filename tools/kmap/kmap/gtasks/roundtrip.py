"""kmap push / pull inside Ghidra: apply the database to the private project, and read back what people
changed in it. Everything kmap writes is USER_DEFINED; pull only looks at USER_DEFINED names and at data
types under the /kyubu category."""
from .common import addr, function_starts, program_context, project_context, transaction, va

CATEGORY = '/kyubu'


# --- data types ------------------------------------------------------------------------------------------

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


def category_for(name):
    from ghidra.program.model.data import CategoryPath
    parent, simple = split_path(name)
    path = CATEGORY + ('/' + parent.replace('::', '/') if parent else '')
    return CategoryPath(path), simple


class Types:
    def __init__(self, program):
        self.dtm = program.getDataTypeManager()
        self.prims = _prims()

    def named(self, name, create=True):
        from ghidra.program.model.data import StructureDataType, DataTypeConflictHandler
        cat, simple = category_for(name)
        dt = self.dtm.getDataType(cat, simple)
        if dt is None and create:
            dt = self.dtm.addDataType(StructureDataType(cat, simple, 0), DataTypeConflictHandler.KEEP_HANDLER)
        return dt

    def resolve(self, t):
        from ghidra.program.model.data import ArrayDataType, PointerDataType
        t = t.replace('const ', '').strip()
        if t.endswith('*'):
            inner = self.resolve(t[:-1])
            return PointerDataType(inner, 4, self.dtm)
        if t.endswith(']') and '[' in t:
            elem, n = t[:-1].rsplit('[', 1)
            e = self.resolve(elem)
            return ArrayDataType(e, int(n, 0), max(1, e.getLength()), self.dtm)
        if t in self.prims:
            return self.prims[t]
        return self.named(t)


GHIDRA_TO_MAPPING = {'int': 'int32_t', 'uint': 'uint32_t', 'short': 'int16_t', 'ushort': 'uint16_t',
                     'sbyte': 'int8_t', 'byte': 'uint8_t', 'char': 'char', 'bool': 'bool', 'float': 'float',
                     'double': 'double', 'longlong': 'int64_t', 'ulonglong': 'uint64_t', 'void': 'void',
                     'wchar16': 'wchar_t', 'undefined': 'uint8_t', 'undefined1': 'uint8_t', 'undefined2': 'uint16_t',
                     'undefined4': 'uint32_t', 'undefined8': 'uint64_t', 'dword': 'uint32_t', 'word': 'uint16_t',
                     'long': 'int32_t', 'ulong': 'uint32_t'}


def type_name(dt):
    """A Ghidra data type in mapping/ notation."""
    from ghidra.program.model.data import Array, Pointer, TypeDef
    if dt is None:
        return 'void'
    if isinstance(dt, Pointer):
        return type_name(dt.getDataType()) + '*'
    if isinstance(dt, Array):
        return f'{type_name(dt.getDataType())}[{dt.getNumElements()}]'
    name = str(dt.getName())
    if name in GHIDRA_TO_MAPPING:
        return GHIDRA_TO_MAPPING[name]
    if isinstance(dt, TypeDef) and str(dt.getCategoryPath().getPath()) == '/':
        return type_name(dt.getBaseDataType())
    path = str(dt.getCategoryPath().getPath())
    if path.startswith(CATEGORY):
        path = path[len(CATEGORY):]
    ns = path.strip('/').replace('/', '::')
    return (ns + '::' if ns else '') + name


def signature(f):
    """A function's signature in mapping/ notation (the ECX parameter of __thiscall included as `this`)."""
    conv = str(f.getCallingConventionName() or '').lstrip('_') or 'cdecl'
    if conv not in ('cdecl', 'stdcall', 'thiscall', 'fastcall'):
        conv = 'usercall'
    params = [[type_name(p.getDataType()), str(p.getName())] for p in f.getParameters()]
    return {'ret': type_name(f.getReturnType()), 'conv': conv, 'name': str(f.getName(True)), 'params': params}


def type_state(dt):
    """Fingerprint of a structure or enum in the project (what pull compares)."""
    from ghidra.program.model.data import Enum, Structure, Union
    if isinstance(dt, Enum):
        return {'kind': 'enum', 'size': dt.getLength(),
                'values': sorted([str(n), int(dt.getValue(n))] for n in dt.getNames())}
    if isinstance(dt, (Structure, Union)):
        comps = []
        for c in dt.getDefinedComponents():
            comps.append([int(c.getOffset()), str(c.getFieldName() or ''), type_name(c.getDataType()),
                          int(c.getLength())])
        return {'kind': 'union' if isinstance(dt, Union) else 'struct',
                'size': 0 if dt.isNotYetDefined() else int(dt.getLength()), 'fields': comps}
    return {'kind': str(type(dt).__name__)}


def apply_type(types, item):
    from ghidra.program.model.data import DataTypeConflictHandler, EnumDataType, StructureDataType, UnionDataType
    cat, simple = category_for(item['name'])
    if item['kind'] == 'enum':
        en = EnumDataType(cat, simple, item['size'])
        for n, v in item['values']:
            en.add(n, v)
        return types.dtm.addDataType(en, DataTypeConflictHandler.REPLACE_HANDLER)
    fields = sorted(item['fields'], key=lambda f: f['offset'])
    ends = [f['offset'] + max(1, types.resolve(f['type']).getLength()) for f in fields]
    size = item['size'] or (max(ends) if ends else 0)
    if item['kind'] == 'union':
        st = UnionDataType(cat, simple)
        for f in fields:
            st.add(types.resolve(f['type']), f['name'], f.get('comment'))
    else:
        st = StructureDataType(cat, simple, size)
        if item.get('base'):
            b = types.named(item['base'], create=False)
            if b is not None and b.getLength() > 0 and b.getLength() <= size:
                st.replaceAtOffset(0, b, b.getLength(), 'base', item['base'])
        for f in fields:
            dt = types.resolve(f['type'])
            length = dt.getLength() if dt.getLength() > 0 else 1
            if f['offset'] + length <= size:
                st.replaceAtOffset(f['offset'], dt, length, f['name'], f.get('comment'))
    return types.dtm.addDataType(st, DataTypeConflictHandler.REPLACE_HANDLER)


# --- symbols -------------------------------------------------------------------------------------------

def namespace(program, path):
    from ghidra.app.util import NamespaceUtils
    from ghidra.program.model.symbol import SourceType
    if not path:
        return program.getGlobalNamespace()
    found = NamespaceUtils.getNamespaceByPath(program, None, path)     # a list in Ghidra 12
    found = list(found) if found is not None and not hasattr(found, 'getName') else ([found] if found else [])
    found = [n for n in found if str(n.getSymbol().getSymbolType()) in ('Class', 'Namespace')]
    if found:
        return next((n for n in found if str(n.getSymbol().getSymbolType()) == 'Class'), found[0])
    return NamespaceUtils.createNamespaceHierarchy(path, None, program, SourceType.USER_DEFINED)


def plate(program, a):
    from ghidra.program.model.listing import CommentType
    c = program.getListing().getComment(CommentType.PLATE, a)
    return str(c) if c else ''


def set_plate(program, a, text):
    from ghidra.program.model.listing import CommentType
    program.getListing().setComment(a, CommentType.PLATE, text or None)


def symbol_state(program, item):
    """What the project shows at an entry's address (compared with pushed.json to detect edits)."""
    a = addr(program, item['address'])
    fm = program.getFunctionManager()
    st = {'comment': plate(program, a)}
    if item['kind'] == 'function':
        f = fm.getFunctionAt(a)
        if f is None:
            st['name'] = None
            return st
        st['name'] = str(f.getName(True))
        st['name_source'] = str(f.getSymbol().getSource())
        st['signature'] = str(f.getPrototypeString(False, False))
        st['signature_source'] = str(f.getSignatureSource())
    else:
        s = program.getSymbolTable().getPrimarySymbol(a)
        st['name'] = str(s.getName(True)) if s else None
        st['name_source'] = str(s.getSource()) if s else None
        if item['kind'] == 'global':
            d = program.getListing().getDataAt(a)
            st['type'] = str(d.getDataType().getDisplayName()) if d else None
    return st


def _signature(types, item):
    from ghidra.program.model.data import FunctionDefinitionDataType, ParameterDefinitionImpl
    p = item['prototype']
    _, simple = split_path(item['name'])
    fd = FunctionDefinitionDataType(simple)
    fd.setReturnType(types.resolve(p['ret']))
    params = p['params']
    if p['conv'] == 'thiscall':
        params = params[1:]     # the ECX parameter: Ghidra adds it as `this` for __thiscall
    fd.setArguments([ParameterDefinitionImpl(n, types.resolve(t), None) for t, n in params])
    fd.setCallingConvention('__' + p['conv'])
    return fd


def apply_symbol(program, types, item):
    from ghidra.app.cmd.function import ApplyFunctionSignatureCmd, CreateFunctionCmd
    from ghidra.program.model.data import DataUtilities
    from ghidra.program.model.symbol import SourceType
    a = addr(program, item['address'])
    parent, simple = split_path(item['name'])
    ns = namespace(program, parent)
    st = program.getSymbolTable()
    warnings = []
    if item['kind'] == 'function':
        f = program.getFunctionManager().getFunctionAt(a)
        if f is None:
            CreateFunctionCmd(a).applyTo(program)
            f = program.getFunctionManager().getFunctionAt(a)
        if f is None:
            warnings.append(f'{item["name"]}: Ghidra could not create a function at 0x{item["address"]:X}')
        else:
            f.setParentNamespace(ns)
            f.setName(simple, SourceType.USER_DEFINED)
            if item.get('prototype'):
                ok = ApplyFunctionSignatureCmd(a, _signature(types, item), SourceType.USER_DEFINED).applyTo(program)
                if not ok:
                    warnings.append(f'{item["name"]}: the prototype could not be applied')
            elif item.get('convention') and item['convention'] != 'usercall':
                try:
                    f.setCallingConvention('__' + item['convention'])
                except Exception:
                    pass
    else:
        existing = [s for s in st.getSymbols(a) if str(s.getName(True)) == item['name']]
        if not existing:
            s = st.createLabel(a, simple, ns, SourceType.USER_DEFINED)
            s.setPrimary()
        elif not existing[0].isPrimary():
            existing[0].setPrimary()
        if item['kind'] == 'global' and item.get('type'):
            dt = types.resolve(item['type'])
            if dt.getLength() > 0:
                try:
                    DataUtilities.createData(program, a, dt, -1,
                                             DataUtilities.ClearDataMode.CLEAR_ALL_UNDEFINED_CONFLICT_DATA)
                except Exception as e:
                    warnings.append(f'{item["name"]}: type {item["type"]} not applied ({e})')
    for alias in item.get('aliases', []):
        ap, asimple = split_path(alias)
        if not any(str(s.getName(True)) == alias for s in st.getSymbols(a)):
            st.createLabel(a, asimple, namespace(program, ap), SourceType.USER_DEFINED)
    set_plate(program, a, item.get('comment'))
    return warnings


def reset_symbol(program, prev):
    """An entry was removed from mapping/: take back what kmap pushed, if nobody changed it since."""
    from ghidra.program.model.symbol import SourceType
    a = addr(program, prev['address'])
    st = program.getSymbolTable()
    for s in list(st.getSymbols(a)):
        n = str(s.getName(True))
        if n in [prev['name']] + prev.get('aliases', []) and s.getSource() == SourceType.USER_DEFINED:
            if s.getSymbolType().toString() == 'Function':
                f = program.getFunctionManager().getFunctionAt(a)
                f.setName(None, SourceType.DEFAULT)
            else:
                s.delete()
    if plate(program, a) == (prev.get('comment') or ''):
        set_plate(program, a, None)


# --- tasks -----------------------------------------------------------------------------------------------

def task_push(req):
    args = req['args']
    out = {}
    with project_context(args['project']) as project:
        for bid, want in args['builds'].items():
            prev = args['pushed'].get(bid, {'symbols': {}, 'types': {}})
            res = {'changes': 0, 'unchanged': 0, 'skipped': [], 'warnings': [], 'pushed': {'symbols': {}, 'types': {}}}
            with program_context(project, bid, write=not args['dry_run']) as program, \
                    transaction(program, 'kmap push'):
                types = Types(program)
                for item in want['types']:
                    key = item['key']
                    p = prev['types'].get(key)
                    dt = types.named(item['name'], create=False)
                    now = type_state(dt) if dt is not None else None
                    if p and p['db'] == item and now == p['ghidra']:
                        res['unchanged'] += 1
                        res['pushed']['types'][key] = p
                        continue
                    if p and now is not None and now != p['ghidra'] and not args['force']:
                        res['skipped'].append(f'type {item["name"]} was changed in Ghidra since the last push')
                        res['pushed']['types'][key] = p
                        continue
                    res['changes'] += 1
                    if not args['dry_run']:
                        dt = apply_type(types, item)
                    res['pushed']['types'][key] = {'db': item, 'ghidra': type_state(dt) if dt is not None else None}
                wanted = {}
                for item in want['symbols']:
                    wanted[item['key']] = item
                    p = prev['symbols'].get(item['key'])
                    now = symbol_state(program, item)
                    if p and p['db'] == item and now == p['ghidra']:
                        res['unchanged'] += 1
                        res['pushed']['symbols'][item['key']] = p
                        continue
                    if p and now != p['ghidra'] and not args['force']:
                        res['skipped'].append(f'{item["name"]} (0x{item["address"]:X}) was changed in Ghidra since the '
                                              'last push')
                        res['pushed']['symbols'][item['key']] = p
                        continue
                    res['changes'] += 1
                    if not args['dry_run']:
                        res['warnings'] += apply_symbol(program, types, item)
                    res['pushed']['symbols'][item['key']] = {'db': item, 'ghidra': symbol_state(program, item)}
                for key, p in prev['symbols'].items():
                    if key not in wanted:
                        res['changes'] += 1
                        if not args['dry_run'] and symbol_state(program, p['db']) == p['ghidra']:
                            reset_symbol(program, p['db'])
            out[bid] = res
    return {'builds': out}


def task_snapshot(req):
    """USER_DEFINED function names and signatures, user labels, and /kyubu data types, per build."""
    from ghidra.program.model.symbol import SourceType
    args = req['args']
    out = {}
    with project_context(args['project']) as project:
        for bid in args['builds']:
            with program_context(project, bid) as program:
                fm = program.getFunctionManager()
                funcs = {}
                for f in fm.getFunctions(True):
                    user_name = f.getSymbol().getSource() == SourceType.USER_DEFINED
                    user_sig = f.getSignatureSource() == SourceType.USER_DEFINED
                    if user_name or user_sig:
                        a = va(f.getEntryPoint())
                        funcs[str(a)] = {'name': str(f.getName(True)), 'name_source': str(f.getSymbol().getSource()),
                                         'signature': str(f.getPrototypeString(False, False)),
                                         'signature_source': str(f.getSignatureSource()),
                                         'prototype': signature(f),
                                         'comment': plate(program, f.getEntryPoint())}
                labels = {}
                it = program.getSymbolTable().getSymbolIterator(True)
                while it.hasNext():
                    s = it.next()
                    if s.getSource() != SourceType.USER_DEFINED or str(s.getSymbolType()) != 'Label' or not s.isPrimary():
                        continue
                    a = s.getAddress()
                    if fm.getFunctionAt(a) is not None:
                        continue
                    d = program.getListing().getDataAt(a)
                    labels[str(va(a))] = {'name': str(s.getName(True)), 'in_function': fm.getFunctionContaining(a) is not None,
                                          'type': str(d.getDataType().getDisplayName()) if d else None,
                                          'comment': plate(program, a)}
                types = {}
                dtm = program.getDataTypeManager()
                from ghidra.program.model.data import CategoryPath
                root = dtm.getCategory(CategoryPath(CATEGORY))
                stack = [root] if root else []
                while stack:
                    c = stack.pop()
                    stack.extend(c.getCategories())
                    for dt in c.getDataTypes():
                        path = str(dt.getCategoryPath().getPath())[len(CATEGORY):].strip('/')
                        name = (path.replace('/', '::') + '::' if path else '') + str(dt.getName())
                        types[name] = type_state(dt)
                out[bid] = {'functions': funcs, 'labels': labels, 'types': types}
    return {'builds': out}


def task_rename(req):
    """Renames the function at an address as a GUI user would (tests of kmap pull)."""
    from ghidra.program.model.symbol import SourceType
    args = req['args']
    with project_context(args['project']) as project, \
            program_context(project, args['build'], write=True) as program, transaction(program, 'rename'):
        f = program.getFunctionManager().getFunctionAt(addr(program, args['address']))
        if f is None:
            raise ValueError(f'no function starts at 0x{args["address"]:X} in {args["build"]}')
        if args['name'] is None:          # back to Ghidra's default name (test clean-up)
            f.setParentNamespace(program.getGlobalNamespace())
            f.setName(None, SourceType.DEFAULT)
        else:
            parent, simple = split_path(args['name'])
            f.setParentNamespace(namespace(program, parent))
            f.setName(simple, SourceType.USER_DEFINED)
        name = str(f.getName(True))
    return {'ok': True, 'name': name}


def _import(project, args):
    import pyghidra
    from java.io import File
    from ghidra.program.model.lang import CompilerSpecID, LanguageID
    from ghidra.program.util import DefaultLanguageService
    from .project import _apply_options
    lang = DefaultLanguageService.getLanguageService().getLanguage(LanguageID(args['language']))
    program = project.importProgram(File(args['binary']), lang, lang.getCompilerSpecByID(CompilerSpecID(args['compiler'])))
    if program is None:
        raise RuntimeError(f'Ghidra could not import {args["binary"]}')
    try:
        if args.get('analyse'):
            _apply_options(program, args['options'])
            print(f'kmap: analysing {args["name"]} (this takes a while)', flush=True)
            pyghidra.analyze(program)
        project.saveAs(program, '/', args['name'], True)
    finally:
        project.close(program)


def _shown(program, addresses, type_names):
    """What the program shows at the given addresses and for the given /kyubu types."""
    from ghidra.program.model.listing import CommentType
    fm, st, listing = program.getFunctionManager(), program.getSymbolTable(), program.getListing()
    symbols = {}
    for x in addresses:
        a = addr(program, x)
        f = fm.getFunctionAt(a)
        symbols[f'0x{x:X}'] = {'function': str(f.getName(True)) if f else None,
                               'prototype': str(f.getPrototypeString(False, True)) if f else None,
                               'names': sorted(str(s.getName(True)) for s in st.getSymbols(a)),
                               'plate': str(listing.getComment(CommentType.PLATE, a) or '')}
    types = Types(program)
    return {'symbols': symbols, 'types': {n: type_state(types.named(n, create=False)) if types.named(n, create=False)
                                          else None for n in type_names}}


def task_apply_script(req):
    """Runs mapping/ghidra/apply.py as a modder would (task 4.6): imports args['binary'] into a fresh project
    (auto-analysis optional), runs the script once per entry of args['runs'] (its arguments), and reports, per
    run, its output, whether the program changed, and what it shows at args['addresses'] and for args['types']."""
    import pyghidra
    args = req['args']
    runs = []
    with project_context(args['project'], create=True) as project:
        if not project.getRootFolder().getFile(args['name']):
            _import(project, args)
        with program_context(project, args['name'], write=True) as program:
            sha = program.getExecutableSHA256()
            for script_args in args['runs']:
                before = program.getModificationNumber()
                out, err = pyghidra.ghidra_script(args['script'], project.getProject(), program,
                                                  script_args=script_args, echo_stdout=False, echo_stderr=False)
                runs.append({'stdout': str(out), 'stderr': str(err),
                             'modified': program.getModificationNumber() != before,
                             'shown': _shown(program, args.get('addresses', []), args.get('types', []))})
            functions = len(function_starts(program))
    return {'sha256': str(sha) if sha else None, 'functions': functions, 'runs': runs}
