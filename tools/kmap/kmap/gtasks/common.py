"""Helpers for code that runs inside Ghidra (imported only by kmap.worker after the JVM started)."""
import contextlib
from pathlib import Path

PROJECT_NAME = 'kyubu'


def open_project(location, create=False):
    from ghidra.base.project import GhidraProject
    from ghidra.framework.model import ProjectLocator
    location = Path(location)
    if ProjectLocator(str(location), PROJECT_NAME).exists():
        return GhidraProject.openProject(str(location), PROJECT_NAME, True)
    if not create:
        raise FileNotFoundError(f'no Ghidra project at {location}: run `kmap init` first')
    location.mkdir(parents=True, exist_ok=True)
    return GhidraProject.createProject(str(location), PROJECT_NAME, False)


@contextlib.contextmanager
def project_context(location, create=False):
    project = open_project(location, create)
    try:
        yield project
    finally:
        project.close()


@contextlib.contextmanager
def program_context(project, build_id, write=False):
    """Opens /<build_id>; saves on a clean exit when `write` is set."""
    if not project.getRootFolder().getFile(build_id):
        raise FileNotFoundError(f'{build_id} is not in the Ghidra project: run `kmap init`')
    program = project.openProgram('/', build_id, not write)
    ok = False
    try:
        yield program
        ok = True
    finally:
        if write and ok:
            project.save(program)
        project.close(program)


@contextlib.contextmanager
def transaction(program, what):
    tx = program.startTransaction(what)
    ok = False
    try:
        yield
        ok = True
    finally:
        program.endTransaction(tx, ok)


def addr(program, va):
    return program.getAddressFactory().getDefaultAddressSpace().getAddress(va)


def va(address):
    return int(address.getOffset())


def monitor():
    from ghidra.util.task import TaskMonitor
    return TaskMonitor.DUMMY


def function_starts(program):
    fm = program.getFunctionManager()
    return sorted(va(f.getEntryPoint()) for f in fm.getFunctions(True) if not f.isExternal())
