#!/usr/bin/env python3
"""Ability icons in the game's own style (change add-skills-unleashed, design D9, spec ability-icons).

    icon_check.py check  <icon.png>...                 exit 1 when an icon is outside the style envelope
    icon_check.py build  <icon.grid>... [--out DIR]    a text grid -> a 14 x 14 RGBA PNG (default DIR: the mod's assets/icons)
    icon_check.py sheet  <icon.png> --vanilla DIR --out FILE    the icon at 8x between six vanilla icons of its tone
    icon_check.py measure <vanilla icon dir> --out tools/icon_style.json     (the owner's PC only: reads the game's icons)

The envelope is `tools/icon_style.json`: numbers only, measured over the game's 14 x 14 ability icons. `check`, `build` and
`sheet` need only Pillow; `measure` also needs numpy and the vanilla icons, which never enter the repository.

The game's ability icons sit on one of four background tones, a smooth glow that is brightest near the middle; the tone
does not follow the class. A subject is drawn over it with a pure black outline. The envelope holds, per tone, the glow as
a cubic in the squared distance from a centre, per channel, and the counts a plausible icon stays within.

The grid format (`icons-src/<name>.grid`):

    # a comment
    tone = red_orange
    a = 255,220,100          one line per letter used in the grid: r,g,b
    b = 210,150,40
    ..............           14 rows of 14 characters: '.' = the tone's background, '#' = black outline,
    ....#####.....           a letter = its colour above
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STYLE = os.path.join(HERE, "icon_style.json")
TONES_SEED = {                       # the corner colour of each tone in the game's icons (only `measure` uses it)
    "deep_blue": (0, 78, 255),
    "light_blue": (0, 144, 255),
    "red_orange": (253, 31, 9),
    "violet": (114, 0, 255),
}
SPECIAL = {"bulwark"}                # a vanilla icon on none of the four tones: left out of the measurement
SIZE = 14


def load_style(path=STYLE):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


# ---- the background model -------------------------------------------------------------------------------------------------

def _poly(coefs, x):
    v = 0.0
    for c in coefs:
        v = v * x + c
    return v


def background(style, tone, x, y):
    """The tone's background colour at pixel (x, y), as three ints."""
    t = style["tones"][tone]
    cx, cy = t["centre"]
    d2 = (x - cx) ** 2 + (y - cy) ** 2
    out = []
    for ch in t["channels"]:
        v = ch[0] if len(ch) == 1 else _poly(ch, d2)
        out.append(max(0, min(255, int(round(v)))))
    return tuple(out)


def background_field(style, tone):
    return [[background(style, tone, x, y) for x in range(SIZE)] for y in range(SIZE)]


# ---- reading an icon ------------------------------------------------------------------------------------------------------

def read_png(path):
    """(width, height, rows of (r, g, b, a)) of a PNG, through Pillow."""
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    px = im.load()
    return w, h, [[px[x, y] for x in range(w)] for y in range(h)]


def classify(style, rows):
    """(tone, matching pixel count) of the tone whose background the icon follows best."""
    tol = style["tolerance"]
    best = (None, -1)
    for tone in style["tones"]:
        field = background_field(style, tone)
        n = 0
        for y in range(SIZE):
            for x in range(SIZE):
                p, b = rows[y][x], field[y][x]
                if max(abs(p[0] - b[0]), abs(p[1] - b[1]), abs(p[2] - b[2])) <= tol:
                    n += 1
        if n > best[1]:
            best = (tone, n)
    return best


def stats(style, rows, tone):
    """The numbers the envelope limits, for an icon read as `tone`."""
    tol = style["tolerance"]
    field = background_field(style, tone)
    subject = 0
    black = 0
    colours = set()
    for y in range(SIZE):
        for x in range(SIZE):
            p, b = rows[y][x], field[y][x]
            if (p[0], p[1], p[2]) == (0, 0, 0):
                black += 1
            if max(abs(p[0] - b[0]), abs(p[1] - b[1]), abs(p[2] - b[2])) > tol:
                subject += 1
                colours.add((p[0], p[1], p[2]))
    return {"subject": subject, "background": SIZE * SIZE - subject, "black": black, "colours": len(colours)}


