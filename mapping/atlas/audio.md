# Audio

Atlas page for the `audio` subsystem: the XAudio2 engine, sound effects, music and the volume options. Reference
pages: [docs/game/audio.md](../../docs/game/audio.md).

## Purpose

Cube.exe plays its sounds through XAudio2 2.7 (created with `CoCreateInstance`, so there is no XAudio2 import). The
sound effects are `.wav` files, read from the data2.db archive or from a file, and played by a sound id; the music is
streamed by `cube::Music`. The sound and music volumes come from options.cfg and the options screen
([ui](ui.md)). Server.exe has no audio.

## Key classes

- [`cube::Audio`](../../docs/game/types/cube_Audio.md) (verified as a class, RTTI): the abstract base, with no
  vtable of its own in the binary.
- [`cube::XAudio2Engine`](../../docs/game/types/cube_XAudio2Engine.md) (verified as a class, RTTI): derives from
  `cube::Audio`. Nine vtable slots: slot 7 is `cube::XAudio2Engine::loadSound()`, slot 8 the deleting destructor,
  slot 5 (0x6222D0) takes a volume (`WinMain()` passes soundVolume × 0.01). The XAudio2 object is at +4, the
  mastering voice at +8, and its own `cube::Database` (data2.db) inside. One instance, a local of `WinMain()`.
- [`cube::XAudio2Engine::Sound`](../../docs/game/types/cube_XAudio2Engine_Sound.md) (verified as a class, RTTI): one
  loaded sound, built by `loadSound()`.
- [`cube::Music`](../../docs/game/types/cube_Music.md) (verified as a class, RTTI): about 1.9 MB (a stream buffer?),
  created by `cube::XAudio2Engine::init()`.
- [`cube::Speech`](../../docs/game/types/cube_Speech.md) (verified as a class, RTTI; in both builds): put under
  `audio` by the survey, but more likely the text of speech bubbles (`cube::SpeechWidget`, [ui](ui.md)). Not read
  yet.

## Entry points

All candidate, Cube.exe:

- `cube::XAudio2Engine::XAudio2Engine()` (0x6215F0) and `cube::XAudio2Engine::~XAudio2Engine()` (0x621770): both
  called by `WinMain()`; the constructor opens data2.db.
- `cube::XAudio2Engine::init()` (0x621E00): `CoInitializeEx`, `CoCreateInstance`, IXAudio2::Initialize,
  `CreateMasteringVoice`, then `cube::Music::Music()` (0x6215B0). When it fails, `WinMain()` shows "Could not
  initialize XAudio2".
- `cube::XAudio2Engine::loadSound()` (0x622330, vtable slot 7): reads a sound from a file (`CreateFileW`) or from
  data2.db (`cube::Database::getBuffer()` and [asset_decode](../../docs/game/core.md)) into a
  `cube::XAudio2Engine::Sound`.
- [play_sound](../../docs/game/audio.md) (0x484820, 7 KB): a switch from a sound id to a `.wav` name (id 0 is
  hit.wav; then blade1.wav … the groans), with a distance computed by `sqrt` for the volume. Called by
  `cube::GameController::update()`, `cube::GameController::render()`, `cube::GameController::onMouseDown()`
  ([input](input.md)), the speech widget, and 0x4847F0 (a wrapper that passes volume 1.0).
  Read from the decompile on 2026-10-12 (add-skills-unleashed): `thiscall(GameController* this, int id, const int64 pos[3],
  float volume, float pitch)`, `ret 0x10`. `pos` is three 64 bit world units (a creature's `+0x10`, or the camera at
  GameController+0x140); the played volume is `(1 - distance / 100) * volume * (GameController+0x184) * 0.01`, so nothing
  plays beyond 100 blocks of the camera. Ids seen: 0 hit.wav, 1 blade1, 2 blade2, 5 hit1, 6 hit2, 7 punch1, 8 punch2,
  11 smash1, 15 swing. A mod calls it through the FFI with `cube::game_controller` (0x7691C8) as `this`; it was called that
  way by `skills_unleashed` (not heard yet). The unnamed `pitch` is passed 1.0 by every caller read.

