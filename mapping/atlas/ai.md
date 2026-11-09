# AI and spawning

Atlas page for the `ai` subsystem: the behaviours that drive monsters, villagers and companions, and how they are
attached to creatures when the world places them. Reference pages: [docs/game/ai.md](../../docs/game/ai.md).

## Purpose

Every creature the player doesn't control acts through a behaviour object: monsters fight (`cube::CombatBehavior`),
villagers walk around or along paths and talk to the player, pets and summons follow their owner. Behaviours are
created with the creature's spawn during world generation ([world](world.md), [creatures](creatures.md)) and are
stepped every frame. The abilities they use are on the [combat](combat.md) page.

## Key classes

All RTTI classes in both builds, `verified` as classes (`rtti:`), with the same sizes (`operator new`) in both:

| Class | Size | What it seems to be |
|---|---:|---|
| [`cube::Behavior`](../../docs/game/types/cube_Behavior.md) | | the abstract base; it has no vtable of its own in the image |
| [`cube::CombatBehavior`](../../docs/game/types/cube_CombatBehavior.md) | 20 | fighting: approach, attack, use abilities; the constructor sets +4 to 20,000 (a range or a timer in ms, guess) |
| [`cube::CompanionBehavior`](../../docs/game/types/cube_CompanionBehavior.md) | 16 | following an owner (pets, summons) |
| [`cube::LookAtPlayerBehavior`](../../docs/game/types/cube_LookAtPlayerBehavior.md) | 4 | turning towards the player; no state besides the vtable |
| [`cube::RandomInteractionBehavior`](../../docs/game/types/cube_RandomInteractionBehavior.md) | 8 | villagers doing things at random (guess from the name) |
| [`cube::RandomWalkBehavior`](../../docs/game/types/cube_RandomWalkBehavior.md) | 8 | wandering |
| [`cube::SequentialBehavior`](../../docs/game/types/cube_SequentialBehavior.md) | 12 | a sequence of behaviours: it owns a container (copied with a length check) and has a destructor |
| [`cube::SpawnLocationBehavior`](../../docs/game/types/cube_SpawnLocationBehavior.md) | 4 | staying near, or returning to, the spawn point (guess from the name) |
| [`cube::WalkPathBehavior`](../../docs/game/types/cube_WalkPathBehavior.md) | 28 | walking a path (guards, villagers) |

The vtable is the same for all: slot 0 is the per-frame step, slot 1 is `clone` (it allocates a copy with
`operator new`). `cube::SequentialBehavior` adds slot 2, its deleting destructor; the others have no destructor in the
vtable, so they are plain objects owned by value or freed without one.

`cube::Spawn` ([creatures](creatures.md)) belongs to both pages: the generators that create spawns create the
behaviours with them.

## Entry points

All candidate, in both builds:

- The eight constructors, `cube::CombatBehavior::CombatBehavior()` … `cube::WalkPathBehavior::WalkPathBehavior()`,
  told apart from `clone` by being called directly. Where a class has a second vtable-storing function, the entry
  notes name it (`cube::CompanionBehavior` 0x43B440, `cube::WalkPathBehavior` 0x58B860, and the copy constructor and
  destructor of `cube::SequentialBehavior`).
- The eight steps (vtable slot 0), named `update`: `cube::CombatBehavior::update()` (8,846 bytes in Cube.exe,
  9,147 in Server.exe; rand, sin, cos, and ability_duration from [combat](combat.md)),
  `cube::CompanionBehavior::update()`, `cube::LookAtPlayerBehavior::update()`,
  `cube::RandomInteractionBehavior::update()`, `cube::RandomWalkBehavior::update()`,
  `cube::SequentialBehavior::update()` (no direct calls: it steps its children through their vtables, guess),
  `cube::SpawnLocationBehavior::update()` and `cube::WalkPathBehavior::update()`.

Who creates them (Cube.exe):

- The region generators under `cube::World::generateZone()` (0x5ADD20, 0x5CAF60, 0x5D4A90,
  0x5D7EB0, 0x5DB130, 0x5EF6F0) each call `cube::CombatBehavior::CombatBehavior()` and `cube::Spawn::Spawn()`: every
  spawned creature gets its behaviour when the region is generated.