def check_rows(style, w, h, rows, name="icon"):
    """The list of problems of an icon (empty when it is inside the envelope)."""
    out = []
    if (w, h) != tuple(style["size"]):
        return [f"{name}: size is {w} x {h}, the game's ability icons are {style['size'][0]} x {style['size'][1]}"]
    clear = [(x, y) for y, row in enumerate(rows) for x, p in enumerate(row) if p[3] != 255]
    if clear:
        out.append(f"{name}: {len(clear)} pixel(s) are not fully opaque, the first at x={clear[0][0]} y={clear[0][1]} "
                   f"(the game's icons are)")
    tone, matching = classify(style, rows)
    s = stats(style, rows, tone)
    if s["background"] < style["background_pixels_min"]:
        out.append(f"{name}: only {s['background']} pixel(s) follow the {tone} background (at least "
                   f"{style['background_pixels_min']}): the subject covers too much, or the background is not one of the "
                   f"game's tones ({', '.join(style['tones'])})")
    if not (style["subject_pixels"]["min"] <= s["subject"] <= style["subject_pixels"]["max"]):
        out.append(f"{name}: the subject is {s['subject']} pixel(s), the game's icons stay from "
                   f"{style['subject_pixels']['min']} to {style['subject_pixels']['max']}")
    lo, hi = style["outline"]["min"], style["outline"]["max"]
    none_ok = style["outline"].get("allow_none", False)
    if not (lo <= s["black"] <= hi) and not (s["black"] == 0 and none_ok):
        out.append(f"{name}: {s['black']} pure black pixel(s); the outline of the game's icons is {lo} to {hi}"
                   + (" (or none at all)" if none_ok else ""))
    cl, ch = style["distinct_colours"]["min"], style["distinct_colours"]["max"]
    if not (cl <= s["colours"] <= ch):
        out.append(f"{name}: {s['colours']} distinct subject colour(s); the game's icons have {cl} to {ch}")
    return out


FILE_NAME = re.compile(r"^[A-Za-z0-9._-]{1,60}\.png$")


def check_file(style, path):
    name = os.path.basename(path)
    out = []
    if not FILE_NAME.match(name) or not 5 <= len(name) <= 64:
        out.append(f"{name}: the file name must be 5 to 64 characters of letters, digits, '.', '-' and '_', ending in .png")
    w, h, rows = read_png(path)
    return out + check_rows(style, w, h, rows, name)


# ---- building -------------------------------------------------------------------------------------------------------------

class GridError(Exception):
    pass


def parse_grid(text, style):
    """(tone, rows of (r, g, b, a)) from the text of a .grid file; raises GridError."""
    tone = None
    palette = {}
    grid = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line == "#" or line.startswith("# "):        # a comment is '#' and a space (a row may start with '#')
            continue
        if "=" in line:
            key, _, val = line.partition("=")
            key, val = key.strip(), val.strip()
            if key == "tone":
                tone = val
            elif len(key) == 1 and key not in ".#":
                try:
                    r, g, b = (int(v) for v in val.split(","))
                except ValueError:
                    raise GridError(f"line {n}: '{key} = {val}' is not r,g,b")
                if not all(0 <= v <= 255 for v in (r, g, b)):
                    raise GridError(f"line {n}: '{key}' has a channel outside 0..255")
                palette[key] = (r, g, b)
            else:
                raise GridError(f"line {n}: unknown setting '{key}'")
            continue
        if len(line) != SIZE:
            raise GridError(f"line {n}: a row is {len(line)} characters, it must be {SIZE}")
        grid.append(line)
    if tone is None:
        raise GridError("no 'tone = ...' line")
    if tone not in style["tones"]:
        raise GridError(f"tone '{tone}' is not one of {', '.join(style['tones'])}")
    if len(grid) != SIZE:
        raise GridError(f"{len(grid)} rows, it must be {SIZE}")
    field = background_field(style, tone)
    rows = []
    for y, line in enumerate(grid):
        row = []
        for x, c in enumerate(line):
            if c == ".":
                r, g, b = field[y][x]
            elif c == "#":
                r, g, b = 0, 0, 0
            elif c in palette:
                r, g, b = palette[c]
            else:
                raise GridError(f"row {y + 1}, column {x + 1}: '{c}' has no colour line")
            row.append((r, g, b, 255))
        rows.append(row)
    return tone, rows


def write_png(path, rows):
    from PIL import Image
    im = Image.new("RGBA", (SIZE, SIZE))
    px = im.load()
    for y in range(SIZE):
        for x in range(SIZE):
            px[x, y] = tuple(rows[y][x])
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    im.save(path, format="PNG", optimize=False)


# ---- the review sheet -----------------------------------------------------------------------------------------------------