## A sound of the mod's own (add-skills-unleashed 8.2f)

Probe [sound-file](../probes/sound-file.toml), replayed 2026-10-12 with the dev mod `sound_file_proto`. `play_sound` maps its ids to
file names and then makes two calls on the sound engine, `[GameController+0x80070C]` (a `cube::XAudio2Engine`): vtable slot 7
(+0x1C, thiscall) takes a pointer to a 24-byte `std::string` (16 bytes of buffer, size, reserve 15) and returns a handle, and
slot 6 (+0x18) plays it: `(handle, volume, pan, pitch, 0)`, where volume is the sound option (GameController+0x184, 0 to 100) times
a factor times 0.01. The name is looked up in data2.db, where a mod's `assets/sounds/*.wav` are served by file name
([assets](../../docs/assets.md)). A mod that makes the same two calls with its own name plays its own file:

- a name that is nowhere gave handle 0 (`su_nowhere.wav`), a vanilla one gave 4 or 5 (`hit.wav`), the mod's `su_bow_test.wav`
  (22,050 Hz, mono, 16 bit, 0.25 s) the next number and the same on three more calls: the engine keeps a loaded sound by name.
- The call must come from the game's thread (it was made from a mid hook in `cube::GameController::update`).
- Heard by nobody yet: a replay has no listener. The pan argument was 0.0 and the pitch 1.0, 1.25 and 1.5.

## Sounds queued by the World: the charge (add-skills-unleashed, 2026-10-17)

