"""Where kmap works: the repository (mapping/), the private workspace ($KMAP_HOME) and the pinned
toolchain and builds recorded in mapping/workspace.toml."""
import os
import tomllib
from dataclasses import dataclass
from functools import cached_property
from pathlib import Path

from .util import KmapError

PACKAGE_REPO = Path(__file__).resolve().parents[3]   # tools/kmap/kmap/config.py -> repository root


def default_home():
    if os.name == 'nt':
        base = os.environ.get('LOCALAPPDATA') or os.path.join(os.path.expanduser('~'), 'AppData', 'Local')
        return Path(base) / 'kyubu-mapping'
    return Path(os.path.expanduser('~')) / '.kyubu-mapping'


def git_worktree(path):
    """The git working tree that contains `path` (which need not exist yet), or None."""
    p = Path(path).expanduser().resolve()
    for d in (p, *p.parents):
        if (d / '.git').exists():
            return d
    return None


@dataclass(frozen=True)
class Build:
    id: str
    file: str
    archive: str | None
    sha256: str
    text_sha256: str
    timestamp: int
    image_base: int
    image_size: int
    text_start: int
    text_end: int
    retired: bool = False   # kept as the record other builds' entries were ported from; the loader no longer runs on it

    def in_image(self, addr):
        return self.image_base <= addr < self.image_base + self.image_size

    def in_text(self, addr):
        return self.text_start <= addr < self.text_end

    def image_range(self):
        return f'0x{self.image_base:X}-0x{self.image_base + self.image_size - 1:X}'


@dataclass(frozen=True)
class GhidraPin:
    version: str
    archive: str
    url: str
    sha256: str
    folder: str
    max_heap: str
    jdk: int


class Workspace:
    """mapping/workspace.toml."""

    def __init__(self, path):
        self.path = Path(path)
        if not self.path.is_file():
            raise KmapError(f'{self.path} not found: kmap needs a repository with mapping/workspace.toml')
        try:
            raw = tomllib.loads(self.path.read_text(encoding='utf-8'))
        except tomllib.TOMLDecodeError as e:
            raise KmapError(f'{self.path}: {e}')
        self.raw = raw

        def need(table, key, kind, where):
            if key not in table or not isinstance(table[key], kind):
                raise KmapError(f'{self.path}: [{where}] needs `{key}` ({kind.__name__})')
            return table[key]

        g = need(raw, 'ghidra', dict, 'ghidra')
        self.ghidra = GhidraPin(
            version=need(g, 'version', str, 'ghidra'), archive=need(g, 'archive', str, 'ghidra'),
            url=need(g, 'url', str, 'ghidra'), sha256=need(g, 'sha256', str, 'ghidra').lower(),
            folder=need(g, 'folder', str, 'ghidra'), max_heap=g.get('max_heap', '4G'), jdk=g.get('jdk', 21))
        self.analysis = need(raw, 'analysis', dict, 'analysis')
        need(self.analysis, 'language', str, 'analysis')
        need(self.analysis, 'compiler', str, 'analysis')
        self.builds = {}
        for b in need(raw, 'build', list, 'build'):
            build = Build(
                id=need(b, 'id', str, 'build'), file=need(b, 'file', str, 'build'), archive=b.get('archive'),
                sha256=need(b, 'sha256', str, 'build').lower(), text_sha256=need(b, 'text_sha256', str, 'build').lower(),
                timestamp=need(b, 'timestamp', int, 'build'), image_base=need(b, 'image_base', int, 'build'),
                image_size=need(b, 'image_size', int, 'build'), text_start=need(b, 'text_start', int, 'build'),
                text_end=need(b, 'text_end', int, 'build'), retired=b.get('retired', False))
            if not isinstance(build.retired, bool):
                raise KmapError(f'{self.path}: [build] {build.id}: `retired` must be true or false')
            self.builds[build.id] = build
        self.cuwo = need(raw, 'cuwo', dict, 'cuwo')
        need(self.cuwo, 'commit', str, 'cuwo')
        self.subsystems = need(raw, 'subsystems', list, 'subsystems')
        self.atlas_pages = [s for s in self.subsystems if s not in raw.get('no_atlas_page', [])]

    @property
    def targets(self):
        """The builds the loader runs on: every build that is not retired, in workspace order."""
        return {k: b for k, b in self.builds.items() if not b.retired}

    def build(self, build_id):
        if build_id not in self.builds:
            raise KmapError(f'unknown build {build_id!r}; supported: {", ".join(self.builds)}')
        return self.builds[build_id]


class Context:
    """Everything a command needs to know about where it runs."""

    def __init__(self, repo=None):
        self.repo = Path(repo or os.environ.get('KMAP_REPO') or PACKAGE_REPO).resolve()
        self.mapping = self.repo / 'mapping'
        env_home = os.environ.get('KMAP_HOME')
        self.home = Path(env_home).expanduser().resolve() if env_home else default_home()
        self.env_ghidra = os.environ.get('KMAP_GHIDRA')
        self.game = None        # kmap check --game: read the builds from this folder, not $KMAP_HOME/inputs

    def check_home(self):
        tree = git_worktree(self.home)
        if tree:
            raise KmapError(
                f'KMAP_HOME is {self.home}, which is inside the git working tree {tree}.\n'
                'The workspace holds the Ghidra project, survey output and decompiler dumps, all derived from '
                "the game's code; they must never end up in a repository.\n"
                f'Unset KMAP_HOME to use the default location ({default_home()}) or choose a folder outside '
                'any checkout.')

    @cached_property
    def ws(self):
        return Workspace(self.mapping / 'workspace.toml')

    @property
    def ghidra_dir(self):
        return Path(self.env_ghidra).expanduser().resolve() if self.env_ghidra else self.home / self.ws.ghidra.folder

    @property
    def project_dir(self):
        return self.home / 'ghidra'

    @property
    def survey_dir(self):
        return self.home / 'survey'

    @property
    def decomp_dir(self):
        return self.home / 'decomp'

    def input_path(self, build):
        return self.home / 'inputs' / build.id / build.file

    @property
    def local_settings(self):
        p = self.home / 'local.toml'
        return tomllib.loads(p.read_text(encoding='utf-8')) if p.is_file() else {}
