# Abilities and the fourth slot

Cube World's skill bar has five slots in use: the two mouse attacks, keys 1 to 3 and Q. A sixth, labelled "4" (key 4), has a widget and an input byte in the game but no ability. Kyubu 1.1 fills that slot. A mod defines an ability, claims slot 4 for a class, and Kyubu puts it on the bar, shows its icon, runs its callbacks, and keeps its rank.

The ability API needs **full trust**. A `restricted` mod has no `kyubu.ability` in Lua, and its native `ability_define` call returns `KY_E_TRUST`. An ability acts on the game, and `restricted` has no game API.

Single-player and a joined client work today on `Cube.exe`. `Server.exe` is not mapped for the slot yet (see "Limits").

## The slot

- Slot 4 appears when the local player's class and specialization have an owner (see "Claims") and the character's rank in that ability is at least 1.
- Otherwise it stays empty, as in vanilla.
- The other five slots are never changed.
- The slot follows the player's class, specialization and rank within one frame.
- If the skill bar does not hold vanilla's five entries when Kyubu fills the slot (another mod changed it), Kyubu leaves the bar alone and logs it once.

The game's own code does the rest: the cooldown sweep, the grey-out, the mp check and the key press. Kyubu answers the game's three look-ups (cooldown, cost, mode duration) for a mod ability and calls the original for every other id, so vanilla abilities are unchanged.

## The registry `kyubu:abilities`

| Numbers | Ids |
|---|---|
| 0 | none |
| 1 to 110 | the vanilla abilities, `cubeworld:<name>` (for example `cubeworld:smash` = 54) |
| 111 | `kyubu:placeholder` |
| 112 to 255 | mod abilities, in the order the mods register them |

The game stores the current ability in one byte, so at most 144 mod abilities fit. The numbers are assigned again at every start, so a mod never saves a number: it saves the string id. The registry is described in [registries.md](registries.md).

Console: `ability list` prints every ability with its number, its owner and its claim.

## Defining an ability

```lua
kyubu.ability.define("mymod:burst", {
  icon        = "cubeworld:smash",   -- the icon of a vanilla ability (the fallback)
  icon_file   = "burst.png",         -- optional: your own icon, a 14 x 14 PNG in the mod's assets/icons/
  duration_ms = 800,
  mp_cost     = 0.1,
  max_rank    = 3,
  cooldown_ms = function(rank) return 6000 - 500 * rank end,
  claim       = { slot = 4, class = "warrior", specialization = "any" },
  on_start = function(ctx) end,
  on_step  = function(ctx) end,
  on_end   = function(ctx) end,
  watch    = { on_start = function(ctx) end, on_step = function(ctx) end, on_end = function(ctx) end },
})
```