Read statically, not probed. `cube::World::update()` does not call [play_sound](../../docs/game/audio.md) itself: it pushes a
sound action (cuwo's: the position as floats, the id at +0xC, pitch +0x10, volume +0x14, 1 by default; 0x18 bytes) on the step
output's list at +8 with 0x5F35B0 ([world](world.md), the step output). `cube::GameController::update()` plays that list in a
loop at 0x4957EE, `play_sound(id, pos, [+0x14], [+0x10])`: the only caller read that passes a pitch other than 1.0.

- The held charge: the dispatch block 0x6144BC (the hold modes 24, 25, 27, 36, 59, 63 and 64, so a bow's held M2 too) queues
  sound 55 (charge2.wav) once in each 200 ms of the mode's time, with pitch `1 + 0.5 * charged_mp` ([combat](combat.md),
  "Charged mp"). That is the rising tone of a charge, which stops rising when the charge reaches the creature's mp. Mode 8's
  own block queues the same sound the same way (0x610C99).
- [cube::World::chargeSoundSite](../../docs/game/combat.md) (0x614555, candidate) is just before the push in 0x6144BC: ESI is
  the creature and the action is at EBP-0x758. `skills_unleashed` writes a volume of 0 there for the player who holds the
  Bow of Light, which plays its own charge sound (not heard yet).

## Data flow

1. **Start-up.** `WinMain()` reads options.cfg (0x4CE450: soundVolume, musicVolume), constructs the engine,
   calls `cube::XAudio2Engine::init()`, sets the volume through slot 5, and passes the engine to
   `cube::GameController::GameController()`.
2. **Playing.** Game code calls [play_sound](../../docs/game/audio.md) with a sound id and a position; it picks the
   `.wav` name, loads the sound through the engine (`loadSound()`, from data2.db) and plays it with a volume that
   falls with distance.
3. **Music.** `cube::Music` is created with the engine; how tracks are chosen and streamed is not read yet.
4. **Options.** The options screen (`cube::OptionsWidget`, "Sound FX Volume", "Music Volume") changes the volumes,
   and 0x4CEBB0 writes them back to options.cfg.
5. **Shut-down.** `WinMain()` calls `cube::XAudio2Engine::~XAudio2Engine()`.

## Client and server

- Audio is client-only: Server.exe has no `cube::XAudio2Engine`, no `cube::Music`, no ole32 audio set-up and no
  `.wav` names.
- Both builds have `cube::Speech`, which fits speech being game data (text) rather than sound.

## Hook points of interest

- [play_sound](../../docs/game/audio.md) (candidate): every sound effect goes through it; a mod can replace or mute
  a sound id, or play a vanilla sound.
- `cube::XAudio2Engine::loadSound()` (candidate): a mod could load its own `.wav` here.
- `cube::XAudio2Engine::init()` (candidate): the XAudio2 object and the mastering voice exist once it returns.

## Sound ids

Read on 2026-10-12 from the switch of [play_sound](../../docs/game/audio.md) (0x484820: `cmp eax, 0x64`, a jump table at 0x486360 with
101 cases, each loading one `.wav` name from data2.db). Read from the code, not heard, except 32 (probe `sound-ids`). Ids to names:

- 0 hit; 1 blade1; 2 blade2; 3 long-blade1; 4 long-blade2; 5 hit1; 6 hit2; 7 punch1; 8 punch2; 9 hit-arrow; 10 hit-arrow-critical;
  11 smash1; 12 slam-ground; 13 smash-hit2; 14 smash-jump; 15 swing; 16 shield-swing; 17 swing-slow; 18 swing-slow2; 19 arrow-destroy;
  20 blade1; 21 punch2; 22 salvo2; 23 sword-hit03; 24 block; 25 shield-slam; 26 roll; 27 destroy2; 28 cry; 29 levelup2;
  30 missioncomplete; 31 water-splash01; 32 step2; 33 step-water; 34 step-water2; 35 step-water3
- 36 channel2; 37 channel-hit; 38 fireball; 39 fire-hit; 40 magic02; 41 watersplash; 42 watersplash-hit; 43 lich-scream; 44 drink2;
  45 pickup; 46 disenchant2; 47 upgrade2; 48 swirl; 49 human-voice01; 50 human-voice02; 51 gate; 52 spike-trap; 53 fire-trap;
  54 lever; 55 charge2; 56 magic02; 57 drop; 58 drop-coin; 59 drop-item
- 60 to 80 the creatures' groans: 60 and 62 male, 61 and 63 female, then goblin 64 and 65, lizard 66 and 67, dwarf 68 and 69, orc 70
  and 71, undead 72 and 73, frogman 74 and 75, 76 monster, 77 troll, 78 mole, 79 slime, 80 zombie
- 81 Explosion; 82 punch2; 83 menu-open2; 84 menu-close2; 85 menu-select; 86 menu-tab; 87 menu-grab-item; 88 menu-drop-item; 89 craft;
  90 craft-proc; 91 absorb; 92 manashield; 93 bulwark; 94 bird1; 95 bird2; 96 bird3; 97 cricket1; 98 cricket2; 99 owl1; 100 owl2

A mod plays one through `combat.play_sound(id, x, y, z)` (`mods-gameplay/skills_unleashed/src/lib/combat.lua`); the Guardian's shout
uses 40. Ids 0 to 100 only: a new `.wav` key in a mod is not reachable by this switch. The ids 20, 21, 56 and 82 repeat a name.

## Open questions

- Whether the last argument of [play_sound](../../docs/game/audio.md) is the pitch as heard: the third is the volume (it scales
  the distance falloff at 0x484AB9), and the World's queued sounds pass their pitch as the fourth (above), but nobody has
  listened for it.
- Whether sounds are cached after the first `loadSound()`, or loaded on every play.
- The other engine slots (0, 1 to 4, 6): play, stop, 3D position, music control.
- How `cube::Music` picks and streams tracks, and where the music files are (no `.ogg` names in the strings; the
  libogg page check is in the binary, see `design.md`).
- What `cube::Speech` is, and why it is in Server.exe.

## Depth

reconnaissance, reviewed 2026-09-27