- The zone creature path (0x60EBAD) gives a new creature the record behaviour of its `cube::Spawn` (a clone, vtable
  slot 1) or, when the Spawn has none, the default: a `cube::SequentialBehavior` holding a `cube::CombatBehavior`
  (argument 20.0f), a `cube::LookAtPlayerBehavior` and, for most kinds, a `cube::RandomWalkBehavior`, each appended
  with `std::list::push_back` (0x4466A0). The four constructors and the append have prototypes (probe
  `spawn-behavior` calls them by hand for a spawned slime); the creature keeps the result at +0x13E4
  (`cube::Creature.behavior`). The full catalogue of what the game builds where is under "Behaviour catalogue".
- 0x5ED1D0 (called from the creature step at 0x60D048 and 0x60D28C, and from 0x6158FD) turns an existing creature into a
  pet: it frees the creature's behaviour, builds a sequence of a `cube::CombatBehavior` and a
  `cube::CompanionBehavior` whose owner is the id it was given, and sets the class byte for three entity types. The
  summon path (0x61423D) builds a sequence of combat, companion and random walk for a new creature.

## Data flow

1. World generation creates a `cube::Spawn` and its behaviour (often a `cube::CombatBehavior`, or a sequence of
   behaviours for villagers).
2. When the creature becomes live the behaviour is copied to it with `clone` (guess: slot 1 has no direct caller, so
   it is called through the vtable, most likely when a spawn becomes a creature).
3. Every frame the creature's behaviour is stepped through vtable slot 0 (the caller is not found yet: the call is
   indirect). The combat step reads the ability stats and chooses attacks.
4. Companions are created at run time by 0x5ED1D0 under the creature step.
5. The behaviour's step is timed in the creature loop of `cube::World::update()`. At `cube::World::aiTickSite` (Cube.exe
   0x611B0A, Server.exe 0x5378CA, verified; add-skills-unleashed-multiplayer 10.3, design D12) the creature's
   `cube::Creature.behavior_time` has just grown by dt. Below 32 ms, or with a stun above 0, the behaviour is not run;
   otherwise its update follows. The creature is at [EBP-0x2B14] (and EAX), the same frame offset on both exes. Only the
   World that owns a computer creature passes there: players skip it, and so does a joined client's World. So on a
   joined game the server runs the monsters' AI. Probe [creature-root-server](../probes/creature-root-server.toml): a
   monster whose mode is held at 0 there, and whose movement is cut at `cube::World::moveStepSite` ([world](world.md)),
   read mode 0 for 250 steps and moved 0.000 blocks under 10 knocks on Server.exe.

## Client and server

- All eight behaviour classes exist in both builds with the same sizes. Six of the eight steps are correlated by
  identical bytes or instructions; the combat and random-interaction steps differ slightly in size (8,846 against
  9,147 bytes, and 3,495 against 3,498).
- So the server runs the same AI for the creatures of a multiplayer world. In single-player the client runs it itself
  (6.2 found no `cube::Server` class in Cube.exe).
- Which side steps a creature in multiplayer: the server runs the monsters' AI, and a joined client's World skips the AI
  tick (`cube::World::aiTickSite`, 10.3 of add-skills-unleashed-multiplayer). Whether the client still moves the creatures it only receives between
  updates is open.

## Hook points of interest

- `cube::CombatBehavior::update()` (candidate): the monsters' fighting; a mod can make them smarter, passive or
  aggressive.
- The other steps (candidate), for example `cube::WalkPathBehavior::update()` for villagers' routes.
- `cube::CombatBehavior::CombatBehavior()` (candidate): runs when a spawn is given its combat behaviour; change its
  parameters (+4) there.
- `cube::Spawn::Spawn()` ([creatures](creatures.md)): what is spawned, before its behaviour is attached.
- `cube::World::aiTickSite` (verified, both exes): once per computer creature per update of the World that owns it,
  before the 32 ms test; a
  mod can hold a creature's AI there (the loader's rooted flag writes `cube::Creature.behavior_time` and the mode to 0).
  followPath still turns a rooted creature; it is not moved.
