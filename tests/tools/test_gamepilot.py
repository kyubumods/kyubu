"""scripts/gamepilot.py: the native backend writes the loader's inbox files; the xdo backend still makes the same xdotool calls
(add-native-replay 3.1)."""
import importlib
import os
import sys
import threading
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'scripts'))
gamepilot = importlib.import_module('gamepilot')


@pytest.fixture
def native(tmp_path, monkeypatch):
    gamepilot.configure(tmp_path, 'native')
    monkeypatch.setattr(gamepilot, 'NATIVE_WAIT', 0.0)
    monkeypatch.setattr(time, 'sleep', lambda s: None)
    yield tmp_path
    gamepilot.configure(None, 'native' if os.name == 'nt' else 'xdo')


@pytest.fixture
def xdo(monkeypatch):
    gamepilot.configure(None, 'xdo')
    calls = []
    monkeypatch.setattr(gamepilot, 'xdo', lambda *a: calls.append(tuple(map(str, a))) or '')
    monkeypatch.setattr(gamepilot, 'origin', lambda: (10, 20))
    monkeypatch.setattr(time, 'sleep', lambda s: None)
    yield calls
    gamepilot.configure(None, 'native' if os.name == 'nt' else 'xdo')


def inbox(sandbox, ext='input'):
    d = sandbox / 'kyubu' / 'inbox'
    files = sorted(p for p in d.iterdir() if p.suffix == '.' + ext) if d.exists() else []
    assert not [p for p in d.iterdir() if p.suffix == '.tmp'] if d.exists() else True   # renamed, never left as .tmp
    return [p.read_text(encoding='utf-8').splitlines() for p in files]


def test_native_move_and_click(native):
    gamepilot.move(5, 6)
    gamepilot.click('start_game')
    assert inbox(native) == [['move 5 6'], ['move 511 457'], ['button down 1', 'button up 1']]


def test_native_files_sort_in_send_order(native):
    for i in range(12):
        gamepilot.move(i, i)
    names = sorted(p.name for p in (native / 'kyubu' / 'inbox').iterdir())
    assert [n for n in names] == names and len(set(names)) == 12
    assert [f[0] for f in inbox(native)] == [f'move {i} {i}' for i in range(12)]


def test_native_key_tap_and_text(native):
    gamepilot.key('Return')
    gamepilot.tap('grave', 0.6)
    gamepilot.type_text('hi there\nx')
    assert inbox(native) == [['down Return', 'up Return'], ['down grave', 'up grave'], ['text hi there x']]


def test_native_hold_keys_and_buttons(native):
    gamepilot.hold_keys('w+mouse2', 0.1)
    assert inbox(native) == [['down w', 'button down 2'], ['button up 2', 'up w']]


def test_native_wheel(native):
    gamepilot.wheel('down', 3)
    assert inbox(native) == [['wheel down 1']] * 3


def test_native_console_sequence(native):
    gamepilot.console(['help'])
    assert inbox(native) == [['down grave', 'up grave'], ['text help'], ['down Return', 'up Return'], ['down grave', 'up grave']]


def test_native_shot_waits_for_the_png_and_copies_it(native):
    shots = native / 'kyubu' / 'shots'

    def loader():     # the loader: removes the .shot, writes the picture
        d = native / 'kyubu' / 'inbox'
        while not (d.exists() and any(p.suffix == '.shot' for p in d.iterdir())):
            pass
        shots.mkdir(parents=True, exist_ok=True)
        (shots / 'my_pic.png').write_bytes(b'PNG')
    t = threading.Thread(target=loader)
    t.start()
    out = native / 'elsewhere' / 'my_pic.png'
    gamepilot.shot(str(out))
    t.join()
    assert out.read_bytes() == b'PNG'
    assert inbox(native, 'shot') == [['my_pic']]


def test_native_shot_times_out(native):
    with pytest.raises(RuntimeError, match='no screenshot'):
        gamepilot.shot(str(native / 'x.png'), timeout=0)


def test_native_frame_hash_and_settle(native):
    assert gamepilot.screen_hash() is None and gamepilot.window() is None
    f = native / 'kyubu' / 'headless'
    f.mkdir(parents=True)
    (f / 'frame').write_text('40 abc123')
    assert gamepilot.screen_hash() == 'abc123' and gamepilot.window() == 'native'
    # a changed picture that then holds still settles; one that never changes costs `most`
    (f / 'frame').write_text('44 ffff')
    assert gamepilot.settle(0.2, 'abc123', still=0.0, step=0.0) < 0.2


def test_native_settle_never_changing_returns_most(native):
    f = native / 'kyubu' / 'headless'
    f.mkdir(parents=True)
    (f / 'frame').write_text('1 same')
    clock = iter(range(0, 1000))
    import gamepilot as gp
    gp.time = type('T', (), {'time': staticmethod(lambda: next(clock)), 'sleep': staticmethod(lambda s: None)})
    try:
        assert gp.settle(5, 'same') == 5
    finally:
        gp.time = time


def test_native_needs_a_sandbox():
    gamepilot.configure(None, 'native')
    saved = gamepilot.SANDBOX
    gamepilot.SANDBOX = None
    try:
        with pytest.raises(RuntimeError, match='sandbox'):
            gamepilot.move(1, 1)
    finally:
        gamepilot.SANDBOX = saved
        gamepilot.configure(None, 'native' if os.name == 'nt' else 'xdo')


def test_native_origin_is_zero(native):
    assert gamepilot.origin() == (0, 0)


# ---- xdo backend: the calls stay as they were ----------------------------------------------------------------------------
def test_xdo_click_and_move(xdo):
    gamepilot.click('start_game')
    assert xdo == [('mousemove', '521', '477'), ('click', '1')]


def test_xdo_key_tap_text(xdo):
    gamepilot.key('Return')
    gamepilot.tap('grave', 0.6)
    gamepilot.type_text('ab')
    assert xdo == [('key', 'Return'), ('keydown', 'grave'), ('keyup', 'grave'), ('type', '--delay', '150', 'ab')]


def test_xdo_hold_and_wheel(xdo):
    gamepilot.hold_keys('w+mouse2', 0.1)
    gamepilot.wheel('up', 2, gap=0)
    assert xdo == [('keydown', 'w'), ('mousedown', '2'), ('mouseup', '2'), ('keyup', 'w'), ('click', '4'), ('click', '4')]