def vanilla_of_tone(style, vanilla_dir, tone):
    """Names and rows of the vanilla 14 x 14 icons on `tone`, sorted by name."""
    found = []
    for f in sorted(os.listdir(vanilla_dir)):
        if not f.lower().endswith(".png"):
            continue
        w, h, rows = read_png(os.path.join(vanilla_dir, f))
        if (w, h) != (SIZE, SIZE) or f[:-4] in SPECIAL:
            continue
        t, n = classify(style, rows)
        if t == tone and n >= style["background_pixels_min"]:
            found.append((f[:-4], rows))
    return found


def make_sheet(style, icon_path, vanilla_dir, out_path, scale=8):
    from PIL import Image, ImageDraw
    w, h, rows = read_png(icon_path)
    if (w, h) != (SIZE, SIZE):
        raise SystemExit(f"{icon_path}: not {SIZE} x {SIZE}")
    tone, _ = classify(style, rows)
    same = vanilla_of_tone(style, vanilla_dir, tone)
    if len(same) < 6:
        raise SystemExit(f"only {len(same)} vanilla icons on {tone}; need 6")
    step = len(same) / 6.0
    six = [same[int(i * step)] for i in range(6)]
    cells = six[:3] + [(os.path.basename(icon_path)[:-4] + " (new)", rows)] + six[3:]
    pad, label = 8, 14
    cell_w = SIZE * scale + pad
    img = Image.new("RGBA", (cell_w * len(cells) + pad, SIZE * scale + 2 * pad + 2 * label + SIZE + pad), (32, 32, 32, 255))
    d = ImageDraw.Draw(img)
    for i, (name, r) in enumerate(cells):
        x0 = pad + i * cell_w
        for y in range(SIZE):
            for x in range(SIZE):
                c = tuple(r[y][x])
                d.rectangle([x0 + x * scale, pad + y * scale, x0 + (x + 1) * scale - 1, pad + (y + 1) * scale - 1], fill=c)
                img.putpixel((x0 + x, pad + SIZE * scale + pad + label + y), c)       # the same icon at 1x
        d.text((x0, pad + SIZE * scale + 2), name[:18], fill=(230, 230, 230, 255))
    d.text((pad, pad + SIZE * scale + pad + label + SIZE + 2), f"tone: {tone}   8x above, 1x below", fill=(230, 230, 230, 255))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    img.save(out_path)
    return tone, [n for n, _ in six]


# ---- measuring the game's icons -------------------------------------------------------------------------------------------

def dump_compact(obj):
    """JSON with two-space indent and every list of plain values on one line."""
    text = json.dumps(obj, indent=2)
    return re.sub(r"\[\s+([^\[\]{}]*?)\s+\]", lambda m: "[" + re.sub(r"\s+", " ", m.group(1)) + "]", text) + chr(10)


