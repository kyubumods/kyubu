# Developer tools

Kyubu draws its tools with Dear ImGui on top of the game (Direct3D 9). They are available to players too; the ones that change running code need **dev mode**. The clip editor [kyubu-animator](#kyubu-animator-the-clip-editor) and the model editor [kyubu-voxedit](#kyubu-voxedit-the-model-editor) are the exceptions: programs of their own, with no game.

## Dev mode

Turn it on with `Cube.exe --kyubu-dev` or `dev_mode = true` under `[loader]` in `kyubu\config.toml`. Dev mode:

- logs at debug level;
- enables the diagnostics panel (**Ctrl+F3**);
- watches mod assets, `kyubu\dev-assets\` and Lua scripts, and reloads them when they change ([assets.md](assets.md), [lua.md](lua.md));
- enables the dev-only console commands `reload` and `lua`.
- activates the SDK's `kyubu_probe` mod, if installed: object inspector, field finder and call tracer on **F8** ([probes.md](probes.md)).

Without dev mode those commands answer `'reload' requires dev mode (--kyubu-dev or loader.dev_mode = true)`.

## Console

Press **`** (the key left of 1; configurable with `console_key`) to open the console; press it again to close it. While it is open, the mouse cursor is shown and the game does not receive keyboard or mouse input (the game's attempts to recentre the cursor are blocked).

Typing helps:

- **Suggestions.** A box above the input lists the commands the first word could become, with their help. A word matches a command when it starts the command's name, or the part after the mod's `id:` (`tp` finds `teleport:tp`). Once the word is a whole command and a space follows, the box shows that command's usage.
- **Tab** completes the first word: one match fills it in, several fill in what they share and list the matches in the console.
- **Up and Down** walk the commands typed before; past the newest, Down brings back the line that was being typed. The last 200 commands are kept in `kyubu/console_history.txt` between runs.

| Command | Does |
|---|---|
| `help` | Lists every command, built-in and from mods. |
| `mods` | Every discovered mod: version, sides, state and the reason when not loaded. |
| `log [n]` | The last `n` log lines (default 20). |
| `sym [name]` | Resolves a symbol: address, status, convention. Without a name: every symbol and whether it is available. |
| `assets <key>` | Which layer serves an asset key (`assets iron-sword1.cub`). |
| `manifest` | The session manifest (JSON). |
| `events` | How often each event has been emitted this session. |
| `hooks [mod]` | Installed hooks and patches: id, kind, owner and target. |
| `game` | The game API's base: the World of each side (address, `joined`, the local player's id, the creatures counted in the map and its size), the game thread, who holds the World's lock, each primitive's availability, and the creature-handle table (live handles per side, handles invalidated since the last report). It also writes the report to the log; `Server.exe` logs it when it first steps its World. `game hold <id>` (dev mode) makes a client-side handle for the creature with that game id (`game hold all`: for every creature); later reports show whether it is still live. |
| `ability list` \| `ability slots` \| `ability rank <id> [rank]` | The fourth ability slot ([abilities.md](abilities.md)): every ability with its number, owner and claim; the owner of slot 4 for each class and specialization; the played character's rank in a mod ability, or set it (clamped to 0 up to the ability's `max_rank`, saved at once). The output is logged too. |
| `anim list` \| `anim show <id>` \| `anim sample <id> [<t>,...]` \| `anim reload` | The animation clips of the enabled mods (`assets/animations/*.anim.toml`): each with its id, duration and track count; one clip's description (what [`kanim describe`](#kanim-the-clip-checker) prints); its pose at fractions of its duration from 0 to 1 (default 0, 0.5 and 1; what `kanim sample` prints); a rescan of the mods' files. The output is logged too. |
| `anim preview <clip> <t_ms>` \| `anim preview off` | (dev mode, with the `anim_capture` mod, [below](#anim_capture-the-animators-game-side)) Holds a clip's pose on the local player at that time, with no ability, mp, cooldown or cast; or gives the draw back to the game. |
| `anim capture <clip> [times_ms] [views]` | (dev mode, with the `anim_capture` mod, [below](#capture-files)) Poses the local player at each time in the two fixed views and writes `sheet.png` and `pose.json` under `kyubu/capture/`. |
| `anim rig` | (dev mode, with the `anim_capture` mod, [below](#rig-and-bake-files)) Writes one frame of the local player at rest: the pose vectors, the part matrices and the model of each slot, under `kyubu/capture/rig-<stamp>/`. |
| `anim bake <pose id> [duration_ms]` | (dev mode, with the `anim_capture` mod, [below](#rig-and-bake-files)) Plays a vanilla pose on the local player, writes every draw's vectors and matrices, plays the baked vectors back as a clip and reports how far the two runs differ. |
| `config [<mod> [<key> <value>] \| reload <mod>]` | Mod options: list the mods with options, show one mod's values, set one, or re-read its file ([config.md](config.md)). |
| `reload <mod>` | (dev) Reloads a mod's scripts; assets reload by themselves in dev mode. |
| `lua <client\|server> <code>` | (dev) Runs Lua in a side's VM and prints the result: `lua server return 1 + 1`. |

Mods add commands, named `<mod id>:<name>`:

```lua
kyubu.command("mymod:heal", "heal <amount>: heal the player", function(args, print)
  print("healing " .. args)
end)
```
```c
static void KYUBU_CALL cmd(const char* args, KyubuPrintFn print, void* ctx, void* user) { print(ctx, "hi"); }
api->register_command(self, "mymod:hi", "say hi", cmd, NULL);
```

Commands run on the client side; their handler faults are isolated like any other callback.

## Mod windows

Mods draw their own windows from `render.overlay` handlers with the `ui_*` functions (`kyubu.ui` in Lua), or with the full Dear ImGui API from C++ through `api->imgui_context()` (built against Dear ImGui 1.91.5; call `ImGui::SetCurrentContext` with it and link the same ImGui version). While the mouse is over a window or a text field is active, the overlay takes the input and the game does not see it, so clicking a button does not also attack.

## Diagnostics panel

**Ctrl+F3** (dev mode) opens a panel with:

- frame time and FPS, process working set, Lua memory per side;
- one row per mod: state, callback time in the last frame (ms) and number of hooks;
- the size of every registry.

A mod whose handlers are slow stands out immediately: a Lua handler that busy-waits 10 ms per frame shows about 10-12 ms in its row (checked with the `fx_slow` fixture). The same numbers are available to mods through `api->mod_stats` / `kyubu.stats`.

## Start-menu notices

On the start menu Kyubu shows a short notice: how many mods loaded, or which mods failed and where the log is. Safe mode and missing-content warnings ("This world uses content from missing mods: ...") use the same corner. Notices fade after a few seconds and are also written to the log.

## Logs and crash reports

- `kyubu\logs\latest.log`: `2026-09-25 17:02:11.482 INFO  [hello_lua] [client] message`. The five previous logs are kept as `log-1.log` (newest) to `log-5.log`.
- `kyubu\logs\crash-<time>.txt` and `.dmp`: see [install.md](install.md#crash-recovery).

## auto_enter, skipping the title menus

`mods-dev/auto_enter` takes the game straight from the title screen into a saved single-player world, with no clicks. Once the
title screen has been up for `delay_frames` frames (20), it does what Start Game and the two Select buttons do, through the game's
own handlers (`character_select_click`, then `world_select_click` on the next frame), and the game loads the world as usual.
Its options (`kyubu/config/auto_enter.toml`):

| Option | Default | Meaning |
|---|---|---|
| `enabled` | `true` | enter by itself; `false` leaves it to the console command |
| `character` | empty | the saved character: empty for the first, a number for its place (0 is the first), or its name |
| `world` | empty | the saved world, the same way |
| `delay_frames` | `20` | frames the title screen stays up first |

`auto_enter:go [character] [world]` does it from the title screen on the next frame. With no saved character or world the mod logs
`no saved character matches` (or world) and leaves the menu alone, so a `new-world` replay still works with it installed.

In a replay it replaces gamepilot's menu clicks: replay a `new-world` record with `--keep`, copy its sandbox
(`$KMAP_HOME/tmp/replay-<id>/wine`) to the folder of a `setup = "menu"` record that lists the mod, delete its `kyubu/logs/latest.log`,
and run that record with `KEEP=1`. [auto-enter-0720](../mapping/probes/auto-enter-0720.toml) entered the world 3.4 s after
`game_ready` that way (2026-10-27); gamepilot's clicks had taken about 60 s.

## kanim, the clip checker

`kanim` checks and describes an ability animation clip (`assets/animations/<name>.anim.toml`) without the game. It is built from the loader's own clip source, so it reports what the game would. Build it with `ninja kanim`; the program is `tools/kanim/kanim.exe` in the build folder.

| Command | What it prints |
|---|---|
| `kanim check <clip> [--json]` | `<clip>: ok`, or each error with its line and field path. With `--json`: one line with the code, the message and a fix hint for each error. |
| `kanim describe <clip> [--ability-ms N] [--charge N] [--speed N] [--rank N]` | The range of each track, its fastest motion (speed, time, size), jumps, pops at the start and end, the duration against the ability's, the hide intervals and the markers. A generator track (`osc`, `spring`, `expr`; [animations.md](animations.md#procedural-tracks)) has no keys, so it is read from 400 samples of the evaluator and its numbers are written out; an `expr` that reads a signal says which, and is shown with the `--charge`, `--speed` and `--rank` given (default 0, 0 and 1). |
| `kanim sample <clip> [--t 0,0.25,0.5] [--ability-ms N] [--charge N] [--speed N] [--rank N]` | The pose of each animated part at those fractions of the clip (default 0, 0.5 and 1), from a base pose of zeros, with the blend weight. A clip with an `expr` track names the signals it was sampled with. |
| `kanim bake <clip> [--ability-ms N] [--charge N] [--speed N] [--rank N]` | The clip as plain TOML with every key written out. A generator track becomes keys, one for every 1/60 s of the clip (at most 256), the two ends included, with the signals frozen at the values given; a comment line after the tracks says which. Comments of the source are not kept. |

The exit code is 0 when the command ran, 1 when the clip is invalid (the errors are printed) and 2 for a usage error or an unreadable file. A clip with `duration = "ability"` needs `--ability-ms` for times in ms; without it `describe` and `sample` assume 1000 ms and say so.

## kyubu-animator, the clip editor

`kyubu-animator` edits ability animation clips ([animations.md](animations.md)) in a window of its own. It is a standalone program: it does not start the game, does not need it and does not call the game's API. It reads and writes the clip file. A game in dev mode reloads a changed clip file by itself, so a save is drawn the next time the ability's key is pressed ([animations.md](animations.md#tools)). Build it with `ninja kyubu-animator`; the program is `tools/animator/kyubu-animator.exe` in the build folder, with `rigs/` beside it.

```
kyubu-animator [clip.anim.toml] [--rig <file.rig.toml>] [--models <folder>] [--library <folder>]
kyubu-animator --sheet <clip.anim.toml> --out <sheet.png> [--t 0,0.5,1] [--ability-ms N] [--rig ..] [--models ..] [--library ..]
kyubu-animator --parity <capture> [--parity <capture> ...] --rig <file.rig.toml> [--models ..] [--tolerance-pos N] [--tolerance-angle N] [--mark]
kyubu-animator --version
```

| Argument | Meaning |
|---|---|
| `clip.anim.toml` | The clip to open. Without it the window opens empty: **File > Open clip** or **New clip**. |
| `--rig <file>` | The rig to draw the figure with. Without it: `rigs/standin.rig.toml` beside the program. |
| `--models <folder>` | Where a rig's named `.cub` models are looked up. A model that is not there or not valid is drawn as a box of 8, and the parts list says why. |
| `--library <folder>` | The pose library folder for this run ([below](#the-window), "Pose library"). Without it: the folder the window last scanned. Not saved by the option itself. |
| `--sheet <clip>` | Writes the labelled sheet of the mimic ([below](#the-offline-sheet)) and exits, with no window. Needs `--out`. |
| `--out <png>` | The sheet's file (with `--sheet`). The folder must exist. |
| `--t <fractions>` | The times of the sheet, as fractions of the clip from 0 to 1, comma between, at most 12 (as `kanim sample --t`). Default `0,0.5,1`. |
| `--ability-ms <n>` | What `duration = "ability"` means for the sheet, in ms. Default 1000, as `kanim` assumes. |
| `--parity <path>` | Compares the rig with the game's part matrices ([below](#the-parity-check)) and exits, with no window. The path is a `pose.json`, a capture folder, or a folder of capture folders; give it more than once. Needs `--rig`. |
| `--tolerance-pos <n>`, `--tolerance-angle <n>` | The limits of the parity check: voxels (default 0.25) and degrees (default 1). |
| `--mark` | Writes `approximate = true` into the rig file for each node outside the tolerance. It never clears a mark. |
| `--version` | Prints the version and exits with no window (for CI). |

The exit code is 0 when done, 1 when the window could not be created or the sheet or the parity could not be written, 2 for a usage error, and 3 when the parity report is written and a node is outside the tolerance.

### The window

- **View.** The figure is drawn from the rig at the scrub time. **Front** and **Left** are the two fixed views of the game's capture (the figure faces the camera, or its left side does); a drag makes the view free. Drag with the left button in empty space to orbit, with the right or middle button to pan, and use the wheel to zoom. **View > Frame the figure**, the ground grid and the axis triad (x red, y green, z blue) are in the View menu.
- **Parts.** The rig as a tree. A `*` marks a part that a track moves now; an orange line names a part the clip writes and the rig lacks. The checks of the clip are the first thing under the clip's name (below).
- **Posing.** Click a part to select it; its handles are drawn in the frame of the rig's own steps: a ring for each rotation the part has and an arrow for each position. Dragging a handle writes a key at the scrub time, with one undo step for the whole drag. A track that does not exist is made, anchored with zero keys at 0 and at 1. The blend weight at that time is divided out, so the part on screen moves by the pointer's distance (not where the weight is below 0.05: scrub to where the blend is full). **Escape** cancels a drag.
- **Timeline.** One row per part and channel that has a track, then `hide` and `markers` (shown, not edited). Click or box-select keys (Ctrl adds), drag to move (snapped to 10 ms; **Snap** changes it), double-click a row to add a key, **Del** deletes, **Ctrl+C** and **Ctrl+V** copy and paste at the scrub time. Ctrl+wheel zooms the ruler, Shift+wheel or a middle drag pans it. A press on the ruler scrubs. **Space** plays and pauses; **Loop** and **Speed** (0.1x to 2x) are beside it.
- **Key and Curve.** For the selected keys: the time, the ease, the three values (sliders or typed), **Reset to neutral**, and **Mirror L/R**, which writes the other hand's tracks from this hand's ([animations.md](animations.md#frame-and-order) has the rule). The curve view draws the track's three components and lets a dot be dragged.
- **Files.** **File > New clip, Open clip, Save** (**Ctrl+S**; the file is replaced whole, so the game never reads half a file) and **Edit > Undo** (**Ctrl+Z**, 100 deep). The program looks at the open file every 400 ms: an outside edit loads at once when there are no unsaved changes, and asks when there are. After 30 s of unsaved changes the text goes to `<clip>.anim.toml.autosave`; the next start offers to recover it. A save, or an undo back to the file's text, removes it. A clip that does not validate cannot be edited: the errors show in the checks list.
- **Checks.** The loader's own validator, so every error has its line and field path, as `kanim check` prints. Two warnings are the Animator's own: **Ability length (ms)**, typed in the window, against a clip's literal `duration` (`longer_than_ability`: the clip's end is never drawn), and a part the clip writes that the rig has no node for (`rig_lacks_part`).
- **Pose library.** The baked vanilla poses that `anim bake <pose id>` writes ([below](#rig-and-bake-files)), read from a folder outside the repository: type its path in the **Pose library** panel and press **Scan** (the path is kept in `%APPDATA%\kyubu-animator\settings.json`; `--library` overrides it for one run). The scan reads the `bake_pose<id>-<stamp>` folders directly under the path and under its `captures` folder; the latest stamp of a pose wins; a `rig` or `capture` file is passed over and a `bake` file that cannot be read is named in orange. A clip with a `[borrow]` is drawn over the borrowed pose at the retimed clock (`offset_ms + elapsed * speed`, as the game does); the view says "borrowed pose N at T ms", and past the end of the baked pose it says that the game draws pose 0 there. A clip that borrows a pose the library lacks is drawn over the neutral pose, and the view and the panel name the pose. **Show** draws one baked pose alone at the **pose time** slider (the clip's tracks are not drawn, and posing is off). **Capture at t** writes the pose's vectors at the pose time as one key in each of 11 `set` tracks (the `pos` of every part but the tail, and the `rot` of all six), at the clip's scrub time: a key that is already there is replaced, and a part that also has an `add` track is named. **Bake to keys** turns the whole pose into keys from the clip's time 0: every draw, thinned to 30 a second at most, in the same 11 tracks; it replaces the `set` tracks of those parts and leaves `add` tracks, and a draw after the clip's end is dropped. Each is one undo step. A baked pose holds the part vectors only: the parts the game moves from the walk and the turn (the feet, the shoulders) stay at the rig's neutral pose.
- **In game.** The **In game** panel is off at every start. With **Send to the game (dev mode)** on, you type the game folder (the one with `Cube.exe`; it is kept in the settings file with the pose library folder) and the clip's id in the game (`<mod id>:<name>`, taken from the clip's path when it is `<mod>/assets/animations/<name>.anim.toml`). The **In game** button then drops `anim preview <clip> <t_ms>` at the scrub time in the game's `kyubu/inbox/` ([the file inbox](#the-file-inbox)). The clip must be saved where the game reads it; the game's hot reload takes the file. `anim preview off` in the game's console ends the preview.
- **Sounds.** The **Sounds** panel plays the rig's [`[[sound]]`s](#sounds) at the clip's markers while the clip plays (**At markers**; off, the view's top line says "sounds off"), stops them all (**Stop**) and sets the volume of every sound (0 to 2). Each of the rig's sounds is a line: a click plays it with its own volume and pitch, its tooltip gives the file found and its format, an orange line is a file not found and a grey one a marker the clip lacks. Below it, type a folder of `.wav` files and press **Scan** (the folder and the volume are kept in the settings file with the pose library folder): the list holds that folder's files and the clip's mod's `assets/sounds/`, a click plays one as it is, and its tooltip gives its format. The game's own sounds are in `<work>/vanilla/sounds` once `cwmod extract` has run ([assets.md](assets.md)). The view's top line counts the sounds playing.
- **Sheet.** The **Sheet** panel writes the same sheet as `--sheet` for the clip on screen, unsaved edits included: the times typed there, the file typed there or, when empty, `<clip>.sheet.png` beside the clip. The status line says where it went, and a note when the base pose was missing.

The Animator edits the text of the file at the spans of the keys it changes: comments and layout stay, and a CRLF file stays CRLF. A key edited from the inspector is written again as a whole (`{ t = ..., v = [...], ease = "..." }`), so the spaces that aligned it are not kept.

### The offline sheet

`kyubu-animator --sheet <clip> --out <png>` draws the clip on the rig at each time, in the **front** and **left** views, with no window and no game, so a session that cannot open the program can read the pose. It uses the layout, the font and the PNG of the capture's `sheet.png` ([Capture files](#capture-files); the header, the label strips and the 256-pixel cells are the same code), so the two sheets are read the same way: a row per time, a column per view.

- **The header names the rig** (`mimic - rig <rig name> - <clip>`), so a sheet of the mimic is never taken for a capture of the game. A header that does not fit the sheet's width loses the clip name first. When a `[borrow]` clip's pose is not in the pose library the header says `no pose <id>` and the command prints a note: the neutral pose stands for it.
- **Each label** is the time in ms and the view, as the capture's: `500ms front`. One camera frames the figure over all the times, so a part's size and place compare from a row to the next.
- **The picture is the mimic's own**: the rig's boxes or `.cub` models with a flat shade and the ground grid, drawn by a small software rasteriser. It is as right as the rig is (the stand-in rig's numbers are made up), and it is not the game's look.
- **The library** is `--library`, or else the folder the window last scanned. A clip with no `[borrow]` does not need one.

### The parity check

`kyubu-animator --parity <capture> --rig <rig>` measures how far the mimic is from the game. Every frame of a capture file ([Capture files](#capture-files): `anim capture`, `anim rig` and `anim bake` write them) holds the pose vectors the game used and the part matrices it left. The command solves the same vectors with the rig and compares, for each node that has a `slot` ([Rig files](#rig-files)), the node's frame with the matrix of that slot. The root node is the reference: every other node is measured from the root's origin, so the creature's place in the world does not matter.

- **Position** is the distance between the two centres of the node, in voxels. The capture's translation is divided by the rig's `[capture] unit`, with the frame's `body_yaw` turned back (the game draws the figure turned by it, and the rig's figure faces +y); with `origin = "corner"` the matrix is moved to the model's centre by half of its size, which is the size the `.cub` file declares.
- **Angle** is the angle of the rotation that takes the rig's axes to the capture's, in degrees. The matrices are orthonormalised first (the game's carry the model's scale). A node that is mirrored in one and not in the other is its own verdict, `mirror`.
- A matrix the game left all zero, or with a number that was not finite (a part hidden by the clip), is not compared in that frame.

The report has a line for each node with the largest errors over all the frames, a verdict (`ok`, or `APPROXIMATE` with what is outside: `position`, `angle`, `mirror`) and, for each node outside the tolerance, the frame of its worst error. A node that is outside is **approximate**: the figure still draws it, but the view tints it and the parts list marks it `~` (hover for the reason). `--mark` writes the mark into the rig file (`approximate = true`), so the next window shows it. Run it over the rest capture, over the poses of a library and over a clip's capture; the tolerance of the mimic gate is 0.25 voxel and 1 degree.

```
parity: rig "player rig", 4 capture(s), 31 frame(s), tolerance 0.25 voxel and 1 degree
node             slot  frames  pos (voxel)  angle (deg)  verdict
head_model          3      31         0.08         0.00  ok
hand_l              9      31        13.94         0.00  APPROXIMATE (position)
...
```

The numbers of the game are not in the program: the unit, the way the yaw turns and the place of the model's origin are in the rig's `[capture]` table, and the matrix each node answers to is its `slot`. A rig without them cannot be checked, and the stand-in rig has none.

### Rig files

A rig (`*.rig.toml`) says where each part rests and how its pose vectors become its place, so the figure can move as the game's does. It holds no code. The stand-in rig `tools/animator/rigs/standin.rig.toml` ships with the program: a box figure with the parts of a clip, **whose numbers are made up** (the frame and the order of angles are those of [animations.md](animations.md#frame-and-order)). The view names the rig at its top right. A rig made from the game is private and does not belong in this repository.

```toml
name = "stand-in rig"          # required: what the view says

[[node]]
name   = "head"                # required: 1 to 48 of a-z, 0-9, _ ; unique
parent = "body"                # every node but one (the root) has one
part   = "head"                # optional: the clip part a click on this node edits
model  = "box 8 8 8"           # optional: `box W D H` in voxels, or the name of a .cub model
approximate = false            # optional: tints the part in the view
steps  = [["translate", 0, 0, 11],
          ["rot_x", "head.rot.x"], ["rot_y", "head.rot.y"], ["rot_z", "head.rot.z"],
          ["translate", "head.pos"]]
```

| Key | Meaning |
|---|---|
| `name` | The rig's name (top level). |
| `[[node]]` | One node of the tree, up to 64. Any other key in a node is an error. |
| `parent` | The node this one hangs from. Exactly one node has none. |
| `part` | The clip part the node answers to: `root`, `body`, `head`, `hand_r`, `hand_l` or `tail`; `weapon_r` and `weapon_l` are accepted as the names of the hands. |
| `model` | `box W D H`, centred on the node, or a `.cub` model's name looked up in `--models`. |
| `approximate` | `true` tints the part in the view. The [parity check](#the-parity-check) sets it with `--mark`. |
| `slot` | 0 to 16: the capture matrix the parity check compares this node with ([Capture files](#capture-files) lists the slots). The root's slot is the reference. |
| `steps` | Up to 16 steps. The node's matrix is their product in the order written: **the first step is the outermost and the last acts on a point first**, so a `translate` of the part's `pos` written last is turned by the part's `rot`. |

A step is one of:

| Step | Arguments |
|---|---|
| `translate` | x, y, z in voxels |
| `rot_x`, `rot_y`, `rot_z` | one angle in degrees (right-handed; x right, y forward, z up) |
| `scale` | one number for every axis, or three |

An optional `[capture]` table (before the first `[[node]]`) says how a capture's matrices sit in the rig's frame, for the parity check:

| Key | Meaning |
|---|---|
| `unit` | Required in the table. The capture's length unit per voxel (the game's matrices are in world units: a voxel is `unit` of them). |
| `yaw` | `"ccw"` (default) or `"cw"`: which way a frame's `body_yaw` turns the figure, seen from above. |
| `origin` | `"centre"` (default) or `"corner"`: where a capture matrix puts a model, at its centre or its lowest corner. |

An argument is a number or the name of a value of the pose: `<part>.pos` (three numbers), `<part>.pos.x` (one), `<part>.rot` (three), `<part>.rot.x` (one). A leading `-` negates it. A rig that is not valid is refused as a whole; each error carries its line and field path, with these codes: `toml_syntax`, `too_large`, `missing_field`, `unknown_key`, `bad_type`, `bad_name`, `bad_model`, `duplicate_node`, `unknown_parent`, `parent_cycle`, `no_root`, `two_roots`, `too_many_nodes`, `too_many_steps`, `unknown_step`, `bad_arg_count`, `unknown_part`, `unknown_ref`, `bad_value`, `out_of_range` and `not_finite`.

#### Particles

A rig can have up to 16 `[[emitter]]`s: each rides on a node and throws small cubes, moved the way the game moves its own particles, so an effect that a mod will build from the game's particles ([`particle_spawn`](game-api.md#particle_spawn), EMTweaks (in the `emtweaks` repository)) is shaped and timed against the clip first. The window and the [offline sheet](#the-offline-sheet) draw them; the view's top line says how many are alive. A clip switches an emitter on and off through a value of the pose (its `amount`), so a preview gives the emitter a channel that the figure does not use, as for a prop.

```toml
[particles]
block   = 11.63                # voxels of the rig in one block of the game
gravity = 30                   # blocks a second squared, down

[[emitter]]
name     = "breath"            # required: as a node's name, unique among the emitters
parent   = "head"              # required: the node it rides on
steps    = [["translate", 0, 18, -4], ["rot_x", -18]]   # its frame in the node's: particles leave along its +y
amount   = "tail.pos.x"        # the share of `rate` thrown: a number or one value of the pose (default 1)
rate     = 240                 # required: particles a second at amount 1
life     = [500, 700]          # required: milliseconds, one number or [least, most]
speed    = [30, 40]            # blocks a second along +y
cone     = 8
spreading = 1.5
size     = 0.5
size_end = 0.1
color    = [1, 0, 0]
color_random = [1, 1, 0]
color_end = [0.4, 0.1, 0.05]
gravity  = true
ground   = "pass"
spin     = 50
seed     = 1
```

| Key | Meaning |
|---|---|
| `[particles]` `block` | Required (above 0). Voxels of the rig in one block: an emitter's lengths and speeds are in blocks, as the game's particle record's are. |
| `[particles]` `gravity` | Required. Blocks a second squared, down, for an emitter with `gravity = true`. The game's own is in the mapping ([combat atlas](../mapping/atlas/combat.md), "What a record becomes"). |
| `parent`, `steps` | The node it rides on and its frame in that node's, as a node's: the particles leave along the frame's +y. |
| `amount` | A number, or one value of the pose (`"tail.pos.x"`, `"-root.rot.z"`): `rate` is multiplied by it; 0 or less throws nothing. Default 1. |
| `rate` | Required, 0 to 5000: particles a second at amount 1. |
| `step_ms` | 1 to 1000, default 16: one burst every `step_ms`, all its particles from one point at one time, as one record of the game is. The counts carry their fractions, so `rate` comes out on average. |
| `speed` | Blocks a second along +y, 0 to 1000: one number, or `[least, most]` for a random speed each. Default 0. |
| `cone` | 0 to 180 degrees: each particle's way is turned at random up to this from +y. Default 0. |
| `spreading` | 0 to 64 blocks a second: each axis of the speed gets a random -spreading to +spreading, as the game's record field does. Default 0. |
| `life` | Required, 1 to 60000 ms: one number or `[least, most]`. |
| `size`, `size_end` | Blocks, the cube's side at birth (above 0 to 16, default 0.25) and at the end of its life (default `size`), in a straight line between. |
| `color`, `color_random`, `color_end` | RGB, 0 to 1 (a fourth number, alpha, is read and not drawn: the game draws these particles opaque). Each particle takes a random mix of `color` and `color_random` when it is given, then turns toward `color_end`, when given, over its life. Default white. |
| `gravity` | `true` (default): it falls by `[particles] gravity`. |
| `ground` | `"pass"` (default): through the ground (z 0), as the game's particle records do. `"stop"`: it lands at z 0 and stays there, turning no more. |
| `spin` | 0 to 3600 degrees a second: each particle turns about each axis at a random rate up to this. Default 0. |
| `seed` | 0 to 2147483647, default 1: the random numbers. The same rig and clip give the same particles at a time, scrubbed forward or back. |
| `at`, `for_ms` | A marker's name, and 1 to 60000 ms (default 100): the emitter throws only for `for_ms` from each of the clip's `[[marker]]`s of that name (in a looping clip, a window that runs past the end goes on at the start), so a burst is timed as a mod times it from an ability's step, with no channel. `amount` still multiplies it. `for_ms` without `at` is refused. |
| `cone_min` | 0 to `cone` degrees, default 0: each way is at least this far from +y, so the ways fill the band between the two cones (`cone_min = 70`, `cone = 85` on an upward frame: low over the ground, every way round). |
| `offset` | Blocks, one number or `[least, most]`, default 0: each particle starts this far from the emitter across the frame's x-z plane, on the side its own way leans (a random side for a straight way), so the particles rise from a ring round +y rather than one point. |

What the game's particle records do, and what a mod adds: a record throws `count` cubes from one point, each with the record's speed (its `accel` field) plus a random -`spreading` to +`spreading` on each axis; they fly straight, fall at the game's gravity unless their type is 4, spin, keep their size and colour, live 4 s and pass through blocks. So `speed` along a way and `spreading` are one record each burst; `cone` is a record per particle (or per few); a `life` shorter than 4 s, `size_end`, `color_end` and `ground = "stop"` need a mod that writes the live particles each step (`skills_unleashed`'s `lib/fx` already writes their speed, life and size). The game's type 1 picks a random colour from red-orange to yellow itself: `color = [1, 0, 0]`, `color_random = [1, 1, 0]`.

An emitter's errors use the codes above (`duplicate_node` for a second emitter of one name, `too_many_nodes` for more than 16); a rig with an emitter and no `[particles]` table is refused with `missing_field`.

A particle with `ground = "stop"` that starts at or under the ground and is thrown upward flies, and stops where it comes back down; one that is not rising stays where it is.

#### Effects

Two more tables, `[[shockwave]]` and `[[pillars]]` (16 of each at most), draw the effects a mod adds round an impact, so they are timed against the clip as the particles are. Each is started by the clip's markers of one name (`at`): at each such marker it is placed by its frame there (its `parent` node and `steps`, as an emitter's) and stays in that place, as a mod spawns it in the world from an ability's step. Their lengths are in blocks, so a rig with one needs the `[particles]` table. The view's top line counts the rings and pillars alive; the window and the [offline sheet](#the-offline-sheet) draw them (the sheet's views are level, so a ring on the ground is seen edge-on there).

```toml
[[shockwave]]
name   = "slam"
parent = "ground"
steps  = [["translate", 0, 15, 0.3], ["rot_x", 90]]   # the ring lies in the frame's x-z plane, facing +y: here flat on the ground
at     = "strike"
inner_radius = 1.5
thickness    = 1.4
expansion    = 0.45
life_ticks   = 34
fade_on_tick = 12
fade_speed   = 0.045
color        = [0.93, 0.86, 0.7, 0.7]
pixel_size   = 0

[[pillars]]
name   = "rocks"
parent = "ground"
steps  = [["translate", 0, 15, 0.3], ["rot_x", 90]]   # they stand on the frame's x-z plane and rise along +y
at     = "strike"
count  = 20
radius = [3.5, 14]
clear  = [-2.8, -12.8, 2.8, 6.8]
wave   = 22.5
delay  = [0, 90]
width  = [1, 2]
height = [1.6, 3.6]
stones = [2, 4]
rubble = [1, 4]
taper  = 0.6
cell   = 0.15
lean   = 14
turn   = 45
rise_ms = 110
hold_ms = [550, 1050]
sink_ms = 420
color  = [0.66, 0.33, 0.21]
color_random = [0.52, 0.42, 0.3]
color_top = [0.33, 0.52, 0.22]
seed   = 1
```

A `[[shockwave]]` is EMTweaks' shockwave (in the `emtweaks` repository): its keys after `at` are the fields of the mod's definition, with their names and units (blocks; a tick is 20 ms), and the Animator builds the ring's square cells with the mod's own code (`mods-gameplay/emtweaks/src/shockwave.hpp`), so the cells grow for a large ring as they do in the game. It is drawn see-through, after everything else, blended by its alpha and hidden by what stands in front of it, as the mod draws it. The mod's ring stands off its plane by its `lift_blocks` option; give the frame the same small lift.

| Key | Meaning |
|---|---|
| `name`, `parent`, `steps`, `at` | Required but `steps`: as an emitter's. `at` names the markers that start a ring. |
| `inner_radius` | Blocks, 0 or more, default 0: the empty middle at birth. |
| `thickness` | Blocks, 0.01 or more, default 0.5: outer radius minus inner. |
| `expansion` | Blocks a tick, default 0: how fast both radii grow; negative shrinks it. |
| `life_ticks` | Whole number, 1 to 6000, default 40. |
| `fade_on_tick`, `fade_speed` | The tick the fade starts on (whole, default 0) and the share of the alpha lost a tick (0 to 1, default 0: no fade). |
| `color` | RGBA, four numbers 0 to 1, default white: the alpha is the opacity at birth. |
| `pixel_size` | Blocks, -1 to 16, default 0: the cells' side; 0 is the mod's default (its `pixel_size` option, 0.25); negative draws a smooth ring. |

A set of `[[pillars]]` is `count` rock pillars that come up round the frame's origin: each slides up out of the ground (fast, about 5 % past its height, then settling), stays, and sinks back. A pillar is a stack of voxel stones on a grid of `cell` blocks: each stone is about as tall as it is wide (a short pillar gets fewer), narrower than the one below by the `taper`, sits a voxel or so off it and leans its own way by a voxel or two; random cuts across its corners and edges make its facets. A groove runs under each stacked stone, the rim of each top is lighter, its foot darker, and some sides have a pale crack. A few smaller stones (`rubble`) lie level round its foot even when it leans. The pillars' random numbers come from the `seed`, the set's place in the rig, the marker's number and the pillar's own, so scrubbing shows the same pillars, stone for stone.

| Key | Meaning |
|---|---|
| `name`, `parent`, `steps`, `at` | Required but `steps`: as an emitter's. `at` names the markers that start a set. |
| `count` | Required, 1 to 256: the pillars of a set. |
| `radius` | Required, blocks, one number or `[least, most]`: their feet fall evenly over the ring between the two. |
| `clear` | Blocks, `[least x, least y, most x, most y]` in the figure's frame: no foot falls in this box (the body standing there); a foot drawn in it is drawn again, up to 16 times, then the pillar is left out. |
| `wave` | Blocks a second, default 0: a pillar `r` blocks out starts `r / wave` after the marker, so the pillars come up as a ring passes (a ring's speed is `expansion` x 50). 0: all at once. |
| `delay` | ms, one number or `[least, most]`, default 0: a random wait more. |
| `width`, `height` | Blocks above 0, one number or `[least, most]`: the side of its foot (up to 16, default 1) and its height out of the ground (up to 64, default 2). |
| `stones` | 1 to 8, one number or `[least, most]`, default `[2, 4]`: the stones stacked in a pillar, rounded; fewer when the pillar is too short for that many. |
| `rubble` | 0 to 8, one number or `[least, most]`, default `[0, 3]`: the loose stones round its foot, rounded. |
| `taper` | 0 to 1, default 1: the top stone's side over the lowest's. |
| `cell` | Blocks, 0.02 to 16, default 0.25: the side of its voxels. A pillar over 40 voxels across or 80 up gets bigger ones. |
| `lean`, `turn` | Degrees: each leans out from the centre by a random 0 to `lean` (up to 60; its foot reaches under the ground so it never floats) and is turned about its axis by a random -`turn` to +`turn` (up to 180). Default 0. |
| `rise_ms`, `hold_ms`, `sink_ms` | ms: its time to come out (default 120), its time out (one number or `[least, most]`, default 800) and its time to go back in (default 400). |
| `color`, `color_random`, `color_top` | RGB, 0 to 1: its stones (default earth brown; each stone, voxel, groove and crack is a shade of it), a random mix towards `color_random` per pillar as for an emitter, and the rim round each stone's top (default its stone's, lightened). |
| `seed` | 0 to 2147483647, default 1. |

The game has no pillar of its own: a mod would draw them as voxel models (as EMTweaks draws its pulse (in the `emtweaks` repository) and chains (in the `emtweaks` repository)), with these numbers. Errors use the codes above (`duplicate_node`, `too_many_nodes` for more than 16 tables, `missing_field` for a missing `at`, `count` or `radius`, or no `[particles]` table).

#### Sounds

A `[[sound]]` table (32 at most) plays a `.wav` file when the playback passes the clip's markers of one name, as a mod plays a sound from an ability's step (`sound.play` in `skills_unleashed`, or the game's `play_sound`). A sound has no place and no frame, so it needs no node and no `[particles]` table.

```toml
[[sound]]
file     = "hit.wav"     # required
at       = "hit"         # required
volume   = 0.8
pitch    = 1.25
delay_ms = 100
```

| Key | Meaning |
|---|---|
| `file` | The file's name: 1 to 64 letters, digits, `_`, `-` and `.`, ending in `.wav`. It is looked for in the clip's mod's `assets/sounds/` first (the clip is `<mod>/assets/animations/<name>.anim.toml`), as the game serves a mod's own file before its own, then in the Sounds panel's folder. |
| `at` | A marker's name: the sound plays at each marker of that name. |
| `volume` | 0 to 4, default 1. 1 plays the file as it is, as `sound.play`'s volume 1 does at a sound option of 100. The panel's volume multiplies it. |
| `pitch` | 0.25 to 4, default 1: the speed it is played at, so 2 is an octave up and half as long (the game's `pitch`). |
| `delay_ms` | 0 to 60000, default 0: it plays this long after the marker. A sound due after the clip's end plays in the next lap when **Loop** is on, and not at all when it is off. |

The playback plays a sound when it passes the sound's time: a scrub is silent, and the first step after **Play** includes the time it starts from, so a marker at 0 is heard (as the game tells it to the first step). Sounds overlap, each on its own voice, and ring out past the clip's end and into the next lap; a pause by hand stops them all. The window plays through XAudio2 (`xaudio2_9.dll`, else `xaudio2_8.dll`), loaded when it opens: where neither is, or there is no output device, the Sounds panel says why and the window works as before, silent. A `.wav` must be PCM of 8, 16, 24 or 32 bits or float of 32 bits, 1 to 8 channels, 1000 to 200000 frames a second; the game itself loads only 16-bit PCM and 32-bit float, and the panel's tooltip says so for a file it would refuse. Errors use the codes above (`missing_field` for a missing `file` or `at`, `bad_value` for a name that is not a `.wav` file's, `out_of_range`, `too_many_nodes` for more than 32 tables).

## kyubu-voxedit, the model editor

`kyubu-voxedit` edits `.cub` voxel models ([assets.md](assets.md)) in a window of its own, beside the Animator: no game, no loader. Build it with `ninja kyubu-voxedit`; the program is `tools/voxedit/kyubu-voxedit.exe` in the build folder.

```
kyubu-voxedit [model.cub | figure.rig.toml] [--models <folder>]
kyubu-voxedit --version
```

It opens either:

- **one `.cub`**, standing on the ground grid;
- **a rig** ([Rig files](#rig-files)): every node with a model, placed by the rig at its rest pose (every pose value 0), as the Animator draws it. Named models are read from `--models`, or else from the rig's own folder, and can be edited; a `box W D H` is drawn but cannot be. The **Pieces** list on the right chooses the piece to edit (or Ctrl+click it in the view); the other pieces are dimmed, and **H** hides them.

| Tool | Key | Does |
|---|---|---|
| Add | 1 | Adds voxels on the face under the pointer (or on the model's floor where there is none). A one-voxel drag stays on the layer it started on. |
| Remove | 2 | Empties voxels. A one-voxel drag stays on its first layer, so it does not dig. |
| Paint | 3 | Recolours voxels. |
| Pick colour | 4 | Takes the colour of the voxel clicked. Shift+click does the same with any tool. |
| Fill | 5 | Recolours the touching voxels of the colour clicked, or (**Every voxel of that colour**) that colour everywhere. |
| Box | 6 | Two clicks for two corners: fills, clears or paints the box between them. Esc drops the first corner. |

- **Brush:** size 1 to 32 (`[` and `]`), cube or sphere, for Add, Remove and Paint.
- **Mirror** (M): every edit is repeated at -x, the plane x = 0 of the figure (drawn in purple). On a rig the image lands on the piece that holds it, so an edit of the left wing reaches the right wing.
- **Slice:** shows and edits only a box of the active piece's cells (x, y and z ranges), to reach the inside and hidden faces. PgUp and PgDn move its top layer.
- **Colour:** an RGB picker, the recent colours and the model's own colours, most used first. Pure black is an empty voxel in a `.cub`, so it is stored as (1, 1, 1).
- **Growing:** an Add past a model's edge grows it by the same number of layers on both sides of that axis, and **Edit > Trim empty borders** removes empty layers the same way, so a piece keeps its place in its rig (a model is centred on its node). A side stays at most 256.
- **Undo** (Ctrl+Z) and **redo** (Ctrl+Y) keep 200 steps; a stroke, a fill or a box is one step, mirror images included.
- **Save** (Ctrl+S) writes every changed model over its file (through a `.tmp` file, so a file is never left half written). The first time a file is overwritten its old bytes are kept beside it as `<file>.bak`, once: the `.bak` stays the original. A model with no voxel left is not saved. **Save as** is for a single model.
- **View:** right drag turns, middle drag (or Shift + right drag) moves, the wheel zooms; Alt + left drag turns too. **Front**, **Back**, **Left**, **Right**, **Top**, **Frame all**, and on a rig **Frame piece** (F).

The pure code (`tools/voxedit/voxel.h`: grid, picking, tool cells, strokes, growth, chunk meshes; `doc.h`: pieces, mirror across pieces, undo) is tested by `tests/unit/test_voxedit.cpp`. The view meshes each model in 16-cell chunks and makes again only the chunks an edit touched, so a model of 60,000 voxels edits without a pause.

## anim_capture, the Animator's game side

`anim_capture` is a developer mod with full trust and no window, in the SDK as `dev-mods/anim_capture/`. It draws nothing itself: the
game draws, and the mod supplies the clip and the clock through the editor's calls (`animation_preview`, `animation_preview_clear`;
[game-api.md](game-api.md#animation_preview)). It needs dev mode: without it the mod does not load (`mods` shows it as failed with the reason
`kyubu_mod_init(client) returned 1`, and the start menu says that a mod did not load).

| Command | Meaning |
|---|---|
| `anim preview <clip> <t_ms>` | The local player is drawn in the pose of the clip `<clip>` (`<mod>:<name>`, as `anim list` shows) at `<t_ms>` milliseconds (0 or more), whatever its real mode. No ability starts. A second call moves the clock. A clip with tracks and no `[borrow]` needs a duration in milliseconds, as [animations.md](animations.md) says. |
| `anim preview off` | The game draws the player again. |
| `anim capture <clip> [times_ms] [views]` | The local player is posed at each time (milliseconds, a comma list, default `0,500,1000`, at most 12) in each view (`front`, `left`, a comma list, default both), and the game's own picture and numbers are written to `kyubu/capture/<clip>-<stamp>/` ([below](#capture-files)). It runs in the background: the reply says it started, and the log line `anim capture: done: <folder>` says where the files are. The preview and the player's yaw are given back at the end. |
| `anim rig` | The local player, standing still, is held at the front view for 1.5 s and one frame is written, with `models` and `appearance` (see [Rig and bake files](#rig-and-bake-files)). Takes no argument. |
| `anim bake <pose id> [duration_ms]` | `<pose id>` is 0 to 73 (the [pose catalogue](../mapping/atlas/combat.md#pose-catalogue-which-pose-a-mode-draws)); `<duration_ms>` is 100 to 8000, default 2000. The pose is played from its start on the preview clock, every draw is kept, and the draws are played back as `set` tracks. Stand still while it runs: it takes about twice the duration. |

The reply names the clip and the time, or the reason: an unknown clip (`anim list` shows the loaded ones), a negative or non-numeric
time, or a missing word (the usage line). The reply is logged too.

A saved clip needs no preview: in dev mode the loader publishes it by itself, and the author presses the ability's key. The preview
is for a pose at one time, such as a borrowed clip at its middle.

### Capture files

`anim capture` holds each step (a time, a view) for 1.5 s of real time, because the game eases its pose vectors, and then reads the
back buffer inside `render.overlay`, which runs before the overlay draws: the console and the notices are not in the picture. The
folder `kyubu/capture/<clip>-<stamp>/` (a `:` in the clip id becomes `_`) holds two files.

- `sheet.png`: a header line (`capture <clip>`), then one row per time and one column per view in the order given. Each cell is
  256 pixels square, cut from the middle of the screen (half of its shorter side) and labelled `<time>ms <view>`. The `front` view
  is `body_yaw` 180 (the face to the camera) and `left` is 90 (the left profile). The layout is the one the Animator's offline
  sheet uses (`capture_sheet.h`).
- `pose.json`: `format` (`kyubu-capture/1`), `kind` (`capture`), `clip`, `build` and `frames`, one per step in the order of the
  sheet. A frame has `index`, `t_ms`, `view`, `body_yaw`, `pose_id` and `mode_time_ms` (the draw record's +8 and +4),
  `vectors` and `matrices`.
  - `vectors` has the parts `root`, `body`, `head`, `hand_r`, `hand_l` and `tail`; each has `rot` (three euler angles in degrees)
    and, except the tail, `pos` (the root has x and y; the others x, y and z, in voxels except the root, which is in world units).
    They are what the game used for the draw, read at the join site **after** the clip's tracks were written there: what the
    clip applied, not what it evaluated.
  - `matrices` has the 17 part matrices of the draw record, 16 floats each, in Direct3D's row-vector form (the translation in
    the fourth row), with the slot order of the creatures atlas page ("Posing"): 0 root, 1 trunk hub, 2 body, 3 head, 4 hair,
    7 the holstered weapon, 9 hand A, 10 hand B, 15 and 16 the feet; a slot the player has no model for is zero.
  - A number that is not finite is written as `null`.

The folder is under the game's own `kyubu/` folder, never under the repository. Under `kmap probe replay` the folder is copied to
`capture/` in the replay's output folder. Times are milliseconds, like `anim preview`: the mod has no call for a clip's duration, so
it cannot turn the fractions of `anim sample` into time.

### Rig and bake files

`anim rig` writes `kyubu/capture/rig-<stamp>/` with `sheet.png` (one cell, the front view) and `pose.json` with `kind` `rig` and one frame. It is
a capture with no clip, so the frame has the same `vectors` and `matrices`, and two more keys:

- `models`: for each model slot of the draw record, the index of its `Model*` in the game's model table (`ModelLoader`, `GameController + 0x2F8`).
  That index is the loader slot, the number the private `catalog.csv` of the pose library maps to a model file. The slots are `head`, `hair`,
  `body`, `shoulder2`, `hands`, `wings`, `tail`, `feet`, `weapon_b`, `weapon_a`, `shoulders` and `neck`; an empty slot is left out, and a model
  that is not in the table is `null`. The game keeps no name for a loaded model that the mod can read, so the file name comes from that catalog.
- `appearance`: the creature's appearance ids (`head`, `hair`, `hands`, `feet`, `body`, `tail`, `shoulder2`, `wings`; -1 for none), the loader
  slot each body part asked for. It equals the model slot's index unless equipment replaced the model.

`anim bake <pose id>` writes `kyubu/capture/bake_pose<id>-<stamp>/` with four files.

- `pose.json`, `kind` `bake`: one frame for **every draw** of the local player while the pose played. `t_ms` is the preview time that draw used
  (milliseconds from the start of the pose), `view` is `live` and `body_yaw` is the player's own. `pose_id` is the pose asked for and
  `mode_time_ms` is `t_ms` rounded: the draw record's own two fields read 0 at the join site from the second draw of a borrowed pose on, so a
  bake names what the mod fed. The vectors and matrices are read as in a capture.
- `replay.json`, `kind` `bake-replay`: the same for the second run, in which the baked vectors were played back.
- `bake.anim.toml`: that second run's clip. Its tracks are all `set` tracks, with a key for every draw that is at least 33 ms after the last kept
  one (30 a second) and one for the last draw. It is an ordinary clip: a clip file can be made from it, and the Animator's "Bake to keys" reads `pose.json` for the same purpose ([above](#the-window), "Pose library").
- `sheet.png`: four cells for each run, taken at 0, 1/4, 2/4 and 3/4 of the duration. The top row is the game's pose, the bottom row the baked
  vectors played back; they should look the same.

The pose is played by a clip with a `[borrow]` table, defined for the run (`anim_capture:bake_pose<id>`), on the preview clock that the mod
advances by real time. The pose starts from the game's neutral pose, so the first draws include the game's own ease into it (about 70 ms to
half-way), as an ability that starts does. The baked vectors are what the game used at the join site, **after** the game's own smoothing.

When the replay ends the log has one line: the number of draws of both runs, and the largest difference between the replay's vectors and the
baked ones at the same time, in voxels and in degrees, with the part it was found on, against the limits of the mimic gate (0.25 voxel and 1
degree). A creature with no tail model gets no tail track, and the tail is left out of the comparison (the line says so). The check is as
fine as the frame rate: at 5 draws a second, as under Wine in a replay, the replay's keys are the few draws there are.

Both commands write under the game's own `kyubu/` folder and nothing under the repository. To keep them, copy the folder to the private pose
library (`RECIPES.md` has the line for this PC); `tools/check_public_files.py` keeps the pose library out of the repository.

### The file inbox

In dev mode the mod also polls the folder `kyubu/inbox/` (made when the mod loads) twice a second, so a program outside the game can ask
for a capture: the Animator's **In game** button, or an AI session that has no window to type in. A request is a text file whose name
ends in `.txt` and whose first non-blank line is one console line, for example `anim capture demo:slam 0,500,1000 front`. Write it under
another name (`.tmp`) and rename it, so the mod never reads half a line.

- Only `anim preview`, `anim capture`, `anim rig` and `anim bake` run. Any other line is refused with a reply and does nothing.
- The mod removes the request first and then runs it, so a crash or a long capture cannot run it twice. One request runs at a time: while
  a capture, rig or bake runs, the next file waits (oldest name first).
- The reply is `<name>.reply` beside it: the line (`request: ...`), what the command answered, and, for a job that takes time, a last
  line `result: ...` with the folder it wrote (or why it failed). The log has the same lines.
- A request file larger than 1 KB, or with no line, is consumed and answered with the reason.
- Without dev mode the mod does not load, so the folder is not made and nothing polls it.

### Adding a subcommand to `anim`

A mod adds `anim <word>` by registering the console command `<mod id>:anim.<word>` with `register_command`. The `anim` builtin
hands a line whose first word it does not know to that command, with the rest of the line as its arguments. `anim` with no word lists
the subcommands that mods added. The builtin's own words (`list`, `show`, `sample`, `reload`) are never handed on, and when two
mods register the same word the first command by name wins. The command is also typed in full (`anim_capture:anim.preview ...`).

## Headless mode

`Cube.exe --kyubu-dev --kyubu-headless` runs the game so that a program, not a person, drives it. It exists for native replays
(`kmap probe replay`) on Windows. Without `--kyubu-dev` the flag is refused and logged, and the game runs normally.

What it changes:

- **Window.** The game window is a no-activate tool window with a 1024x768 client, placed right of the virtual screen so no monitor
  shows it. It has no taskbar button and never takes the foreground.
- **Input.** Real keyboard and mouse messages and focus loss are dropped, so typing in another window does nothing to the game.
  `GetFocus` returns the game window, `GetCursorPos` returns a virtual cursor, `SetCursorPos` moves only that cursor, and DirectInput
  `Acquire` and `GetDeviceState` answer `DI_OK` with a synthetic state. Input comes only from the files below.
- **Frame rate.** Nothing is capped except the game's own `minTimeStep` (about 106 fps in the spike). Every 5 s the log has
  `headless: <fps> fps (min <fps>), frame <n>`. `min` comes from `GetTickCount64` (steps of about 15.6 ms), so use it to spot big dips only.

Files go into `kyubu/inbox/`. Write them under another name (`.tmp`) and rename. The loader reads them on every frame, in name order,
and removes each before it applies it. A `.cmd` file (a console line) works as before.

| File | Content |
|---|---|
| `*.input` | One event per line: `down <key>`, `up <key>`, `button down\|up 1\|2\|3`, `move <x> <y>` (client coordinates), `wheel up\|down <ticks>`, `text <utf-8>`. Blank lines and lines starting with `#` are skipped. A bad line is logged as `headless input: line N: reason` and skipped. |
| `*.shot` | The first line names a picture (only `A-Z a-z 0-9 . _ -` kept, 80 characters at most). At the next `Present` the loader writes `kyubu/shots/<name>.png`, the back buffer as an 8-bit RGB PNG. |

Key names are xdotool's, in any case: `w`, `space`, `Return`, `grave`, `1` to `9`, `F1` to `F12`, `shift`, `ctrl`. Events are applied
at the start of the next game frame. An `up` is never applied in the same frame as its `down`: it and the lines after it wait one frame, so
each press is seen by at least one frame. A held key stays in the synthetic state until its `up`.

`kyubu/headless/frame` holds `<present count> <hash>`, rewritten every 4th frame. The hash is FNV-1a over a 64x48 reduction of the
back buffer. A program that waits for a still picture reads the file until the hash stops changing.

What is faked, and what is not: the game sees a focused window, a cursor and a keyboard that only the files move. Rendering,
audio, timing and game logic are real. The Wine replays (`scripts/run_replay.sh`, deprecated) stay the reference for parity. A `SetCursorPos` the game calls (for example to re-centre at world entry) is a warp: the virtual cursor moves and the game reads no mouse motion from it.

## Test fixtures

`tests/fixtures/mods/` holds small mods that exercise one behaviour each (faults, crashes, script errors, trust levels, missing dependencies, patch conflicts, duplicate ids, hot reload, the Lua API parity checklist, input cancellation, a slow handler). They are staged into `build/fixture-mods/` and are useful references for how Kyubu reacts to broken mods. See [building.md](building.md) for how they are run.
