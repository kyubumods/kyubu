"""tools/icon_check.py and tools/icon_style.json (change add-skills-unleashed, tasks 4.1 to 4.3; spec ability-icons).

Every failure of the spec has an icon made for it here. The vanilla icons are the game's: they are read from
KYUBU_VANILLA_ICONS or kyubu-mapping/pose-library/vanilla/icons on the owner's PC and never enter the repository; the tests
that need them are skipped without.
"""
import json
import os
import sys

import pytest
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import icon_check as ic  # noqa: E402

VANILLA = os.environ.get('KYUBU_VANILLA_ICONS') or os.path.join(os.path.dirname(ROOT), 'pose-library', 'vanilla', 'icons')
NO_VANILLA = not os.path.isdir(VANILLA)

STYLE = ic.load_style()

SWORD = """# a sample subject
tone = {tone}
a = 230,230,235
b = 150,150,165
c = 255,200,60
..............
..........##..
.........#aa#.
........#aab#.
.......#aab#..
......#aab#...
.....#aab#....
.##.#aab#.....
.#c##ab#......
..#c##b#......
..##c##.......
.#c#.#c#......
.##...##......
..............
"""


def grid(tone='red_orange'):
    return SWORD.format(tone=tone)


def build(text, tone_style=STYLE):
    tone, rows = ic.parse_grid(text, tone_style)
    return tone, rows


def problems(rows, w=14, h=14):
    return ic.check_rows(STYLE, w, h, rows, 'test')


def edit(rows, fn):
    r = [list(row) for row in rows]
    fn(r)
    return r


def test_style_file_is_numbers_only():
    # covers: ability-icons/The style envelope: numbers only
    def walk(v, path):
        if isinstance(v, dict):
            for k, x in v.items():
                walk(x, path + [k])
        elif isinstance(v, list):
            for x in v:
                walk(x, path)
        else:
            assert isinstance(v, (int, float, bool)) or path[0] in ('_comment',) or path[-1] in ('without_outline',) or \
                path[0] in ('tones', 'sample'), (path, v)
    walk(STYLE, [])
    assert set(STYLE['tones']) == {'deep_blue', 'light_blue', 'red_orange', 'violet'}
    assert STYLE['size'] == [14, 14]


def test_a_built_icon_passes_on_every_tone():
    # covers: ability-icons/Original pixel art from text grids: A grid builds
    for tone in STYLE['tones']:
        t, rows = build(grid(tone))
        assert t == tone
        assert problems(rows) == [], tone
        assert ic.classify(STYLE, rows)[0] == tone


def test_build_writes_a_14_by_14_opaque_png(tmp_path):
    # covers: ability-icons/The format of an ability icon: A built icon
    src = tmp_path / 'icons-src'
    src.mkdir()
    (src / 'berserker.grid').write_text(grid(), encoding='utf-8')
    assert ic.main(['build', str(src / 'berserker.grid'), '--out', str(tmp_path / 'icons')]) == 0
    png = tmp_path / 'icons' / 'berserker.png'
    im = Image.open(png)
    assert im.size == (14, 14) and im.mode == 'RGBA'
    assert all(im.getpixel((x, y))[3] == 255 for x in range(14) for y in range(14))
    assert ic.main(['check', str(png)]) == 0


def test_build_defaults_to_the_mods_assets_icons(tmp_path):
    mod = tmp_path / 'mod'
    (mod / 'icons-src').mkdir(parents=True)
    (mod / 'icons-src' / 'ninja.grid').write_text(grid('violet'), encoding='utf-8')
    assert ic.main(['build', str(mod / 'icons-src' / 'ninja.grid')]) == 0
    assert (mod / 'assets' / 'icons' / 'ninja.png').is_file()


def test_wrong_size_is_refused():
    # covers: ability-icons/The format of an ability icon: A 16 x 16 icon
    _, rows = build(grid())
    out = ic.check_rows(STYLE, 16, 16, [[(0, 0, 0, 255)] * 16] * 16, 'big')
    assert len(out) == 1 and '16 x 16' in out[0]


def test_a_translucent_pixel_is_refused():
    # covers: ability-icons/The format of an ability icon: A translucent pixel
    _, rows = build(grid())
    bad = edit(rows, lambda r: r[0].__setitem__(0, (r[0][0][0], r[0][0][1], r[0][0][2], 128)))
    out = problems(bad)
    assert any('not fully opaque' in p and 'x=0 y=0' in p for p in out)


def test_a_file_name_the_loader_would_refuse_is_refused(tmp_path):
    # covers: ability-icons/Format: A file name the loader refuses
    _, rows = build(grid())
    for name, ok in (('berserker.png', True), ('a.png', True), ('.png', False), ('with space.png', False), ('icon.jpg', False),
                     ('x' * 61 + '.png', False)):
        path = tmp_path / name
        ic.write_png(str(path), rows)
        got = ic.check_file(STYLE, str(path))
        assert (got == []) == ok, (name, got)


def test_a_background_that_is_no_tone_is_refused():
    # covers: ability-icons/The style envelope: A flat grey background
    _, rows = build(grid())
    grey = [[(128, 128, 128, 255) if rows[y][x] == ic.background(STYLE, 'red_orange', x, y) + (255,) else rows[y][x]
             for x in range(14)] for y in range(14)]
    out = problems(grey)
    assert any('background' in p for p in out)


