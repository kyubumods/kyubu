"""Reading the user's game files: only builds whose SHA-256 is pinned in workspace.toml, never writing to
the game folder (Server.exe is read straight out of Server.zip when the game ships it zipped)."""
import zipfile
from pathlib import Path

from .util import KmapError, sha256_bytes, write_if_changed


def read_build(game, build):
    """Returns (bytes, where) for one build, verified against the pin."""
    game = Path(game)
    direct = game / build.file
    if direct.is_file():
        data, where = direct.read_bytes(), str(direct)
    elif build.archive and (game / build.archive).is_file():
        with zipfile.ZipFile(game / build.archive) as z:
            members = [n for n in z.namelist() if n.replace('\\', '/').split('/')[-1].lower() == build.file.lower()]
            if len(members) != 1:
                raise KmapError(f'{game / build.archive} does not contain exactly one {build.file}')
            data, where = z.read(members[0]), f'{game / build.archive}!{members[0]}'
    else:
        raise KmapError(f'{build.file} not found in {game}' + (f' (nor {build.archive})' if build.archive else ''))
    actual = sha256_bytes(data)
    if actual != build.sha256:
        raise KmapError(f'{where} is not the supported {build.id} build; refusing to use it.\n'
                        f'  expected SHA-256 {build.sha256}\n  actual   SHA-256 {actual}')
    return data, where


def stage_inputs(ctx, game, build_ids):
    """Verifies every requested build first, then copies the verified bytes into $KMAP_HOME/inputs/."""
    game = Path(game)
    if not game.is_dir():
        raise KmapError(f'game folder {game} not found')
    staged = {}
    for bid in build_ids:
        build = ctx.ws.build(bid)
        staged[bid] = read_build(game, build)
    paths = {}
    for bid, (data, where) in staged.items():
        build = ctx.ws.build(bid)
        dest = ctx.input_path(build)
        write_if_changed(dest, data)
        paths[bid] = dest
        print(f'{bid}: {where} verified (SHA-256 {build.sha256[:16]}...)')
    return paths


def load_input(ctx, build):
    """The verified copy of a build in the workspace (after `kmap init`), or the build read straight from the game
    folder given to `kmap check --game`."""
    if ctx.game is not None:
        return read_build(ctx.game, build)[0]
    p = ctx.input_path(build)
    if not p.is_file():
        raise KmapError(f'{p} not found: run `kmap init --game <folder>` first')
    data = p.read_bytes()
    if sha256_bytes(data) != build.sha256:
        raise KmapError(f'{p} no longer matches the pinned {build.id} hash: run `kmap init --game <folder>` again')
    return data