def measure(vanilla_dir, out_path):
    """Fits the four tones and writes the envelope. Needs numpy and the vanilla icons (the owner's PC)."""
    import numpy as np
    icons = {}
    for f in sorted(os.listdir(vanilla_dir)):
        if not f.lower().endswith(".png"):
            continue
        w, h, rows = read_png(os.path.join(vanilla_dir, f))
        if (w, h) == (SIZE, SIZE):
            icons[f[:-4]] = np.array([[p[:3] for p in r] for r in rows], dtype=float)
    groups = {}
    for tone, seed in TONES_SEED.items():
        groups[tone] = [n for n, a in icons.items() if n not in SPECIAL and all(abs(a[0, 0][i] - seed[i]) <= 8 for i in range(3))]
    yy, xx = np.mgrid[0:SIZE, 0:SIZE]
    tones = {}
    for tone, names in groups.items():
        A = np.stack([icons[n] for n in names])
        med = np.median(A, axis=0)
        agree = (np.abs(A - med[None]) <= 1.5).all(axis=3).mean(axis=0) >= 0.6
        agree &= ~((med <= 0.5).all(axis=2))
        agree &= ~((med >= 254.5).all(axis=2))
        best = None
        for cx in np.arange(5.5, 8.3, 0.25):
            for cy in np.arange(5.5, 9.5, 0.25):
                d2 = (xx - cx) ** 2 + (yy - cy) ** 2
                tot, chans, ok = 0.0, [], True
                for ch in range(3):
                    v = med[:, :, ch]
                    if np.ptp(v[agree]) <= 1.0:
                        chans.append([int(round(float(np.median(v[agree]))))])
                        continue
                    use = agree & (v > 0) & (v < 255)
                    if use.sum() < 8:
                        ok = False
                        break
                    p = np.polyfit(d2[use], v[use], 3)
                    pred = np.clip(np.polyval(p, d2), 0, 255)
                    tot = max(tot, float(np.abs(pred[agree] - v[agree]).max()))
                    chans.append([float("%.5g" % c) for c in p])
                if ok and (best is None or tot < best[0]):
                    best = (tot, float(cx), float(cy), chans)
        tot, cx, cy, chans = best
        tones[tone] = {"centre": [cx, cy], "channels": chans, "fit_max_error": round(tot, 2)}
    style = {
        "_comment": "Measured over the game's 14 x 14 ability icons by `tools/icon_check.py measure`. Numbers only: no pixels. "
                    "A channel of one number is constant; four numbers are a cubic in the squared distance from `centre`, highest power first.",
        "version": 1,
        "size": [SIZE, SIZE],
        "tolerance": 8,
        "tones": tones,
    }
    # the counts, over every icon that follows one of the four tones
    rows_stats = []
    per_tone = {t: 0 for t in tones}
    for n, a in icons.items():
        if n in SPECIAL:
            continue
        rows = [[(int(a[y, x, 0]), int(a[y, x, 1]), int(a[y, x, 2]), 255) for x in range(SIZE)] for y in range(SIZE)]
        tone, matching = classify(style, rows)
        s = stats(style, rows, tone)
        if s["background"] == 0:                 # on none of the four tones (a utility skill's green)
            continue
        per_tone[tone] += 1
        rows_stats.append((n, tone, s))
    subj = [s["subject"] for _, _, s in rows_stats]
    black = [s["black"] for _, _, s in rows_stats if s["black"] > 0]
    cols = [s["colours"] for _, _, s in rows_stats]
    bgs = [s["background"] for _, _, s in rows_stats]
    style["background_pixels_min"] = min(bgs)
    style["subject_pixels"] = {"min": min(subj), "max": max(subj)}
    style["outline"] = {"colour": [0, 0, 0], "min": min(black), "max": max(black),
                        "allow_none": any(s["black"] == 0 for _, _, s in rows_stats)}
    style["distinct_colours"] = {"min": min(cols), "max": max(cols)}
    style["sample"] = {"icons": len(rows_stats), "per_tone": per_tone,
                       "without_outline": sorted(n for n, _, s in rows_stats if s["black"] == 0)}
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(dump_compact(style))
    return style, rows_stats


# ---- command line ---------------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description="Ability icons in the game's style")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("icons", nargs="+")
    c.add_argument("--style", default=STYLE)
    b = sub.add_parser("build")
    b.add_argument("grids", nargs="+")
    b.add_argument("--out", help="folder for the PNGs (default: assets/icons of the mod that holds icons-src/)")
    b.add_argument("--style", default=STYLE)
    s = sub.add_parser("sheet")
    s.add_argument("icon")
    s.add_argument("--vanilla", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--style", default=STYLE)
    m = sub.add_parser("measure")
    m.add_argument("vanilla_dir")
    m.add_argument("--out", default=STYLE)
    a = ap.parse_args(argv)

    if a.cmd == "measure":
        style, rows = measure(a.vanilla_dir, a.out)
        print(f"icon_check: {len(rows)} icons measured; wrote {a.out}")
        for t, n in style["sample"]["per_tone"].items():
            print(f"  {t:11s} {n:3d} icons   fit error {style['tones'][t]['fit_max_error']}")
        for k in ("background_pixels_min", "subject_pixels", "outline", "distinct_colours"):
            print(f"  {k}: {style[k]}")
        print(f"  without an outline: {', '.join(style['sample']['without_outline'])}")
        return 0

    style = load_style(a.style)
    if a.cmd == "check":
        bad = 0
        for path in a.icons:
            try:
                problems = check_file(style, path)
            except OSError as e:
                problems = [f"{os.path.basename(path)}: {e}"]
            for p in problems:
                print(p)
            if problems:
                bad += 1
            else:
                tone, _ = classify(style, read_png(path)[2])
                print(f"{os.path.basename(path)}: ok ({tone})")
        return 1 if bad else 0
    if a.cmd == "build":
        rc = 0
        for g in a.grids:
            with open(g, encoding="utf-8") as fh:
                text = fh.read()
            try:
                tone, rows = parse_grid(text, style)
            except GridError as e:
                print(f"{os.path.basename(g)}: {e}")
                rc = 1
                continue
            out_dir = a.out or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(g))), "assets", "icons")
            dest = os.path.join(out_dir, os.path.splitext(os.path.basename(g))[0] + ".png")
            write_png(dest, rows)
            print(f"{os.path.basename(g)} -> {dest} ({tone})")
        return rc
    if a.cmd == "sheet":
        tone, six = make_sheet(style, a.icon, a.vanilla, a.out)
        print(f"icon_check: sheet {a.out} ({tone}; beside {', '.join(six)})")
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