def test_a_subject_that_covers_everything_is_refused():
    # covers: ability-icons/The style envelope: A subject that fills the square
    _, rows = build(grid())
    full = [[(200, 40 + (x * 7) % 90, 50 + (y * 5) % 80, 255) for x in range(14)] for y in range(14)]
    out = problems(full)
    assert any('subject' in p for p in out)


def test_an_icon_with_a_few_black_pixels_is_refused_and_one_with_none_is_allowed():
    # covers: ability-icons/The style envelope: A missing outline
    _, rows = build(grid())

    def clear_outline(r, keep):
        n = 0
        for y in range(14):
            for x in range(14):
                if r[y][x][:3] == (0, 0, 0):
                    n += 1
                    if n > keep:
                        r[y][x] = (90, 90, 100, 255)

    some = edit(rows, lambda r: clear_outline(r, 5))
    assert any('pure black' in p for p in problems(some))
    none = edit(rows, lambda r: clear_outline(r, 0))
    assert STYLE['outline']['allow_none'] is True
    assert not any('pure black' in p for p in problems(none))


def test_noise_has_too_many_colours():
    # covers: ability-icons/The style envelope: A photographic subject
    _, rows = build(grid())
    noisy = [[(10 + (x * 17 + y * 5) % 240, 10 + (x * 3 + y * 29) % 240, 10 + (x * 11 + y * 13) % 240, 255)
              if (x + y) % 3 else rows[y][x] for x in range(14)] for y in range(14)]
    # keep it below the subject-size limit so the colour count is what is judged
    out = ic.check_rows(STYLE, 14, 14, noisy, 'noise')
    assert any('distinct' in p or 'subject' in p for p in out)


def test_grid_errors_are_named():
    # covers: ability-icons/Original pixel art from text grids: A malformed grid
    rows13 = '\n'.join(grid().splitlines()[:-1])
    for text, needle in [
        (grid('purple'), 'tone'),
        (grid().replace('tone = red_orange\n', ''), "no 'tone"),
        (rows13, 'rows'),
        (grid().replace('..........##..', '..........##...'), '15 characters'),
        (grid().replace('c = 255,200,60\n', ''), "has no colour"),
        (grid().replace('c = 255,200,60', 'c = 255,200'), 'r,g,b'),
        (grid().replace('c = 255,200,60', 'c = 255,200,300'), '0..255'),
    ]:
        with pytest.raises(ic.GridError) as e:
            ic.parse_grid(text, STYLE)
        assert needle in str(e.value), (needle, str(e.value))


def test_a_row_may_start_with_a_hash_and_a_comment_is_a_hash_and_a_space():
    t = grid().replace('# a sample subject', '# a sample subject  (a comment)')
    tone, rows = ic.parse_grid(t, STYLE)
    assert rows[1][10][:3] == (0, 0, 0)


def test_sheet_needs_six_vanilla_icons_of_the_tone(tmp_path):
    _, rows = build(grid())
    png = tmp_path / 'x.png'
    ic.write_png(str(png), rows)
    empty = tmp_path / 'vanilla'
    empty.mkdir()
    with pytest.raises(SystemExit):
        ic.make_sheet(STYLE, str(png), str(empty), str(tmp_path / 'sheet.png'))


@pytest.mark.skipif(NO_VANILLA, reason='the game\'s icons are not here (KYUBU_VANILLA_ICONS)')
def test_every_vanilla_ability_icon_on_the_four_tones_passes():
    # covers: ability-icons/The style envelope: The game's own icons pass
    failed, passed = [], 0
    for f in sorted(os.listdir(VANILLA)):
        if not f.endswith('.png'):
            continue
        w, h, rows = ic.read_png(os.path.join(VANILLA, f))
        if (w, h) != (14, 14):
            continue
        tone, matching = ic.classify(STYLE, rows)
        if f[:-4] in ic.SPECIAL or matching == 0:
            continue
        out = ic.check_rows(STYLE, w, h, rows, f)
        if out:
            failed.append(out)
        else:
            passed += 1
    assert failed == []
    assert passed == STYLE['sample']['icons']


@pytest.mark.skipif(NO_VANILLA, reason='the game\'s icons are not here (KYUBU_VANILLA_ICONS)')
def test_built_backgrounds_stay_within_the_tolerance_of_the_games(tmp_path):
    # covers: ability-icons/Original pixel art from text grids: A grid builds (the glow matches the game's)
    out = tmp_path / 'sheet.png'
    for tone, ref in (('red_orange', 'slam'), ('light_blue', 'teleport'), ('deep_blue', 'smash'), ('violet', 'perforate')):
        w, h, rows = ic.read_png(os.path.join(VANILLA, ref + '.png'))
        field = ic.background_field(STYLE, tone)
        near = sum(1 for y in range(14) for x in range(14)
                   if max(abs(rows[y][x][i] - field[y][x][i]) for i in range(3)) <= STYLE['tolerance'])
        assert near >= STYLE['background_pixels_min'], (tone, near)
    tone, six = ic.make_sheet(STYLE, os.path.join(VANILLA, 'slam.png'), VANILLA, str(out))
    assert tone == 'red_orange' and len(six) == 6 and out.is_file()
