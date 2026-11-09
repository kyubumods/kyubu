"""kmap - the Kyubu mapping workspace.

  kmap ghidra install [--jdk <folder>]   download, verify and unpack the pinned Ghidra; set up PyGhidra
  kmap ghidra status                     show the pin, the installed Ghidra and the JDK in use
  kmap init --game <folder> [--rebuild]  build the private Ghidra project from verified game files
  kmap push [--dry-run]                  apply mapping/ to the Ghidra project
  kmap pull [--write]                    turn changes made in Ghidra into mapping/ edits (diff; --write applies)
  kmap survey [--pass <name>]            run the survey passes (raw JSON under $KMAP_HOME/survey/)
  kmap survey query ...                  look things up in the strings and imports index
  kmap report                            write mapping/survey-summary.md
  kmap import-cuwo <checkout> [--write]  propose candidate types and enums from a cuwo checkout
  kmap export [--force]                  regenerate symbol maps, headers, Lua types, docs, Ghidra mappings
  kmap check [--offline | --game <dir>]  validate mapping/ and the generated artifacts (--offline: no game files,
                                         for CI; --game: read the builds from a game folder, for the contract level)
  kmap lock [--release [<version>]]      list, or on release add, the exported names in mapping/exported-names.lock
  kmap decompile <symbol|class|subsystem> write pseudo-C for reading under $KMAP_HOME/decomp/
  kmap probe new <id> | list | replay <id> | report <id>
                                         create, list, replay or report on probe records (mapping/probes/)
  kmap twin init | survey | match [--variant <id>] [--combine]
                                         the Infinite-Cubes debug build ($KMAP_HOME/twin/): import it with its
                                         PDB into its own Ghidra project, then read its facts

Environment: KMAP_HOME (private workspace; default %LOCALAPPDATA%\\kyubu-mapping on Windows,
~/.kyubu-mapping elsewhere; never inside a git checkout), KMAP_GHIDRA (an existing Ghidra of the pinned
version), KMAP_JDK (a JDK 21 folder).
"""
import argparse
import sys

from .util import KmapError