- `cube::CombatBehavior::targetScanSite` (candidate; build 07-20: Cube.exe 0x42D0EE, Server.exe 0x40350E, found for the
  Ninja in add-skills-unleashed-multiplayer 8.6): the scan's range test, with the same registers on both exes ("Target
  choice" below). On a joined game the server's monsters scan there.

## Open questions

- The instruction that calls slot 0 (the behaviour pointer is `cube::Creature.behavior`, +0x13E4). It follows
  `cube::World::aiTickSite` in the creature loop (Data flow, 5), but the call itself is not named. A probe that logs the
  callers of `cube::RandomWalkBehavior::update()` (a small function) would find it.
- What the floats mean: CombatBehavior's +8 (20.0f at every site; unchanged in game) and the WalkPathBehavior float
  (2.0f, or 10.0f for a leader; unchanged in game). 8.2's probe did not vary them. CombatBehavior +4 counts down from
  20,000 to 0 (a cooldown or timer, guess), and the waypoint index at WalkPathBehavior +0x10 is confirmed (0 to 3 on a
  four-corner path).
- How the combat step picks an ability, and whether `cube::HostileType` changes the aggro scan. The choice of the target is read from the code
  ("Target choice" below, task 7.2a of add-skills-unleashed); no replay has run it yet.
- What `cube::RandomInteractionBehavior` interacts with. It is built only into two region records (a town
  character and one monster pack) and has no state but +4. The talk strings (`creature:strong`, `creature:stupid`,
  `creature:clever` in Cube.exe 0x4E0B30, Server.exe 0x42B350) are not reached from it; villagers' talk probably
  comes from the interaction prompt of hostile type 3, not from a behaviour. 8.2 built one by hand into a sequence
  and saw nothing for 240 s at 4 and 10 blocks (no turn, no move, no state), and every hostile-3 creature has the
  [R] TALK prompt without it.
- Whether Server.exe builds the same records (7.4).

## Target choice

Read from the decompile of `cube::CombatBehavior::update()` on 2026-10-12 and confirmed in the game the same day
(add-skills-unleashed, task 7.2a, probe [taunt-target](../probes/taunt-target.toml)). The choice is the creature's
**threat map**, `cube::Creature.threat` (+0x13A4):

1. Each step the AI asks `cube::Creature::topThreat()` for the id with the largest value in the map and looks that id up in
   the World's creature map. A creature found is the target. It is **not** compared with the others by distance, so it is
   kept as long as the entry is the largest and the creature exists.
