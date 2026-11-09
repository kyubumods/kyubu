"""Runs one headless Ghidra task inside the PyGhidra venv (started by kmap.ghidra.run_worker).

    python -m kmap.worker <request.json> <result.json>

The request names the task and its arguments; the result is JSON. Ghidra's own settings and caches go
under $KMAP_HOME, so the workspace is self-contained.
"""
import importlib
import json
import sys
import traceback
from pathlib import Path

TASKS = {
    'init': 'kmap.gtasks.project:task_init',
    'function_starts': 'kmap.gtasks.project:task_function_starts',
    'decompile': 'kmap.gtasks.project:task_decompile',
    'push': 'kmap.gtasks.roundtrip:task_push',
    'snapshot': 'kmap.gtasks.roundtrip:task_snapshot',
    'survey': 'kmap.gtasks.survey:task_survey',
    'rename': 'kmap.gtasks.roundtrip:task_rename',
    'apply_script': 'kmap.gtasks.roundtrip:task_apply_script',
    'twin_init': 'kmap.gtasks.twin:task_twin_init',
    'twin_survey': 'kmap.gtasks.twin:task_twin_survey',
}


def start_ghidra(req):
    import pyghidra
    home = Path(req['home'])
    launcher = pyghidra.HeadlessPyGhidraLauncher(install_dir=Path(req['ghidra']))
    launcher.add_vmargs(f'-Xmx{req["heap"]}',
                        f'-Dapplication.settingsdir={home / "ghidra-settings"}',
                        f'-Dapplication.cachedir={home / "ghidra-cache"}')
    launcher.start()


def main(req_path, resp_path):
    req = json.loads(Path(req_path).read_text(encoding='utf-8'))
    try:
        mod_name, fn_name = TASKS[req['task']].split(':')
        start_ghidra(req)
        fn = getattr(importlib.import_module(mod_name), fn_name)
        result = fn(req) or {}
    except Exception as e:   # reported to kmap as a task error, with the trace on stderr
        traceback.print_exc()
        result = {'error': f'{type(e).__name__}: {e}'}
    Path(resp_path).write_text(json.dumps(result, sort_keys=True), encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
