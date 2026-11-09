# Combat and abilities

Atlas page for the `combat` subsystem: the class abilities, their cooldowns, costs, durations and strengths, hits and
experience. Reference pages: [docs/game/combat.md](../../docs/game/combat.md).

## Purpose

Combat is what creatures do to each other: attacks, the class abilities (smash, cyclone, mana shield, sneak, …), the
damage they deal and the experience a kill gives. The attackers are creatures ([creatures](creatures.md)); monsters and
NPCs choose their attacks through their behaviour ([ai](ai.md)). The skill tree and its tooltips are UI
([ui](ui.md)), but they are the best way into the ability stats, so they are mapped here.

## Key classes

There is no RTTI class for abilities. The state is identified by numbers:

- [`cube::AbilityId`](../../docs/game/types/cube_AbilityId.md) (verified, `mapping/types/combat.toml` since 9.2): the
  value of `cube::CreatureData.current_mode`, one byte. `cube::Creature::modeDuration` switches on every id from 0 to
  105, and the per-frame dispatch (9.3) has cases up to 110, so the list is 0 to 110 (111 values; 106 to 110 were
  added after 9.3). 36 ids have a name from the game's own strings, and 0 is `NONE` (no mode); the others are
  `MODE_<id>`:
  - the cast bar: `cube::GameController::update()` shows the current mode's name with a switch on current_mode - 30
    (0x49340C, index table 0x49D5F0, targets 0x49D5A0) over the UTF-16 strings at 0x700960 … 0x700AE4. It names 23
    ids: 30 fire swirl, 31 fire vortex, 32 water swirl, 33 water vortex, 34 healing stream, 37 fireball, 38 to 40
    firebolt, 41, 42 and 44 water drop, 43 water splash, 45 water salvo, 46 fire salvo, 49 teleport, 87 explosion,
    88 fire explosion, 89 lava, 92 summon, 94 firebeam, 95 fireray, 105 teleport to city. Ids with one string
    for several ids get `_2`, `_3` (`FIREBOLT_2`).
  - the skill tooltips: the ids that the tooltip code passes to the stat functions, with their placeholders (below).
    All 16 match cuwo's names and values, so the enum keeps cuwo's names.

  Some ids are known by what the code does with them (notes on the values): 57 and 59 are a new warrior's M1 and M2
  (59 is followed by 60), 80 is the potion, and the ability switch sends 3, 4, 23, 31, 33, 37, 45, 46, 65, 66, 94 and
  95 to `cube::Creature::weaponAttack`. `kmap import-cuwo` leaves the enum out of `cuwo-constants.toml` now.

  | Id | Ability | Tooltip placeholders |
  |---:|---|---|
  | 21 | ranger kick | `@rangerkickcooldown`, `@rangerkickknockback` |
  | 34 | healing stream | `@healingstreamcost` |
  | 48 | intercept | `@interceptcooldown` |
  | 49 | teleport | `@teleportcooldown` |
  | 50 | retreat | `@retreatcooldown`, `@retreatdistance` |
  | 54 | smash | `@smashcooldown` |
  | 79 | sneak | `@sneakcooldown`, `@sneakspeed`, `@sneakstealth` |
  | 86 | cyclone | `@cyclonecooldown`, `@cycloneduration` |
  | 88 | fire explosion | `@fireexplosioncooldown`, `@fireexplosionknockback` |
  | 96 | shuriken | `@shurikencooldown` |
  | 97 | camouflage | `@camouflagecooldown`, `@camouflageduration` |
  | 99 | aim | `@aimcooldown`, `@aimstealth` |
  | 100 | swiftness | `@swiftnesscooldown`, `@swiftnessamount`, `@swiftnessduration` |
  | 101 | bulwark | `@bulwarkcooldown`, `@bulwarkamount`, `@bulwarkduration` |
  | 102 | war frenzy | `@warfrenzycooldown`, `@warfrenzyamount`, `@warfrenzyduration` |
  | 103 | mana shield | `@manashieldcooldown`, `@manashieldpower`, `@manashieldduration` |

- [`cube::SkillId`](../../docs/game/types/cube_SkillId.md), [`cube::ClassType`](../../docs/game/types/cube_ClassType.md)
  and [`cube::Specialization`](../../docs/game/types/cube_Specialization.md) (candidate, cuwo): the skill tree and the
  class that owns an ability.

## Entry points

The ability stats, all candidate, in both builds (thiscall on a creature, guess; the tooltip passes the object at
GameController+0x8006C8):

- [ability_cooldown](../../docs/game/combat.md) (Cube.exe 0x43EF40): ability id and skill level in, cooldown in
  milliseconds out. Used for every ability in the table.
- [ability_power](../../docs/game/combat.md) (0x43F600): ability id and skill level in, a float out: the bulwark and
  war frenzy amounts, mana shield's power, sneak's speed and stealth, and camouflage's duration (with level -1).
- [ability_duration](../../docs/game/combat.md) (0x447960): ability id in, milliseconds out (cyclone). Also called by
  `cube::CombatBehavior::update()`, so the monsters' AI times its abilities with it.
- [ability_cost](../../docs/game/combat.md) (0x4450E0): ability id and skill level in, a fraction out (healing stream's
  cost; the tooltip shows it times 800).
- [skill_description_fill](../../docs/game/combat.md) (Cube.exe only, 0x478D30): replaces the `@` placeholders of a
  skill description with the numbers above. Its caller, 0x4A58D0, builds the skill tooltip (strings `skill:level`,
  `skill:nextlevel`, `description:`). Some durations in the tooltip are constants (war frenzy and bulwark show 10).

The input-to-ability-start path (9.1, probe [ability-start](../probes/ability-start.toml)), all in Cube.exe:

- `cube::GameController::update()` walks the player's ability table each frame: a `std::vector<int>` at
  GameController+0x80080C, one ability id per skill-bar slot (M1, M2, 1-4; a new warrior has 57 and 59). The Q slot
  (the potion) is handled apart, by 0x4A2940 (current_mode 80).
- Slot 0 (M1, the primary attack) goes to `cube::Creature::primaryAttack` (0x445A10, candidate), which sets the next
  attack's mode from `cube::Creature::nextAttackMode` once the current one has run its `cube::Creature::modeDuration`.
- For a held slot 1 or higher: `cube::Creature::modeInterruptible` (0x43EBF0), then `cube::Creature::canUseAbility`
  (0x43EE40: mp against [ability_cost](../../docs/game/combat.md), and the cooldown map at Creature+0x139C), then a
  switch on the id (101 ids, about 20 cases). Most ids end in `cube::GameController::abilityStartSite` (0x49BF68),
  where the id has just become current_mode and the mode time is cleared: it fires exactly once per use. Weapon
  attacks (ids 3, 4, 23, …) go through `cube::Creature::weaponAttack` (0x445CA0) instead, and the buffs (97, 100 to
  103) through 0x5940A0, a World method that starts their cooldown without a mode.
- The next World step (`cube::World::update`) sees the mode time at 0 and stores
  [ability_cooldown](../../docs/game/combat.md)(mode, -1) in the creature's cooldown map, once per new mode and for
  every creature.