def build_parser():
    p = argparse.ArgumentParser(prog='kmap', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--repo', help='Kyubu checkout to work on (default: the one containing kmap)')
    sub = p.add_subparsers(dest='cmd', required=True, metavar='<command>')

    g = sub.add_parser('ghidra', help='install or inspect the pinned Ghidra')
    gs = g.add_subparsers(dest='ghidra_cmd', required=True)
    gi = gs.add_parser('install')
    gi.add_argument('--jdk', help='JDK 21 folder to remember for this workspace')
    gi.add_argument('--skip-python', action='store_true', help='do not set up the PyGhidra venv')
    gs.add_parser('status')

    i = sub.add_parser('init', help='build the private Ghidra project')
    i.add_argument('--game', required=True, help='game folder with Cube.exe and Server.exe or Server.zip')
    i.add_argument('--rebuild', action='store_true', help='delete and rebuild an existing project')
    i.add_argument('--build', action='append', help='only this build (repeatable)')

    ps = sub.add_parser('push', help='apply mapping/ to the Ghidra project')
    ps.add_argument('--dry-run', action='store_true')
    ps.add_argument('--build', action='append')

    pl = sub.add_parser('pull', help='propose mapping/ edits from the Ghidra project')
    pl.add_argument('--write', action='store_true', help='write the proposed edits')
    pl.add_argument('--build', action='append')

    s = sub.add_parser('survey', help='run the survey passes')
    s.add_argument('--pass', dest='passes', action='append', help='run only this pass (repeatable)')
    s.add_argument('--build', action='append')
    ss = s.add_subparsers(dest='survey_cmd')
    q = ss.add_parser('query', help='look up strings, imports or functions in the survey index')
    q.add_argument('--build', default='cube-0x51D2CE9C')
    qg = q.add_mutually_exclusive_group(required=True)
    qg.add_argument('--string', help='strings containing this text')
    qg.add_argument('--import', dest='imp', help='callers of an import, as dll!function')
    qg.add_argument('--function', help='what the survey knows about a function address')
    sc = ss.add_parser('commit', help='propose reviewed survey results as mapping/ entries')
    sc.add_argument('--write', action='store_true')

    sub.add_parser('report', help='write mapping/survey-summary.md')

    c = sub.add_parser('import-cuwo', help='propose candidates from a cuwo checkout')
    c.add_argument('checkout')
    c.add_argument('--commit', help='accept this commit instead of the pinned one')
    c.add_argument('--write', action='store_true', help='write mapping/types/cuwo-*.toml')

    e = sub.add_parser('export', help='regenerate every exported artifact')
    e.add_argument('--force', action='store_true', help='export even when kmap check finds errors')
    e.add_argument('--offline', action='store_true', help='skip artifacts that need the game files')

    k = sub.add_parser('check', help='validate mapping/ and the generated artifacts')
    kg = k.add_mutually_exclusive_group()
    kg.add_argument('--offline', action='store_true', help='only checks that need no game files')
    kg.add_argument('--game', metavar='FOLDER',
                    help='read the builds from this game folder (verified, read-only) instead of $KMAP_HOME/inputs')

    lk = sub.add_parser('lock', help='the names shipped in releases (mapping/exported-names.lock)')
    lk.add_argument('--release', nargs='?', const=True, default=None, metavar='VERSION',
                    help='add every exported name that is not locked yet, as shipped in VERSION '
                         '(default: the project version in CMakeLists.txt)')

    d = sub.add_parser('decompile', help='write pseudo-C under $KMAP_HOME/decomp/')
    d.add_argument('target', help='function name or address, class name, or subsystem')
    d.add_argument('--build', default=None)

    pr = sub.add_parser('probe', help='probe records')
    prs = pr.add_subparsers(dest='probe_cmd', required=True)
    pn = prs.add_parser('new')
    pn.add_argument('id')
    pn.add_argument('--build', default='cube-0x51D2CE9C')
    prs.add_parser('list')
    prp = prs.add_parser('replay', help="run a record's [replay] unattended and report per entry (docs/probes.md)")
    prp.add_argument('id')
    prp.add_argument('--game', required=True, help='the game folder (only read: the replay runs in a copy)')
    prp.add_argument('--kyubu', required=True,
                     help='a Kyubu release folder (Windows) or the CMake build folder (Linux, Wine)')
    prp.add_argument('--probe', default=None,
                     help='the built kyubu_probe mod folder (default: <build>/dev-mods/kyubu_probe)')
    prp.add_argument('--out', default=None, help='where the report and the log go (default: $KMAP_HOME/replays/)')
    prp.add_argument('--keep', action='store_true', help='keep the game copy in $KMAP_HOME/tmp/')
    pra = prs.add_parser('replay-all', help='replay several records, -j at once, and print one summary table '
                                            '(records that use port 12345 run one at a time)')
    pra.add_argument('ids', nargs='*', help='record ids')
    pra.add_argument('--all', action='store_true', help='every replayable record of a build that is not retired')
    pra.add_argument('-j', '--jobs', type=int, default=1, help='how many replays run at once (default 1)')
    pra.add_argument('--game', required=True)
    pra.add_argument('--kyubu', required=True)
    pra.add_argument('--probe', default=None)
    pra.add_argument('--out', default=None, help='the folder for one subfolder per record and summary.md')
    pra.add_argument('--keep', action='store_true')
    prr = prs.add_parser('report', help="report per entry from the log of a session played by hand with the record's "
                                        'script as probe.cfg (docs/probes.md)')
    prr.add_argument('id')
    prr.add_argument('--log', required=True, help="the game's kyubu/logs/latest.log (only read)")
    prr.add_argument('--out', default=None, help='where the report and a copy of the log go '
                                                 '(default: $KMAP_HOME/replays/)')

    tw = sub.add_parser('twin', help='the Infinite-Cubes debug build matched to Cube.exe')
    tws = tw.add_subparsers(dest='twin_cmd', required=True)
    twi = tws.add_parser('init', help="import the twin's Cube.exe with its PDB into $KMAP_HOME/twin/ghidra/")
    twi.add_argument('--variant', action='append', help='only this variant id from build.toml (repeatable)')
    twi.add_argument('--rebuild', action='store_true', help='import and analyse again even when up to date')
    twv = tws.add_parser('survey', help="the twin's facts, PDB names, source files, RTTI, strings and types")
    twv.add_argument('--variant', action='append', help='only this variant id from build.toml (repeatable)')
    twm = tws.add_parser('match', help="pair the twin's functions with Cube.exe's: matches.json and names.tsv")
    twm.add_argument('--variant', action='append', help='only this variant id from build.toml (repeatable)')
    twm.add_argument('--combine', action='store_true',
                     help='also vote over the variants by twin name: $KMAP_HOME/twin/combined/')
    return p


def run(args, ctx):
    from . import ghidra
    if args.cmd == 'ghidra':
        return ghidra.cmd_install(ctx, args.jdk, args.skip_python) if args.ghidra_cmd == 'install' \
            else ghidra.cmd_status(ctx)
    ghidra.check_ghidra_if_configured(ctx)
    if args.cmd == 'init':
        from . import project
        return project.cmd_init(ctx, args.game, args.rebuild, args.build)
    if args.cmd == 'push':
        from . import roundtrip
        return roundtrip.cmd_push(ctx, args.dry_run, args.build)
    if args.cmd == 'pull':
        from . import roundtrip
        return roundtrip.cmd_pull(ctx, args.write, args.build)
    if args.cmd == 'survey':
        from . import survey
        if args.survey_cmd == 'query':
            return survey.cmd_query(ctx, args)
        if args.survey_cmd == 'commit':
            return survey.cmd_commit(ctx, args.write)
        return survey.cmd_survey(ctx, args.passes, args.build)
    if args.cmd == 'report':
        from . import report
        return report.cmd_report(ctx)
    if args.cmd == 'import-cuwo':
        from . import cuwo
        return cuwo.cmd_import(ctx, args.checkout, args.commit, args.write)
    if args.cmd == 'export':
        from . import export
        return export.cmd_export(ctx, args.force, args.offline)
    if args.cmd == 'check':
        from . import check
        if args.game:
            from pathlib import Path
            ctx.game = Path(args.game)
            if not ctx.game.is_dir():
                raise KmapError(f'game folder {ctx.game} not found')
        return check.cmd_check(ctx, args.offline)
    if args.cmd == 'lock':
        from . import lock
        return lock.cmd_lock(ctx, args.release)
    if args.cmd == 'decompile':
        from . import project
        return project.cmd_decompile(ctx, args.target, args.build)
    if args.cmd == 'probe':
        from . import probes
        return probes.cmd_probe(ctx, args)
    if args.cmd == 'twin':
        from . import twin
        if args.twin_cmd == 'init':
            return twin.cmd_init(ctx, args.variant, args.rebuild)
        if args.twin_cmd == 'match':
            return twin.cmd_match(ctx, args.variant, args.combine)
        return twin.cmd_survey(ctx, args.variant)
    raise KmapError(f'unknown command {args.cmd}')


def main(argv=None):
    from .config import Context
    args = build_parser().parse_args(argv)
    try:
        ctx = Context(args.repo)
        ctx.check_home()
        return run(args, ctx) or 0
    except KmapError as e:
        print(f'kmap: error: {e}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print('kmap: interrupted', file=sys.stderr)
        return 130


if __name__ == '__main__':
    sys.exit(main())