2. With no target (the map is empty or the id is gone) and the delay at CombatBehavior +0x10 spent, the AI scans every
   creature of the World: alive, not a pet, `creatures_are_enemies()` true, inside the radius at +8 (20 blocks, the
   squared distance divided by a factor that grows with the other creature's `Creature + 0x1190`), and in sight
   (0x59D760). The nearest wins and gets threat 1.0 in this creature's map; `cube::World::alertAllies()` gives its
   allies within 8 blocks threat 0.5.
3. The map is fed by `cube::World::applyHit()`: damage x 2 + 0.01 for the attacker (x 1 for a ranged attacker, x 10
   for entity type 25, x 20 for a tank weapon), so the creature that hurts it most becomes its target. A target that
   is far or not in sight is chased (CombatBehavior +0xC counts the ms), and its threat falls to 0.9 of itself each
   step; after 20 s without reaching it the AI clears the map and waits 3 s before it scans again.
4. A pet (`hostile_type` 5) does not use this: it follows its owner and takes the entry of `cube::Creature.hit_counts`
   (+0x13AC) with the most hits.

Measured (probe `taunt-target`, five level-1 green slimes with the zone's combat behaviour): with their maps emptied and 25 to
42 blocks away they had no target for 15 s; with the player's id written as 1e7 every World step they walked to the player from 27
to 49 blocks in 4 to 8 s and stayed. A creature that once fought the player keeps an entry (0.01 is enough) and chases from any
distance with no write at all: the 20 blocks are only the radius of the scan for a first target. The AI removes an entry only in
the give-up; to end a taunt, write the old value back (or a small one) or clear the map.

What a mod can do with it: write a large value for an id into a monster's map (the game's own `creature_map_slot()` on
the map returns the value's address; the key is the id at Creature +8). The monster then walks to that creature from any
distance, whatever else hits it, and the entry should be written again each step, because the chase lowers it and a
give-up clears the map. This is the taunt of the Guardian. It changes the World that runs the monster only: in
multiplayer that is the server.

The other way round, a creature can be made to forget a target and not find it again (the Ninja's true stealth, probe
[ninja-scan-skip](../probes/ninja-scan-skip.toml), 2026-10-19):

- **Forget.** `topThreat()` starts its best value at 0 and keeps an entry only when it is not below the best, so an entry
  of −1 is never the target, and a map holding only that gives no target. The entry must be written **every step**: a single
  write read 0.01 again within 150 ms while the monster stood next to its target (not from the scan and not from applyHit's
  write to the victim's map; the writer is not found). Written every step, five slimes in contact stopped hitting within 1.5 s
  and stood still.
- **Not found again.** The scan's range test of each candidate is `cube::CombatBehavior::targetScanSite` (0x42D8AE): EBX is
  the candidate, XMM2 its squared distance, already divided by the square of the candidate's stealth factor (1 − 1.5^k ×
  `cube::Creature.stealth`, never below 0.1, so a fully stealthed creature is still found within 2 of the 20 blocks). A
  huge XMM2 written there for one creature keeps every monster from picking it.
- Taken together a monster neither keeps nor picks the creature. Damage it takes from the creature puts the attacker back in
  its map (applyHit), so the forgetting must stop when the creature attacks.
- **Leave no entry behind.** The −1 entry stays in the map after the writes stop and comes back as 0.01, which makes the
  creature the monster's target from any distance. Write it only on monsters that already hold the creature, and erase it at
  the end with `creature_map_erase` (0x64F6C0, the World map's erase; a threat map has the same nodes). Found in play on
  2026-10-19: written on every monster of the World, it sent all of them after the player once the stealth ended.

## Behaviour catalogue

Static reading of every place Cube.exe builds a behaviour (task 8.1 of `add-entity-spawner`); nothing here is
verified in game yet (8.2). The constructors 8.1 gave prototypes to are candidate. Sizes and fields:

| Class | Size | Fields (offsets from the object) |
|---|---:|---|
| `cube::SequentialBehavior` | 12 | +4 a `std::list` of child pointers (append with `std::list::push_back` on +4) |
| `cube::CombatBehavior` | 20 | +4 = 20,000, +8 the constructor's float (20.0f at all 24 sites), +0xC and +0x10 zero |
| `cube::LookAtPlayerBehavior` | 4 | none |
| `cube::RandomWalkBehavior` | 8 | +4 zero |
| `cube::SpawnLocationBehavior` | 4 | none (the home position is the creature's, +0x1C0) |
| `cube::RandomInteractionBehavior` | 8 | +4 zero |
| `cube::CompanionBehavior` | 16 | +4 not written (zero the block); +8 and +0xC the owner's creature id (a uint64; zero from the constructor) |
| `cube::WalkPathBehavior` | 28 | +4 a `std::vector` of `cube::LongVector3` waypoints (24 bytes each; append with `WaypointVector::push_back`, 0x5AC920), +0x10 zero (the waypoint index), +0x14 not written by the constructor (a timer: zero it; the copy constructor does), +0x18 the constructor's float |

**Where they are built.** A behaviour reaches a creature by one of four routes, all ending in
`cube::Creature.behavior`:

1. *Record*: the region generators build it into the Spawn (+0x109C) and the zone path (0x60EBAD) clones it (vtable
   slot 1) into the creature. Spawn fields the sites set: +0x28 hostile type, +0x2C entity type, +0x30 class byte (the
   creature's +0x140); +0x34 and +0x54 come from the region and are not decoded.
2. *Zone default*: a Spawn without a record gets a fresh sequence (below). A Spawn whose hostile type is TARGET (6) gets
   nothing at all.
3. *Pet*: 0x5ED1D0(this, owner id low, owner id high, creature).
4. *Summon*: 0x61423D builds a new creature and its behaviour.

**Record behaviours** (Cube.exe; Seq = `cube::SequentialBehavior`, every Combat takes 20.0f, every Walk takes 2.0f
unless noted). The entity type is either fixed, picked at random from a list, or human (2 or 3); no site keys its
behaviour on the entity type:

| Builder | Spawn | Behaviour |
|---|---|---|
| 0x5B9654 (0x5ADD20) | hostile 3, class 0x88 | Seq[LookAt] |
| 0x5BB846 to 0x5BC0BC | hostile 3, human, class 0x84, 0x80, 0x82, 0x81, 0x83 | Seq[Combat, LookAt, Walk] |
| 0x5BC91D | hostile 3, class 0x88 | Seq[Combat, LookAt, SpawnLocation, RandomInteraction, RandomWalk] |
| 0x5BD259, 0x5BD972, 0x5BDCB9 | hostile 1 | Seq[Combat, Walk] |
| 0x5D26A2 (0x5CAF60) | hostile 1 | Combat alone, no sequence |
| 0x5D38DD, 0x5D4374, 0x5D47E7 | hostile 1 or 3 (a flag; 3 is human) | Seq[Combat, Walk] |
| 0x5D3F22 | hostile 1 | Seq[Combat, Companion (owner = another Spawn's id), Walk] |
| 0x5D5B75 (0x5D4A90), leader | copied | Seq[Combat, LookAt, Walk 10.0f, RandomWalk] |
| 0x5D60DA, followers | the leader's | Seq[Combat, LookAt, Companion (owner = the leader's id), Walk (a copy of the leader's), RandomWalk] |
| 0x5D8304 (0x5D7EB0) | hostile 1 | Seq[Combat, RandomWalk] |
| 0x5DD533 (0x5DB130) | hostile 1 | Seq[Combat, Walk, RandomInteraction, RandomWalk] |
| 0x5DD8ED | hostile 1 | Seq[Combat, Walk, RandomWalk] |
| 0x5DDCC9 | hostile 1 | Seq[Combat, Companion (owner = another Spawn's id), RandomWalk] |
| 0x5F3162 (0x5EF6F0) | hostile 3, classes 0x83, 0x85 to 0x87, 0x89, 1 to 4 | Seq[Combat, LookAt, Walk] |
| 0x5B07DE, 0x5D0FE1 to 0x5D1A17, 0x5F2247 | hostile 6 (TARGET): scarecrow (140), vase (143) | none |

The walk path of every Walk gets one waypoint, the Spawn's own position (`WaypointVector::push_back` with Spawn
+0x10, e.g. 0x5BB92E). The Sequential's children run in order.

**Zone default** (0x60EBAD, only when the Spawn has no record and its hostile type is not 6): Seq[Combat 20.0f,
LookAt] and, unless the creature's class byte is 0x80 to 0x87 or bit 0x40 of the byte at creature +0x7E is set,
RandomWalk. The hostile type decides whom the combat step fights, not which behaviours a creature has: the flag at
0x5D3866 gives the same Seq[Combat, Walk] to a hostile 1 or a hostile 3 creature.

**Run time.**

| Builder | Result |
|---|---|
| Pet, 0x5ED1D0 | frees the old behaviour (operator delete on the object only), stores Seq[Combat, Companion (owner = the id passed in)], writes the floats 100.0, 1.0, 1.0, 1.0, 1.0 at creature +0x178 to +0x188 and, for entity type 25, 86 or 104 (turtle, spitter, snout beetle), the class bytes at +0x140 (0x102, 0x103) |
| Summon, 0x61423D | new creature with hostile type 3 if the caster is a player (hostile 0), else 1; Seq[Combat, Companion (owner = the caster's id), RandomWalk]; the caster's id also goes to creature +0x198 and +0x19C |

**What this means for the API (8.3).** The game has no "behaviour for this entity type" rule; the closest is the zone
default. The kinds worth exposing are: *monster* (the zone default, done in 6.2), *friendly* (Seq[LookAt,
RandomWalk], or the zone default), *companion* (Seq[Combat, Companion(owner id)], as the pet builds it, or with
RandomWalk as the summon does), *guard* (Seq[Combat, LookAt, Walk] with a path, the villagers' record), *default*
(the zone default with its exceptions, TARGET none) and *still*. The owner is an id, not a pointer: a creature handle
is turned into its `cube::Creature.id`.

## Depth

reconnaissance, reviewed 2026-09-27

### What each class does in game (probe `spawn-behavior-all`, task 8.2)

Wine replay, Cube.exe, a level-1 player, hand-built objects (`mods-dev/behavior_proto`: `operator_new` block zeroed,
the constructor, the children appended with `std::list::push_back`, the pointer written to `cube::Creature.behavior`,
the creature's +0x1C0 set to its position). Friendly test creatures are hostile type 3 (the player cannot hurt them).
Nothing here raised a status: the constructors stay `candidate` until the user has seen the table.

| Class | Result | What was seen |
|---|---|---|
| `CombatBehavior`, alone (no sequence) as `creature.behavior` | works | a hostile slime went from 6 blocks to 2.2 blocks of the player at once and followed it; the player's hp fell 265, 167.6, 23.6. +4 counts down (0x4BC1, 0x18C6, 0), +8 stays 20.0f |
| `CompanionBehavior` (owner id at +8) in Seq[Combat, Companion] | works | walked from 12 to 2 to 3 blocks of the player, followed it over 44 blocks, and fought the hostile slime the player did not touch (its hp 100, 99.3, 15.4, 1.5; the companion's fell to 15.9) |
| `RandomWalkBehavior`, alone | works | wandered up to 32 to 35 blocks from its spawn (mean 9 to 11), yaw changing every sample |
| `SpawnLocationBehavior` in Seq[SpawnLocation, RandomWalk] | no effect seen | wandered as far as the lone RandomWalk (up to 31 to 34 blocks) although +0x1C0 was set; no pull to the home position up to 34 blocks. Needs another condition, a longer range, or it is inert as the first child |
| `LookAtPlayerBehavior` in Seq[LookAt] | works within about 9 blocks | yaw -180 (facing the player, bearing 180) at 4 blocks, following the player round; unchanged at 9.6 to 10 blocks, turned at 8.8 |
| `RandomInteractionBehavior` in Seq[RandomInteraction] | no effect seen | 240 s at 4 and 10 blocks: no turn, no move, +4 zero |
| `WalkPathBehavior` in Seq[WalkPath] and Seq[Combat, WalkPath] | works once +0x14 is zero | index 0, 1, 2, 3 round a four-corner square, turning at each corner, within 7 blocks of its spawn; a one-waypoint path 6 blocks away was walked for 4 to 5 blocks (facing it) |

Two traps the hand-built objects showed. The constructors of `WalkPathBehavior` (+0x14) and `CompanionBehavior` (+4) do not
write every field: the game reaches its creatures through a clone (the copy constructors write them), so a plain
`operator_new` block holds garbage there; the first three runs' `WalkPath` creatures did not walk. And the loader's
spawn recipe leaves `cube::Creature` +0x1C0 (`spawn_pos`) empty; the zone path copies the Spawn's position there, and a
`SpawnLocationBehavior` would read it.

For 8.3: `KY_SPAWN_COMPANION` (needs the owner id) and the wandering, look-at and path kinds have evidence; `SpawnLocation`
and `RandomInteraction` show nothing, so a friendly kind that includes them changes nothing visible (leave them out of
`KY_SPAWN_FRIENDLY`, or keep them only for parity with the game's town character). The lone `CombatBehavior` works,
so the monster kind may skip the sequence if a caller wants a plain fighter.
