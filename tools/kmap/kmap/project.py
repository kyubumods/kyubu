"""kmap init (the private Ghidra project) and kmap decompile (local reading aid)."""
import json
import shutil

from . import ghidra
from .game import stage_inputs
from .util import KmapError, dump_json, say, sha256_bytes, write_if_changed


def _marker(ctx):
    return ctx.project_dir / 'init.json'


def read_marker(ctx):
    p = _marker(ctx)
    return json.loads(p.read_text(encoding='utf-8')) if p.is_file() else None


def starts_path(ctx, build_id):
    return ctx.project_dir / f'function-starts-{build_id}.json'


def cmd_init(ctx, game, rebuild=False, only=None):
    ws = ctx.ws
    ids = only or list(ws.builds)
    for bid in ids:
        ws.build(bid)
    paths = stage_inputs(ctx, game, ids)    # refuses unverified files before any project exists
    ghidra.require_ghidra(ctx)

    toolchain = {'ghidra': ws.ghidra.version, 'analysis': ws.analysis}
    marker = read_marker(ctx)
    if rebuild and ctx.project_dir.exists():
        say(f'--rebuild: deleting {ctx.project_dir}')
        shutil.rmtree(ctx.project_dir)
        marker = None
    if marker and {k: marker.get(k) for k in toolchain} != toolchain:
        raise KmapError(f'the project in {ctx.project_dir} was built with different toolchain settings '
                        f'(Ghidra {marker.get("ghidra")}); run `kmap init --game <folder> --rebuild`')
    done = (marker or {}).get('builds', {})
    todo = [bid for bid in ids if done.get(bid, {}).get('sha256') != ws.build(bid).sha256]
    if not todo:
        say(f'the Ghidra project in {ctx.project_dir} is up to date ({", ".join(ids)}); nothing to do '
            '(use --rebuild to start over)')
        return 0

    result = ghidra.run_worker(ctx, 'init', {
        'project': str(ctx.project_dir), 'language': ws.analysis['language'], 'compiler': ws.analysis['compiler'],
        'options': ws.analysis.get('options', {}),
        'builds': [{'id': bid, 'path': str(paths[bid])} for bid in todo]})
    marker = dict(toolchain, builds=dict(done))
    for bid, r in result['builds'].items():
        starts = dump_json([f'0x{a:X}' for a in r['starts']])
        write_if_changed(starts_path(ctx, bid), starts)
        marker['builds'][bid] = {'sha256': ws.build(bid).sha256, 'functions': r['functions'],
                                 'function_starts_sha256': sha256_bytes(starts.encode()),
                                 'analysis_seconds': r['seconds']}
        say(f'{bid}: {r["functions"]} functions, analysis took {r["seconds"]} s; '
            f'function starts in {starts_path(ctx, bid)}')
    write_if_changed(_marker(ctx), dump_json(marker))
    pushed_file = ctx.project_dir / 'pushed.json'
    if pushed_file.is_file():      # a fresh import holds nothing kmap pushed
        pushed = json.loads(pushed_file.read_text(encoding='utf-8'))
        write_if_changed(pushed_file, dump_json({k: v for k, v in pushed.items() if k not in todo}))
    say(f'Ghidra project ready: {ctx.project_dir / "kyubu.gpr"}. Next: `kmap push`.')
    return 0


def resolve_targets(ctx, db, target, build_id):
    """Function addresses for a decompile target: a function name/alias/address, a class, or a subsystem."""
    builds = [build_id] if build_id else list(ctx.ws.builds)
    for bid in builds:
        if target.lower().startswith('0x'):
            return bid, [int(target, 16)], target
        fn = db.find_function(bid, target)
        if fn:
            return bid, [fn['address']], fn['name']
    bid = build_id or next(iter(ctx.ws.builds))
    if target in ctx.ws.subsystems:
        addrs = [f['address'] for f in db.functions(bid) if f.get('subsystem') == target and f.get('kind') != 'site']
        return bid, addrs, target
    for bid in builds:
        cls = db.find_class(bid, target)
        if cls:
            addrs = set(f['address'] for f in db.functions(bid) if f['name'].startswith(target + '::'))
            for vt in cls.get('vtable', []):
                addrs.update(a for a in vt.get('slots', []) if a)
            return bid, sorted(addrs), target
    raise KmapError(f'{target!r} is not a mapped function, alias, address, class or subsystem')


def cmd_decompile(ctx, target, build_id=None):
    from . import db as dbmod
    from . import roundtrip
    db = dbmod.load(ctx)
    bid, addrs, label = resolve_targets(ctx, db, target, build_id)
    if not addrs:
        raise KmapError(f'nothing to decompile for {label} in {bid}')
    roundtrip.cmd_push(ctx, False, [bid], quiet=True)    # current names and types first
    out = ctx.decomp_dir / bid
    r = ghidra.run_worker(ctx, 'decompile', {'project': str(ctx.project_dir), 'build': bid,
                                             'addresses': addrs, 'out': str(out)})
    for p in r['written']:
        say(p)
    say(f'{len(r["written"])} file(s) under {out}. Reading aid only: never commit or share them.')
    return 0 if r['written'] else 1
