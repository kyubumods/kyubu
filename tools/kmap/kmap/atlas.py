"""The atlas lint (game-atlas, "Page per subsystem"): mapping/atlas/<subsystem>.md, checked by kmap check.

- Every subsystem in workspace.toml (except no_atlas_page) has a page, and no other page exists besides
  primitives.md (the hand-off inventory). A repository without mapping/atlas/ has no atlas yet and is not checked
  for missing pages.
- Every subsystem page has the eight `## ` sections of SECTIONS, in that order; other sections may be added.
- The first line of "Depth" is `not started`, or `reconnaissance, reviewed YYYY-MM-DD` / `deep dive, reviewed
  YYYY-MM-DD`.
- Every code span outside fenced blocks that is a qualified name (`cube::Creature`, `cube::Creature::update()`,
  `cube::CreatureData.hp`, `cube::AbilityId::TELEPORT`) is a reference and resolves to a mapping entry's name or
  alias. This applies to every page, primitives.md included.
- mapping/atlas/_template.md is the page template: not a page, but it must have the sections too.
"""
import datetime
import re

from .db import Problem

SECTIONS = ('Purpose', 'Key classes', 'Entry points', 'Data flow', 'Client and server', 'Hook points of interest',
            'Open questions', 'Depth')
EXTRA_PAGES = ('primitives',)
TEMPLATE = '_template.md'
DEPTH = re.compile(r'^(not started|(reconnaissance|deep dive), reviewed (\d{4}-\d{2}-\d{2}))\.?$')
FENCE = re.compile(r'^\s*(```|~~~)')
CODE_SPAN = re.compile(r'(`+)(.+?)\1')
QUALIFIED = re.compile(r'^~?[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)+(?:\.[A-Za-z_]\w*)?$')


def references(text):
    """(line, name) of every qualified-name code span outside fenced code blocks."""
    out = []
    fenced = False
    for n, line in enumerate(text.splitlines(), 1):
        if FENCE.match(line):
            fenced = not fenced
            continue
        if fenced:
            continue
        for m in CODE_SPAN.finditer(line):
            name = m.group(2).strip()
            if name.endswith('()'):
                name = name[:-2]
            if QUALIFIED.match(name):
                out.append((n, name))
    return out


def sections(text):
    """{heading: (line, [body lines])} of the `## ` sections, in order."""
    out = {}
    fenced = False
    current = None
    for n, line in enumerate(text.splitlines(), 1):
        if FENCE.match(line):
            fenced = not fenced
        if not fenced and line.startswith('## '):
            current = line[3:].strip()
            out.setdefault(current, (n, []))
            continue
        if current is not None:
            out[current][1].append(line)
    return out


def check_sections(rel, text, problems):
    found = sections(text)
    missing = [s for s in SECTIONS if s not in found]
    for s in missing:
        problems.append(Problem(rel, f'missing section "## {s}"'))
    present = [s for s in found if s in SECTIONS]
    if not missing and present != list(SECTIONS):
        problems.append(Problem(rel, 'sections out of order; expected ' + ', '.join(SECTIONS)))
    return found


def page_depth(found):
    """(depth, date) from a page's Depth section: depth is 'not started', 'reconnaissance' or 'deep dive';
    None when the first line is not one of the allowed forms."""
    line = next((l.strip() for l in found.get('Depth', (0, []))[1] if l.strip()), '')
    m = DEPTH.match(line)
    if not m:
        return None
    if m.group(1) == 'not started':
        return 'not started', None
    try:
        return m.group(2), datetime.date.fromisoformat(m.group(3))
    except ValueError:
        return None


def check_atlas(ctx, db, problems, *args):
    root = ctx.mapping / 'atlas'
    if not root.is_dir():
        return
    names = db.names()
    pages = set(ctx.ws.atlas_pages)
    for s in ctx.ws.atlas_pages:
        if s not in db.atlas:
            problems.append(Problem(f'mapping/atlas/{s}.md', f'missing: every subsystem has an atlas page '
                                                            f'(copy mapping/atlas/{TEMPLATE})'))
    template = root / TEMPLATE
    if template.is_file():
        check_sections(db.rel(template), template.read_text(encoding='utf-8'), problems)
    else:
        problems.append(Problem(f'mapping/atlas/{TEMPLATE}', 'missing: the atlas page template'))
    for stem, path in sorted(db.atlas.items()):
        rel = db.rel(path)
        text = path.read_text(encoding='utf-8')
        if stem in pages:
            found = check_sections(rel, text, problems)
            if 'Depth' in found and page_depth(found) is None:
                line = found['Depth'][0]
                problems.append(Problem(f'{rel}:{line}', 'the Depth section starts with `not started`, '
                                        '`reconnaissance, reviewed YYYY-MM-DD` or `deep dive, reviewed YYYY-MM-DD`'))
        elif stem not in EXTRA_PAGES:
            problems.append(Problem(rel, f'not an atlas page: the pages are the subsystems in workspace.toml '
                                         f'and {", ".join(p + ".md" for p in EXTRA_PAGES)}'))
        for line, name in references(text):
            if name not in names:
                problems.append(Problem(f'{rel}:{line}', f'`{name}` is not the name or alias of any mapping entry'))
