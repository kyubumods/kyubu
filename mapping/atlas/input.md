# Input

Atlas page for the `input` subsystem: the keyboard and the mouse, from DirectInput and the window messages to the
game controller's handlers. Reference pages: [docs/game/input.md](../../docs/game/input.md).

## Purpose

Cube.exe reads the keyboard and the mouse two ways. `WinMain()` polls two DirectInput 8 devices every loop (a
keyboard and a mouse) and copies the movement keys and the mouse state into the game controller; the window
messages go through [window_proc](../../docs/game/input.md) to the `plasma::` engine, which calls the controller's
key and mouse handlers. `cube::Controller`, the base of `cube::GameController` ([core](core.md)), keeps which keys
and buttons are down. There is no gamepad support (no XInput, no joystick strings). Server.exe has no input.

## Key classes

- [`cube::Controller`](../../docs/game/types/cube_Controller.md) (verified as a class, RTTI; Cube.exe only): the
  input interface, 12 vtable slots. What its slots do, from the base implementations:
  - slot 2 `cube::Controller::onKeyDown()` and slot 3 `cube::Controller::onKeyUp()`: set or clear a byte in a
    256-entry key array at +0x19 (indexed by the key code);
  - slot 7 `cube::Controller::onMouseDown()` and slot 8 `cube::Controller::onMouseUp()`: the same for the mouse
    buttons, at +0x119;
  - slot 4 (resize, width and height) and slot 6 (mouse move, x and y as floats) are empty in the base;
  - slot 1 returns a bool that `WinMain()` checks before it captures the cursor (whether the game wants the
    mouse?); slot 0 is the deleting destructor.
- [`cube::GameController`](../../docs/game/types/cube_GameController.md) (verified as a class, [core](core.md))
  overrides slots 2, 3, 4, 6, 7 and 8 (below). `WinMain()` also writes single bools at +0x4 to +0x17 of it from the
  DirectInput keyboard state: probably the movement and action keys (which bool is which key is open).
- The game controller pointer is the global 0x7691C8, set by `WinMain()` (not mapped yet, 7.2).

## Entry points

All candidate, Cube.exe:

- `cube::Controller::Controller()` (0x43BD60): called by `cube::GameController::GameController()`; clears the arrays.
- The base handlers `cube::Controller::onKeyDown()` (0x43BE60), `cube::Controller::onKeyUp()` (0x43BE70),
  `cube::Controller::onMouseDown()` (0x43BE20) and `cube::Controller::onMouseUp()` (0x43BE40).
- The overrides, each of which calls its base first:
  - `cube::GameController::onKeyDown()` (0x47E6D0): also the chat line's commands (/connect, /disconnect, /namepet),
    which reach [network](network.md);
  - `cube::GameController::onKeyUp()` (0x47EEF0);
  - `cube::GameController::onResize()` (0x482F10, slot 4): called after [reset_device](../../docs/game/rendering.md);
    lays the widgets out again;
  - `cube::GameController::onMouseMove()` (0x47EF20, slot 6): called by `WinMain()` with the cursor position;
  - `cube::GameController::onMouseDown()` (0x47BB30, 10 KB, slot 7): calls
    [item_received_message](../../docs/game/items.md) and [play_sound](../../docs/game/audio.md), so clicks on items
    and in the world are handled here;
  - `cube::GameController::onMouseUp()` (0x47E2F0, slot 8).
- [window_proc](../../docs/game/input.md) (0x4C82A0): the window class's procedure; forwards the messages to three
  plasma engine functions (0x651870, 0x651890, 0x6518F0) and destroys the window on close.

## Data flow

1. **Start-up.** `WinMain()` registers the window class with [window_proc](../../docs/game/input.md), calls
   `DirectInput8Create` and creates two devices: the keyboard (a 256-byte state) and the mouse (a 16-byte
   `DIMOUSESTATE`), the mouse in the window's cooperative level. When it fails, it shows "Could not initialize
   DirectInput8".
