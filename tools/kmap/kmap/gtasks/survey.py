"""Survey facts inside Ghidra: one deterministic record per function, written as sorted JSON to
$KMAP_HOME/survey/<build>/functions.json. The survey passes (kmap.survey) work on these facts and on the
verified executable; nothing here names anything."""
import hashlib
import json
from pathlib import Path

from .common import program_context, project_context, va


def _external_name(program, ref):
    sym = program.getSymbolTable().getPrimarySymbol(ref.getToAddress()) if ref else None
    ext = program.getExternalManager().getExternalLocation(sym) if sym is not None and sym.isExternal() else None
    if ext is None:
        return None
    return f'{str(ext.getLibraryName()).lower()}!{ext.getLabel()}'


def _callee(program, fm, ins):
    """(entry of the called function, or None; 'dll!name' for imports, or None)."""
    for ref in ins.getReferencesFrom():
        if not ref.getReferenceType().isCall():
            continue
        to = ref.getToAddress()
        if to.isExternalAddress():
            return None, _external_name(program, ref)
        f = fm.getFunctionAt(to)
        if f is None:
            # CALL dword ptr [IAT]: a data reference to the pointer, which refers to the external
            for r2 in program.getReferenceManager().getReferencesFrom(to):
                if r2.getToAddress().isExternalAddress():
                    return None, _external_name(program, r2)
            return va(to), None
        if f.isThunk():
            t = f.getThunkedFunction(True)
            if t is not None and t.isExternal():
                loc = t.getExternalLocation()
                return va(f.getEntryPoint()), f'{str(loc.getLibraryName()).lower()}!{loc.getLabel()}'
        return va(f.getEntryPoint()), None
    return None, None


def function_facts(program):
    from ghidra.program.model.lang import OperandType
    fm = program.getFunctionManager()
    listing = program.getListing()
    mem = program.getMemory()
    relocs = set()
    it = program.getRelocationTable().getRelocations()
    while it.hasNext():
        r = it.next()
        a = va(r.getAddress())
        relocs.update(range(a, a + 4))
    out = []
    for f in fm.getFunctions(True):
        if f.isExternal():
            continue
        entry = va(f.getEntryPoint())
        body = f.getBody()
        ranges = [[va(r.getMinAddress()), va(r.getMaxAddress()) + 1] for r in body.getAddressRanges()]
        size = sum(b - a for a, b in ranges)
        masked = bytearray()
        mnem = []
        calls, imports, data_refs, call_seq, vt_stores = [], [], set(), [], []
        recent_pushes = []
        n_ins = 0
        for ins in listing.getInstructions(body, True):
            n_ins += 1
            a = va(ins.getAddress())
            raw = bytes(b & 0xFF for b in ins.getBytes())
            masked += bytes(0 if a + i in relocs else b for i, b in enumerate(raw))
            m = str(ins.getMnemonicString())
            mnem.append(m)
            for ref in ins.getReferencesFrom():
                rt = ref.getReferenceType()
                if rt.isData() and not ref.getToAddress().isExternalAddress() and mem.contains(ref.getToAddress()):
                    data_refs.add(va(ref.getToAddress()))
            if m == 'PUSH' and ins.getNumOperands() == 1 and OperandType.isScalar(ins.getOperandType(0)):
                s = ins.getScalar(0)
                recent_pushes.append(int(s.getUnsignedValue()) if s is not None else None)
                recent_pushes = recent_pushes[-3:]
            elif m == 'CALL':
                callee, imp = _callee(program, fm, ins)
                if callee is not None:
                    calls.append(callee)
                if imp is not None:
                    imports.append(imp)
                call_seq.append([imp if imp is not None else callee, [p for p in recent_pushes if p is not None]])
                recent_pushes = []
            elif m == 'MOV' and ins.getNumOperands() == 2 and OperandType.isScalar(ins.getOperandType(1)) \
                    and OperandType.isDynamic(ins.getOperandType(0)):
                s = ins.getScalar(1)
                objs = ins.getOpObjects(0)
                disp = 0
                regs = 0
                for o in objs:
                    cls = str(o.getClass().getSimpleName())
                    if cls == 'Register':
                        regs += 1
                    elif cls == 'Scalar':
                        disp = int(o.getSignedValue())
                if s is not None and regs == 1:
                    vt_stores.append([disp, int(s.getUnsignedValue())])
            elif m not in ('PUSH',) and recent_pushes and m.startswith('J'):
                recent_pushes = []
        thunk_target = None
        if f.isThunk():
            t = f.getThunkedFunction(True)
            if t is not None:
                if t.isExternal():
                    loc = t.getExternalLocation()
                    thunk_target = f'{str(loc.getLibraryName()).lower()}!{loc.getLabel()}'
                else:
                    thunk_target = va(t.getEntryPoint())
        out.append({
            'entry': entry, 'name': str(f.getName(True)), 'name_source': str(f.getSymbol().getSource()),
            'ranges': ranges, 'size': size, 'instructions': n_ins,
            'bytes_sha1': hashlib.sha1(bytes(masked)).hexdigest(),
            'mnemonics_sha1': hashlib.sha1('|'.join(mnem).encode()).hexdigest(),
            'calls': sorted(set(calls)), 'imports': sorted(set(imports)), 'data_refs': sorted(data_refs),
            'call_seq': call_seq, 'vtable_stores': sorted(set(map(tuple, vt_stores))),
            'thunk_target': thunk_target,
        })
    return out


def rtti_symbols(program):
    """Names Ghidra's RTTI analyzer gave to type descriptors, complete-object locators and vtables."""
    out = {'type_descriptors': {}, 'locators': {}, 'vtables': {}}
    it = program.getSymbolTable().getAllSymbols(True)
    kinds = {'RTTI_Type_Descriptor': 'type_descriptors', 'RTTI_Complete_Object_Locator': 'locators',
             'vftable': 'vtables'}
    while it.hasNext():
        s = it.next()
        n = str(s.getName())
        key = kinds.get(n) or ('locators' if n.startswith('RTTI_Complete_Object_Locator') else
                               'vtables' if n.startswith('vftable') else None)
        if key:
            ns = s.getParentNamespace()
            out[key][str(va(s.getAddress()))] = str(ns.getName(True)) if ns and not ns.isGlobal() else ''
    return out


def task_survey(req):
    args = req['args']
    written = {}
    with project_context(args['project']) as project:
        for bid in args['builds']:
            with program_context(project, bid) as program:
                facts = function_facts(program)
                symbols = rtti_symbols(program)
            out = Path(args['out']) / bid
            out.mkdir(parents=True, exist_ok=True)
            (out / 'functions.json').write_text(json.dumps(facts, sort_keys=True, indent=0, separators=(',', ':'))
                                                + '\n', encoding='utf-8')
            (out / 'ghidra-rtti.json').write_text(json.dumps(symbols, sort_keys=True, indent=1) + '\n',
                                                  encoding='utf-8')
            written[bid] = {'path': str(out), 'functions': len(facts)}
    return {'builds': written}
