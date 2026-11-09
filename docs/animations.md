# Ability animations

A mod ability (see [abilities.md](abilities.md)) is drawn with the game's idle pose unless it names an animation clip. A clip is a data file: no code, no numbers, nothing in the saves. Any trust level may bind a clip to its own ability.

**What works today.** A clip with a `[borrow]` table makes the ability wear one of the game's own poses, with its own timing, and can hide parts of the body while it plays (phase 1, "the borrowed look"). The `[[track]]` rows move a part of the body: the loader writes them over the game's pose for each draw (phase 2, "the override"; see "Tracks" below). A clip may have both: the borrowed pose is then the base that the tracks move. A track may also be a generator (`osc`, `spring` or `expr`) in place of its keys: a few numbers or a short expression that the loader evaluates at each draw ("Procedural tracks" below).

Clips are drawn on the client only. `Server.exe` draws nothing, and a client without the mod draws the "unknown mode" look (pose 0).

## The file

A clip is `<mod>/assets/animations/<name>.anim.toml`: one clip per file. Its id is `<mod id>:<file stem>`, so `demo/assets/animations/slam.anim.toml` is `demo:slam`. The stem is at most 64 bytes. A clip file is TOML (the same as `mod.toml`).

<!-- sample: tests/fixtures/kanim/design_example.anim.toml -->
```toml
duration = "ability"
end = "hold"
blend_in = 60
blend_out = 120

[[track]]
part = "weapon_r"
channel = "rot"
mode = "add"
keys = [
  { t = 0.00, v = [0, 0, 0] },
  { t = 0.35, v = [-110, 0, 10], ease = "ease_out" },
  { t = 0.50, v = [30, 0, 0],    ease = "ease_in" },
  { t = 1.00, v = [0, 0, 0],     ease = "smooth" },
]

[[hide]]
from = 0.0
to = 0.2
parts = ["head"]

[[marker]]
t = 0.50
name = "hit"
```

### Top-level keys