2. **Each loop, when the window has the focus.** `WinMain()` reads the keyboard state and copies about a dozen keys
   into the controller's bools, reads the mouse state and the cursor position, calls slot 6
   (`cube::GameController::onMouseMove()`), and keeps the cursor in the window (`SetCursorPos`) when slot 1 says so.
   *Focus tests (checked 2026-11-06 for add-native-replay 2.1, 07-20 `Cube.exe`).* The imports hold `GetFocus`, `SetFocus`,
   `GetCursorPos`, `SetCursorPos`, `ScreenToClient`, `ClientToScreen`, `ShowCursor`, `GetClientRect`. No `GetActiveWindow`,
   `GetForegroundWindow`, `ClipCursor`, `IsIconic`, `GetCapture` or `GetAsyncKeyState`. All call sites lie in `WinMain()`
   (0x4C8900 to 0x4C9300): `SetFocus` once at 0x4C9037, `GetFocus` once (loaded at 0x4C9052). Infinite-Cubes `main.cpp`
   shows the one test: keyboard and mouse state are read only when `GetFocus()` is the window. Its window procedure has
   no `WM_ACTIVATE`/`WM_ACTIVATEAPP` case (they go to `DefWindowProcW`), `WM_KILLFOCUS` is a no-op, and `WM_SETFOCUS` and
   `WM_SIZE` call the device reset. No second focus test exists. `getFocusWidget()` is the UI widget focus, not the window.
3. **Key and button events.** Window messages go through [window_proc](../../docs/game/input.md) to the plasma
   engine, which dispatches them to the widgets and to the controller's slots 2, 3, 7 and 8 (to confirm: the engine
   side is not read yet).
4. **Actions.** The per-frame game code (`cube::GameController::update()`) reads the key array and the bools; the
   click handler `cube::GameController::onMouseDown()` starts actions directly. The path from a key or click to an
   ability is 9.1's question.

## Client and server

- Input is client-only: Server.exe imports no DirectInput and has no `cube::Controller`.
- A player's actions reach the server only as network data (the creature update, hits, shots: [network](network.md)).

## Hook points of interest

- `cube::GameController::onKeyDown()` and `cube::GameController::onKeyUp()` (candidate): key bindings and chat
  commands; a mod can add its own keys or commands here.
- `cube::GameController::onMouseDown()` (candidate): clicks in the world.
- The key array at `cube::Controller`+0x19 and the button array at +0x119: which keys are down, for a mod that
  polls.
- Ability keys ([combat](combat.md), probes `ability-slot4` and `ability-mode-end`): key 4 sets GameController+9, +5 is
  the byte the player's 10 s mode rule tests, and +0xA and +0xB are the middle mouse button and W, which the dodge roll
  needs together. The input loop in `cube::GameController::update` reads them against `cube::GameController.ability_bar`, so a fourth slot is a
  sixth bar entry pushed at `cube::GameController::abilityBarBuiltSite`, not a key hook.

## Open questions

- Measured, not open any more (add-native-replay 2.3, 2026-11-06, `kyubu_probe` field finder on `[cube::game_controller]+0x4`, 07-20 build, synthetic keys held one by one; evidence `evidence/native-replay-2026-10-27/headless-2.3/headless23.log`): the 256-entry key array at +0x19 is indexed by virtual-key codes (`W` at index 0x57, `4` 0x34, `1` 0x31, Space 0x20, Shift 0x10). The bools at +0x4 are filled from the DirectInput keyboard state: `1` is +0x6, `4` +0x9, `W` +0xB, Space +0x12, Shift +0x14 and +0x15. Still unmapped: the other bools.

- The plasma engine's side of [window_proc](../../docs/game/input.md): which message goes to which controller slot,
  and whether widgets see the input first.
- Slot 1 of `cube::Controller` and `cube::GameController` (0x47F690): its meaning.
- Where the key bindings live (are they fixed, or read from a file?).
- The input-to-ability path through `cube::GameController::onMouseDown()` or `cube::GameController::update()` (9.1).

## Depth

reconnaissance, reviewed 2026-09-27
