#!/usr/bin/env python3
"""gamepilot - drive Cube World for smoke tests (menu clicks, console commands, screenshots).

Two backends behind the same functions, chosen by `os.name` (or by `configure`):
  xdo     Wine/Xvfb on Linux: xdotool and ImageMagick against the X display.
  native  Windows: files in the sandbox's kyubu/inbox/ for a game started with `--kyubu-dev --kyubu-headless`
          (docs/dev-tools.md, "Headless mode"); `settle` reads kyubu/headless/frame.
Coordinates are for a 1024x768 client area (the xdo backend has the window at the top-left of the X screen).
Used by the Kyubu smoke tests until the in-game test harness (add-feature-testing) exists.
"""
import hashlib
import os
import shutil
import subprocess
import time

DISPLAY = os.environ.get('DISPLAY', ':99')
ENV = dict(os.environ, DISPLAY=DISPLAY)
# The run's kyubu/logs/latest.log (kmap probe replay sets it). With it, a click that loads a world returns once the log
# says "world entered" instead of after a fixed time.
WORLD_LOG = None
WORLD_WAIT = 150     # the longest a world may take to load (seconds)
WORLD_SETTLE = 5     # after "world entered": the first chunks and the HUD

BACKEND = 'native' if os.name == 'nt' else 'xdo'
SANDBOX = os.environ.get('KYUBU_SANDBOX')   # native: the game's working folder (it holds kyubu/inbox, kyubu/headless, ...)
# The waits below were sized for the Wine game (about 4 fps under software rendering). The native game runs at 60 fps or more
# and sees a file within one frame, so the native backend scales its waits down; the xdo backend keeps them as they are.
NATIVE_WAIT = 0.25
_seq = [0]

# Positions of menu elements inside the 1024x768 client area.
POS = {
    'start_game': (511, 457),
    'first_slot_create': (691, 439),     # "CREATE" in the empty character/world slot
    'first_entry': (512, 244),           # hovering the first existing character/world shows SELECT
    'first_entry_select': (691, 298),
    'name_field': (512, 737),
    'seed_field': (512, 687),
    'create_character': (854, 714),
    'create_world': (853, 711),
    'connect_to_server': (853, 711),     # on the world list, under "CREATE"
    'server_address': (512, 303),        # the field on the connect screen (it needs a click before typing)
    'connect': (512, 380),
}


def configure(sandbox=None, backend=None):
    """Chooses the backend and, for native, the sandbox (default: $KYUBU_SANDBOX)."""
    global BACKEND, SANDBOX
    if backend:
        BACKEND = backend
    if sandbox:
        SANDBOX = str(sandbox)
    _seq[0] = 0


def native():
    return BACKEND == 'native'


def pause(seconds):
    time.sleep(seconds * NATIVE_WAIT if native() else seconds)


def _dir(*parts):
    if not SANDBOX:
        raise RuntimeError('gamepilot: the native backend needs a sandbox (configure(sandbox) or $KYUBU_SANDBOX)')
    return os.path.join(SANDBOX, 'kyubu', *parts)