| Key | Meaning | Range |
|---|---|---|
| `duration` | `"ability"` (the default: the ability's `duration_ms`) or a number of milliseconds. | above 0, at most 600,000 |
| `end` | What happens after the clip's time runs out while the ability goes on: `"hold"` (stay on the last pose) or `"loop"`. | `hold`, `loop` |
| `blend_in`, `blend_out` | Ramps in milliseconds. The clip's effect grows from 0 over `blend_in` and falls to 0 over the last `blend_out` of the run. The game draws its own pose again when the ability ends, so `blend_out` is measured back from the end of the run, not of the clip. | 0 to 60,000 |
| `[borrow]` | Phase 1. See "Borrowing a vanilla pose". | |
| `[[track]]` | Phase 2. See "Tracks". At most 64. | |
| `[[hide]]` | Parts that are not drawn for a time. At most 32. | |
| `[[marker]]` | A named time in the clip. At most 64. | |

Time inside a clip is a fraction `t` from 0 (the start) to 1 (the end of `duration`).

### `[[hide]]`

| Key | Meaning |
|---|---|
| `from`, `to` | The interval, as fractions of the duration, 0 to 1. The part is hidden from `from` up to, but not including, `to`. An interval that ends at 1 stays on while a `hold` clip holds. |
| `parts` | A list of groups: `head`, `hair`, `body`, `shoulders`, `weapons`. |

The groups map one by one to the game's own hide bits. Hiding works in phase 1.

### `[[marker]]`

`t` (0 to 1) and `name` (at most 48 bytes). A marker is a name at a time. It costs nothing in the draw. An ability's `on_step` reads the markers its step crossed with `ctx:marker()` (see [abilities.md](abilities.md#the-callbacks)), so the hit of a swing lands on the frame the author marked:

```lua
kyubu.ability.define("demo:slam", { duration_ms = 700, cooldown_ms = 6000, anim = "demo:slam",
  on_step = function(ctx)
    for _, name in ipairs(ctx:marker()) do
      if name == "hit" then kyubu.info("slam: hit at " .. ctx.elapsed_ms .. " ms") end
    end
  end })
```

- The list holds the markers in `(previous step, this step]` of the ability's mode time, in the order crossed, once each. The first step covers the time from the start, so a marker at `t = 0` is told once. `on_start` gets none; `on_end` gets those between the last step and the end, so a marker at `t = 1` is told once.
- The time is the ability's own mode time on the side that runs it, not the draw's: the server tells the same names whether or not a client draws the clip. A clip with `duration = "ability"` uses the ability's `duration_ms`.
- An ability with no bound clip, or a clip with no marker, gets an empty list. A `loop` clip tells each marker once per lap.

## Borrowing a vanilla pose (phase 1)

```toml
[borrow]
mode = 18        # a pose id from the catalogue
speed = 1.0      # 1 is the game's own speed
offset_ms = 0    # where in the pose the clip starts
```

`mode` is a **pose id**, 0 to 73, not a mode id. The game's modes (0 to 110) share the 74 poses, and a pose is what the body does. The [pose catalogue](../mapping/atlas/combat.md#pose-catalogue-which-pose-a-mode-draws) lists which modes draw which pose: borrow pose 18 to wear what modes 3, 9, 19 and 62 draw. The file validator accepts 0 to 110 and the loader refuses a pose id above 73 when it draws, so keep to the catalogue's pose ids.

| Key | Meaning | Range |
|---|---|---|
| `mode` | The pose id to wear. | 0 to 110 (a pose above 73 is not drawn) |
| `speed` | Multiplies the elapsed time. 0 holds one frame. The pose's time is `offset_ms + elapsed * speed`, and never below 0, so a negative speed runs from `offset_ms` back to the pose's start and stays there. | -10 to 10 |
| `offset_ms` | The pose's own time at the start of the clip. | -600,000 to 600,000 |

How the pose is drawn: when the creature's mode is a bound mod ability, the loader changes the draw's mode and time to the borrowed pose and `offset_ms + elapsed * speed`, adds the clip's `[[hide]]` intervals to the hide mask, draws, and puts the record back. A creature in a vanilla mode costs one compare. A clip with no `[borrow]` is not worn in phase 1.

## Binding a clip to an ability

```lua
kyubu.ability.define("demo:slam", { duration_ms = 700, cooldown_ms = 6000, anim = "demo:slam" })
kyubu.animation.bind("demo:slam", "demo:slam_wide")   -- replaces it
```

- `anim = "<mod>:<clip>"` in `kyubu.ability.define` binds the clip after the ability is defined. A clip that does not exist makes the call return `nil` and the reason; the ability stays defined with no clip.
- `kyubu.animation.bind(ability_id, clip_id)` (native `ability_animation_bind`) binds or rebinds. One clip per ability; a second bind replaces the first.
- A mod may bind only its own abilities: another mod's ability gives `KY_E_NAMESPACE`, a vanilla ability is refused.
- The call needs the primitive `animation_pose`, which is available on `Cube.exe` only. The reference is [game-api.md](game-api.md#ability_animation_bind).

## Tracks (phase 2)

A `[[track]]` moves one part of the body. A clip with tracks and no `[borrow]` is drawn over the game's neutral pose (pose 0).

| Key | Meaning |
|---|---|
| `part` | `root`, `body`, `head`, `hand_r`, `hand_l`, `tail`. `hand_r` is the player's right hand (the game's hand B), `hand_l` the left (hand A). `weapon_r` and `weapon_l` are other names of `hand_r` and `hand_l`: a weapon follows its hand. |
| `channel` | `pos` (x, y, z in voxels, each -100 to 100) or `rot` (x, y, z Euler degrees, each -720 to 720). |
| `mode` | `add` (the default: an offset on the game's pose) or `set` (the clip replaces the vector). |
| `keys` | At most 256 keys of `{ t, v, ease }`, in order of `t`. `v` has three numbers. A track has `keys` or one generator, not both. |
| `osc`, `spring`, `expr` | A generator in place of `keys`: see [Procedural tracks](#procedural-tracks). |

The `ease` of a key shapes the segment that **arrives** at it from the previous key; the first key's ease is not used. The easings are `linear` (the default), `smooth`, `ease_in`, `ease_out` and `hold` (keep the previous value until `t`, then jump). Before the first key a track holds the first value, and after the last key the last value. Two tracks on one part and channel stack, in the order of the file.

The feet, shoulders and wings are not parts: the game moves them from the walk phase and no vector can be written for them. Two channels have no vector either and are not drawn: the `pos` of `tail`, and the z of the `pos` of `root`.

The tracks change one draw only. The loader puts the game's own values back after each draw, so an `add` is the same offset in every frame, `blend_in` and `blend_out` are exact, and the game's pose is back in the frame after the ability ends.

If the loader cannot hook the join site of the draw (the log says `tracks are off` with the reason), tracks are not drawn: one error line names each such clip, and a clip with a `[borrow]` table keeps its borrowed look.

### Procedural tracks

A track has `keys` or one generator, not both. A generator is a few numbers or a short expression written in the file like a key. It has no code, so every trust level may use it, and the loader evaluates it with the same function as a track of keys: `kanim` links that function, so it prints the game's numbers. Under `mode = "add"` its value is an offset on the game's pose, and under `"set"` it is the whole vector, as for a key. A result is held to the channel's range (-100 to 100 voxels, -720 to 720 degrees). To lay a generator over keyed motion, put it in a second track of the same part and channel: tracks stack.

<!-- sample: tests/fixtures/kanim/procedural.anim.toml -->
```toml
duration = 800
end = "hold"
blend_in = 40
blend_out = 100

# The body dips when the spring is let go at 25 % of the clip, overshoots a little and settles.
[[track]]
part = "body"
channel = "pos"
spring = { to = [0, 0, -2], at = 0.25, freq = 3, damping = 0.35 }

# The head shakes and dies down.
[[track]]
part = "head"
channel = "rot"
osc = { amp = [4, 0, 6], freq = 6, decay = 5 }

# The weapon hand lifts with the charge the creature holds, and rolls once over the clip.
[[track]]
part = "weapon_r"
channel = "rot"
expr = ["charge * 60", "0", "sin(u * 360) * 8"]

[[marker]]
t = 0.25
name = "hit"
```

Write a generator as one inline table on one line, as above: the Animator edits a clip in place, and a `[track.osc]` sub-table is not a form it can take out with its track.

| Generator | Keys | What it gives on each axis |
|---|---|---|
| `osc` | Required: `amp` (three numbers) and `freq` (cycles per second, above 0, at most 30). Optional: `centre` (three numbers, default 0), `phase` (degrees of the cycle, -360 to 360, default 0), `decay` (1/s, 0 to 100, default 0). | `centre + amp * sin(360 * freq * s + phase) * e^(-decay * s)`, with `s` the clip's time in seconds. A shake or a hover. With a `decay` it settles on `centre`; with none it goes on. |
| `spring` | Required: `to` (three numbers) and `freq` (the spring's own cycles per second, above 0, at most 30). Optional: `from` (three numbers, default 0), `at` (0 to 1, the fraction of the clip where it is let go, default 0), `damping` (the damping ratio, above 0, at most 4, default 0.3). | `from` until `at`, then a damped spring let go from rest toward `to`: the overshoot and settle after a strike. A damping under 1 overshoots (0.3 by 37 %, 0.5 by 16 %), 1 settles fastest with no overshoot, and above 1 creeps. |
| `expr` | Three strings, one for x, y and z. Write `"0"` for an axis that does not move. | The value of each expression. |

An expression is numbers, the names below, `+ - * /`, a minus sign, parentheses and calls of the functions below. It has no loop, no assignment and no user function, so it cannot recurse. It is bounded to 64 nodes (a number, a name, a minus, an operator and a call are one node each), 32 levels of nesting and 256 bytes, and it is parsed once at load. A division by zero and any result that is not a number is 0. The same text and the same inputs give the same number.

| Name | Is |
|---|---|
| `u` | the clip's time, 0 to 1 |
| `t` | the clip's time in milliseconds, from the start of the clip's lap |
| `charge` | the creature's `charged_mp`. The game grows it while a hold ability is held (a vanilla hold mode: 8, 24, 25, 27, 36, 59, 63, 64) and spends it on release. A mod ability's own mode does not grow it, so it reads 0 there unless a mod writes it. |
| `speed` | the length of the creature's velocity on the ground plane, in the game's own units (about 10 at a walk) |
| `rank` | the local player's rank in the bound mod ability; 1 for another creature, for a vanilla mode and for the Animator's preview |

| Function | Gives |
|---|---|
| `sin(deg)`, `cos(deg)` | the sine or cosine of an angle in **degrees**, as every angle of a clip |
| `abs(x)`, `min(a, b)`, `max(a, b)` | the absolute value, the lower, the higher |
| `clamp(x, lo, hi)` | `x` held from `lo` to `hi` |
| `lerp(a, b, f)` | `a + (b - a) * f`; `f` is not held to 0 to 1 |
| `smoothstep(e0, e1, x)` | 0 below `e0`, 1 above `e1`, a smooth S between |
| `pow(a, b)` | `a` to the power `b` |
| `noise(x)` | smooth value noise from -1 to 1, with one value at each whole number of `x` |

The signals are live, so a clip that reads one can start away from the base pose when the creature already holds a charge or is running: give it a `blend_in`. `kanim describe` and `kanim sample` show a clip with `charge` 0, `speed` 0 and `rank` 1 unless `--charge`, `--speed` and `--rank` give others. `kanim bake` writes each generator as plain keys, one for every 1/60 s of the clip, with the signals frozen at those values; a baked clip has no live signal.

An error in an expression names the string and the column, for example `track[0].expr[2]: column 10: unknown name` followed by the name and the list of names.

### Frame and order

- **Frame.** x is toward the player's right, y forward, z up; right-handed, in degrees. A positive x on `rot` tips the top of a part backward, a positive y rolls its top toward the player's right, and a positive z turns it counter-clockwise seen from above.
- **Mirror, left to right.** The Animator's Mirror L/R is the reflection through the plane that splits the player left from right (x = 0 of the frame above). A `pos` flips its x. A `rot` keeps its x (a turn about the axis that crosses the plane) and flips its y and z (turns about axes that lie in the plane). `hand_r` and `hand_l` are the pair; the other parts have none. Worked out from the frame, not yet checked against the game.
- **A hand's position is not turned by its own angles.** Measured on the part matrices of baked poses and of a clip capture (the Animator's parity check): `pos` 14 on z lifts the right hand by the same amount at `rot` 0, 80 and 160 on x, and the body's turn carries the hand with its position. So a hand's `pos` is an offset in the body's frame, and its `rot` turns the hand about its own centre. The head and the other parts were not measured with a part turned and a position set, so for them the reading from the game's code stands: the rotation is applied outside the translation, so a `pos` is turned by the part's `rot`. (A reading from screenshots, that `pos` 20 on x with `rot` 90 on z moved the hand to the side and `rot` alone did not, fits both.)
- **The three angles compose in a fixed order.** In each part's list the first axis is the outermost rotation, so the last one acts on a point first. Read from the game's code, not yet confirmed on screen: `root` and `body` use z, then y, then x; `head` and `tail` use x, then y, then z. The hands use z, then y, then x: measured, the one of the six orders that matches the game's part matrices exactly over three baked poses.
- **`add` stacks by adding.** An `add` track adds its value to the game's angles, axis by axis, and two `add` tracks on one part and axis sum. The order above decides what a sum does to the part.
- **Not settled.** Whether the head's `pos` is turned by the head's `rot` (measured for the hands only). A hand that holds a weapon in two hands: the game may move it after the vectors ([below](#what-the-animator-does-not-draw)).

## Tools

- `kanim` checks and describes a clip without the game: [dev-tools.md](dev-tools.md#kanim-the-clip-checker). Every error carries a line and a field path, such as `track[0].keys[2].t`.
- The console commands `anim list`, `anim show <id>`, `anim sample <id>` and `anim reload` ([dev-tools.md](dev-tools.md)).
- With dev mode on, the loader looks for changed clip files twice a second at most and publishes the new set at once; a clip that fails to load is rejected with an error, and every other clip stays as it was. A clip is read again on `anim reload`, and a boot reads every enabled mod's clips.
- **The Animator**, `kyubu-animator`, edits a clip in a window of its own, with no game ([dev-tools.md](dev-tools.md#kyubu-animator-the-clip-editor)): a figure posed from the clip at the scrub time, handles to turn and move a part, a timeline, a key inspector and curve view, mirroring left to right, undo, and a list of checks. It writes the clip file and nothing else, so the game draws a save through the hot reload above, at the next press of the ability's key. `kyubu-animator --sheet <clip> --out <png>` writes a labelled sheet of the mimic (the front and left views, a row for each time) with no window and no game ([dev-tools.md](dev-tools.md#the-offline-sheet)).
- The calls for a dev mod in the game (full trust and dev mode): `animation_define_from_text`, `animation_preview` and `animation_preview_clear` ([game-api.md](game-api.md#animation_define_from_text)). `animation_preview` draws a clip on the local player without an ability, so a borrowed clip can be scrubbed. The Animator does not call them; the dev mod `anim_capture` does, for the console commands `anim preview <clip> <t_ms>` and `anim preview off` ([dev-tools.md](dev-tools.md#anim_capture-the-animators-game-side)).

### What the Animator does not draw

The figure is a mimic, not the game: it moves as the rig's steps say, and it is only as close as the rig. The stand-in rig has made-up sizes, so it shows timing and direction, not the real look of the character. It does not draw:

- breathing and the walk cycle: the figure stands still;
- the game's ease from the previous pose into the neutral pose when an ability starts (about 70 ms to half-way);
- the weapon trail, the game's own particles and the cast bar (a rig's emitters draw particles moved the game's way, for a mod's
  effect: [dev-tools.md](dev-tools.md#particles));
- a `[[hide]]` span: the timeline shows it and the figure keeps drawing the part, so the sheet does too;
- another weapon family, or other equipment, than the one the rig's models show;
- **the hands, when a weapon is held in two hands.** Measured against the game's part matrices (the parity check, over the baked poses 3, 18 and 61 and a clip capture; tolerance 0.25 voxel and 1 degree): the game's rig of the body, head, hair and the club is exact, and a hand's angles are exact, but in poses 3 and 18 a hand sits up to 14 voxels from where its vectors put it, while pose 61 and the clip capture match. The mapping has the game giving hand A a grip on hand B's weapon in four pose cases, which fits and is not tested. The hands are marked approximate in the game's rig, so the view tints them. A clip that borrows a grip pose, or sets a hand in one, is drawn with the hands in the wrong place by the Animator and right by the game: check it in the game (`anim capture`);
- **the feet and the shoulders**: no vector drives them, so they stay at the rig's neutral pose. The game turns a foot in some poses (the left foot by 60 degrees in pose 18).

Two more limits of the file, not of the picture. The stand-in rig's numbers are made up, but its order of angles and its hands follow what was measured (above). A `[borrow]` pose is drawn only from a baked pose in the Animator's pose library ([dev-tools.md](dev-tools.md#kyubu-animator-the-clip-editor), "Pose library"): a clip that borrows a pose the library lacks is drawn over the neutral pose, and the view names the pose. A baked pose has the part vectors only, so the feet and the shoulders stay at the rig's neutral pose. The list above of the parts that differ from the game is the parity check's, for the game's own rig, which is private; `kyubu-animator --parity` ([dev-tools.md](dev-tools.md#the-parity-check)) measures any rig against captures.

## Authoring with an AI

An AI session can write a clip with no window and no person at the keyboard. It needs four things: the clip file as text, a checker that answers in words and numbers, a picture of the pose it can read, and the numbers the game itself used. All four are files and console commands that already exist. There is no other protocol and no test driver to write.

### The loop

1. **Read what to write.** Read [Tracks](#tracks-phase-2) and [Frame and order](#frame-and-order) above, and the [rig reference](#the-rig-reference-in-words) below. For a pose close to the brief, pick a vanilla pose from the [pose catalogue](../mapping/atlas/combat.md#pose-catalogue-which-pose-a-mode-draws) and borrow it (phase 1) instead of building one from tracks.
2. **Write `<name>.anim.toml`** in a mod's `assets/animations/` folder (or any folder, for the offline tools). Start from a small clip: one track, three keys.
3. **Check it.** `kanim check <clip> --json` prints each error with its line, its field path (`track[0].keys[2].t`) and a hint. Fix the file and run it again until it prints `ok`.
4. **Read it in numbers.** `kanim describe <clip> --ability-ms N` gives the range of each track (a generator is read from samples), its fastest motion (speed, time, size), pops at the start and the end, the clip's duration against the ability's, and the markers. Compare these with the brief ("the hit at 50 %", "the fastest swing between 35 and 50 %", "no pop at the end"). `kanim sample <clip> --t 0,0.5,1` gives the pose of each part at those fractions. See [dev-tools.md](dev-tools.md#kanim-the-clip-checker).
5. **Read it as a picture.** `kyubu-animator --sheet <clip> --out <png> --t 0,0.25,0.5,0.75,1` writes a labelled sheet of the mimic, a row for each time and the front and left views side by side, with no window and no game ([dev-tools.md](dev-tools.md#the-offline-sheet)). Adjust the clip and repeat steps 3 to 5. These steps need no game.
6. **Confirm in the game** (needs the game, in dev mode). The command `anim capture <clip> [times_ms] [views]` poses the local player and writes `sheet.png` and `pose.json` to the game's `kyubu/capture/<clip>-<stamp>/` ([dev-tools.md](dev-tools.md#capture-files)). The sheet is the real character with no console in the picture; `pose.json` holds the vectors the game applied and the 17 part matrices it left. Read the sheet first, then compare the vectors with what `kanim sample` said. Use this step for what the Animator does not draw ([above](#what-the-animator-does-not-draw)): a grip pose, the feet, a `[[hide]]` span. With the game open in dev mode, a program that cannot type in the console asks by a file: write `<game>/kyubu/inbox/<name>.tmp` holding the one line (`anim capture demo:slam 0,500,1000`), rename it to `<name>.txt`, and read `<name>.reply` a few seconds later; its last line, `result: ...`, names the capture folder ([dev-tools.md](dev-tools.md#the-file-inbox)). Only the `anim` words of the capture mod run from the inbox.
7. **Show the person.** The same file opens in `kyubu-animator`, which reloads an outside edit by itself, and in the game through the hot reload. A note such as "less lean" becomes an edit in step 2.

### Which unit goes where

| Tool | Time is | Why |
|---|---|---|
| `kanim sample --t`, `kyubu-animator --sheet --t` | a fraction of the clip, 0 to 1 | the clip's own time |
| `anim capture`, `anim preview` | milliseconds | the game's clock has no call for a clip's duration |

To capture the frames that a sheet showed at 0, 0.25 and 0.5 of a clip of 700 ms, pass `0,175,350`. A clip with `duration = "ability"` needs `--ability-ms` for the offline tools and a literal duration for a preview with tracks and no `[borrow]` ([Limits](#limits)).

### The rig reference in words

What each part does when its angle is positive, as drawn in the game (30 degrees on each axis, read from sheets; a larger angle was not tested). The numbers of the rest pose are not in this repository.

| Part | Moves | Positive x | Positive y | Positive z |
|---|---|---|---|---|
| `root` | the whole figure about a pivot near its middle; the feet swing the other way | leans back | rolls toward the player's right | turns counter-clockwise from above |
| `body` | the trunk, the weapon on the back and both hands; the head stays upright over the neck and the feet stay planted | leans back | rolls | turns |
| `head` | the head and its hair only; the cleanest part of the rig | looks up | tilts | turns |
| `hand_r`, `hand_l` | the fist, and a weapon held in it; the holstered weapon does not follow | turns the fist | | |
| `tail` | the tail, on creatures that have one (the player has none: no change) | | | |

The creature decides which parts exist: the human male warrior has a head, hair, a body, hands, feet and a club, and no tail, wings or neck. A track on a part the creature lacks does nothing. The full reference, with the probe that checked it, is the "Rig reference" part of [the creatures atlas page](../mapping/atlas/creatures.md#posing).

### The pose library

`anim bake <pose id>` plays one vanilla pose and writes every draw's vectors. Those files hold numbers derived from the game, so they live in a **private folder outside the repository** (the Animator's "Pose library" panel and `--library` read it; `tools/check_public_files.py` keeps it out of the repository). The library has two uses for a session:

- the Animator and `--sheet` draw a `[borrow]` clip over its baked pose, so the mimic shows what a borrowed pose looks like (a pose that is not baked is drawn over the neutral pose, and the sheet's header says `no pose <id>`);
- **Capture at t** and **Bake to keys** in the Animator turn a baked pose into `set` keys, a start for a clip that should look like a vanilla pose and then differ.

A session that has no library still has phase 1: borrow a pose by its id and read the game's own capture of it.

### What a session should not trust

- The Animator's figure is a mimic. The hands in a two-handed grip pose, the feet and the shoulders are not exact ([above](#what-the-animator-does-not-draw)). The sheet of the mimic and the game's capture can differ there, and the capture is right.
- A fresh clip is a guess until it has been looked at. Do not state a pose "looks right" from `kanim describe` alone: it checks the numbers, not the look.
- A generator is read from its numbers. `kanim describe` says how far it overshoots and how fast it moves, but only a picture shows whether a spring looks like a strike: read the sheet of a clip with generators as you would one with keys. An `expr` that reads `charge`, `speed` or `rank` is shown by the checker at their defaults.
- A clip that is valid and has the right look can still be too long or too short for its ability: `kanim describe --ability-ms` shows the duration against the ability's.

## Limits

- **Signals are read from the creature that wears the clip.** The local player's rank in the ability is the ranks file's; any other creature has a rank of 1. `charge` is the game's own `charged_mp`, so it moves for a vanilla hold mode (on the preview clock, the local player holds a vanilla ability) and not for a mod ability's mode.
- **A generator has no state.** `osc` and `spring` are functions of the clip's time, so a scrub and a seek agree. A spring cannot be thrown by an event in the middle of a clip: place it with `at`.
- **Phase 1 draws only a game pose.** A borrowed pose lasts as long as the game's mode: after the mode's own duration the game draws pose 0, so a borrowed pose is never longer than its mode. The clip's `duration` does not stretch it.
- **63 of the 74 poses are reached** from a mode on the local player. Not reached: 4, 7, 12, 13, 14, 36, 52, 56, 58, 59 and 64. Forcing mode 65 or 83 stopped the game in a test, so do not borrow poses that only those two modes draw.
- **The preview needs a duration in milliseconds for tracks.** A clip with tracks, no `[borrow]` and `duration = "ability"` has no clip time on the preview clock, which has no ability: the game's own pose is drawn and one warning says so.
- **The tested angles are 30 and 90 degrees.** 30 degrees on each axis was drawn on the player's parts with no fault; 90 degrees was drawn on the right hand. Larger angles, and a weapon held in a hand, are not tested.
- **The ends cannot be drawn exactly.** The draw record reads about 140 to 155 ms more than the pinned time, so 0 % of a borrowed pose is not drawable. At 100 % the ability has ended and the game draws pose 0.
- **The time is in milliseconds.** The game gives the draw no frame fraction, so the clip steps at the game's own rate.
- **Blends.** Whether `blend_in` stacks on the game's own ease of about 70 ms is not tested.
- **The weapon trail and the cast bar.** The trail (modes 5, 17, 20 for one class, 48, 93) and the cast bar read the creature's mode, not the clip. A mod mode shows an empty name on the cast bar.
- **Weapons.** Pose 2 hides the club by itself. The club is drawn in front of the body for a mod ability and behind the head for vanilla ability 57 with the same pose and time; the cause is not known.
- **Multiplayer.** Nothing is sent. Each client draws the clip from the mode it sees, so a client without the mod draws pose 0.
- **Limits of a file:** 4 MB of text. A file with more than 32 errors lists the first 32.
