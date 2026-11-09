---@meta kyubu
-- LuaLS type stubs for the Kyubu mod loader Lua API (API version 1.1).
-- Add this folder to "workspace.library" in your .luarc.json (see sdk/lua/.luarc.json).
-- Nothing here runs in the game: the real `kyubu` table is created by the loader for each mod.

---@alias KyubuSideName "client"|"server"
---@alias KyubuPhaseName "boot"|"preinit"|"register"|"post_register"|"game_ready"|"shutdown"
---@alias KyubuTrust "full"|"restricted"
---@alias KyubuPackable nil|boolean|number|string|table

---Event object passed to every handler. Fields beyond the common ones depend on the event
---(see docs/events.md); custom events carry the emitted Lua value in `value`.
---@class KyubuEvent
---@field name string event name, e.g. "frame.begin"
---@field side KyubuSideName side the handler runs on
---@field cancellable boolean
---@field network_thread boolean true when delivered on the game's socket thread
---@field raw lightuserdata pointer to the native payload struct (use with kyubu.ffi)
---@field value any value passed to kyubu.emit (custom events)
---@field frame? number frame.begin / frame.end / sim.tick: frame or tick counter
---@field dt? number frame.begin / sim.tick / sim.world_step: milliseconds since the previous one (the step's length)
---@field world? number sim.world_step: the cube::World's address
---@field step_out? number sim.world_step: the address of the step's output
---@field effects? number sim.world_step: the address of the step's set of changed zones
---@field device? lightuserdata render.*: IDirect3DDevice9*
---@field width? number render.*: back buffer width
---@field height? number render.*: back buffer height
---@field vk? number input.key: Windows virtual-key code
---@field down? boolean input.key / input.mouse_button: pressed (true) or released
---@field is_repeat? boolean input.key: auto-repeat
---@field codepoint? number input.char: UTF-16 code unit
---@field char? string input.char: the character as UTF-8
---@field button? number input.mouse_button: 0 left, 1 right, 2 middle
---@field x? number input.mouse_*: client-area x
---@field y? number input.mouse_*: client-area y
---@field dx? number input.mouse_move
---@field dy? number input.mouse_move
---@field delta? number input.mouse_wheel: wheel delta (120 per notch)
---@field db? string asset.request: database file name, e.g. "data1.db"
---@field key? string asset.request: asset key, e.g. "iron-sword1.cub"; config.changed: the option that changed
---@field mod? string config.changed: id of the mod whose option changed
---@field path? string asset.db_open: path of the database being opened
---@field socket? number net.*: socket handle
---@field data? string net.recv_raw / net.send_raw: the bytes
---@field address? string net.connect / net.disconnect: peer address
---@field port? number net.connect / net.disconnect: peer port
---@field world? string lifecycle.world_enter / world_exit: world name
local KyubuEvent = {}

---Cancels the event (only has an effect when `cancellable` is true).
function KyubuEvent:cancel() end

---net.send_raw only: replaces the outgoing bytes (may shrink, not grow; "" drops the send).
---@param bytes string
function KyubuEvent:set_data(bytes) end

---CPU registers at a mid-function hook site (an FFI `KyubuCpuContext*`). Writes are applied on return.
---@class KyubuCpuContext
---@field eax integer
---@field ebx integer
---@field ecx integer
---@field edx integer
---@field esi integer
---@field edi integer
---@field ebp integer
---@field eflags integer
---@field esp integer read-only: ESP at the hook site
---@field eip integer read-only: address of the hook site
---@field xmm { f32: number[], u32: integer[] }[] XMM0-7 at the site, `xmm[0]` to `xmm[7]` (1.1): `ctx.xmm[2].f32[0]` is the low float of XMM2. Writes are applied on return

---@class KyubuHostInfo
---@field host "client"|"server" Cube.exe or Server.exe
---@field mode "local"|"remote_client"|"dedicated"
---@field build string e.g. "cube-0x51D2CE9C"
---@field image_base number
---@field dev_mode boolean
---@field safe_mode boolean

---@class KyubuModInfo
---@field id string
---@field version string
---@field state "loaded"|"disabled"|"failed"|"faulted"|"skipped"
---@field reason string why the mod is not loaded (empty when loaded)

---@class KyubuRegistryApi
local registry = {}
---Creates registry `name` (in the mod's namespace). PreInit only.
---@param name string e.g. "mymod:spells"
---@param schema_version? integer default 1
---@param persistent? boolean keep numbers stable per save (default true)
---@return true|nil ok
---@return string? err
function registry.create(name, schema_version, persistent) end
---Adds entry `id` (in the mod's namespace) with an optional payload. Register phase only.
---@param reg string
---@param id string
---@param payload? string
---@return true|nil ok
---@return string? err
function registry.add(reg, id, payload) end
---Replaces the payload of an existing entry (e.g. a vanilla model). Register phase only; the last mod in load order wins.
---@param reg string
---@param id string
---@param payload? string
---@return true|nil ok
---@return string? err
function registry.remap(reg, id, payload) end
---Numeric id of an entry (after the registries are frozen, i.e. from PostRegister on).
---@param reg string
---@param id string
---@return integer|nil number
---@return string? err
function registry.lookup(reg, id) end
---String id of a numeric entry.
---@param reg string
---@param number integer
---@return string|nil id
---@return string? err
function registry.name(reg, number) end
---Payload of an entry (the remapped payload if a remap applies).
---@param reg string
---@param id string
---@return string|nil payload
---@return string? err
function registry.payload(reg, id) end
---@param reg string
---@return integer|nil count
---@return string? err
function registry.count(reg) end
---Entry at `index` (0-based, in numeric order).
---@param reg string
---@param index integer
---@return string|nil id
---@return integer|string number_or_err
function registry.entry(reg, index) end
---SHA-256 (hex) of the frozen registry contents; equal hashes mean identical numbering.
---@param reg string
---@return string|nil hash
---@return string? err
function registry.hash(reg) end

---@class KyubuModelApi
local model = {}
---Registers a new model slot that loads `key` from data1.db (mod assets or providers). Register phase.
---@param id string e.g. "mymod:crate"
---@param key string e.g. "mymod-crate.cub"
---@return true|nil ok
---@return string? err
function model.add(id, key) end
---Makes a vanilla model slot load a different key. Register phase.
---@param id string e.g. "cubeworld:iron-sword1"
---@param key string
---@return true|nil ok
---@return string? err
function model.remap(id, key) end
---Model slot number of a registered or vanilla model (from PostRegister on).
---@param id string
---@return integer|nil slot
---@return string? err
function model.slot(id) end

---@class KyubuTextApi
local text = {}
---Registers a text entry; it is merged into dict_en.xml / dict_de.xml as <speech key="id">. Register phase.
---@param id string
---@param en string English text
---@param de? string German text (defaults to the English text)
---@return true|nil ok
---@return string? err
function text.add(id, en, de) end
---Looks up a text entry in `lang` ("en"/"de"), or in the game's current language when omitted.
---@param id string
---@param lang? "en"|"de"
---@return string|nil
function text.get(id, lang) end

---@class KyubuAssetsApi
local assets = {}
---Serves keys matching `glob` (`*` and `?`) from a function. Return the asset bytes, or nil to decline.
---@param db string "data1.db" (models), "data2.db" (sounds), "data3.db" (icons) or "data4.db" (text)
---@param glob string e.g. "proc-*.cub"
---@param fn fun(db: string, key: string): string|nil
---@return integer|nil id
---@return string? err
function assets.provider(db, glob, fn) end
---Reads an asset through all layers (mods, providers, vanilla).
---@param db string
---@param key string
---@return string|nil data
---@return string? layer e.g. "vanilla", "mod:gold_sword", "provider:mymod", "merged"
function assets.read(db, key) end
---Which layer serves (or would serve) a key.
---@param db string
---@param key string
---@return string layer
function assets.served_by(db, key) end

---@class KyubuChannelApi
---@field SERVER integer target for client -> server sends (0)
---@field LOCAL_CLIENT integer the in-process client in single-player (1)
---@field ALL_CLIENTS integer every connected client (0xFFFFFFFF)
local channel = {}
---Registers a handler for channel `name` ("modid:name") on this script's side (or `side`).
---Tables sent with kyubu.channel.send arrive unpacked.
---@param name string
---@param fn fun(connection: integer, data: any)
---@param side? "client"|"server"|"both"
---@return true|nil ok
---@return string? err
function channel.register(name, fn, side) end
---Sends a string or a packable table (max 64 KiB). Delivered on the receiver's next update, never synchronously.
---@param name string
---@param connection integer kyubu.channel.SERVER from the client; a connection id or ALL_CLIENTS from the server
---@param data string|table
---@return true|nil ok
---@return string? err "not allowed from this side" (wrong direction), "remote does not support channels" (joined client)
function channel.send(name, connection, data) end

---Immediate-mode UI (Dear ImGui). Only valid inside `render.overlay` handlers on the client.
---@class KyubuUiApi
local ui = {}
---@param title string
---@param open? boolean pass a boolean to get a close button
---@return boolean visible
---@return boolean open
function ui.begin(title, open) end
---Closes the window opened by ui.begin (call it even when begin returned false). Same as ui.finish().
ui["end"] = function() end
function ui.finish() end
---@param s string
function ui.text(s) end
---@param label string
---@return boolean clicked
function ui.button(label) end
---@param label string
---@param value boolean
---@return boolean changed
---@return boolean value
function ui.checkbox(label, value) end
---@param label string
---@param value number
---@param min number
---@param max number
---@return boolean changed
---@return number value
function ui.slider_float(label, value, min, max) end
---@param label string
---@param value string
---@param capacity? integer default 256
---@return boolean changed
---@return string value
function ui.input_text(label, value, capacity) end
function ui.same_line() end
function ui.separator() end
---True while the overlay wants the mouse or keyboard (e.g. a window is hovered or a text field is active).
---@return boolean
function ui.want_input() end

---The game API (docs/game-api.md). Full trust only for now: `kyubu.game` is nil at trust level "restricted".
---@class KyubuGameApi
local game = {}
---Whether a game-API primitive ("world", "handles", ...) is available on this build.
---@param primitive string
---@return boolean available
---@return string? why the reason when it is not (also for an unknown primitive)
function game.available(primitive) end
---The values of a verified mapped enum ("cube::EntityType", ...) with their names, in value order.
---@param name string the mapping's name of the enum
---@return {value: integer, name: string}[]? values nil when the enum is candidate or unknown
---@return string? why the reason
function game.enum(name) end
---A creature's state, in the game's units.
---@param creature KyubuCreature
---@return KyubuCreatureInfo? info nil when the call fails (for a gone creature: "the creature is gone")
---@return string? why the reason
function game.creature_get(creature) end
---The client side's own creature (its player). Client side only; nil and "no world is loaded" at the start menu.
---@return KyubuCreature? creature
---@return string? why the reason
function game.creature_player() end
---A creature of this side's world by its game id (`info.id`), as an object of this side: how the server side reaches
---the player (the client side sends `player:get().id`), for `owner` in creature_spawn.
---@param id integer the game's creature id
---@return KyubuCreature? creature nil when there is none
---@return string? why the reason
function game.creature_find(id) end
---Moves a creature this side owns to x, y, z (whole units, 65,536 to a block; x east, y north, z up) and stops it. On
---the client side that is the local player. Nothing checks the ground: the creature falls until the target zone loads.
---@param creature KyubuCreature
---@param x integer
---@param y integer
---@param z integer
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.creature_teleport(creature, x, y, z) end
---Turns a creature this side owns: sets its body yaw, in degrees counter-clockwise from north. The position is
---creature_teleport's.
---@param creature KyubuCreature
---@param yaw number degrees
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.creature_set_yaw(creature, yaw) end
---Sets a creature's flags (all of them at once; a key left out is false, so `{}` clears). A creature this side owns; `rooted` on
---both exes, the other two on Cube.exe only. They end with the
---creature or the world. A mod that applies a status (snare, freeze, launch, pull) must do nothing to an unstoppable creature.
---@param creature KyubuCreature
---@param flags KyubuCreatureFlags
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.creature_set_flags(creature, flags) end
---Reads a creature's movement and combat state. Any creature of this side's World.
---@param creature KyubuCreature
---@return KyubuCreatureState? state
---@return string? why the reason
function game.creature_state_get(creature) end
---Writes the keys `t` has (those of KyubuCreatureState; a vector's part left out keeps its value) to a creature this side owns.
---A rooted creature is still held at its next move step.
---@param creature KyubuCreature
---@param t KyubuCreatureState
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.creature_state_set(creature, t) end
---Server.exe: the projectile packets (packet 9) the joined clients sent since the last call, oldest first. `data` is the
---game's 0x70-byte cube::Projectile. The game still relays each one to the other clients.
---@return {connection: integer, data: string}[]? shots an empty list when none; nil when the call fails
---@return string? why the reason
function game.shots() end
---The same as `game.shots`.
---@return {connection: integer, data: string}[]? shots
---@return string? why
function game.shot_take() end
---Server.exe, inside a World step: copies a cube::Projectile (a 0x70-byte string) into the World's projectile list; the World's
---projectile step flies it, stops it on blocks and makes its hits. It is not sent to the clients.
---@param data string
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.projectile_push(data) end
---Defines a body: model pieces on a rig, posed by the Animator's clips (docs/game-api.md, "Creature bodies"). `rig` is the text of a `.rig.toml`,
---`pieces` maps each rig node that has a model to a model slot of the mod, `clips` maps a clip name to the text of an `.anim.toml`.
---@param t {name: string?, rig: string, pieces: table<string, integer>, clips: table<string, string>?}
---@return integer? body the body id; nil when it is refused
---@return string? why the reason
function game.body_define(t) end
---The names of a body's pieces and clips.
---@param body integer
---@return {pieces: string[], clips: string[]}? names
---@return string? why the reason
function game.body_name(body) end
---Attaches a body to a creature (scale: blocks a voxel); a nil body detaches. The creature's own parts are no longer drawn.
---@param creature KyubuCreature
---@param body integer?
---@param scale number?
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.creature_body_attach(creature, body, scale) end
---Plays a clip of the creature's body. `loop` loops it (else it holds its last pose); `fade` is the cross-fade in ms from the clip before.
---@param creature KyubuCreature
---@param clip string
---@param opts {start: number?, loop: boolean?, fade: number?}?
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.creature_body_play(creature, clip, opts) end
---Adds an offset to one pose value of the clip (part: root, body, head, hand_r, hand_l, tail; channel: pos or rot); with only the creature it clears them all.
---@param creature KyubuCreature
---@param part string?
---@param channel string?
---@param v number[]?
---@param group integer?
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.creature_body_offset(creature, part, channel, v, group) end
---Sets the weight of an offset group (0 to 7); every group starts at 1.
---@param creature KyubuCreature
---@param group integer
---@param weight number
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.creature_body_weight(creature, group, weight) end
---Reads the playing clip (clip index from 0, time and fade_left in ms) and the markers passed since the last read.
---@param creature KyubuCreature
---@return {clip: integer, time: number, fade_left: number, loop: boolean, markers: string[]}? state
---@return string? why the reason
function game.creature_body_read(creature) end
---The world point in blocks of a point in the voxels of a piece's model, for the playing pose (clip nil) or a clip alone at t ms.
---@param creature KyubuCreature
---@param piece string
---@param p number[]
---@param clip string?
---@param t number?
---@return number? x
---@return number? y
---@return number? z
function game.creature_body_point(creature, piece, p, clip, t) end
---Puts an item into a creature's bags with the game's own insert. Client side only (the local player's bags);
---Cube.exe only for now (single-player, or joined to any server: the bags are the client's own, and the server never sees them). Coins (`type` 12; `material` 10 copper, 12 silver, 11 gold) go to the coin counter, the rest
---into the first empty slot of a bag. An inventory window that is already open shows the item when it is opened again.
---A field of the wrong type or range raises.
---@param creature KyubuCreature
---@param item KyubuItemSpec
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.creature_give_item(creature, item) end
---Reads the ability a creature is in. `number` 0 means none (then `id` is ""); `id` is "" too for a number the registry has no name for.
---Either side.
---@param creature KyubuCreature
---@return KyubuAbilityInfo? info nil when the call fails
---@return string? why the reason
function game.ability_get(creature) end
---The milliseconds of cooldown a creature has left for an ability, 0 for none. `ability` is its number (0 to 255) or its id.
---A cooldown starts at the ability's first World step. Either side.
---@param creature KyubuCreature
---@param ability integer|string
---@return integer? ms nil when the call fails
---@return string? why the reason
function game.ability_cooldown(creature, ability) end
---Sets the milliseconds of cooldown a creature has left for an ability (1 to 255, or its id); 0 gives it back. The game's map is
---written when it has an entry (made at the ability's first World step). On Server.exe it also sets when the server admits a
---remote player's start again. Either side, for a creature the side owns.
---@param creature KyubuCreature
---@param ability integer|string
---@param ms integer 0 to 3600000
---@return boolean? ok
---@return string? why
function game.ability_cooldown_set(creature, ability, ms) end
---Starts a mod ability on a creature this side owns (on the client side, the local player), with the checks of a press of key 4
---except the key. A refusal (on cooldown, not enough mp, busy) fails with the reason. A vanilla ability is an invalid argument.
---@param creature KyubuCreature
---@param ability integer|string
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.ability_start(creature, ability) end
---Ends the creature's mod ability at the next safe point, with the reason "cancelled". Fails when it is in none.
---@param creature KyubuCreature
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.ability_end(creature) end
---The local player's rank in a mod ability: the one kept next to the character save, 1 when none was set. A client-side call.
---@param ability integer|string
---@return integer? rank the rank; nil when the call fails (no such mod ability, the ranks are not mapped for this build, no character yet)
---@return string? why the reason
function game.ability_rank_get(ability) end
---Sets the local player's rank in a mod ability, clamped to 0 up to the ability's maximum rank, and writes it to the ranks file.
---Slot 4 shows it within one frame (rank 0: the slot is empty).
---@param ability integer|string
---@param rank integer
---@return integer? stored the rank stored; nil when the call fails
---@return string? why the reason
function game.ability_rank_set(ability, rank) end
---Publishes a clip from its text as "<this mod's id>:<name>" with no file written (the Animator's call). It replaces a clip of that id
---that a file gives, until the game restarts. Needs full trust and dev mode, client side only.
---@param name string 1 to 64 characters of a-z, 0-9, '_', '-' and '.'
---@param text string the clip, as an `.anim.toml` file's text
---@return boolean? ok true; nil when the name or the text is bad (nothing changes)
---@return string? errors one per line, "<source>:<line>: <path>: <message>"
function game.animation_define_from_text(name, text) end
---The preview clock: draws the local player in the pose of a clip at a time, whatever its real mode, until
---kyubu.game.animation_preview_clear. No ability starts. Needs full trust and dev mode, client side only.
---@param clip string the clip's id ("demo:slam")
---@param t_ms number milliseconds, 0 or more
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.animation_preview(clip, t_ms) end
---Turns the preview clock off: the local player is drawn in the game's pose again.
---@return boolean? ok true; nil when the call fails
---@return string? why the reason
function game.animation_preview_clear() end
---The same call as `kyubu.animation.bind`, which every trust level has.
---@param ability_id string
---@param clip_id string
---@return boolean? ok
---@return string? why
function game.ability_animation_bind(ability_id, clip_id) end
---The same call as `kyubu.animation.bind_for`, which every trust level has.
---@param ability_id string
---@param creature_id integer
---@param clip_id string?
---@return boolean? ok
---@return string? why
function game.ability_animation_bind_for(ability_id, creature_id, clip_id) end
---The UI's scene tree and the render device are read only, client side only, and on the main thread (docs/game-api.md).
---What kyubu.game.ui_node_find, ui_node_get and ui_node_child return.
---@class KyubuUiNode
---@field node integer the node's address (a plasma::Node*): its identity, valid only while the node exists
---@field parent integer the parent's address; 0 for the root
---@field name string the node's name, UTF-8, at most 63 bytes; "" when it has none (names repeat)
---@field children integer how many children it has
---@field depth integer 0 for the root of the tree
---@field colour number[] the node's display colour, {r, g, b, a}; {1, 1, 1, 1} when it has none

---The `nth` (0 first) node named `name`, walking the scene tree from its root in drawing order.
---@param name string
---@param nth? integer default 0
---@return KyubuUiNode? node nil when the call fails (KY_E_NOT_FOUND: "no node has that name")
---@return string? why the reason
function game.ui_node_find(name, nth) end
---The node at an address that a node table, a child call or a hooked draw call gave; nil or no argument is the root. A
---node the tree no longer holds is nil and "gone": the address is accepted only when the tree holds it.
---@param node? integer
---@return KyubuUiNode? node
---@return string? why the reason
function game.ui_node_get(node) end
---The `index`th (0 first) child of a node, in drawing order (the first 256 of a node are reachable).
---@param node? integer nil is the root
---@param index integer
---@return KyubuUiNode? node
---@return string? why the reason
function game.ui_node_child(node, index) end
---The game's Direct3D 9 device (an IDirect3DDevice9*, as an address) and the size of the client area the engine draws into.
---@return {device: integer, width: integer, height: integer}? info
---@return string? why the reason
function game.render_get() end
---Starts one of the game's own particle effects (small cubes that fly out of `pos` and fall): the spray of a hit, a bomb, a splash. Put
---on the next World step's output, so it plays with the game's own. Local: other players do not see it. Client side only.
---@param p KyubuParticleSpec
---@return true? ok
---@return string? why the reason
function game.particle_spawn(p) end
---Starts a point light: it lights the models, the particles and the ground round `pos`, as the game's lamps and spell flashes do.
---Daylight hides it. At most 16 lights of mods are alive at once. Local: other players do not see it. Client side only.
---@param l KyubuLightSpec
---@return integer? id the light's id, for light_set and light_remove
---@return string? why the reason
function game.light_spawn(l) end
---Gives a light of this mod new fields (move it every frame to make it follow something); a life_ms above 0 counts again from now.
---@param id integer
---@param l KyubuLightSpec
---@return true? ok
---@return string? why the reason (no light of this mod has that id)
function game.light_set(id, l) end
---Ends a light of this mod now.
---@param id integer
---@return true? ok
---@return string? why the reason (no light of this mod has that id)
function game.light_remove(id) end
---The block at x, y, z (blocks; each is rounded down) of this side's World: its type byte (the kind is type % 32: 0 air,
---1 solid, 2 water, 3 wet ground, 4 and up ground kinds) and its colour. nil and "unknown" for a place in no loaded zone
---(never read it as air). Either side, on the game thread.
---@param x number
---@param y number
---@param z number
---@return integer? type
---@return integer|string? r_or_why red, or the reason ("unknown")
---@return integer? g
---@return integer? b
function game.world_block(x, y, z) end
---A hit through the game's own cube::World::hitCreature, as a melee hit: armor (resistance when `magic`), dodge, block, the hit flash,
---the damage number, kills, XP and threat. The stun and the knock are exactly the ones given. Only inside a World step (a
---`sim.world_step` handler, or later in the same step). On a joined client the hit is sent to the server, as the game's own.
---@param attacker KyubuCreature? nil for none
---@param target KyubuCreature
---@param hit {damage: number, stun_ms?: integer, knock?: number[], magic?: boolean} knock {x, y, z}, blocks per second
---@return boolean? landed true when it went through, false when void, dodged or blocked
---@return string? why the reason it was refused
function game.hit(attacker, target, hit) end
---Same as kyubu.game.hit.
---@param attacker KyubuCreature?
---@param target KyubuCreature
---@param hit {damage: number, stun_ms?: integer, knock?: number[], magic?: boolean}
---@return boolean? landed
---@return string? why
function game.creature_hit(attacker, target, hit) end

---@class KyubuAnimationApi
local animation = {}

---Binds an animation clip to an ability of this mod (`kyubu.ability.define` does the same through its `anim` key). The ability is drawn
---with the clip while it runs; binding again replaces the clip. Any trust level, either side: a server side only records the binding.
---@param ability_id string the mod's own ability ("demo:slam"), defined first
---@param clip_id string the clip's id ("demo:slam_clip")
---@return boolean? ok true; nil when the call fails (another mod's ability, a vanilla one, an ability or a clip that does not exist, or a build whose pose provider is not verified)
---@return string? why the reason
function animation.bind(ability_id, clip_id) end

---Binds a clip to an ability of this mod for one creature only, named by its game id (`creature:get().id`): while that creature runs
---the ability it is drawn with this clip and its steps' markers come from it; every other creature keeps the ability's clip. A Fire
---Mage's release clip after the charge is one. The binding lasts until it is dropped (clip nil), the creature goes or the world is left.
---@param ability_id string the mod's own ability, defined first
---@param creature_id integer the creature's game id
---@param clip_id string? the clip's id; nil drops the creature's binding
---@return boolean? ok true; nil when the call fails (as for `animation.bind`, or creature id 0)
---@return string? why the reason
function animation.bind_for(ability_id, creature_id, clip_id) end

---What kyubu.game.ability_get reads.
---@class KyubuAbilityInfo
---@field number integer the ability's number in kyubu:abilities (vanilla 1 to 110, a mod's from 111); 0 for none
---@field id string its id ("cubeworld:smash", "demo:burst")
---@field elapsed_ms integer the milliseconds since it started

---What kyubu.game.creature_give_item adds.
---What kyubu.game.light_spawn starts and light_set changes. `pos` and `radius` are required.
---@class KyubuLightSpec
---@field pos {x: integer, y: integer, z: integer}|integer[] world units: 65,536 per block; x east, y north, z up
---@field radius number blocks, above 0 to 64 (a lamp of rarity 4 reaches 42)
---@field color? {r: number, g: number, b: number}|number[] 0 to 16 each, the strength in it (the game's lamps 0.45 to 1.05); 1, 0.8, 0.6 by default
---@field life_ms? integer it ends by itself after this many ms; 0 (the default) lives until light_remove
---@field fade_ms? integer the last fade_ms of its life dim it to nothing; at most life_ms

---What kyubu.game.particle_spawn starts. Only `pos` is required.
---@class KyubuParticleSpec
---@field pos {x: integer, y: integer, z: integer}|integer[] world units: 65,536 per block; x east, y north, z up
---@field type? integer 0 solid, 1 bomb (the default), 2 no spread, 3 no acceleration, 4 no gravity
---@field count? integer how many particles, 1 to 1000 (the default is 8; the game's own sites make 3 to 20)
---@field scale? number the size of one particle, above 0 to 16 (the default is 0.25; the game's sites use 0.1 to 0.5)
---@field spreading? number how far they scatter, 0 to 64 (the default is 3; the sites use 3 and 8)
---@field accel? {x: number, y: number, z: number}|number[] the default is 0, 0, 10, the game's hit effect
---@field color? number[] {red, green, blue} or {red, green, blue, alpha}, each 0 to 1; the default is white
local _ParticleSpec = {}

---@class KyubuItemSpec
---@field type integer cube::ItemData.type, 1 to 25 (3 is a weapon, 12 coins)
---@field sub_type? integer the kind within the type; default 0
---@field level? integer 1 to 32767: the stored level (the game shows its power); the quantity for coins and stackable types; default 1
---@field rarity? integer 0 to 4; default 0
---@field material? integer 0 to 255; default 0
---@field modifier? integer 0 to 4294967295, the seed of the stat roll; default 0
---Builds a creature the way the game builds its own, and returns it. Server side only; single-player Cube.exe only
---for now. Inside a World step it enters the world after the step. A field of the wrong type raises.
---@param spawn KyubuSpawn
---@return KyubuCreature? creature nil when the call fails (a value it refuses: the reason names the field)
---@return string? why the reason
function game.creature_spawn(spawn) end

---What kyubu.game.creature_spawn builds.
---@class KyubuSpawn
---@field type integer cube::EntityType (kyubu.game.enum("cube::EntityType") lists them)
---@field hostile integer cube::HostileType other than 0: 1 hostile, 2 friendly, 3 named friendly (a name and a talk prompt), 5 pet (nameless), 6 target
---@field level integer 1 or more
---@field pos {x: number, y: number, z: number}|number[] world units (a fraction is dropped): 65,536 per block
---@field yaw? number facing, degrees counter-clockwise from north (+y); default 0
---@field behavior? "still"|"monster"|"friendly"|"companion"|"guard"|"default" "still" (default): stands still; "monster": the zone default of a monster, it fights, looks at the player and wanders; "friendly": looks at the player, wanders and fights the creatures hostile to it when they come close; "companion": follows `owner` and fights what it fights (hostile 3 is a named summon, 2 a nameless creature); "guard": fights and walks `path`; "default": what the game gives a creature nobody chose a behaviour for (the monster's, nothing for a target)
---@field owner? KyubuCreature "companion" only, required: the creature it follows (a creature of this side)
---@field path? {x: number, y: number, z: number}[]|number[][] "guard" only, required: 1 to 64 waypoints in world units, walked in turn

---A creature from the game API: one object per creature per side, so `==` and table keys work.
---`tostring(c)` gives "creature 0x<handle> (id <game id>)", or "(gone)".
---@class KyubuCreature
local KyubuCreature = {}
---Whether the creature still exists. Never raises.
---@return boolean
function KyubuCreature:valid() end
---Same as kyubu.game.creature_get(c).
---@return KyubuCreatureInfo? info nil when the call fails (for a gone creature: "the creature is gone")
---@return string? why the reason
function KyubuCreature:get() end
---Same as kyubu.game.creature_teleport(c, x, y, z).
---@param x integer
---@param y integer
---@param z integer
---@return boolean? ok
---@return string? why the reason
function KyubuCreature:teleport(x, y, z) end
---Same as kyubu.game.creature_set_yaw(c, yaw).
---@param yaw number degrees
---@return boolean? ok
---@return string? why the reason
function KyubuCreature:set_yaw(yaw) end
---Same as kyubu.game.creature_set_flags(c, flags).
---@param flags KyubuCreatureFlags
---@return boolean? ok
---@return string? why the reason
function KyubuCreature:set_flags(flags) end
---Same as kyubu.game.creature_state_get(c).
---@return KyubuCreatureState? state
---@return string? why the reason
function KyubuCreature:state() end
---Same as kyubu.game.creature_state_set(c, t).
---@param t KyubuCreatureState
---@return boolean? ok
---@return string? why the reason
function KyubuCreature:set_state(t) end
---Same as kyubu.game.creature_give_item(c, item).
---@param item KyubuItemSpec
---@return boolean? ok
---@return string? why the reason
function KyubuCreature:give_item(item) end
---Same as kyubu.game.ability_get(c).
---@return KyubuAbilityInfo? info
---@return string? why the reason
function KyubuCreature:ability() end
---Same as kyubu.game.ability_cooldown(c, ability).
---@param ability integer|string
---@return integer? ms
---@return string? why the reason
function KyubuCreature:cooldown(ability) end
---Same as kyubu.game.ability_cooldown_set(c, ability, ms).
---@param ability integer|string
---@param ms integer
---@return boolean? ok
---@return string? why the reason
function KyubuCreature:set_cooldown(ability, ms) end

---The flags of a creature (kyubu.game.creature_set_flags; kyubu.game.creature_get reads them).
---@class KyubuCreatureFlags
---@field unstoppable boolean? a hit gives no push, no stun and no slow (a mod that snares, freezes, launches or pulls must skip it)
---@field invulnerable boolean? a hit does no damage (no hp loss, no number)
---@field rooted boolean? does not walk, is not knocked back and does not act; its AI stays in place (both exes)

---A creature's movement and combat state (kyubu.game.creature_state_get, creature_state_set).
---@class KyubuCreatureState
---@field pos {x: integer, y: integer, z: integer}? world units, 65,536 per block
---@field velocity {x: number, y: number, z: number}? blocks per second
---@field knock {x: number, y: number, z: number}? the knockback, blocks per second
---@field aim {x: number, y: number, z: number}? where the aim ray meets the world, blocks from pos
---@field mode integer? the activity, 0 for none
---@field mode_ms integer? ms since the mode started
---@field stun_ms integer? the stun left; 0 or less is none
---@field slowed_ms integer? the slow left
---@field charge number? the charged mp

---What kyubu.game.creature_get reads.
---@class KyubuCreatureInfo
---@field id integer the game's id (negative for creatures made at run time)
---@field type integer cube::EntityType (kyubu.game.enum("cube::EntityType") names it)
---@field hostile integer cube::HostileType: 0 player, 1 hostile, 2 friendly, 3 named friendly, 5 pet, 6 target
---@field level integer
---@field hp number
---@field pos {x: integer, y: integer, z: integer} world units: 65,536 per block; x east, y north, z up
---@field yaw number facing, degrees counter-clockwise from north (+y)
---@field flags KyubuCreatureFlags what creature_set_flags set

---This mod's options (docs/config.md), declared in its config.toml; the player's values live in
---kyubu/config/<id>.toml. Available at every trust level, and only for the mod's own options.
---@class KyubuConfigApi
local config = {}
---Reads an option.
---@param key string
---@return boolean|number|string|nil value nil when the option is not declared
---@return string? why the reason when there is no value
function config.get(key) end
---Sets an option: validated like the player's file, then saved. Handlers of "config.changed" run on the next frame.
---@param key string
---@param value boolean|number|string
---@return boolean|nil ok
---@return string? why the reason when the value is not allowed or the option is not declared
function config.set(key, value) end

---The API table of the running mod. Each mod gets its own table per side.
---@class Kyubu
---@field id string this mod's id
---@field side KyubuSideName side this script runs on
---@field trust KyubuTrust
---@field registry KyubuRegistryApi
---@field model KyubuModelApi
---@field text KyubuTextApi
---@field assets KyubuAssetsApi
---@field channel KyubuChannelApi
---@field ui KyubuUiApi
---@field config KyubuConfigApi this mod's options
---@field animation KyubuAnimationApi the animation binding (any trust level)
---@field ffi? ffilib LuaJIT FFI (full trust only)
---@field game? KyubuGameApi the game API (full trust only)
kyubu = {}

-- ---------------------------------------------------------------- logging & info
---@param ... any
function kyubu.log(...) end
---@param ... any
function kyubu.debug(...) end
---@param ... any
function kyubu.info(...) end
---@param ... any
function kyubu.warn(...) end
---@param ... any
function kyubu.error(...) end
---@return KyubuPhaseName
function kyubu.phase() end
---@return KyubuHostInfo
function kyubu.host() end
---Side of the running callback.
---@return KyubuSideName|nil
function kyubu.current_side() end
---@return string
function kyubu.game_dir() end
---Per-mod writable folder (kyubu/data/<id>), created on first use.
---@return string
function kyubu.data_dir() end
---Reads a file from this mod's package (folder or zip).
---@param path string relative to the package root
---@return string|nil
function kyubu.read_file(path) end
---@param id string
---@return string|nil version the version of the loaded mod, nil if not loaded
function kyubu.is_mod_loaded(id) end
---The session manifest (JSON): build, mods, registry hashes.
---@return string
function kyubu.manifest() end
---@return KyubuModInfo[]
function kyubu.mods() end
---Callback time (ms, last frame) and number of hooks of a mod.
---@param id string
---@return number|nil ms_per_frame
---@return integer? hooks
function kyubu.stats(id) end
---@param id string
---@return string|nil state "loaded", "disabled", "failed", "faulted" or "skipped"
function kyubu.mod_state(id) end
---Names of all native API functions with the side they may be called from (for tooling and tests).
---@return table<string, "client"|"server"|"both">
function kyubu.api_functions() end

-- ---------------------------------------------------------------- scheduling
---Runs `fn` on the main thread at the start of the next frame (thread-safe).
---@param fn fun()
---@return true|nil ok
---@return string? err
function kyubu.run_on_main_thread(fn) end
---Registers a function to run when the script is unloaded (hot reload or shutdown).
---@param fn fun()
function kyubu.on_unload(fn) end

-- ---------------------------------------------------------------- events
---Subscribes to an event. Higher priority runs first; equal priorities run in load order.
---@param name string e.g. "frame.begin", "lifecycle.register", "mymod:custom"
---@param fn fun(ev: KyubuEvent)
---@param priority? integer default 0
---@param side? "client"|"server"|"both" default: this script's side
---@return integer|nil id
---@return string? err "unavailable" if this build has no hook for the event
function kyubu.on(name, fn, priority, side) end
---@param id integer
---@return true|nil ok
---@return string? err
function kyubu.off(id) end
---Emits a custom event in this mod's namespace ("<id>:name"). Handlers see `value` as ev.value.
---@param name string
---@param value? any
---@param cancellable? boolean
---@return boolean|nil cancelled
---@return string? err
function kyubu.emit(name, value, cancellable) end

-- ---------------------------------------------------------------- channels helpers
---Serialises nil/boolean/number/string/table values into a string (used by channel.send).
---@param v KyubuPackable
---@return string
function kyubu.pack(v) end
---Reverses kyubu.pack; returns non-packed strings unchanged.
---@param s string
---@return any
function kyubu.unpack(s) end

-- ---------------------------------------------------------------- interfaces
---Publishes a pointer (FFI cdata address as a number, or lightuserdata) under `name` for other mods.
---@param name string "<id>:name"
---@param version integer
---@param value number|lightuserdata
---@return true|nil ok
---@return string? err
function kyubu.publish(name, version, value) end
---@param name string
---@param min_version? integer
---@return lightuserdata|nil pointer
---@return integer|string version_or_err
function kyubu.interface(name, min_version) end

-- ---------------------------------------------------------------- console
---Registers a console command (client only). `print` writes a line to the console.
---@param name string "<id>:name"
---@param help string
---@param fn fun(args: string, print: fun(...))
---@return true|nil ok
---@return string? err
function kyubu.command(name, help, fn) end

-- ---------------------------------------------------------------- symbols & memory
---Resolves a symbol from the symbol maps (verified symbols only).
---@param name string e.g. "Database::get"
---@return number|nil address
---@return string? err
function kyubu.symbol(name) end
---@param rva integer
---@return number address
function kyubu.rva_to_addr(rva) end
---@param address number
---@return integer rva
function kyubu.addr_to_rva(address) end
---Guarded memory read; returns nil instead of crashing on a bad address.
---@param address number
---@param length integer
---@return string|nil bytes
---@return string? err
function kyubu.read(address, length) end

-- ---------------------------------------------------------------- full trust only
---Guarded memory write. Full trust only.
---@param address number
---@param bytes string
---@return true|nil ok
---@return string? err
function kyubu.write(address, bytes) end
---Patches bytes (restored on unhook/unload; overlapping patches of other mods are refused). Full trust only.
---@param address number
---@param bytes string
---@return integer|nil id
---@return string? err
function kyubu.patch(address, bytes) end
---Allocates with the game's operator new. Full trust only.
---@param size integer
---@return number|nil address
function kyubu.game_alloc(size) end
---Frees memory from kyubu.game_alloc / the game. Full trust only.
---@param address number
function kyubu.game_free(address) end
---Removes a hook or patch installed by this mod.
---@param id integer
---@return true|nil ok
---@return string? err
function kyubu.unhook(id) end
---Hooks a function. `ctype` is an FFI function pointer type including the calling convention,
---e.g. "bool(__thiscall*)(void*, void*, void**, uint32_t*)". The handler receives `next` first. Full trust only.
---@param target string|number symbol name or address
---@param ctype string
---@param fn fun(next: function, ...): any
---@param priority? integer
---@return integer|nil id
---@return string? err
function kyubu.hook(target, ctype, fn, priority) end
---Hooks an imported function in the host executable's import table. Full trust only.
---@param dll string e.g. "kernel32.dll"
---@param func string e.g. "GetTickCount"
---@param ctype string
---@param fn fun(next: function, ...): any
---@param priority? integer
---@return integer|nil id
---@return string? err
function kyubu.hook_import(dll, func, ctype, fn, priority) end
---Hooks a virtual method (slot `index` of the object's vtable). Full trust only.
---@param object number|lightuserdata
---@param index integer
---@param ctype string
---@param fn fun(next: function, ...): any
---@param priority? integer
---@return integer|nil id
---@return string? err
function kyubu.hook_vtable(object, index, ctype, fn, priority) end
---Runs `fn` with the CPU registers at an instruction address. Full trust only.
---@param target string|number symbol name or address
---@param fn fun(ctx: KyubuCpuContext)
---@return integer|nil id
---@return string? err
function kyubu.hook_mid(target, fn) end
