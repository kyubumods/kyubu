"""Twin tasks inside Ghidra (add-twin-matching): import the twin's Cube.exe with its PDB into the twin's own project,
and read its facts. The project is $KMAP_HOME/twin/ghidra/, never kmap's; one program per variant id."""
import json
import time
from pathlib import Path

from .common import addr, program_context, project_context, transaction
from .project import _apply_options
from .survey import function_facts, rtti_symbols


def task_twin_init(req):
    import pyghidra
    from java.io import File
    from ghidra.app.plugin.core.analysis import PdbUniversalAnalyzer
    from ghidra.program.model.lang import CompilerSpecID, LanguageID
    from ghidra.program.util import DefaultLanguageService
    args = req['args']
    out = {'programs': {}}
    with project_context(args['project'], create=True) as project:
        for p in args['programs']:
            t0 = time.time()
            root = project.getRootFolder()
            old = root.getFile(p['id'])
            if old:
                old.delete()
            lang = DefaultLanguageService.getLanguageService().getLanguage(LanguageID(args['language']))
            comp = lang.getCompilerSpecByID(CompilerSpecID(args['compiler']))
            program = project.importProgram(File(p['exe']), lang, comp)
            if program is None:
                raise RuntimeError(f'Ghidra could not import {p["exe"]}')
            try:
                _apply_options(program, args['options'])
                with transaction(program, 'kmap twin: the PDB'):
                    PdbUniversalAnalyzer.setPdbFileOption(program, File(p['pdb']))
                    PdbUniversalAnalyzer.setAllowUntrustedOption(program, False)
                print(f'kmap: analysing {p["id"]} with its PDB (this takes a while)', flush=True)
                pyghidra.analyze(program)
                project.saveAs(program, '/', p['id'], True)
                fm = program.getFunctionManager()
                total = named = 0
                for f in fm.getFunctions(True):
                    if f.isExternal():
                        continue
                    total += 1
                    named += str(f.getSymbol().getSource()) == 'IMPORTED'
            finally:
                project.close(program)
            out['programs'][p['id']] = {'functions': total, 'pdb_named': named, 'seconds': round(time.time() - t0)}
            print(f'kmap: {p["id"]}: {total} functions, {named} named by the PDB, {round(time.time() - t0)} s',
                  flush=True)
    return out


def _source_file(sfm, program, entry):
    """The source file of a function's entry from the PDB's line numbers, or None."""
    if sfm is None:
        return None
    try:
        entries = sfm.getSourceMapEntries(entry)
    except Exception:   # an address without line information
        return None
    paths = sorted(str(e.getSourceFile().getPath()) for e in entries) if entries is not None else []
    return paths[0] if paths else None


def struct_types(program):
    """Every structure in the program's data types: path, size and defined fields, sorted."""
    out = []
    it = program.getDataTypeManager().getAllStructures()
    while it.hasNext():
        s = it.next()
        fields = []
        for c in s.getDefinedComponents():
            name = c.getFieldName()
            fields.append({'offset': int(c.getOffset()), 'name': str(name) if name is not None else None,
                           'type': str(c.getDataType().getName()), 'size': int(c.getLength())})
        out.append({'path': str(s.getPathName()), 'size': int(s.getLength()),
                    'fields': sorted(fields, key=lambda f: (f['offset'], f['name'] or ''))})
    return sorted(out, key=lambda t: t['path'])


def task_twin_survey(req):
    args = req['args']
    written = {}
    with project_context(args['project']) as project:
        for vid in args['programs']:
            with program_context(project, vid) as program:
                facts = function_facts(program)
                try:
                    sfm = program.getSourceFileManager()
                except Exception:
                    sfm = None
                fm = program.getFunctionManager()
                for f in facts:
                    f['pdb_name'] = f['name'] if f['name_source'] == 'IMPORTED' else None
                    fn = fm.getFunctionAt(addr(program, f['entry']))
                    f['source_file'] = _source_file(sfm, program, fn.getEntryPoint()) if fn is not None else None
                symbols = rtti_symbols(program)
                types = struct_types(program)
            out = Path(args['out']) / vid
            out.mkdir(parents=True, exist_ok=True)
            (out / 'functions.json').write_text(json.dumps(facts, sort_keys=True, indent=0, separators=(',', ':'))
                                                + '\n', encoding='utf-8')
            (out / 'ghidra-rtti.json').write_text(json.dumps(symbols, sort_keys=True, indent=1) + '\n',
                                                  encoding='utf-8')
            (out / 'types.json').write_text(json.dumps(types, sort_keys=True, indent=0, separators=(',', ':'))
                                            + '\n', encoding='utf-8')
            written[vid] = {'path': str(out), 'functions': len(facts), 'types': len(types),
                            'source_lines': sfm is not None}
    return {'programs': written}
