"""kmap check: every rule on mapping/ plus the staleness of everything generated from it.

--offline runs only what needs no game files (CI); the full check also regenerates the symbol-map
signatures from the verified inputs in $KMAP_HOME and compares them."""
from . import db as dbmod
from . import rules
from .util import say


def run_checks(ctx, offline=True, db=None):
    db = db or dbmod.load(ctx)
    problems = list(db.problems)
    for check in rules.ALL_CHECKS:
        check(db, problems)
    from . import atlas, export, lock, report
    atlas.check_atlas(ctx, db, problems)
    lock.check_lock(ctx, db, problems)
    report.check_summary(ctx, db, problems, offline)
    export.check_exports(ctx, db, problems, offline)
    return db, problems


def cmd_check(ctx, offline=False):
    db, problems = run_checks(ctx, offline)
    for p in problems:
        print(f'FAIL {p}')
    n = sum(1 for _ in db.all_entries())
    mode = 'offline check' if offline else 'check'
    say(f'kmap {mode}: {len(db.files)} files, {n} entries, {len(db.probes)} probes, {len(db.atlas)} atlas pages: '
        + ('ok' if not problems else f'{len(problems)} problem(s)'))
    return 1 if problems else 0