def send(*lines, ext='input'):
    """Native: one inbox file, written under .tmp and renamed (the loader reads in name order). Returns its path."""
    d = _dir('inbox')
    os.makedirs(d, exist_ok=True)
    _seq[0] += 1
    path = os.path.join(d, f'p{int(time.time() * 1000) % 10**9:09d}-{_seq[0]:05d}.{ext}')
    with open(path + '.tmp', 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(path + '.tmp', path)
    return path


def origin():
    if native():
        return 0, 0     # client coordinates
    w = window()
    out = xdo('getwindowgeometry', w) if w else ''
    for line in out.splitlines():
        if 'Position:' in line:
            x, y = line.split('Position:')[1].split('(')[0].strip().split(',')
            return int(x), int(y)
    return 0, 0


def xdo(*args):
    return subprocess.run(['xdotool', *map(str, args)], env=ENV, capture_output=True, text=True).stdout.strip()


def window():
    if native():
        return 'native' if os.path.exists(_dir('headless', 'frame')) else None
    ids = xdo('search', '--name', '^Cube$').split()
    return ids[0] if ids else None


def wait_window(timeout=60):
    t = time.time()
    while time.time() - t < timeout:
        w = window()
        if w:
            return w
        time.sleep(1 if not native() else 0.2)
    raise RuntimeError('game window did not appear')


def fit_window():
    if native():
        wait_window()    # the loader fixes the window at 1024x768
        return
    w = wait_window()
    xdo('windowsize', w, 1024, 768)
    xdo('windowactivate', w)
    time.sleep(3)


def move(x, y):
    """The mouse to client position (x, y)."""
    if native():
        send(f'move {int(x)} {int(y)}')
    else:
        ox, oy = origin()
        xdo('mousemove', x + ox, y + oy)


def mouse_click():
    if native():
        send('button down 1', 'button up 1')   # one file: the loader keeps the down one frame before the up
    else:
        xdo('click', 1)


def click(name_or_xy, wait=1.0):
    x, y = POS[name_or_xy] if isinstance(name_or_xy, str) else name_or_xy
    move(x, y)
    pause(0.8)
    mouse_click()
    pause(wait)


def hold_keys(keys, seconds):
    """`keys` as in a record's `hold` step: `w`, or `w+mouse2` for several; mouse1..mouse3 are buttons. All go down in
    order, stay for `seconds`, and come up in reverse order."""
    parts = keys.split('+')

    def is_button(p):
        return p in ('mouse1', 'mouse2', 'mouse3')
    if native():
        send(*[f'button down {p[5:]}' if is_button(p) else f'down {p}' for p in parts])
        time.sleep(seconds)
        send(*[f'button up {p[5:]}' if is_button(p) else f'up {p}' for p in reversed(parts)])
        return
    for p in parts:
        xdo('mousedown', p[5:]) if is_button(p) else xdo('keydown', p)
    time.sleep(seconds)
    for p in reversed(parts):
        xdo('mouseup', p[5:]) if is_button(p) else xdo('keyup', p)


def wheel(direction, ticks, gap=0.5):
    """The mouse wheel over the game window (camera zoom), one tick per `gap` seconds."""
    for _ in range(int(ticks)):
        if native():
            send(f'wheel {direction} 1')
        else:
            xdo('click', '4' if direction == 'up' else '5')
        time.sleep(gap)


def frame_hash():
    """Native: the hash in kyubu/headless/frame; None while there is no file."""
    try:
        with open(_dir('headless', 'frame'), encoding='ascii') as f:
            parts = f.read().split()
    except OSError:
        return None
    return parts[1] if len(parts) >= 2 else None


def screen_hash():
    if native():
        return frame_hash()
    r = subprocess.run(['import', '-window', 'root', '-crop', '1030x800+0+0', 'rgb:-'], env=ENV, capture_output=True)
    return hashlib.md5(r.stdout).hexdigest() if r.returncode == 0 and r.stdout else None


def settle(most, before, still=1.5, step=0.5):
    """Waits until the screen differs from `before` (taken before the click) and then holds still for `still` seconds,
    for at most `most` seconds. A screen that never settles (an animation, a blinking cursor) or a capture that fails
    costs the old fixed wait, `most`."""
    if native():
        still, step = still * NATIVE_WAIT, step * NATIVE_WAIT
    t0 = time.time()
    changed, last, since = False, before, None
    while time.time() - t0 < most:
        time.sleep(step)
        h = screen_hash()
        if h is None:
            continue
        if not changed:
            changed = h != before
            last, since = h, time.time()
            continue
        if h != last:
            last, since = h, time.time()
        elif time.time() - since >= still:
            return time.time() - t0
    return most


def step(name, most):
    """A menu click whose next screen takes time: returns once that screen has settled, after `most` s at the latest."""
    x, y = POS[name]
    move(x, y)
    pause(0.8)
    before = screen_hash()   # after the move: a hover highlight is not the next screen
    mouse_click()
    took = settle(most, before)
    print(f'gamepilot: {name}: next screen after {took:.1f} s' + (' (the old fixed wait)' if took >= most else ''), flush=True)


def world_count():
    """How many worlds the log says were entered: a local world logs "world entered", a joined server "connected to"
    (a joined client never logs "world entered", so a join used to wait the whole WORLD_WAIT)."""
    try:
        with open(WORLD_LOG, encoding='utf-8-sig', errors='replace') as f:
            text = f.read()
        return text.count('] world entered: ') + text.count('] connected to ')
    except (OSError, TypeError):
        return 0


def enter(name, most):
    """The click that loads a world. With WORLD_LOG, it returns WORLD_SETTLE s after the log says a world was entered
    (WORLD_WAIT s at the latest); without, after `most` s, as before."""
    if not WORLD_LOG:
        click(name, most)
        return
    n = world_count()
    click(name, 0)
    t0 = time.time()
    while time.time() - t0 < WORLD_WAIT and world_count() <= n:
        time.sleep(0.5)
    print(f'gamepilot: {name}: world entered after {time.time() - t0:.1f} s', flush=True)
    pause(WORLD_SETTLE)


def type_text(text, delay=150):
    if native():
        send('text ' + text.replace('\n', ' '))
        return
    xdo('type', '--delay', delay, text)


def key(k):
    if native():
        send(f'down {k}', f'up {k}')
        return
    xdo('key', k)


def tap(k, hold=0.4):
    """A key held for `hold` seconds: the game polls keys once a frame (about 4 fps under software rendering), so xdotool's
    instant press can be over before a frame sees it and the console then opens or closes out of step. Native: the loader
    shows every press to a frame, so `hold` is not used."""
    if native():
        send(f'down {k}', f'up {k}')
        return
    xdo('keydown', k)
    time.sleep(hold)
    xdo('keyup', k)


def shot(path, crop='1030x800+0+0', timeout=10):
    if native():
        # the loader writes <sandbox>/kyubu/shots/<name>.png at the next Present; `path` gets a copy if it is elsewhere
        name = ''.join(c for c in os.path.splitext(os.path.basename(path))[0] if c.isalnum() or c in '._-')[:80] or 'shot'
        made = os.path.join(_dir('shots'), name + '.png')
        if os.path.exists(made):
            os.remove(made)
        send(name, ext='shot')
        t0 = time.time()
        while not os.path.exists(made) and time.time() - t0 < timeout:
            time.sleep(0.05)
        if not os.path.exists(made):
            raise RuntimeError(f'gamepilot: the game wrote no screenshot {name}')
        time.sleep(0.05)    # the loader has closed the file
        if os.path.abspath(made) != os.path.abspath(path):
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
            shutil.copyfile(made, path)
        return
    subprocess.run(['import', '-window', 'root', path + '.full.png'], env=ENV)
    subprocess.run(['convert', path + '.full.png', '-crop', crop, path])
    os.remove(path + '.full.png')


def console(commands, per_command_wait=4.0):
    """Opens the Kyubu console, runs each command, closes it again."""
    tap('grave', 0.6)
    pause(2)
    for c in commands:
        type_text(c, 300)
        pause(0.5 + len(c) * 0.12)
        tap('Return', 0.6)
        pause(per_command_wait)
    tap('grave', 0.6)
    pause(1)


def new_character_and_world(name='Tester', seed='1234', world='testworld'):
    step('start_game', 8)
    step('first_slot_create', 8)
    click('name_field', 1)
    type_text(name)
    pause(3)
    key('Return')
    pause(2)
    step('create_character', 10)
    step('first_slot_create', 6)
    click('seed_field', 1)
    type_text(seed)
    pause(2)
    click('name_field', 1)
    type_text(world)
    pause(4)
    enter('create_world', 25)


def join_server(address='127.0.0.1', name='Tester'):
    """A new character, then Connect to server on the world list: the address, Connect, and time to load."""
    step('start_game', 8)
    step('first_slot_create', 8)
    click('name_field', 1)
    type_text(name)
    pause(3)
    key('Return')
    pause(2)
    step('create_character', 10)
    step('connect_to_server', 6)
    click('server_address', 1)
    type_text(address)
    pause(3)
    enter('connect', 30)


def existing_world():
    step('start_game', 8)
    move(*POS['first_entry'])
    pause(3)
    step('first_entry_select', 10)
    move(*POS['first_entry'])
    pause(3)
    enter('first_entry_select', 30)


def log_lines(sandbox):
    p = os.path.join(sandbox, 'kyubu', 'logs', 'latest.log')
    try:
        return open(p, encoding='utf-8-sig', errors='replace').read().splitlines()
    except FileNotFoundError:
        return []


if __name__ == '__main__':
    import sys
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'fit':
        fit_window()
    elif cmd == 'new-world':
        fit_window(); new_character_and_world()
    elif cmd == 'existing-world':
        fit_window(); existing_world()
    elif cmd == 'join':
        fit_window(); join_server(*sys.argv[2:3])
    elif cmd == 'console':
        console(sys.argv[2:])
    elif cmd == 'shot':
        shot(sys.argv[2])
    else:
        print(__doc__)