Call it in the `lifecycle.register` event, on either side. An `anim = "<mod>:<clip>"` key binds an animation clip to the ability after it is defined ([`ability_animation_bind`](game-api.md#ability_animation_bind)); a clip that does not exist makes the call return `nil` and the reason, and the ability stays defined without a clip. [animations.md](animations.md) has the clip format. The native call is `ability_define(self, id, const KyubuAbilityDef*)` ([native-api.md](native-api.md)).

| Field | Meaning | Range |
|---|---|---|
| `name`, `description` | `kyubu:text` ids. A missing id only logs a warning. | |
| `icon` | The id of a vanilla ability whose icon slot 4 draws. It is also the fallback when `icon_file` cannot be shown. | a vanilla ability with an icon |
| `icon_file` | The name of a PNG in the mod's `assets/icons/` folder ([assets.md](assets.md)): slot 4 and the skill-tree button draw it instead of `icon`. | 5 to 64 characters, letters, digits, `.`, `-`, `_`, ending in `.png` |
| `duration_ms` | How long the ability lasts. | 1 to 60,000 |
| `mp_cost` | A fraction of the character's mp, taken once at the start. For a warrior, mp is rage. | 0 to 1 |
| `max_rank` | The highest rank: the loader keeps one cooldown per rank up to it. A tree rank above it reads as `max_rank`. | 1 to 255 |
| `cooldown_ms` | A number, or a function of the rank. Lua calls the function once, for the ranks 0 to `max_rank`, at the definition. | each 0 to 3,600,000 |
| `claim` | See "Claims". Without it the ability is defined and callable by the API, but it is on no bar. | |
| `on_start`, `on_step`, `on_end` | The callbacks. Any may be missing. | |
| `watch` | A table of the same three callbacks for a joined client's view of another creature's run (see "Watching another player"). Missing: the ability is not watched. | |
| `restarts` | How many times one run writes its own mode time back to about 0 (a charge, then a release clip played from its start). A server and a bystander then keep the run going; see "Restarting the mode time". Default 0. | 0 to 15 |

A field outside its range makes `define` return `nil` and the reason, and the log names the field. The full rules and result codes are in [registries.md](registries.md).

## Claims

`claim = { slot = 4, class = <class>, specialization = <spec> }`:

- `class`: `"warrior"`, `"ranger"`, `"mage"`, `"rogue"` (or `cube::ClassType` 1 to 4).
- `specialization`: `"any"`, `0`, `1`, or the name of that specialization for the class (for example `"guardian"`).

Kyubu resolves the owner of each of the four classes by two specializations when the registries freeze:

1. the claim for that exact specialization;
2. else the class's `any` claim;
3. else `kyubu:placeholder`, if `[abilities] placeholder` is on in `config.toml` ([install.md](install.md));
4. else nobody.

A second claim of the same pair, or a second `any` for a class, fails with `KY_E_CONFLICT` and adds nothing. A claim is not a block for another mod's claim on a different specialization of the same class.

Console: `ability slots` prints the owner of each pair. Lua: `kyubu.ability.slot_owner(class, specialization)`.

## The placeholder

`kyubu:placeholder` holds every pair that no mod claims. It lasts 500 ms, costs no mp and has a 2 s cooldown. When used it shows "`<Class>` ability 4: not made yet" on Kyubu's overlay for 3 seconds and writes the same text to the log. Its icon is smash's. `[abilities] placeholder = false` removes it: slot 4 is then empty unless a mod claims it.

## The callbacks

The callbacks run on the game thread, inside the World step, under the World's lock, and only on the side whose World runs the creature's ability:

- single-player: the one World;
- a joined client: its own player only;
- a server: never a joined player.

The `watch` callbacks run elsewhere; see "Watching another player".

| Callback | When |
|---|---|
| `on_start(ctx)` | Once, at the ability's first World step. The mp cost is taken just before it. |
| `on_step(ctx)` | At every World step while the ability lasts (about every 20 ms), the first included. |
| `on_end(ctx)` | Once, when the ability ends, with `ctx.reason`. |

`ctx` fields:

| Field | Meaning |
|---|---|
| `creature` | The creature that runs it, as a [game API creature](game-api.md#creature-handles). |
| `id`, `number` | The ability's string id and its number. |
| `elapsed_ms` | The creature's mode time: about 0 on the first step. |
| `dt_ms` | `elapsed_ms` less the last step's. On the first step, `elapsed_ms`. |
| `steps` | The World steps of this activation, 1 on the first. |
| `rank` | The character's rank in the ability. 1 for a creature that is not the local player; in a `watch` callback, the creature's own rank as the server sent it (`skills[9]`), 1 to `max_rank`. |
| `side` | `"client"`, `"server"`, or `"watch"` in a `watch` callback. |
| `restarts` | How many restarts of this run were seen (see "Restarting the mode time"); 0 before the first. Native: `KyubuAbilityCtx.restarts`, when `size` covers it. |
| `reason` | In `on_end` only: why it ended. |
| `state` | A table that lasts from `on_start` to `on_end` of one activation. |
| `cancel` | `ctx:cancel()` ends the ability at the next safe point with `cancelled`. (`end` is a Lua keyword, so the method is `cancel`.) |
| `marker` | `ctx:marker()` returns an array of the names of the [clip markers](animations.md#marker) the ability's bound clip crossed since the previous step, in the order crossed. Empty when there is no clip or no marker in the span, and in `on_start`. `on_end` gets those between the last step and the end. Native: `KyubuAbilityCtx.markers` and `marker_count`, when `size` covers them. |

The native context is `KyubuAbilityCtx` in `kyubu_api.h`; a native callback is `void (*)(void* user, const KyubuAbilityCtx*)`.

**End reasons** (`KY_ABILITY_END_*`):

| Reason | When |
|---|---|
| `done` | The duration passed. If the game does not end the mode by itself, Kyubu does. |
| `cancelled` | `ctx:cancel()` or `ability_end`, or a callback failed (a Lua error or a native fault, reported like any mod fault). |
| `stunned` | The creature was stunned (only where the primitive `ability_stun` is available; otherwise `interrupted`). |
| `died` | The creature's hp reached 0. |
| `interrupted` | The game replaced the ability with another activity, such as a dodge roll or another ability. |
| `gone` | The creature was removed, or the world was left. |

After the end the creature is in no ability, unless the game had already started another.

## Ranks

A character has a rank in each mod ability: 1 by default, 0 up to `max_rank`. The rank picks the cooldown (`cooldown_ms[rank]`) and shows in `ctx.rank`. Rank 0 empties slot 4.

The ranks are kept in `Save\characters.kyubu-abilities.json`, next to the game's character file, by string id. They survive a restart and a renumbering, and a rank of a mod that is not installed stays in the file until the mod returns. A character is found by its save slot and applies only when its name matches, so another character in the same slot reads rank 1. A file that cannot be read is logged and treated as empty. The file is written atomically (a temporary file, then a rename).

Read and write them with `ability_rank_get` and `ability_rank_set` ([game-api.md](game-api.md#ability_rank_get)), or from the console: `ability rank <id> [rank]`. Both calls are client-side.

### Learning in the skill tree

By default (`[abilities] skill_tree = true` in `kyubu\config.toml`) the skill tree teaches the fourth ability of a class whose slot 4 a mod holds. The panel shows its tenth button, the one vanilla hides, at the end of the ability row, with the ability's icon (its `icon_file`, else the vanilla ability named by `icon`). The button follows the specialization the panel shows, as the game's own ability buttons do: a click on the other specialization's button shows that specialization's fourth ability and icon before LEARN, and a change of specialization or of character changes the icon too. The player spends points on it like on any skill: the game's prerequisite (the third ability at rank 5), its point cost, LEARN and CANCEL apply. The game's click handler has no ceiling for this button, so Kyubu holds the panel's working rank of button 9 at `max_rank` every frame (a point over it is never spent). The rank is the character's `skills[9]`, clamped to `max_rank`: a new character has rank 0 and slot 4 stays empty until the first point is learned. `ability_rank_get`, `ability_rank_set` and `ability rank` read and write `skills[9]` for that ability, and the game saves it; the ranks file is used for every other ability.

The placeholder is not learned (the button stays hidden and it stays at rank 1). `skills[9]` is one number per character, so it is the rank of the current holder. If no mod holds the slot, the value and its points stay and nothing reads them (a respec gives the points back). The hover tooltip of the button shows the level lines but no name or description yet. It needs the primitive `ability_skilltree` (`Cube.exe` only); without it the panel is vanilla's and the ranks come from the file.

## Using the API

`ability_get` reads the ability a creature is in, `ability_cooldown` the cooldown it has left for one, `ability_start` starts a mod ability from code (with the checks of a key press), and `ability_end` ends it. Each has an entry in [game-api.md](game-api.md): the side, the errors and an example. A vanilla ability cannot be started or ended by the API.

## An example

<!-- sample: mods-examples/fourth_ability_demo/scripts/client.lua -->
```lua
-- fourth_ability_demo: one ability on the warrior's slot 4 (docs/abilities.md). Abilities need full trust: a restricted mod has no
-- kyubu.ability. The callbacks only log; a real ability would use ctx.creature (a game API creature) to act.
local ID = "fourth_ability_demo:burst"

kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("fourth_ability_demo: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define(ID, {
    icon = "cubeworld:smash",      -- the icon of a vanilla ability: slot 4 draws it
    duration_ms = 800,
    mp_cost = 0,                   -- testing: no mp. The design value is 0.1, a tenth of the mp bar, taken once at the start
    max_rank = 3,
    cooldown_ms = function(rank) return 6000 - 500 * rank end,   -- 6000 ms at rank 0 down to 4500 ms at rank 3
    claim = { slot = 4, class = "warrior", specialization = "any" },
    anim = "fourth_ability_demo:burst",   -- assets/animations/burst.anim.toml: the weapon arm swings over the 800 ms

    on_start = function(ctx)
      ctx.state.steps = 0
      kyubu.info(("fourth_ability_demo: on_start %s number %d rank %d side %s"):format(ctx.id, ctx.number, ctx.rank, ctx.side))
    end,
    on_step = function(ctx)       -- once for each World step, about every 20 ms
      ctx.state.steps = ctx.state.steps + 1
      kyubu.info(("fourth_ability_demo: on_step %d elapsed %d dt %d"):format(ctx.steps, ctx.elapsed_ms, ctx.dt_ms))
    end,
    on_end = function(ctx)        -- reason: done, cancelled, stunned, died, interrupted, gone
      kyubu.info(("fourth_ability_demo: on_end %s after %d steps, %d ms"):format(ctx.reason, ctx.state.steps, ctx.elapsed_ms))
    end,
  })
  kyubu.info(("fourth_ability_demo: define: %s"):format(ok and "ok" or tostring(err)))
end)
```

`mods-examples/fourth_ability_demo` is this mod: it needs `trust = "full"` and `kyubu = "^1.1"` in its `mod.toml`. It takes the warrior's slot 4 from the placeholder. In a game with this mod, a warrior presses 4 and the log shows `on_start`, each `on_step` and `on_end ... done`.

## Limits

- **Full trust only.** A `restricted` mod has no ability API.
- **Mapped for both builds, in part.** `Server.exe` has the slot's state, the per-creature dispatch (`ability_step`) and the cost, cooldown and duration functions (`ability_stats`). It lacks the bar, the icons, the ranks file, the skill tree, the icon file and the game's `modeInterruptible` and `canUseAbility` (`ability_use`): those have no twin there (the client-only parts of the slot). The game API's table in [game-api.md](game-api.md) says which entries each build has. On `Server.exe`, `ability_start` and `ability_end` work for a creature the server side owns, but `ability_start` skips the two game checks and refuses only a creature that is already in a mode.
- **Multiplayer.** A joined client runs its own player's ability and the server receives the ability's number in the player's creature data (checked with one client: [ability-joined](../mapping/probes/ability-joined.toml)). `Server.exe` runs the mod's server side for that player: `on_start`, each `on_step` and `on_end` run on the server's copy of the player (a mod with `sides` that include `server`; [fourth_ability_demo](../mods-examples/fourth_ability_demo) does this, probe `server-ability-slot-0720`). The server uses the rank the client sent in the player's creature data (bounded by the ability's maximum). A server skips a mod that has only a client side, and it does not compare registries itself: a mod that changes the game declares `gate = true`, so the join gate refuses a client whose gate mods differ ([mods.md](mods.md), "Join gate").
- **The server refuses what the player cannot afford.** On the first step of a mod mode, `Server.exe` checks the remote player: rank 0, then cooldown (the ability's cooldown at that rank, counted from the last accepted start), then mp (the mp the client sent, or the mp before the start, whichever is higher; the cost is taken once). The server's copy of a remote player follows that player's updates the way a watcher does: a step back of up to 100 ms is the same run (not a new start to admit), and the run ends `done` when the mode went to 0 within 250 ms of the duration. On a refusal the log has one line, `Server.exe refuses <id> (number N) on creature ...: <reason>`, the server's copy goes back to mode 0, and no callback runs. The client is **not** corrected: it does not apply server updates to its own player, so it still shows the ability, while the server (and the other clients) see none ([record](../mapping/probes/server-ability-refusal-0720.toml): the client started twice, the server once). The server does not track mp gained from hits, arrows, blocks or stealth, so a client that over-reports its mp is believed. Another client sees the mode only as the game draws a creature in that mode; [Checking a second client](#checking-a-second-client) says how to look.
- **The button's hover tooltip** has the level lines but no name or description. `[abilities] skill_tree = false` turns the learning off (see "Learning in the skill tree"): the tree then does not show a mod ability, every character has rank 1 from the start, and a mod sets the rank itself (`ability_rank_set`, or `ability rank` in the console).
- **144 mod abilities** at most. Entries over the cap are left out with an error.
- **Numbers change** between starts. Use the string id.
- **A deleted character's ranks.** A new character in the same slot with the same name inherits the ranks, because the game's character deletion is not mapped.
- **Icons.** A mod's own icon needs `icon_file` and the primitive `ability_iconfile` (`Cube.exe` only). The game's icons are 14 x 14 PNGs with an alpha channel (RGBA, 8 bits): make yours the same size, and, to look like them, fill the whole square (the game draws the icon in a frame with a coloured background). Kyubu gives the icon the same drawing flags as the game's own, so it is drawn with hard pixel edges. If the file is missing, fails the PNG check or cannot be loaded, the log says so once and slot 4 draws `icon`. The game keeps each icon for the whole run, so changing the PNG needs a restart. The icon has no tooltip name or description yet.
- **Names that are not UTF-8** in a character's name are not handled in the ranks file.

## Effects on the world

An ability that hits, moves, roots or throws something does it on the side that owns the World, with these calls
(all on both exes; [game-api.md](game-api.md) has each one):

- **When:** in a World step. Subscribe to [`sim.world_step`](events.md) and keep a queue: a callback or a channel message adds to
  it, the next step does the work. `creature_hit` and `projectile_push` refuse anywhere else (`KY_E_PHASE`).
- **A hit:** [`creature_hit`](game-api.md#creature_hit) (Lua `kyubu.game.hit`) goes through the game's `hitCreature` with the
  stun and the push direction you give, so XP, threat, armor and the damage number work as for any hit.
- **A move:** [`creature_state_set`](game-api.md#creature_state_set) writes velocity, knockback, mode and mode time, stun, slow,
  charge or aim; [`creature_state_get`](game-api.md#creature_state_get) reads them, with the creature's box (its feet).
- **A hold:** the `rooted` flag of [`creature_set_flags`](game-api.md#creature_set_flags): no walk, no knockback, no AI step
  until it is cleared.
- **The ground:** [`world_block`](game-api.md#world_block) gives the block at a point, the same on both exes, for landings and
  for checks that a spot is clear.
- **Arrows:** on `Server.exe`, [`shot_take`](game-api.md#shot_take) gives the projectile packets the joined players sent, and
  [`projectile_push`](game-api.md#projectile_push) puts a projectile in the World, which the game's own step flies.

**Who owns what.** A side may write only a creature it owns (else `KY_E_SIDE`). On `Cube.exe` the client side owns the local
player alone; the monsters of a single-player World belong to the server side. So the client's `on_step` may hop its own
player, but a hit, push or root of a monster is the server side's work, in single-player too: the client sends a channel
message (it arrives over the loopback, from connection `KY_CONN_LOCAL_CLIENT`) and the mod's server side does it at the next
World step. On a joined game the same message reaches `Server.exe` from the player's connection (`creature id + 1`). The server
checks what the client says (rank, run, range, charge) against its own copy of the player before it acts. Send only what the
player aimed at (a spot, a direction, a charge); let the server compute the rest. SkillsUnleashed does this for all its
abilities (the `skills_unleashed` repository, "Combat").

**Telling the other clients.** What the server owns in the world (a trap, a snare) it tells every joined client with
`channel_send(..., KY_CONN_ALL_CLIENTS, ...)`; the caster and the bystanders draw from the same messages. A client takes them
only from `KY_CONN_SERVER`.

## Watching another player

On a `Cube.exe` joined to a server, the game runs no mode of another player (or of a monster): the server does, and sends the mode and its time to every client. So that a client still shows another player's mod ability, an ability may define `watch = { on_start, on_step, on_end }`. When any creature but the local player has the ability's number as its mode, its `watch` callbacks run on that client, with `ctx.side` `"watch"` (native: `KyubuAbilityDef.watch_start`, `watch_step`, `watch_end`, `watch_user`, and `KyubuAbilityCtx.watch` 1).

- **Presentation only.** Draw, light and play sounds. Do not hit, push, write threat, take mp or set a mode: the server does all of that, and a write to a creature this client does not own is overwritten by the next update.
- **The time is the server's.** `elapsed_ms` is the mode time the server sent, stepped on by this client between updates. An update may set it back a little; a step back of up to 100 ms is held (that step has `dt_ms` 0 and the same `elapsed_ms`), a larger one is a new run (the old one ends `interrupted`). The clip's markers follow that time, so `ctx:marker()` works as in `on_step`.
- **It ends when the mode leaves the number**, never at `duration_ms` on its own: `done` when the mode went to 0 within 250 ms of the duration (the last update trails the owner's end), else `interrupted`, `stunned`, `died`, `gone`, or `cancelled` when a `watch` callback failed (the steps stop, the end waits for the mode). `ctx:cancel()` and `ability_end` do nothing to a watched run.
- **Not run** in single-player, on `Server.exe`, or for the local player. No mp is taken and no cooldown starts.
- `ctx.state` of a watched run is its own, apart from the creature's runs on any other side.

What a watched ability can rebuild, and what it cannot (the game's stealth, a mod's own state), is in the SkillsUnleashed multiplayer design (`openspec/changes/add-skills-unleashed-multiplayer/design.md`, "What another client sees").

## Restarting the mode time

A clip plays at the ability's mode time, so an ability that changes clips halfway (a charge held, then a release) writes the mode time 0 to play the second clip from its start, and binds that clip for the one creature with [`kyubu.animation.bind_for`](game-api.md#ability_animation_bind_for). On the creature's own game (single-player, or the local player) the loader reads the drop as a new run: an `interrupted` end, then a start, which the mod expects.

Elsewhere the run is another game's report: `Server.exe` reads a remote player's mode as its client sent it, and a bystander client as the server sent it. There a drop would be a new start, which the server refuses for the cooldown, taking the mode back for everyone. So an ability that restarts declares `restarts = n`: on a reported run, a step back of more than 100 ms to a mode time of 250 ms or less is then a restart, up to `n` times in one run. The run goes on: no end, no start, no admission, no mp, no cooldown. That step has `dt_ms` 0, `ctx.restarts` counts it, the log has `<id> restarts (1 of n)`, and the markers are told again from the clip's start (the creature's own binding first). A further drop, or one to a later time, is a new start as before.

The server's admission starts the full cooldown at the run's start. An ability that gives its cooldown back on the client
(a charge released too early, so nothing was cast) gives it back in its server module too, with
[`kyubu.game.ability_cooldown_set`](game-api.md#ability_cooldown_set)`(ctx.creature, ctx.number, 0)` in `on_end`; else the
server refuses the player's next start for the cooldown.

## Checking a second client

No replay covers a second client: the replay harness joins one. Check it by hand:

1. Start a Kyubu-loaded `Server.exe` and two Kyubu-loaded `Cube.exe` clients, A and B, on one machine or one network. Put the same ability mod (for example `mods-examples/fourth_ability_demo`) in the `mods` folder of both clients.
2. Join both clients to the server with a warrior each. Stand them next to each other, facing each other.
3. On client A, press key 4. Look at A's player on client B's screen.
4. Expect: A's log has `on_start` and `on_end` of the ability. B's log has neither, because B runs only its own player's modes; for an ability with `watch` callbacks B's log has `<id> (watched) ends ...` instead, and B shows what those callbacks draw. B draws A's player in the ability's bound clip, if it has one; else in the game's look for a mode it does not know (the mod's number is above 110), which may be nothing or a vanilla pose. Write down what B showed.