Where the current ability is stored (9.2, probe [ability-force](../probes/ability-force.toml)): the creature's
`cube::CreatureData.current_mode` (Creature+0x68, a `cube::AbilityId`) and `cube::CreatureData.mode_start_time`
(Creature+0x6C, the milliseconds spent in the mode, despite cuwo's name). Writing an id there with the mode time at 0,
through the inspector and with no button pressed, makes the player perform the ability, exactly as
`cube::GameController::abilityStartSite` does it (the site itself never fires):

- 59 (a new warrior's M2): the next World step stores ability_cooldown(59); one step later the game sets the
  follow-up 60 itself and starts its cooldown too. The warrior swings the club as on a real press (screenshots in
  the evidence folder; one caught a yellow glow). 60 has no timed end
  (`cube::Creature::modeDuration` gives 0), so without the button it stays 60. A real press (9.4) shows the order:
  59 is the charge and lasts while the button is held (charged_mp grows), and 60 starts on release and spends the
  charge.
- 86 (cyclone), which a level-1 warrior has not learnt: the warrior spins, and each write starts its cooldown again.
  Neither the skill tree nor the cooldown map is checked: those checks belong to the input path
  (`cube::Creature::canUseAbility`), not to the mode.

Where each ability runs per frame (9.3, probe [ability-dispatch](../probes/ability-dispatch.toml)): inside
`cube::World::update()`, which for each creature in each step switches on current_mode - 1 at
`cube::World::abilityDispatchSite` (Cube.exe 0x610745, Server.exe 0x537B05; 110 cases, ids 1 to 110, into 18 case
blocks). Mode 0 and the ids without a block go to the default. A block runs a creature's mode only when World+0xB4 is 0
and the creature is not a player (hostile_type != 0), or when the creature is World+0xB8; in single-player +0xB4 is 0
and +0xB8 is the player (id 1). The blocks (Cube.exe addresses; Server.exe has the same groups):

| Block | Ids |
|---|---|
| 0x610CA3, the melee case | 1-7, 9-21, 30-33, 51, 54, 57, 58, 60-62, 65-70, 72, 74-78, 86-88, 91, 93, 104 |
| 0x6144BC | 24, 25, 27, 36, 59, 63, 64 |
| 0x616303 | 37-46, 108 |
| 0x614571 | 28, 94, 95 |
| 0x61596A | 22, 23, 26 |
| 0x615F16 | 50, 55, 96 |
| 0x613B0E | 89, 90 |
| 0x610AB8 | 47, 48 |
| own blocks | 8 (0x610B44), 34 (0x61354B), 49 (0x613DA0), 73 (0x615626), 92 (0x6140B8), 101 (0x613891), 103 (0x6139BD), 105 (0x610763), 110 (0x6156D2) |
| the default 0x6147C9 | 0, 29, 35, 52, 53, 56, 71, 79-85, 97-100, 102, 106, 107, 109 |

Inside the melee case, the mode is tested again: 86 (cyclone), 72, 5, 11, 30-33 and 91 take the branch at 0x611620,
which runs the effect between 0x43D330's time (a per-mode switch like `cube::Creature::modeDuration`) and that time
plus [ability_duration](../../docs/game/combat.md). The probe traced the dispatch while two abilities were forced on
the player: 59 went through the 0x6144BC block only, cyclone through the melee case and its branch at 0x611620 only
(the 60 that follows 59 uses the melee case without that branch). Idle, neither block ran. At each block's entry the
low byte of ECX is the mode (0x3B, 0x3C, 0x56 were seen). Some blocks build a stack object from the creature (0x4663A0,
then 0x42C6E0 with the creature's data) with a type id (0x11 in the melee case, 0x37 in 59's block) and pass it to
0x5F35B0 (a `std::list` push) on the step output's list at +8: it is a sound, cuwo's sound action (the position as
floats, the sound id at +0xC, pitch and volume at +0x10 and +0x14, 1 by default), not a hit (9.5).

Costs and limits (9.4, probe [ability-costs](../probes/ability-costs.toml)). All are paid inside
`cube::World::update()`, per creature, not on the input path, which only checks them:

- **Fixed mp costs.** [ability_cost](../../docs/game/combat.md) is non-zero only for 3 and 4 (0.1), the spells 31, 33,
  37, 43, 45, 46 and 95 (0.3) and healing stream 34 (0.125, less with its power); every other id costs 0, including a
  warrior's 57, 59, 60 and cyclone. The melee case takes the cost from mp on the mode's first step, at
  `cube::World::abilityCostSite` (Cube.exe 0x611E74), and keeps it in `cube::Creature.spent_mp` (+0x13B4). The vortices
  31 and 33 pay again on each step while they run (0x6120B4), and 28 drains 0.0002 mp per ms in its own block.
- **Charged mp.** The hold modes 8, 24, 25, 27, 36, 59, 63 and 64 build up `cube::CreatureData.charged_mp` once
  their start time (0x43D330) has passed, at `cube::World::mpChargeSite` (0x619064): 0.00075 per ms, scaled by attack
  speed and some buffs, never above mp. On the next step of a melee mode (the 60 that follows 59),
  `cube::World::chargedMpSpendSite` (0x611ECA) subtracts it from mp, moves it to spent_mp and zeroes it. So a
  warrior's M2 costs whatever was charged while the button was held. 59's own block plays sound 0x37 with
  charged_mp * 0.5 + 1 as its pitch (9.5 corrected 9.4's reading of it as a strength). That block is every hold mode's, so a
  bow's held charge (25) gets the same rising charge2.wav, once in each 200 ms (`cube::World::chargeSoundSite`, audio.md).
- **Cooldowns.** `cube::Creature.ability_cooldowns` (+0x139C) is a `std::map` from ability id to the milliseconds left.
  On every mode's first step, `cube::World::cooldownStartSite` (0x616DE7) stores
  [ability_cooldown](../../docs/game/combat.md)(mode, -1) there; each step counts every entry down to 0 (0x6109EE).
  Only 15 ids have a cooldown at all (21, 48, 49, 50, 54, 72, 86, 88, 96 to 103), so a warrior's M1 and M2 have none.
  `cube::Creature::canUseAbility` refuses an id while its entry is above 0.
- **Stun.** While `cube::CreatureData.stun_time` is above 0 the step counts it down and holds the mode at 0, and
  canUseAbility refuses every id except 101. A monster's mode 54 (SMASH) sets it (about 2000 ms) when it hits the player;
  so did 9 and 74 in 3 of 4 sweeps, 20 and 88 once each (probe [ability-interrupts](../probes/ability-interrupts.toml)).
- **Slowed.** The timer at Creature+0x130 that `cube::World::update` counts down at 0x610654 is `slowed_time`
  (`data+0x120`), in ms, clamped to 0. While it is above 0 the step also stores `modeDuration` + 1 in the mode time
  (the call at 0x61066A runs). That had no visible effect: the mode time kept counting and a running 57 or 115 was
  not ended (probe `ability-interrupts`, phases 2 and 3).
- **Walk speed** (probe [ability-walk-speed](../probes/ability-walk-speed.toml), 2026-10-19). The input sets only the
  acceleration (Creature+0x40); the velocity (Creature+0x34) is changed in the World step, which holds a speed cap per
  creature in a local ([EBP-0x2B58] of `cube::World::update`):
  - It starts at 6.0 (0x617B00), 8.0 when 0x444BC0 is true or bit 0x200 of the appearance flags (+0x7E) is set, 12.0 when
    Creature+0x60 is 5. Some modes override it later (0x618A86 to 0x618FDA): 0 while 68, 69 or 93 wind up, then 50 for
    part of the mode; 60 in 76, 77 and 78; 12 in 72; 0.1 in 57 and 74 after their release time; 30 when Creature+0x60 is 4.
  - It is doubled while bit 0x40 of `data.flags` is set (0x618653). Bit 0x40 means "full speed". For the player,
    `cube::GameController::update` sets it each frame after the input (0x498500): set when GameController+0x14 is 0
    (while gliding, bit 0x10, it follows GameController+0x12 and needs stamina). Then, only when GameController+0x14 is
    0, the mode rule (0x49851B to 0x4985DF, between `walkLockSite` and `walkLockEndSite`) clears it and sets bit 4,
    lock-on: for the whole mode in the hold modes 8, 0x18, 0x19, 0x1B, 0x1C, 0x23, 0x24, 0x3B, 0x3F and 0x40, and in any
    other mode while its time is below `cube::Creature::modeDuration` + 200 ms. Modes 0x0B, 0x22, 0x4F to 0x54, 0x69 to
    0x6B and 0x6D are left alone; 0x57 to 0x59 and 0x5C lose the bit without lock-on. Monsters get a like rule from
    `cube::CombatBehavior::update` (0x42DA32). So most abilities halve the walk's cap while they run: a mod ability's
    walk peaked at 7.11 blocks/s against 9.84 on foot.
  - `make_blue_time` (Creature+0x134) above 0 halves it and `speed_up_time` (+0x138) above 0 multiplies it by 1.5
    (0x6191B4 to 0x6191CD).
  - With lock-on set and the walk pointing away from the facing (Creature+0x160), it is multiplied by 0.5 · cos + 1 of
    the angle between them (`cube::World::lockOnWalkPenaltySite`, 0x6192C7): 0.5 walking straight backwards.
  - At 0x619304 a speed above the cap is scaled down to it, x and y written back through `cube::FloatVector3::at`. This is
    what binds: on foot the cap of 12 is reached (the frame's peak 9.84 is after the drag); in the probe's mode it cut the
    speed a mean 0.70 a step, against 0.92 on foot.
  - Then `cube::World::positionNoiseForce` (a noise field of the position, a wind or current guessed) is scaled by the
    step's time and added to the velocity (0x6195FE), and a later part of the step (0x61B8A0 to 0x61C0A6) slows it by about
    5 % a step (the drag).
- **Stamina.** `cube::Creature.stamina` (+0x1194, 0 to 1, outside the network data) is what the HUD's stamina bar
  shows while it is below 1. The step drains it while swimming (up to 0.0005 per ms, scaled by a value 0x43EF00 reads) and
  climbing (0.0002 per ms) and refills it otherwise (0.0001 per ms); a dodge roll (0x4A71FA) takes 0.25 and sets
  roll_time to 600 ms. No ability uses it.
- **Mp gains.** mp regenerates in the step at 0.00025 per ms times the creature's stealth, `cube::Creature.stealth` at +0x1190 (0x618580; [creatures](creatures.md) "Stealth"), and the
  field cuwo calls block_power (CreatureData+0x164) is added to mp and charged_mp and run down (0x5958B0), like mp
  still to be gained. Where a warrior's rage drains is not found.

Hits and damage (9.5, probes [hit-damage](../probes/hit-damage.toml) and
[hit-authority](../probes/hit-authority.toml)). A hit is a `cube::Hit`, the 0x48 bytes of cuwo's hit packet: the
attacker's and the target's ids, the damage, a critical flag, stun and slow times, a position, a knockback direction
and a hit type. Everything happens in the World step of the side that runs the attacker's mode:

- **Detection.** A dispatch block looks for targets and asks `cube::World::canHit(attacker, target)`: a player and a
  monster (hostile_type 0 and 1) hit each other, two monsters never do, a player hits a TARGET (6, such as a bush)
  too, and a neutral creature (2 or 3) only when it is flagged (0x20 at Creature+0x124; probe `hostile-types`: level-1
  slimes of 1 and 6 lost hp, of 2 and 3 none). The melee case computes the damage at 0x612E81:
  `cube::Creature::modeDamageFactor(mode)` (2 for 37, 43, 57, 58 and 68; 0.1 to 0.6 for some spells and kicks; 1
  otherwise) times the attacker's 0x43E4F0 (a damage stat) times 5 * spent_mp² plus a base, times a random 0.75 to
  1.25, doubled on a critical hit. So a charged M2 hits harder through `cube::Creature.spent_mp`.
- **Armour and recording.** `cube::World::hitCreature(target, attacker, damage, …)` (Cube.exe 0x595580) subtracts
  the target's `cube::Creature::armor` (`cube::Creature::resistance` for the vortex spells) times a factor and clamps
  the damage at 0; both grow with the level, so a level-1 creature's cyclone (about 1 damage) does nothing to a
  player, as the probe saw. It then builds the hit and appends it to the hit list of the step's output (a
  `std::list` of hits, then sounds at +8). The melee case calls it at 0x613088; six other places in the step
  (projectiles, spells) call it too.
  It records the hit in two ways (read 2026-10-12): the normal path builds the Hit at ESP+0xC4 and inserts it
  inline (0x596ED7, site `cube::World::hitListSite`); its special paths (hit types 3 and 5) call
  `step_output_push_hit`. The damage number the game shows is read from the list copy, so a cut of the damage
  must be made before that copy (the list site or the push), not only at `hitDamageSite`.
- **Application.** Unless the World is a joined client's, hitCreature then calls `cube::World::applyHit`
  (0x5942B0). applyHit finds both creatures by id, updates the threat maps (Creature+0x13A4 and +0x13AC), lowers the
  target's stealth at +0x1190, applies the stun (`cube::CreatureData.stun_time`, zeroing the velocity), the slow
  (`slowed_time`) and the knockback (`extra_vel`), handles the hit types (1 gives 0.25 mp, 3 is ignored, 4 calls
  0x43D7B0, 5 hits the shields in the list at Creature+0x1178 first) and subtracts the damage from hp at
  `cube::World::hitDamageSite` (0x59483E). A negative damage heals, up to max hp (0x4453B0, which scales with
  max_hp_multiplier; the step clamps hp to it, so a probe that raises hp must raise the multiplier first). At hp 0 it
  plays sound 0x1B and handles the death; the killer gets its experience ("You gain 1 XP." in the probe).

Who decides (single-player and multiplayer):

- Single-player: the World (World+0xB4 `cube::World.joined` 0, `local_player` the player) runs every creature's
  mode, detects every hit and applies it at once. The probe forced the player's cyclone next to a hostile creature:
  hitCreature from 0x61308D and applyHit from 0x596F25, the damage site once per hit, and the creature's hp fell.
- A joined client (`client_connect` sets GameController+0x390, that is World+0xB4): its World runs only its own
  player's mode, so it detects only its player's hits; hitCreature records them and does not apply them, and skips
  its own player as a target. The recorded hits are sent to the server (cuwo's packet 7).
- The server (`joined` 0, `local_player` 0): its World runs the monsters' and NPCs' modes, never a player's. Each step
  applies the hits the clients sent (`cube::World::update`'s third argument, looped over at
  `cube::World::receivedHitSite`, Server.exe 0x532AD0), so the server applies the damage to monsters, but the
  amount is the one the attacking client computed. The probe saw each M2 hit of the joined client go through that
  loop into applyHit's damage test; a new warrior's uncharged M2 carries a damage of exactly 0 (single-player, phase
  3), so it had nothing to subtract. A hit on a player, whether a monster's or received, is
  not applied: applyHit forwards it (`cube::World::hitForwardSite`, 0x59435C in Cube.exe, 0x4CEA9C in Server.exe) to
  that player's client, which subtracts it from its own hp in `cube::GameController::update()`
  (`cube::GameController::receivedHitDamageSite`, 0x49049B) and reports its hp back like the rest of its creature.
  So each player's hp is changed only by that player's client; a monster's hp is changed by the server, by the
  amount the attacking client computed.

Projectiles (9.6, probe [projectile-spawn](../probes/projectile-spawn.toml), the fireball 37). A projectile is a
`cube::Projectile`, the 0x70 bytes of cuwo's shoot packet: the shooter's id, a position, a velocity, the damage, a
scale, the mana, a skill flag, a kind and an age. It lives in the World, not in the shooter:

- **Spawn.** The dispatch block of the projectile modes (37 to 46 and 108, at 0x616303) shoots when the mode time
  passes the release time (0x43D330) and the shooter's aim `cube::CreatureData.ray_hit` isn't zero (the player's is
  rewritten from the camera every frame by `cube::GameController::update()`, so a mod can't aim it by writing it).
  It builds the projectile on the stack (velocity: the aim's direction at 50 blocks a second; damage: modeDamageFactor(mode) times
  0x4474D0 times a random 0.75 to 1.25), pushes it on `cube::World.projectiles` (World+0x14, a `std::list`) at
  `cube::World::projectileSpawnSite` (0x616A26), copies it to the step output's shoot list (+0x20; a joined client
  sends it to the server) and plays a sound. 45 and 46 shoot again every third of the ability's duration; 108 throws
  a falling kind-4 projectile.
- **Flight.** Each step, the loop over the list (`cube::World::projectileUpdateSite`, 0x61F31D, once per projectile)
  moves it by velocity × dt in sub-steps of about half a block, adds dt to its age, and looks for creatures along the
  way. Kind 1 (the spells) flies straight; kind 2 (a boomerang) turns back to its shooter after 1100 ms; the others
  fall.
- **Hit and removal.** For each creature it meets (hp above 0, not the shooter, `cube::World::canHit`, the boxes
  overlap, a clear line from 0x59D760), an arrow or a boomerang (no skill flag) hits it through
  `cube::World::hitCreature` (called from 0x6206DC), so armour, recording and application are those of the melee
  hits. Kinds 0, 1 and 4 stop there; 2 and 3 go on. A spell (kind 1 or 4 with the skill flag, like the fireball)
  never hits directly: it stops at the first creature or block, plays a sound and explodes, hitting every creature
  in its box through hitCreature from 0x621305 (the explosion has the dispatch's guard: a joined client explodes
  only its own player's shots). A projectile with 2 at +0x64 applies negated damage (a heal) with applyHit instead.
  The projectile then goes to the removal list (`cube::World::projectileRemoveSite`, 0x61F5B8), as it does when it
  is 5 s old (all but boomerangs), or when a boomerang is back or its shooter is gone.
- **The hit site** (build 07-20, add-skills-unleashed-multiplayer 10.9a, design D15): `cube::World::projectileHitSite`
  (Cube.exe 0x621E28, Server.exe 0x547BE8, candidate) runs once per creature a non-water projectile hits, before its
  `cube::World::hitCreature` call (Cube.exe 0x621E8C, Server.exe 0x547C4C). EDI is the projectile, [EBP-0x2B54] the target
  and ESI the shooter (may be 0). To skip the hit, go on at 0x621EAA (the target still gets the slow) or 0x621EC1 (no slow).
  On a client, the hit it makes is sent as packet 7. The server's twin is the same code at Cube.exe - 0xDA240, with the stop
  (`cube::World::projectileStopSite`, 0x547CDE) and the block test (`cube::World::projectileSurfaceSite`, 0x547EC2); nothing
  in it asks which side runs it, so on the server a projectile in `cube::World.projectiles` hits whoever its shooter is.

Experience is handled inside `cube::GameController::update()`, which uses the strings ` HIT`, ` HITS`, `You gain `,
` XP.` and `LEVEL UP!`. The per-frame creature step 0x60AEA0 ([creatures](creatures.md)) also calls
ability_cooldown, ability_power and ability_duration.

Levels and experience. These entries were read statically on 2026-09-30; the formulas are in the section "Levels,
power and experience" below.

- [level_curve](../../docs/game/combat.md) (0x43D2F0, candidate): the saturating curve f(x) = 1 − 1/(0.05(x − 1) + 1)
  behind power, XP to the next level, kill XP and max HP. It has 22 call sites, and the same term is inlined in
  several more functions.
- [creature_level_up](../../docs/game/combat.md) (0x448150, candidate): the level-up loop. While
  `cube::CreatureData.current_xp` reaches ⌊1000·f(level) + 50⌋, it takes that much, adds a level and refills hp with
  `cube::Creature::maxHp()`. There is no level cap. It is a `__thiscall` with one stack argument, the World (`ret 4`,
  never read): a replacement must pop it, or the game stops at the next kill that levels a player (task 4.1).
- [kill_record_apply](../../docs/game/combat.md) (0x59F4E0, candidate): applies a kill record from
  `cube::World::applyHit`'s death branch, feeding the local player and, in a World that isn't a joined client's, the
  killer's pet.
- [is_giant_creature](../../docs/game/combat.md) (0x444BC0, candidate): the ten entity types whose kill XP is ×20.
- [creature_attacks_from_range](../../docs/game/combat.md) (0x444B10, candidate): picks the threat factor in
  applyHit.
- [creature_attack_speed](../../docs/game/combat.md) (0x447D50, candidate): the speed factor that scales hold-mode
  charging; it grows with a monster's power_base.
- creature_power_delay_ms (0x4455F0, guess): 3000 − 375·power_base ms for non-players and 1500 for players. Its
  purpose is unknown.

Creature stats (read statically on 2026-09-30, all candidate). Each takes the `cube::Creature` in ECX. The character
sheet (0x435620) prints Power, HP, ARMOR, RESI, TEMPO and REG right after calling
[creature_power](../../docs/game/combat.md), `cube::Creature::maxHp()`, `cube::Creature::armor()`,
`cube::Creature::resistance()`, [creature_attack_speed](../../docs/game/combat.md) and
[creature_regeneration](../../docs/game/combat.md), which names them. Each adds a level term to the item getters of
the worn items ([items](items.md)):

- [creature_power](../../docs/game/combat.md) (0x446510): ⌊100·f(level) + 1⌋ as an int, the same formula as
  [item_power](../../docs/game/items.md). [can_equip_item](../../docs/game/items.md) computes it inline instead.
- [creature_damage](../../docs/game/combat.md) (0x43E4F0): the attacker's damage stat. It reads the level, the
  damage multiplier (Creature+0x180) and [item_damage](../../docs/game/items.md) of the held weapons, and all six
  callers are mode blocks of `cube::World::update()`.
- [creature_projectile_damage](../../docs/game/combat.md) (0x4474D0): the projectiles' damage stat, built the same
  way. Of its 12 call sites, three lead up to `cube::World::projectileSpawnSite`; others are mode blocks that don't
  shoot, and 0x5940A0.
- [creature_crit](../../docs/game/combat.md) (0x43F2E0) and [creature_tempo](../../docs/game/combat.md) (0x447F00):
  a level term plus [item_crit](../../docs/game/items.md) or [item_tempo](../../docs/game/items.md) of the nine
  worn items. creature_tempo feeds [creature_attack_speed](../../docs/game/combat.md), its only caller. The sheet's
  crit is 0x43F280: 1.0 while a buff of kind 11 is on, otherwise creature_crit plus 0.15 × Creature+0x1190.
- [creature_regeneration](../../docs/game/combat.md) (0x4467A0): [item_regeneration](../../docs/game/items.md) of
  the worn items. Only the character sheet calls it; where regeneration is applied in play isn't traced.

Where a mode ends (task 2.2 of add-ability-api, probe [ability-mode-end](../probes/ability-mode-end.toml); static scan
of the stores of 0 to Creature+0x68, a byte, in the evidence folder `ability-mode-end-2026-10-04`). Cube.exe addresses.
`modeDuration` is not a generic end: no code outside the mode blocks compares the mode time with it to clear the mode.

| Where | Ends | When |
|---|---|---|
| `cube::World::update` 0x616DBD | any mode, every creature, every step | data.stun_time (+0x12C) is still above 0 after the step's time is taken off it: mode and mode time are both cleared. The probe wrote 2000 during mode 115: mode 0 within 0.5 s |
| `cube::World::update` 0x617D0E | any mode | bit 0 of `data.flags` (+0x124) is set (not run in the probe) |
| `cube::GameController::update` 0x49C4A8 | the player's mode only | key byte GameController+5 is 0, the mode is not one of 8, 28, 29, 35, 36, 79, 82, 83, 84, 106, 107, and the mode time is above 10000 ms. The probe: 115 (key 4 tapped for 0.3 s) was still on 8 s after the press and gone 12 s after it (mode time 11176); a forced 57 stayed 6 s and was gone at 12 s |
| `cube::World::update` 0x6189B4, 0x6171CD, 0x617BC2 | modes 48 (after 500 ms), 83 and 84 (a chance), 107 | the modes' own timers |
| `cube::World::applyHit` 0x594417 | mode 84 | the creature is hit |
| `cube::GameController::update` 0x494DA2, 0x49C1BB, `onMouseDown` 0x47DA4E | the player's mode (28 at 0x49C1BB) | input paths, not read further |
| FUN_004A6D10 (`this` is the GameController; the player's movement each frame) 0x4A7251 | the player's mode only | the dodge roll (task 2.7, probe ability-interrupts): GameController+0xA (the middle mouse button) and +0xB (W) are down, stamina is at least 0.25 (0x744CE8), `roll_time` is 0 or less (or a mode runs) and `physics_flags` & 3. It stores `roll_time` 600 (0x4A723F), takes 0.25 stamina and writes 0 to current_mode. The probe held W and the middle button during mode 115: mode 0, `roll_time` counting down, stamina down; either one alone did nothing |
| `cube::CombatBehavior::update`, `cube::RandomInteractionBehavior::update`, `cube::RandomWalkBehavior::update` | a monster's mode | the AI, not read further |

So an id above 110 (the default case of the dispatch, no per-frame code) is ended only by the generic rows: for the
player the 10 s rule or a stun, for another creature a stun or the flag (the AI rows were read statically only). Mode
time counts on in it, as it does at mode 0. A mod that wants a timed ability must end it itself, by writing 0 to
current_mode.

Who reads the ability bar (task 2.3 of add-ability-api, static scan in the evidence folder `ability-bar-readers-2026-10-04`,
probe [ability-bar-soak](../probes/ability-bar-soak.toml)). Cube.exe addresses. Every read of GameController+0x80080C is
inside `cube::GameController::update`; the constructor, the destructor and two unwind thunks only zero, free or destroy it.

| Where | Reader | An id above 110 |
|---|---|---|
| 0x48BFC0 | learned gate (i = 2 up, writes 0 when `skills[i+4]` is 0) | runs before the push at 0x48BFDC: never sees it |
| 0x49B7CE to 0x49C121 | input loop, i = 1 .. size-1 | goes to the default of the switch on `id - 3` (0x49BF13): `ability_cost` against mp, then the byte store of the id to Creature+0x68 |
| 0x49BF8B, 0x49BFA9 | key not held: switch on `id - 8`, range 0 to 56 | nothing |
| 0x48C010 to 0x48C343 | HUD loop, bounded by the 7 slot widgets, not by the bar | the empty-icon branch past the bar; cooldown map and icon map look-ups insert a default node; the sweep needs a non-zero `ability_cooldown` |

The bar is built in the same function (task 2.5): `IntVector::clear` (0x4AC230: `end = begin`, capacity kept) at 0x48BB0B,
then `IntVector::push_back` (0x5BE230, `ret 4`) for M1, M2 and three class ids. `cube::GameController::abilityBarBuiltSite`
(0x48BFDC) is the point after the learned gate where the bar is complete; the field is `cube::GameController.ability_bar`.

The HUD's icons come from `cube::GameController.ability_icons` (+0x800818, task 2.6): a `std::map<int, T*>` that the
constructor fills with 72 inserts (0x45A6CB to 0x464A39), each value the result of `cube::GameController::loadIcon`
(0x486DF0, an image path in, an object from the engine at +0x800708 out). The only readers are in
`GameController::update`: the HUD pass (0x48C010 to 0x48C343) and a switch at 0x48E9AD to 0x48EBBD, both through
`IntMap::operator[]` (0x4044D0, which inserts a zero value for a missing id) and both skip a null. Nothing writes the
values after the constructor. The destructor (0x467300, reached by the scalar deleting destructor 0x46D7A0 that WinMain
calls at 0x4C92BB when its loop ends) frees the map's nodes through the range erase 0x67E1D0 and never the values.
Probe `ability-icons`: slot 4 was empty while 115's node held null; copying smash's pointer into it drew smash's icon.

The game stores the id as a byte (`mov [creature+0x68], al`, compared with `movzx`), so an id above 255 would never
match its bar entry: 255 is the highest usable id. No reader indexes past the bar. The soak replay kept 115 on the
bar for 5 min 21 s, with key 4 held 11 times and the class changed 4 times, and no fault.

The fourth slot, from the start (tasks 2.1 and 2.4 of add-ability-api, probes [ability-slot4](../probes/ability-slot4.toml)
and [ability-stats](../probes/ability-stats.toml); the entries are listed under "Abilities" on the
[primitives](primitives.md) page):

- A sixth bar entry pushed at `cube::GameController::abilityBarBuiltSite` draws a sixth HUD slot labelled 4 with
  a blank grey icon (the icon map has no entry), and the HUD does not grey it. Key 4 sets GameController+9 and the input
  loop writes 115 to `current_mode` on the first frame. `ability_cost(115)` is 0, so no mp is taken.
- A held key restarts the mode every frame (19 starts in 2 s): `modeInterruptible` is true for an id above 110.
- The learned gate zeroes a level-1 character's class slots (the bar read 57, 59, 0, 0, 0, 115). The class and the
  specialization pick the three class ids: warrior 54/86/102 or 101, ranger 21/50/99 or 100, rogue 48/79/97 or 96, mage
  88/103/49 or 34/103/49 (with M1 40 or 42).
- The stat functions take their arguments as follows: `ability_cooldown(id, level)` returns ms in EAX (`ret 8`) and is
  called with level -1 by the HUD and the step; `ability_cost(id, level)` returns a float in st0 (`ret 8`) and is mostly
  called inside `canUseAbility(id)` (`ret 4`); `modeDuration()` takes no argument and reads `current_mode`. The skill
  tree's tooltip takes the cooldown from `ability_cooldown(id, rank)` / 1000: a hook that returns 77777 for id 54
  showed 77.78 s.
- Both mode 54 and mode 115 ended by themselves about 10 s after the press (the player's 10 s rule above).

## Data flow

1. The player opens the skill tree: 0x4A58D0 loads the skill texts, and skill_description_fill fills them in from the
   stat functions for the current and next skill level.
2. When the player presses a skill-bar slot, `cube::GameController::update()` checks it (modeInterruptible,
   canUseAbility) and writes the slot's ability id to current_mode (`cube::GameController::abilityStartSite` for most
   ids); the next World step starts the cooldown from ability_cooldown (`cube::World::cooldownStartSite`).
3. Each World step, `cube::World::update()` runs every creature's current mode through
   `cube::World::abilityDispatchSite`, the switch on the mode (9.3). There the mode pays its mp: its fixed cost on its
   first step, or the mp charged while a hold mode was held (9.4).
4. Monsters and NPCs use abilities from `cube::CombatBehavior::update()`.
5. A mode that reaches a target (`cube::World::canHit`) computes the damage and calls `cube::World::hitCreature`,
   which appends a `cube::Hit` to the step's output and, unless the World is a joined client's, applies it with
   `cube::World::applyHit` (hp falls at `cube::World::hitDamageSite`) (9.5).
6. In multiplayer, a joined client sends its player's hits to the server (cuwo's packet 7), whose World step applies
   them to monsters and forwards the hits on players (`cube::World::hitForwardSite`) to the players' clients; each
   client applies those to its own player (`cube::GameController::receivedHitDamageSite`) and reports its hp.
7. A projectile mode instead pushes a `cube::Projectile` on `cube::World.projectiles`; later steps move it and hit
   the creatures it meets, or those around its explosion, through `cube::World::hitCreature`, as in 5 (9.6).
8. Hits are counted and shown, and experience and level-ups are applied, in `cube::GameController::update()`.
9. **Experience:**
   - When a hostile creature dies in `cube::World::applyHit`, the World works out its kill XP. It is
     max(1, 20·f(level + power_base)), then ×20 for a giant, ×10 with appearance flag 0x200, and 0 with 0x800.
   - Every player in the victim's threat map (Creature+0x13A4) gets a kill record with the full amount. The XP is not
     split.
   - [kill_record_apply](../../docs/game/combat.md) adds it to the local player and runs
     [creature_level_up](../../docs/game/combat.md).
   - `cube::GameController::update()` then compares the level and XP with the last shown values (Creature+0x1D3C and
     +0x1D40). It prints "XP +N" and "You gain N XP.", or "LEVEL UP!" plus a call to
     [level_up_teach_formulas](../../docs/game/items.md).

## Client and server

- The four stat functions are in both builds with the same bytes or instructions (the survey's correlation), so the
  server computes the same cooldowns, costs and durations. The server needs them at least for the monsters' AI.
- The skill tree, its tooltips (skill_description_fill) and the hit and XP messages are client-only.
- Kill XP is computed where the victim dies, which is the server for monsters in multiplayer (applyHit is in both
  builds). The level-up loop runs on each player's client. A joined client applies the kill records it receives in the
  loop of `cube::GameController::update()` (the call at 0x48FD25 passes each to
  [kill_record_apply](../../docs/game/combat.md); read statically, 2026-10-04), so the XP in the record is the server's.
- A hit is detected by the side that runs the attacker's mode: a joined client for its own player, the server for
  the monsters and NPCs (9.5). The server applies the damage to monsters; a player's hp is changed only by that
  player's own client, which applies the hits the server forwards to it. So a modded client decides its own hp and
  its hits on monsters, and the server trusts both.
- The per-frame dispatch is in both builds with the same case groups (`cube::World::abilityDispatchSite`). Its guard
  (`cube::World.joined`, `cube::World.local_player`) makes a joined client run only its own player's modes; the
  server's World has neither set, so it runs every non-player's mode (probe hit-authority).
- `cube::World::applyHit`, `hitCreature`, `canHit` and the two hit sites are in both builds with the same code
  (Server.exe 0x4CE9F0, 0x4CFCC0, 0x4D1830, 0x4CEF7E, 0x4CEA9C); the survey's correlation missed them.
- The three projectile sites are in both builds with the same code (Server.exe 0x53DDE6, 0x5466DD, 0x546978, at
  Cube.exe − 0xD8C40 like `cube::World::update` itself). A client's own shots fly in its World and are sent to the
  server in the step output's shoot list; the server's World flies the monsters' shots.
  - A joined player's shot is not flown by the server: Server.exe queues the packet and `cube::Server::takeBullets` relays
    it to the other clients ([network](network.md), task 10.9a of add-skills-unleashed-multiplayer).
  - A projectile pushed into the server's `cube::World.projectiles` inside a World step is flown, stopped on the server's
    blocks and makes its hits there. The SkillsUnleashed Sniper flies its checked copy so (replay server-sniper-0720 run 6:
    a hit for 188.9, stopped on a block).
- `cube::World::hitCreature` takes a stun **chance**, not a time: a stun it gives is the target's own maximum, and only
  3 s after the last one. A mod that wants a set stun and direction writes `cube::Hit.stun_duration` and
  `cube::Hit.hit_dir` in the Hit at `cube::World::hitListSite` (ESP+0xC4, both exes, verified), for its own call only
  (design D11; the loader's `creature_hit`).
- A hit the server makes on a player goes twice. `cube::World::hitCreature` pushes the Hit on the step output at
  `cube::World::hitListSite`, then calls `cube::World::applyHit`. The server's World has no local player, so for a player
  target applyHit pushes the same Hit again (`cube::World::hitForwardSite`), and the client takes both copies from its hp
  (replay server-guardian-0720 run 5). The miss, block and dodge paths push and apply their own Hit without the list site.
  The loader's `server_hits` fix drops the second copy for the list site's Hit (`cube::Hit.target_id`'s creature data has
  `cube::CreatureData.hostile_type` 0); those other paths still go twice. Cube.exe applies a hit on its own player once.
- The creature stats that hits use are in both builds (2026-09-30, candidates): `cube::Creature::maxHp()` (Server.exe
  0x40F730), `cube::Creature::armor()` (0x407EF0) and `cube::Creature::resistance()` (0x410ED0), both called only by
  `cube::World::hitCreature`, and [creature_damage](../../docs/game/combat.md) (0x408B60). Each has the same length and
  callees as Cube.exe's, with 83 to 90% of the bytes equal. So the server computes monster stats itself, and a mod
  that changes stats must run there too.

## Hook points of interest

- [ability_cooldown](../../docs/game/combat.md), [ability_cost](../../docs/game/combat.md),
  [ability_power](../../docs/game/combat.md) and [ability_duration](../../docs/game/combat.md) (candidate): change an
  ability's balance for one id without touching the rest. Hooking them changes the tooltip too, which is a cheap way
  to check such a hook in game (open the skill tree).
- [skill_description_fill](../../docs/game/combat.md) (candidate): add placeholders for modded abilities.
- `cube::CombatBehavior::update()` (candidate): the monsters' use of abilities ([ai](ai.md)).
- `cube::GameController::abilityStartSite` (a mid-function site, client only): runs once per player ability start,
  with the id in current_mode; a mod can react to or replace the start there.
- `cube::CreatureData.current_mode` and `mode_start_time` (verified): the simplest way for a mod to start an ability
  on any creature is to write the id and clear the time, as the inspector does; costs, cooldown checks and the skill
  tree are skipped.
- `cube::World::abilityDispatchSite` (a mid-function site, both builds): runs per creature per step with the creature
  in EAX; a new ability id (above 110) goes to the default block today, so a mod would run its per-frame behaviour
  from here (9.7). Task 4.4 of add-ability-api runs it so (probe [ability-step-runner](../probes/ability-step-runner.toml)): the
  World steps 20 ms of game time at a time, the site reads the mode time before the step adds its 20 ms (the first step of a
  mode reads 0), and a mode above 110 is left alone by the game until its 10 s rule, a stun or the flag, so the loader writes
  mode 0 and time 0 itself once the duration is reached.
- `cube::World::abilityCostSite`, `chargedMpSpendSite`, `mpChargeSite` and `cooldownStartSite` (mid-function sites,
  Cube.exe): where a mod can change or refund an ability's mp cost or its cooldown for every creature, including the
  monsters, after the input path's checks.
- `cube::GameController::walkLockSite` and `walkLockEndSite` (mid-function sites, Cube.exe, verified): the player's
  "full speed" bit is cleared between them while an ability runs; keeping the value it had at the first site and writing it
  back at the second keeps the walk's full cap (skills_unleashed `lib/sites` `on_walk`). `cube::World::lockOnWalkPenaltySite`
  (candidate) is where lock-on's cut for walking away from the facing can be made 1.

- `cube::World::hitCreature` and `cube::World::applyHit` (both builds): every hit passes through them, with the
  `cube::Hit` as data, so a mod can change damage, stun or knockback for any attacker, or veto a hit. On the server,
  applyHit is also where a mod would check the clients' hits. `cube::World::hitDamageSite` is the one place hp falls
  from a hit on that side.
- `cube::World::projectileSpawnSite` and `cube::World::projectileUpdateSite` (mid-function sites, both builds): a
  mod can change a new projectile (its speed, damage or kind) before it is stored, or steer one in flight; writing a
  `cube::Projectile` into `cube::World.projectiles` would spawn one for any creature (9.7).
- `cube::World::projectileHitSite` (candidate, both exes): where a projectile's hit on one creature can be skipped. On a
  client it is the one place the player's own shot makes its hit (and its packet 7).
- Levels and experience (candidates, 2026-09-30):
  - [creature_level_up](../../docs/game/combat.md) is the one place a level rises, so it is where a level cap or a
    new XP table goes.
  - [level_curve](../../docs/game/combat.md) changes most level-dependent numbers at once. It is also inlined in
    [creature_level_up](../../docs/game/combat.md), [can_equip_item](../../docs/game/items.md) and
    `cube::Creature::maxHp()`, so those need their own hooks.
  - The kill-XP computation sits inline in `cube::World::applyHit` (Cube.exe from about 0x594B50). Hooking
    [kill_record_apply](../../docs/game/combat.md) changes what the local player receives instead.
- Creature stats (candidates, 2026-09-30): `cube::Creature::maxHp()`, `cube::Creature::armor()`,
  `cube::Creature::resistance()` and [creature_damage](../../docs/game/combat.md) change a stat for every creature at
  once, in both builds. The character sheet reads the same functions, so a hook shows up there, which is a cheap
  check in game. To change what gear gives instead, hook the item getters or [stat_scale](../../docs/game/items.md)
  ([items](items.md)).

## Open questions

- No single function starts a player's ability: the start is inline in `cube::GameController::update()`, so its
  site is the hook point.
- Where a warrior's rage drains (9.4 found the regeneration and the costs, not the drain), and what buffs 7 to 11
  are (`cube::Creature::findBuff`, 0x444500: 2 and 10 change the charging, 9 and 11 the costs; +0x1190 is the stealth, 2026-10-19). The
  Server.exe twins of the four cost and cooldown sites are mapped as candidates (2.2 of add-skills-unleashed-multiplayer);
  no replay has hooked them on Server.exe yet.
- Whether gamepilot can press M1 under Wine (its mouse1 did nothing; its mouse2 once made an M1 attack), so the
  primary attack can be probed (still open after 9.3, which forced its abilities instead).
- The factor applied to armour, and the base term of the melee damage; how the hit strings in
  `cube::GameController::update()` connect to the hits. What [creature_damage](../../docs/game/combat.md) reads is
  answered (level, damage multiplier, the held weapons' [item_damage](../../docs/game/items.md)); its exact formula
  is not.
- Of the other six callers of `cube::World::hitCreature`, 9.6 placed the two in the projectile update (0x6206DC,
  direct hits; 0x621305, explosions); the other four (0x60C6A8, 0x60DDED, 0x614FBD, 0x616CB1) are probably spells. The step output's shoot
  list (+0x20) is mapped by its push only, and where a joined client's received shots enter its World isn't known.
  What Creature+0x140 = 3 (the heal flag's condition) is. [creature_projectile_damage](../../docs/game/combat.md)
  (0x4474D0) reads the same inputs as creature_damage; why some non-projectile mode blocks call it is open.
- What the object at GameController+0x8006C8 is (the player's creature?). The stat functions take the
  `cube::Creature` in ECX (the level at +0x190), so they act as its methods whether or not the class declares them.
- What ends mode 60 (it starts on release; a forced 59 stays in 60 for good), and the meaning of the 69 ids the game
  names nowhere (`MODE_<id>`). Watching current_mode while monsters fight would name some of them.
- What the modes 106 to 110 are: the dispatch has cases for them (108 and 110 have blocks; `cube::World::update()`
  also compares the mode with 106 and 107), but `cube::Creature::modeDuration` stops at 105. The enum has them as
  `MODE_106` to `MODE_110` since 9.4.
- What 0x43D330 (a per-mode time in ms, called by most dispatch blocks) and World+0xB4 are exactly.
- The client's side of multiplayer hits is mapped statically only: the probe runs on the server, and a joined client
  has no probe. `cube::GameController::receivedHitDamageSite` is shown only by the server's copy of the player's hp.
- How a joined client gets its kill XP: the server builds the kill records (cuwo's kill action carries xp_gained) and the
  client applies them in `cube::GameController::update()` (call 0x48FD25, static read). The packet that carries them
  is not traced, and whether a pet levels in multiplayer is not either. The pet branch of
  [kill_record_apply](../../docs/game/combat.md) runs only in a World that isn't a joined client's.
- Where the 2 skill points per level are granted: only the mana-cube point (1 per 4 cubes, in
  `cube::GameController::update()`) was found.
- What 0x4455F0's delay is for.
- The walk speed (probe `ability-walk-speed`): what GameController+0x14 and +0x12 are (+0x14 at 0 gives the player's "full
  speed" bit and lets the mode rule run; a slow-walk key is a guess); why the probe saw mode 0 with the bit still clear for
  much of the time key 4 was held after `charge_proto`'s mode (mode 0's own 200 ms after each restart is one guess); what
  0x444BC0 and Creature+0x60 values 4 and 5 are (they set the cap to 8, 30 and 12); and whether the noise force is wind or
  water.
- What the received items of type 13 that add 10 XP each are. They come from a list of received items in
  `cube::GameController::update()`, and the same loop turns type 25 (mana cubes) into skill points.

## Depth

reconnaissance, reviewed 2026-09-30

## Levels, power and experience

Read statically on 2026-09-30, for the level-rework design. Everything below is `candidate` unless it says otherwise.
Throughout, f is [level_curve](../../docs/game/combat.md): f(x) = 1 − 1/(0.05(x − 1) + 1). It is 0 at level 1 and
rises towards 1 without reaching it.

| Quantity | Formula | Level 1 | Level 21 | Level 100 | Level 1981 |
|---|---|---|---|---|---|
| Power (players and items) | ⌊100·f + 1⌋ | 1 | 51 | 84 | 100 |
| XP to the next level | ⌊1000·f + 50⌋ | 50 | 550 | 881 | 1,040 |
| Level factor of max HP | 2^(3·f) | 1.0 | 2.83 | 5.64 | 7.84 |
| Item stat scale at rarity 0 ([stat_scale](../../docs/game/items.md), item level) | 2^(3·f) | 1.0 | 2.83 | 5.64 | 7.84 |

- **Levels have no cap.** Power reaches 100 only at level 1981, and power 99 already at level 981. Past about level
  100, levels add little: level 100 has 5.6× of the 7.8× maximum.
- **Kill XP** (inline in `cube::World::applyHit`):
  - The base is max(1, ⌊20·f(level + power_base)⌋) of the victim, so 1 to 20.
  - It is ×20 for [is_giant_creature](../../docs/game/combat.md) and ×10 with appearance flag 0x200, which 1% of wild
    hostile spawns get. `cube::AppearanceData.flags` also has 0x800, which means no XP.
  - It doesn't depend on the killer's level. With monsters at the player's own level, each level past about 30 costs
    about 55 kills, and old level 1981 about 108,000 normal kills.
- **Pets** level through the same loop: [kill_record_apply](../../docs/game/combat.md) credits the pet, and the pet
  item keeps its XP and level. This is seen only outside joined clients.
- **Skill points:** 1 per 4 mana cubes, from the received-items loop of `cube::GameController::update()`. There is no
  per-level grant: the budget `2·level − 2` is computed where a rank is added (see [ui](ui.md)).
- **Ability strength** (ability_power, 0x43F600): 1 − 1/(0.1·rank + 1), which is again saturating.
  - A non-player with rank 0 uses rank = ⌊level/2⌋ + power_base + 1. So monster abilities scale with the raw level.
  - [creature_attack_speed](../../docs/game/combat.md) uses the same rank rule for its buff term.
- **power_base** (the "+N" shown after a creature's name; 0 to 4 from world generation, see [world](world.md)) scales:
  - max HP by 2^(N/4);
  - [creature_attack_speed](../../docs/game/combat.md) as 0.75 + N/16 for non-players;
  - the 0x4455F0 delay as 3000 − 375·N ms;
  - ability ranks by +N;
  - kill XP through f(level + N).
- **Threat:** applyHit adds damage × factor + 0.01 to the victim's threat map for the attacker. The factor is 1 for
  [creature_attacks_from_range](../../docs/game/combat.md), otherwise 2. It is 10 for attacker entity type 25, and 20
  when the weapon in equipment slot 7 passes 0x444E20 and the specialization is 1.

## Level rework sites

Sites that the `level_rework` mod uses or watches (change `add-level-rework`, design D2, D2a and D3; tasks 3.x, 4.x and 8.x). All
are `candidate` unless noted, and each `Server.exe` entry says it is the twin of the `Cube.exe` one. Addresses are
`Cube.exe`, then `Server.exe`. The mod's formulas and tables are in the `level_rework` repository.

- **Strength level terms.** Each site is the instruction after a `cvtdq2ps` of the level (Creature+0x190), where a register
  holds the level as a float. A mid hook writes `old_float(N)` there, and the function goes on as vanilla (the register is XMM2,
  and XMM0 at the two hit sites).
  - Creature stats: `cube::Creature::maxHpLevelSite` (0x4453CF, 0x40F74F), `cube::Creature::armorLevelSite` (0x43D89F, 0x407F0F)
    and `cube::Creature::resistanceLevelSite` (0x446E0F, 0x410EEF).
  - Damage: [creature_damage_level_site](../../docs/game/combat.md) (0x43E52D, 0x408B9D) with its `_flag8` and `_flag10` copies
    (0x43E5E9 and 0x43E694; 0x408C59 and 0x408D04), [creature_damage_alt_level_site](../../docs/game/combat.md) (0x43EA4F,
    0x4090AF), and [creature_projectile_damage_level_site](../../docs/game/combat.md) with its two copies (0x44750D, 0x4475CA,
    0x447672; 0x41149D, 0x41155A, 0x411602).
  - [creature_crit_level_site](../../docs/game/combat.md) (0x43F2FF, 0x4096CF) and
    [creature_tempo_level_site](../../docs/game/combat.md) (0x447F1F, 0x411CAF).
  - The hit chance: `cube::World::hitAttackerLevelSite` (0x596CFE, 0x4D143E) and `cube::World::hitTargetLevelSite` (0x596D18,
    0x4D1458) stand in for `cube::World::hitLevelDifferenceSite` (0x596D04), which is a call and cannot be hooked.
  - Probes: `level-terms` (the sites), `level-strength` (the mod's hooks) and, on `Server.exe`, `server-level-sites`. The server sites
    0x4090AF, 0x41149D and 0x410EEF were not reached by a probe, and no replay has checked what the server's strength sites do to a hit.
- **Kill XP.** `cube::World::killXpSite` (0x594BDD, 0x4CF31D) is in `cube::World::applyHit`'s death branch, right after the kill
  XP is computed: the float at [EBP-0x2F8] is replaced by the mod's value, once per death. `cube::World::killRecordXpSite` (0x5950E4,
  0x4CF824) is where that float is truncated into each player's kill record; it is only watched. `cube::GameController::itemXpSite`
  (0x48FB04) is the zone bonus: items of type 13 add 10 XP per unit, and the mod divides the 10 by 10 so that the bonus is paid at
  1 XP per unit (task 4.6, probe `quest-bonus`). Probes: `kill-xp-replacement`, `server-level-rework-kill`.
- **Level-up and the XP bar.** [creature_level_up](../../docs/game/combat.md) is replaced by the mod's table and cap, and
  [creature_xp_to_next](../../docs/game/combat.md) (0x445360, six callers in the HUD) is replaced so that the bar divides by the
  same table and never by 0. Probe: `level-up-replacement`.
- **Ability ranks.** [ability_power_rank_site](../../docs/game/combat.md) (0x43F643, 0x409A13) is the monster rule of `ability_power`
  after both loads: EAX = level, ECX = power_base. The mod writes `floor(old(N))`, and the displaced instructions halve it and add
  power_base + 1. [ability_power_rank_join](../../docs/game/combat.md) (0x43F64B, 0x409A1B) is where the player's and the monster's
  rank meet; it is only watched (`verified` in `Cube.exe`). [creature_attack_speed_rank_site](../../docs/game/combat.md) (0x447EB5,
  0x411C45) holds the same rule for the attack speed; no replay has reached it, and the `Server.exe` twin was found by matching
  instructions. Probes: `rank-and-equip-gate`, `ability-rank-replacement`.

Open: the strength effect of the server's sites on a hit and the attack-speed sites need a replay. The respec price sites are on the
[ui](ui.md) page.

## Who reads the mode for looks

What else besides the pose switch (see [creatures](creatures.md), "Posing") reads `current_mode` (`Creature + 0x68`) to decide how a creature
looks or sounds, and what each does with an id above 110 (`add-ability-animation` 2.7; the evidence README has the run and the counts):

- **The cast bar** (`cube::GameController::update()`, the switch at 0x49340C): ids 30 to 105 pick a name through an index table; every
  other id takes the "no name" target (0x493BD5, the empty string at 0x6FBCAC). A mod's ability shows a blank name. Static.
- **The weapon trail** (the block before the five trail calls at 0x4B5CD3 in `cube::GameController::render()`): drawn when the
  creature is class 4, specialization 1, inside its mode time and in mode 5, 17 or 20; or in mode 48 or 93; or when the buff list at
  `Creature + 0x1178` holds buff 12 or 3 (0x444500(n) finds a node by its id byte). A mod id gives no trail unless a buff does. Static.
- **Footsteps and other sounds:** the step sound (`play_sound` id 32, `step2.wav`; 33 is the water step) does not depend on the mode:
  it played with the player in modes 0, 57, 58, 67 and a forced 115. The sound comes from an event queue read at 0x49580x, whose
  producer is not traced. Run (probe `sound-ids`).

## Pose catalogue: which pose a mode draws

What `draw_creature` draws for each vanilla mode id (`add-ability-animation` 4.5; the evidence README has the runs, the labels and the sheets). Probed on the
local player, a level-1 warrior with a club, with the mode forced and the mode time pinned at 100, 450 and 800 ms. A pose is drawn only while the mode time is
below `modeDuration`: after that the draw reads pose 0 and time 0 (so a borrowed pose is never longer than its mode). The first pose of a mode is listed.

| Pose | Modes that draw it |
|---|---|
| 0 | 0, 29, 52-53, 56, 85, 97, 99-100, 102, 106-107 |
| 1 | 47, 54 |
| 2 | 57, 59-60 |
| 3 | 58 |
| 5 | 64 |
| 6 | 63 |
| 8 | 98 |
| 9 | 8 |
| 10 | 10 |
| 11 | 28 |
| 15 | 2 |
| 16 | 1 |
| 17 | 4, 18 |
| 18 | 3, 9, 19, 62 |
| 19 | 7 |
| 20 | 6 |
| 21 | 22-23 |
| 22 | 24-25, 50, 55 |
| 23 | 37, 43 |
| 24 | 39, 41 |
| 25 | 40, 42 |
| 26 | 38, 44-46, 95 |
| 27 | 36 |
| 28 | 11 |
| 29 | 12 |
| 30 | 13 |
| 31 | 14 |
| 32 | 15 |
| 33 | 5 |
| 34 | 48 |
| 35 | 16-17 |
| 37 | 67 |
| 38 | 68 |
| 39 | 69 |
| 40 | 70 |
| 41 | 71-72 |
| 42 | 73 |
| 43 | 74 |
| 44 | 75 |
| 45 | 76 |
| 46 | 77 |
| 47 | 78 |
| 48 | 65 |
| 49 | 61, 66, 93-94 |
| 50 | 20-21 |
| 51 | 80 |
| 53 | 82 |
| 54 | 79 |
| 55 | 84 |
| 57 | 81 |
| 60 | 86 |
| 61 | 30-34, 49, 87-90, 92, 105 |
| 62 | 35 |
| 63 | 91 |
| 65 | 96 |
| 66 | 26 |
| 67 | 27 |
| 68 | 101, 103 |
| 69 | 104 |
| 70 | 51 |
| 71 | 108 |
| 72 | 109 |
| 73 | 110 |

- Poses 4, 7, 12, 13, 14, 36, 52, 56, 58, 59 and 64 were drawn by no mode (not the player's, or modes 65 and 83, below).
- Pose 0 is the idle look: modes 0, 29, 52, 53, 56, 85, 97, 99, 100, 102, 106, 107 draw it. The game itself moves 59 to 60, 81 to 83 and 48 to 20, and resets 52,
  106 and 107 to 0.
- **Forcing mode 65 or 83 with the time pinned stopped the game** (no more draws; cause not found). Do not force them.
- Not drawn: the other three classes and the ten weapon subtypes of the second weapon family (task 2.6): the equipment stayed the warrior's.

### The warrior's abilities by name

The same catalogue read from the other side, for the `add-skills-unleashed` briefs (task 5.1): one row for each ability the warrior's bar can hold, with the
mode it starts and the pose that mode draws. The ability ids are the table of "Key classes"; the bar is `[57, 59, class 1, class 2, class 3]` and the
specialization changes class 3 only (probe [ability-slot4](../probes/ability-slot4.toml)). The picture labels are from the catalogue's eye table (club, level 1).

| Ability | Mode id | On the bar of | Pose id | The 450 ms picture shows |
|---|---:|---|---:|---|
| M1 (club) | 57 | both | 2 | club head in front of the body |
| M2 | 59, then 60 | both | 2 | the game moves 59 to 60 at once; pose 2 at 100 ms, idle at 450 ms (the mode ends before it) |
| smash | 54 | both, class 1 | 1 | bent forward, back turned (mode 47 draws the same pose) |
| cyclone | 86 | both, class 2 | 60 | club on the back, low |
| war frenzy | 102 | berserker, class 3 | 0 | idle: no pose of its own |
| bulwark | 101 | guardian, class 3 | 68 | club raised on the left (mode 103, mana shield, draws the same pose) |

- Abilities with a pose of their own: smash (1), cyclone (60), bulwark (68) and the two M1/M2 swings (2). **War frenzy has none**: a clip for it borrows
  nothing from its mode and must name a pose by what it should look like.
- Pose 3 (mode 58, "arms at the sides, legs crossed") and pose 18 (modes 3, 9, 19, 62) are drawn by modes that no warrior ability starts. A clip may borrow them.
- Only the club (one-hand family) was drawn. The other warrior weapons may pose M1 and M2 differently; no row here is known for them.

## The bow's modes

Read statically and replayed on 2026-10-12 for `add-skills-unleashed` 8.2a (probe [bow-modes](../probes/bow-modes.toml): a ranger
made of the level-1 warrior by `class_type` 2, `sub_type` 6 on the right weapon slot, Creature+0xAA8). The weapon in hand picks the
ids, not the class: the bar builder in `cube::GameController::update` and `cube::Creature::nextAttackMode` read the right weapon
(equipment 7, +0xAA8 type and +0xAA9 `sub_type`) and the left (equipment 6, +0x990 and +0x991) every frame.

| Weapon (`sub_type`) | M1 (slot 0) | M2 (slot 1) | M2 released into |
|---|---|---|---|
| bow (6) | 22, through `primaryAttack`; a held M1 repeats it | 25 (a hold mode: `charged_mp` grows) | 55: four arrows |
| crossbow (7) | 22 | 24 (a hold mode) | 23: one arrow |
| boomerang (8) | 26 | 27 (a hold mode) | 26: the throw |

- **The bar** of a ranger with a bow read `[22, 25, 0, 0, 0, 111]` (the class slots 0 on a level-1 character; 111 is the mod's own slot 4).
  A bow in either hand counts for the bar; the same weapon test picks the mode of `cube::Creature::nextAttackMode`.
- **The held charge.** The input loop of `cube::GameController::update` runs for the ids of slots 1 and up while their key byte
  (GameController+4+slot) is set: id 25 becomes the current mode at once (mode time 0). `cube::World::mpChargeSite` adds
  0.00075 per ms to `charged_mp`, at most the mp. When the key byte falls (the release case, one jump table per id) the game writes
  the id that follows: 24 gives 23, 25 gives 55, 27 gives 26 (`cube::GameController::bowReleaseSite`). M1 is not in that loop: with
  no other slot held it goes to `primaryAttack`, which starts 22 whenever the current attack has run its time.
- **Mode 55** (a bow's M2 release; also 50 and 96, which share its dispatch block 0x615F16) shoots four arrows, one in each quarter of
  the mode, with `charged_mp` turned into `spent_mp` at the first. Replayed: a charge of 0.92 gave arrows of kind 0 with the skill flag
  set, velocity 150 blocks a second along the aim, scale 1 and mana 0.23, 110 ms apart (damage 8 to 12 for a level-1 character).
  [cube::World::chargedShotSite](../../docs/game/combat.md) is where each is made.
- **Modes 22, 23 and 26** (block 0x61596A) shoot one arrow when the mode time passes its release time: kind 0 (2 for the boomerang),
  100 blocks a second, skill flag 0 for a plain 22 and 1 for 23 or a charged shot. `charged_mp` becomes `spent_mp` there too, and the
  damage grows with `spent_mp` squared. Replayed: a held M1 shot every 0.6 s, damage 8 to 10. Made at `cube::World::bowShotSite`.
- **Making M1 start the charge.** The key bytes at GameController+4..0x17 are written by `WinMain` each loop and read by the input
  loop later in the same `update` call, so a mid hook at `cube::GameController::abilityBarBuiltSite` (0x48BFDC, between the two) that
  copies byte +4 (M1) onto +5 (M2) makes the loop run M2's id and skip `primaryAttack`. Replayed with the mod `bow_modes_proto`: M1
  held gave 0 or 22, then 25 (charge, `charged_mp` 0.875), then 55 on release.

### A straight arrow that goes through creatures

Probe [projectile-pierce](../probes/projectile-pierce.toml) (replayed 2026-10-12 with the dev mod `pierce_proto`; `add-skills-unleashed`
8.2c). No kind of the game flies straight and goes through creatures at once: kind 1 is straight but its hit sets the stop flag
(kinds 0, 1 and 4 do), kinds 2 and 3 go on, but a boomerang turns back after 1.1 s and re-hits every 100 ms after its first 300 ms, and kind 3
falls and hits only when its age passes a whole second. The pieces that make one:

- **The new arrow can be changed before it is pushed** ([cube::World::bowShotSite](../../docs/game/combat.md) for modes 22, 23 and
  26, `cube::World::chargedShotSite` for 50, 55 and 96): the `cube::Projectile` is complete on the stack, so its kind, skill flag
  and velocity can be written, and XMM1 holds the damage (a mid hook may change it). A kind 1 arrow with skill flag 0 flies straight
  at its velocity (no fall) and hits every creature it meets through `cube::World::hitCreature`.
- **The stop** is a byte at [EBP-0x2B64] of the World step, set after a hit for kinds 0, 1 and 4. `cube::World::projectileStopSite`
  runs just after it; writing 0 there lets the arrow go on to the next creature and the next sub-step.
- **One hit for each creature** needs memory the game does not have: the arrow is inside a creature's box for several sub-steps
  (a half block each), and each one can call hitCreature. [cube::World::hitCreature](../../docs/game/combat.md) takes 14 stack words (`ret
  0x38`): target, attacker, damage (float), a flag byte, the skill flag, mana (float), the position, a pointer, the step output, the
  effects struct, the kind-1 flag, 0, the **projectile** (13th), 1. A detour that knows the arrow (it was named at
  `cube::World::projectileUpdateSite`, EDI) and returns 0 for a creature it has hit already costs nothing else.
- **The end at a surface.** With the stop cleared, a kind 1 arrow that meets a block takes the spell's end (an explosion that hits
  every creature near). `cube::World::projectileSurfaceSite` is reached with the arrow in a solid block: writing kind 0 there makes
  the game end it as its own arrow (the puff of particles, no damage, removal).
- **Replayed:** a ranger with a bow and three creatures lined up in front. The game's own M2 (a volley of four, mode 55) hit the
  first creature four times, 8 to 12 each, and nothing behind it. The mod's arrow (mode 23 from the release, kind 1, speed 150
  blocks a second, a charge of 0.88 mp, damage 27.5 as made, 26.5 and 24.5 on the two hits) hit two of the three once each, when it
  was 15.5 and 21.5 blocks out, went on, and met a surface 131 blocks out after 50 steps (kind put back to 0). The third creature
  was not hit by either bow; why is not known.
- **How it is drawn.** `cube::GameController::render` loops over `World.projectiles` and jumps on the kind
  (`cube::GameController::projectileDrawKindSite`, ESI the projectile): kind 1 is a spell, drawn unturned with its own model
  (slot 0x835); kind 0 an arrow, turned along its velocity, with model slot 840 got through `ModelLoader::get`; kind 2 the
  shooter's weapon model. So a straight arrow that is to look like an arrow needs the kind written 0 for the length of its draw
  and put back at `cube::GameController::projectileDrawEndSite`. A mod model in place of slot 840, for that draw only: the
  kind 0 case calls `ModelLoader::get` (the World's loader, GameController+0x2F8) with 840, and
  `cube::GameController::projectileArrowModelSite` is the instruction after that call, with the model in EAX: a mid hook writes
  the mod's model there for its own arrow. (The first version detoured `ModelLoader::get` from Lua while an arrow flew: a white
  arrow of 28 voxels was seen tilted along the flight in a picture of a slow arrow, but that getter is called for every part of
  every creature, and putting the detour in and out stopped the game's threads at each shot.) Not changed for any other
  projectile.
- **Where the arrow goes.** The shot block copies the shooter's position into the arrow and gives it the direction of
  `ray_hit` (the aimed point, from that position) at 100 blocks a second. The aimed point lies on the crosshair's ray, which runs
  from the camera's eye (`cube::GameController.camera_position`) through a point that follows the player, so an aim at an enemy
  at arm's length is a short, steep line, and an arrow that does not stop there flies on along it (seen in play: "a random
  direction"). `skills_unleashed` keeps the game's direction for an aimed point 12 blocks or more away and pushes a nearer one
  out to 12 blocks along the camera's heading, at its own height, at `cube::World::bowShotSite`.
- **The release.** The game writes mode 55 when M2's key byte falls (`cube::GameController::bowReleaseSite`); a mod that writes 23
  there gets the one-arrow path (it takes `charged_mp` the same way).
- **Multiplayer.** The arrow is copied to the step output's shoot list as it is pushed, with the changed kind and velocity, so
  another client draws a straight arrow that stops at the first creature it meets (it does not run the mod's stop and hit changes).
  The hits are the owner's: a joined client records its own player's hits and sends them (see "Who decides"). Not tried (8.2g).

## Buffs, and no poison (add-skills-unleashed 11.2b and 11.2c)

Read statically on 2026-10-19 in the disassembly; nothing here was run. A creature's buffs are the std::list at Creature +0x1178.
A node's value starts at +8 with the kind byte, then a float and the time left in ms; cuwo's passive packet is the same record,
with a source id that cuwo says is set only for poison.

- `cube::Creature::findBuff` (0x444500) finds a node by its kind; `cube::Creature::addBuff` (0x447140) adds a record. Kinds 1, 2, 3,
  6, 7, 9, 10 and 11 replace one of the same kind; the others are appended, so they could stack.
- The ability buffs come from one switch on the ability id (0x5940A0): bulwark (101) gives kind 1, war frenzy (102) kind 2,
  camouflage (97) kind 3 for `ability_power(97)` ms, mana shield (103) kind 6, swiftness (100) kind 12. Other sites add 7, 8, 9, 10
  and 11.
- Kind 3 is what makes camouflage hide: while it is on, the World step sets the creature's stealth to 1.0 (0x618546; see
  [creatures](creatures.md) "Stealth").
- **No poison.** No site in Cube.exe makes a kind-4 or kind-5 record (cuwo's poison is 4), no caller of findBuff asks for one (the list
  walks read in hitCreature test 1 and 6, the one at 0x60D2EB tests 7), and the exe has no "poison" string, in either encoding. The hit types of `cube::Hit.hit_type` match cuwo's other names (1 block, 3 miss, 4 dodge,
  5 absorb). So in this build every damage a player deals goes through the hit sites with the player as attacker; there is no
  damage over time to tell apart. Server.exe was not read.

## Particles

Read on 2026-10-11 (static, from `cube::World::hitCreature` and `cube::World::update`) for `add-particle-effects`; evidence
`evidence/particles-2026-10-11/`. Entries are `candidate`.

- **The record.** A particle effect is a `cube::ParticleData`, cuwo's 0x48-byte particle packet: `pos` (int64 x 3, world
  units), `accel` (float x 3), `color` (float x 4, RGBA), `scale`, `count`, `particle_type`, `spreading` and a word the
  game does not write. One record is `count` small cubes started at `pos`.
- **The list.** The World step's output (`cube::StepOutput`, the `step_out` argument of `cube::World::update`) keeps
  a `std::list` of them at +0x10, next to the hits (+0), the sounds (+8) and the shots (+0x20). The only function
  that appends to it is `step_output_push_particle` (0x486940): a `thiscall` on the list that copies the 0x48 bytes
  into a 0x50-byte node. Its callers are the eight places the game spawns particles; the controller plays the list.
- **What a record becomes** (read 2026-10-12 from `cube::GameController::update`, for `add-skills-unleashed`; static, and
  agreed with play: a ring meant to gather flew outward). After the World step the controller turns each record into `count`
  particles in a `std::list` at GameController+0x800738 (at most 5000; a node is next, prev, then the particle). A particle has
  its type at +0, its age in ms at +4, its position at +8 (three int64), its velocity at +0x54 (three floats, blocks a second),
  its scale at +0xAC and its life at +0xB4. **The record's `accel` is the particle's starting velocity**, to which a random
  vector of `spreading` times 2 is added; nothing accelerates it afterwards except gravity (30 blocks a second squared, not for
  type 4; type 3 does not move at all). The constructor (0x4661D0) sets the life to 4000 ms, and the particle is removed when
  its age passes it. So a record cannot gather particles at a point: a mod that wants that writes the velocity and the life of
  the live particles (`skills_unleashed`'s `lib/fx`, `steer`).
- **The eight places** (record fields that the code writes as constants; colour and acceleration are built by helpers):

  | Site | In | scale | count | type | spreading |
  |---|---|---|---|---|---|
  | 0x59570C | `cube::World::hitCreature`, a hit with mode 0x1C | about 0.5 (random) | 8 | 1 | 3 |
  | 0x60D625 | `cube::World::update`, a creature of entity type 0x90 in its mode 0 | 0.25 | 20 | 1 | |
  | 0x60D829 | `cube::World::update` | 0.5 | 3 | | |
  | 0x611024 | `cube::World::update` | 0.25 | 20 | 1 or 2 | 8 |
  | 0x6126C1 | `cube::World::update` | 0.5 | 3 | | |
  | 0x6153CA | `cube::World::update` | 0.1 | 5 | 1 | |
  | 0x61D03E | `cube::World::update` | 0.4 | 15 | | |
  | 0x620A80 | `cube::World::update` | 0.1 | 4 | | |

  Which spell or event each is, and what each type looks like, is not read yet (task 2.3 of the change; the replay
  `particle-types` draws every type).
- **What a mod can do.** `particle_spawn` of the game API queues a record and the World step's detour pushes it with
  the game's own push after the step ran ([primitives.md](primitives.md), Particles).

## Design note: hook points for a new ability id

The smallest set of hooks a mod needs to add an ability with a new id (above 110, so no game code knows it). Every
entry below is `verified`; "side" is where the hook runs. In single-player the client's own World runs everything.

| Hook point | Kind | Build and address | Side | What the mod does there |
|---|---|---|---|---|
| `cube::GameController::abilityStartSite` | site | Cube.exe 0x49BF68 | client | on the player's key press, replace the id the input path chose with the new id (write `current_mode`, clear `mode_start_time`) |
| `cube::CreatureData.current_mode`, `cube::CreatureData.mode_start_time` | fields | CreatureData | both | the new ability's state: the id, and the time spent in it; writing them starts the ability on any creature (probe ability-force), and writing 0 to `current_mode` ends it |
| `cube::World::abilityDispatchSite` | site | Cube.exe 0x610745 | client (server twin candidate) | once per creature per step, with the creature in EAX: when its mode is the new id, run the ability's per-frame behaviour, then skip the default block; end the mode when its own timer runs out |
| `cube::World::abilityCostSite`, `cube::World::cooldownStartSite` | sites | Cube.exe 0x611E74, 0x616DE7 | client | charge the new id's mp and start its cooldown (the game's stat functions know nothing of it) |
| `cube::Creature.ability_cooldowns` | field | Creature+0x139C | client | read or set the new id's cooldown |
| `cube::World::hitCreature` | function | Cube.exe 0x595580, Server.exe 0x4CFCC0 | both | a melee or area effect: call it for each target with the ability's damage; it builds the `cube::Hit`, sends it and applies it |
| `cube::World::applyHit` | function | Cube.exe 0x5942B0, Server.exe 0x4CE9F0 | both | where the hit lands: change or veto a hit from the new ability (stun, knockback, heals by negative damage) |
| `cube::World.projectiles`, `cube::Projectile` | field, type | World+0x14; 0x70 bytes | client (the server's World has its own list) | a ranged effect: push a projectile (shooter, velocity, kind) on the list; the World step flies it and hits with it |
| `cube::World::projectileSpawnSite`, `cube::World::projectileUpdateSite`, `cube::World::projectileRemoveSite` | sites | Cube.exe 0x616A26, 0x61F31D, 0x61F5B8 | client (server twins candidate) | reuse an existing projectile mode (37, the fireball) and change the projectile before it is stored, steer it in flight, or react when it ends |
| `cube::CreatureData.ray_hit` | field | CreatureData+0x150 | client | the shooter's aim; the player's is the camera's hit point, rewritten every frame, so read it, don't write it |

Notes for the custom-ability demo:

- Start with the client alone (single-player): the start site, the two mode fields and the dispatch site are enough
  for an ability that only moves or animates the player; add `cube::World::hitCreature` for damage and the
  projectile list for a shot.
- In multiplayer a joined client runs only its own player's modes, sends its hits and shots to the server, and the
  server trusts them (see Client and server), so a player's new ability needs no server hook. A monster using the new
  id needs the server's dispatch site, which is still `candidate`.
- Left out because they are `candidate`: the stat functions (`ability_cooldown`, `ability_cost`,
  `ability_duration`, `cube::Creature::modeDuration`, `cube::Creature::modeDamageFactor`) and
  `cube::Creature::canUseAbility`. The sites above make them unnecessary for a new id; hooking them would be the way
  to give it a tooltip and a skill-tree entry.
- The cast bar's switch (0x49340C) takes an id below 30 or above 105 to its "no name" target, so a mod id shows an
  empty name (static, `add-ability-animation` 2.7). The other readers of the mode for looks are in "Who reads the
  mode for looks" below.

## Server twins of the ability sites (add-skills-unleashed-multiplayer 2.1)

Task 2.1, 2026-10-27, build 07-20: each client site the ability runtime needs, and its twin in `Server.exe`
(`server-0x51EA94D5`). Twins were found by matching the instruction sequence around the client site in a disassembly of
`Server.exe`; the World step is the same code in both exes (SSE, same struct offsets). "Entry" says whether the twin is
already in `mapping/server-0x51EA94D5/functions/combat.toml`; everything new is added as `candidate` by task 2.2, with
probes (`client = "join"`) before any raise. Infinite-Cubes (`WorldUpdate.cpp`, `CreatureStats.cpp`, `Network.cpp`) agrees
with each "no twin" row.

| Client site | Cube.exe | Server.exe twin | Entry | Note |
|---|---|---|---|---|
| `cube::World::abilityDispatchSite` | 0x611E04 | 0x537BC4 | yes, verified | default case 0x53BC80 |
| `cube::World::abilityCostSite` | 0x613559 | 0x539319 | yes, candidate | same 12 instructions: `ability_cost(mode,-1)` (0x40FB20), `spent_mp = cost`, `mp -= cost` |
| `cube::World::chargedMpSpendSite` | 0x6135AF | 0x53936F | yes, candidate | |
| `cube::World::mpChargeSite` | 0x61A7C8 | 0x540588 | yes, candidate | after `creature_attack_speed` (server 0x412150) |
| `cube::World::cooldownStartSite` | 0x618507 | 0x53E2C7 | yes, candidate | `ability_cooldown(mode,-1)` (0x409780) into the map at +0x139C |
| cooldown count-down and refill | 0x61209E, 0x611FFE | 0x537D5F to 0x537E4B (refill call 0x537DC8) | no | exact hook address chosen in 2.2 |
| `ability_cooldown`, `ability_power`, `ability_duration`, `ability_cost` | 0x43E6A0, 0x43ED60, 0x447310, 0x444AE0 | 0x409780, 0x409DE0, 0x411D60, 0x40FB20 | yes, candidate | all callers on the server are in `cube::World::update` or the stat table, none in an input path |
| `ability_power_rank_site`, `creature_attack_speed_rank_site` | 0x43EDA3, 0x447865 | 0x409A13, server entry | yes | a player's rank is read from `skills[]` in the creature data |
| `cube::World::hitCreature`, `applyHit` | 0x596D30, 0x595A60 | 0x4CFD50, 0x4CEA80 | yes, verified | |
| `cube::World::hitListSite` | 0x598679 | 0x4D1699 | yes, verified | `lea eax,[esp+0xC4]` after `cmp [ecx+0x68],0x1c`; the Hit is at ESP+0xC4 |
| `cube::World::hitDamageSite`, `hitForwardSite`, `canHit` | 0x595FEE, 0x595B0C, 0x5988A0 | 0x4CF00E, 0x4CEB2C, 0x4D18C0 | yes | `hitForwardSite` sends a hit on a player to its client |
| `cube::World::receivedHitSite` | (the server's World step, third argument) | 0x532B40 | yes, verified | where the server takes the hits its clients sent |
| projectile spawn, update, remove sites | 0x61814F, 0x620ACD, 0x620D68 | 0x53DF0F, 0x54688D, 0x546B28 | yes, candidate | |
| `cube::Creature::canUseAbility`, `modeInterruptible` | 0x43E5A0, 0x43E350 | **no twin** | | Infinite-Cubes: `canUseSkill` is called only by the monster AI and by the player's `GameController`. The server never refuses a player's ability: it trusts the mode a client sends |
| `cube::GameController::abilityStartSite`, `abilityBarBuiltSite`, `walkLockSite` | 0x49BD98, 0x48BD8C, 0x4983E2 | **no twin** (client input and bar) | | A server player has no `GameController`; the start is the mode written in the creature data |
| skill-tree learn path (`SkillWidget`, `skillButton9HideSite`) | ui.toml | **no twin** (client UI) | | The learnt ranks travel as `skillLevels` in the creature update (`Network.cpp:78`); the server's `creature_update_read` (network.toml) takes them, nothing to hook for learning |

The "Entry" column was brought up to date on 2026-11-09. The later tasks of the change added, on both exes unless noted:
the block walk and `cube::World::worldStepSite` and `cube::World::moveStepSite` ([world](world.md)),
`cube::World::aiTickSite` ([ai](ai.md)), the receive and send sites of the channel transport and the shoot packet's
`Net::receiveProjectileSite` and `cube::Server::takeBullets` (Server.exe; [network](network.md)), and
`cube::World::projectileHitSite` (above, "The hit site"). The [primitives](primitives.md) page lists them by primitive.

What this means for the later tasks (decisions to write in 2.4):

1. Cost and cooldown on the server are applied by the same World-step sites, so a mod's slot-4 number is charged and cooled on
   the server with the same two hooks as on the client (twins found). What is missing is a **refusal**: nothing on the server
   checks cost, cooldown or learnt rank before a mode is accepted. D2 ("the server wins") needs a new check at the dispatch
   site or at `creature_update_read`, because there is no twin to reuse (task 3.3).
2. Hits: the server already applies the hits its clients send (`receivedHitSite`), so today a client's damage is trusted.
   D2 needs the ability's hits computed on the server and the client's own hits from the mod dropped.
3. The skill tree needs no server work; the rank is already in the creature data.
