/*
 * kyubu_api.h - Kyubu mod loader public C ABI, version 1.2
 *
 * Rules of the ABI (see openspec native-mod-api):
 *  - Plain C, 32-bit x86, cdecl for every function pointer in the table.
 *  - Within major version 1 the KyubuApi table only grows by appending members.
 *    Always check api->size before using a member added after 1.0. Only added
 *    after 1.1: the members of the mod-management section, API 1.2. *  - No C++ types and no loader-owned heap memory cross the boundary. Strings are
 *    UTF-8 and NUL terminated; output buffers are owned by the caller.
 *  - Every function returns a KyubuResult (0 = success, negative = error) unless noted.
 */
#ifndef KYUBU_API_H
#define KYUBU_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KYUBU_API_VERSION_MAJOR 1
#define KYUBU_API_VERSION_MINOR 4
#define KYUBU_API_VERSION ((KYUBU_API_VERSION_MAJOR << 16) | KYUBU_API_VERSION_MINOR)

#if defined(_MSC_VER)
#define KYUBU_CALL __cdecl
#define KYUBU_EXPORT __declspec(dllexport)
#else
#define KYUBU_CALL __attribute__((__cdecl__))
#define KYUBU_EXPORT __attribute__((__dllexport__))
#endif

/* ------------------------------------------------------------------ results */
typedef int32_t KyubuResult;
enum {
    KY_OK = 0,
    KY_E_INVALID_ARG = -1,   /* bad argument (null pointer, malformed name, ...) */
    KY_E_PHASE = -2,         /* call not allowed in the current lifecycle phase */
    KY_E_SIDE = -3,          /* call not allowed from the current logical side */
    KY_E_NOT_FOUND = -4,     /* named thing does not exist */
    KY_E_UNAVAILABLE = -5,   /* feature/symbol not available on this game build */
    KY_E_CONFLICT = -6,      /* overlaps something owned by another mod */
    KY_E_UNSAFE = -7,        /* refused because it cannot be done safely */
    KY_E_ACCESS = -8,        /* invalid memory address */
    KY_E_NAMESPACE = -9,     /* identifier outside the caller's namespace */
    KY_E_DUPLICATE = -10,    /* identifier already registered */
    KY_E_TOO_LARGE = -11,    /* payload exceeds a limit */
    KY_E_VERSION = -12,      /* version requirement not met */
    KY_E_INTERNAL = -13,     /* loader bug; see log */
    KY_E_BUFFER = -14,       /* output buffer too small; required size returned */
    KY_E_NO_REMOTE = -15,    /* remote does not support channels */
    KY_E_FAULTED = -16,      /* mod has been disabled after a fault */
    KY_E_DEV_ONLY = -17,     /* requires dev mode */
    KY_E_TRUST = -18,        /* not permitted at this trust level */
    KY_E_GONE = -19,         /* 1.1: the creature a handle named no longer exists */
    KY_E_REFUSED = -20,      /* 1.1: the game state refuses the request (an ability on cooldown, not enough mp, a busy creature) */
    KY_E_DENIED = -21        /* 1.2: the caller does not have the permission (the mod-management members: full trust and manage_mods = true in mod.toml) */
};

/* ------------------------------------------------------------------ basic types */
typedef struct KyubuMod_* KyubuModHandle;   /* identifies the calling mod */
typedef uint32_t KyubuId;                   /* handle for hooks, subscriptions, ... (0 = none) */
typedef uint64_t KyubuCreature;             /* 1.1, game API: a creature handle the loader issued (0 = none) */

typedef enum KyubuSide {
    KY_SIDE_NONE = 0,
    KY_SIDE_CLIENT = 1,
    KY_SIDE_SERVER = 2,
    KY_SIDE_BOTH = 3            /* only valid where documented (subscriptions, channels) */
} KyubuSide;

typedef enum KyubuPhase {
    KY_PHASE_BOOT = 0,
    KY_PHASE_PREINIT = 1,
    KY_PHASE_REGISTER = 2,
    KY_PHASE_POSTREGISTER = 3,
    KY_PHASE_GAMEREADY = 4,
    KY_PHASE_SHUTDOWN = 5
} KyubuPhase;

typedef enum KyubuHost {
    KY_HOST_UNKNOWN = 0,
    KY_HOST_CLIENT = 1,         /* Cube.exe */
    KY_HOST_SERVER = 2          /* Server.exe */
} KyubuHost;

typedef enum KyubuHostMode {
    KY_MODE_MENU_OR_SINGLEPLAYER = 0,   /* Cube.exe: both logical sides */
    KY_MODE_REMOTE_CLIENT = 1,          /* Cube.exe connected to a server: client side only */
    KY_MODE_DEDICATED = 2               /* Server.exe: server side only */
} KyubuHostMode;

typedef enum KyubuLogLevel {
    KY_LOG_DEBUG = 0, KY_LOG_INFO = 1, KY_LOG_WARN = 2, KY_LOG_ERROR = 3
} KyubuLogLevel;

typedef struct KyubuHostInfo {
    uint32_t size;              /* = sizeof(KyubuHostInfo) */
    KyubuHost host;
    KyubuHostMode mode;
    char build_id[32];          /* e.g. "cube-0x51D2CE9C" */
    uintptr_t image_base;       /* runtime base of the host executable */
    uint32_t image_size;
    int dev_mode;
    int safe_mode;
} KyubuHostInfo;

/* ------------------------------------------------------------------ events */
typedef struct KyubuEvent {
    uint32_t size;              /* = sizeof(KyubuEvent) */
    const char* name;           /* e.g. "frame.begin" */
    KyubuSide side;
    void* data;                 /* event-specific struct, see below; may be NULL */
    uint32_t data_size;
    int cancellable;
    int cancelled;              /* set via api->event_cancel */
    int network_thread;         /* 1 when delivered on the network thread */
} KyubuEvent;

typedef void (KYUBU_CALL* KyubuEventFn)(KyubuEvent* ev, void* user);

/* Event payloads (data member) */
typedef struct { uint32_t frame; double dt; } KyubuFrameData;                         /* frame.begin/end, sim.tick */
typedef struct { void* device; uint32_t width, height; } KyubuRenderData;             /* render.* (IDirect3DDevice9*) */
typedef struct { uint32_t vk; int down; int repeat; } KyubuKeyData;                   /* input.key */
typedef struct { uint32_t codepoint; } KyubuCharData;                                 /* input.char */
typedef struct { int button; int down; int x, y; } KyubuMouseButtonData;              /* input.mouse_button (0 L,1 R,2 M) */
typedef struct { int x, y, dx, dy; } KyubuMouseMoveData;                              /* input.mouse_move */
typedef struct { int delta; } KyubuMouseWheelData;                                    /* input.mouse_wheel */
typedef struct { const char* db; const char* key; } KyubuAssetRequestData;            /* asset.request */
typedef struct { const char* path; } KyubuDbOpenData;                                 /* asset.db_open */
typedef struct { uintptr_t socket; const uint8_t* data; uint32_t len;                  /* net.recv_raw / net.send_raw */
                 uint8_t* mutable_data; uint32_t* mutable_len; } KyubuNetData;         /* send_raw: may edit/drop (len=0) */
typedef struct { uintptr_t socket; const char* address; uint16_t port; } KyubuNetConnData; /* net.connect / net.disconnect */
typedef struct { const char* world; } KyubuWorldData;                                 /* lifecycle.world_enter/exit */
typedef struct { const char* mod; const char* key; } KyubuConfigChangedData;          /* 1.1: config.changed */

/* ------------------------------------------------------------------ hooks */
/* CPU context passed to mid-function hooks. Edits are applied on return. */
typedef struct KyubuCpuContext {
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;   /* pushad order */
    uint32_t eflags;
    uint32_t esp;               /* value of ESP at the hook site (read-only) */
    uint32_t eip;               /* address of the hook site (read-only) */
    /* 1.1: XMM0-XMM7 at the site (the game's float code is SSE), read as four floats or four 32-bit integers, with
       lane 0 first. Edits are applied on return, like the general registers. Appended after eip, so a mod built against
       1.0 never reads it; a mod that uses it requires kyubu = "^1.1". */
    union { float f32[4]; uint32_t u32[4]; } xmm[8];
} KyubuCpuContext;

typedef void (KYUBU_CALL* KyubuMidHookFn)(KyubuCpuContext* ctx, void* user);

/* ------------------------------------------------------------------ assets */
/* Provider callback: return KY_OK and fill *out (allocated with api->buffer_alloc) to serve
 * the key, or KY_E_NOT_FOUND to decline. */
typedef KyubuResult (KYUBU_CALL* KyubuAssetProviderFn)(const char* db, const char* key,
                                                       void** out, uint32_t* out_len, void* user);

/* ------------------------------------------------------------------ channels */
typedef void (KYUBU_CALL* KyubuChannelFn)(uint32_t connection, const uint8_t* data, uint32_t len, void* user);
#define KY_CONN_SERVER 0u            /* target for client->server sends */
#define KY_CONN_ALL_CLIENTS 0xFFFFFFFFu
#define KY_CONN_LOCAL_CLIENT 1u      /* the in-process client in single-player */
#define KY_CHANNEL_MAX_PAYLOAD 65536u

/* ------------------------------------------------------------------ console / misc */
typedef void (KYUBU_CALL* KyubuPrintFn)(void* ctx, const char* text);
typedef void (KYUBU_CALL* KyubuCommandFn)(const char* args, KyubuPrintFn print, void* print_ctx, void* user);
typedef void (KYUBU_CALL* KyubuTaskFn)(void* user);

/* ------------------------------------------------------------------ game API (docs/game-api.md) */
/* 1.1: a creature's verified state, as creature_get reads it. The caller sets `size`; the loader fills what fits,
   and later versions append fields. */
typedef struct KyubuCreatureInfo {
    uint32_t size;          /* sizeof(KyubuCreatureInfo), set by the caller; the loader fills what fits */
    uint32_t entity_type;   /* cube::EntityType */
    int64_t  id;            /* the game's id; signed, as the game compares ids */
    int64_t  pos[3];        /* world units: 65,536 per block; x east, y north, z up */
    float    body_yaw;      /* degrees counter-clockwise from north (+y) */
    float    hp;
    int32_t  level;
    uint8_t  hostile_type;  /* cube::HostileType */
    uint8_t  reserved[3];   /* 0 */
    uint32_t flags;         /* 1.3: KyubuCreatureFlags set by creature_set_flags (0 for none) */
    uint32_t reserved2;     /* 0 */
} KyubuCreatureInfo;

/* 1.3: the bits of a creature's `flags` (creature_set_flags, creature_get). */
typedef enum KyubuCreatureFlags {
    KY_CREATURE_UNSTOPPABLE = 1,  /* a hit gives no push, no velocity reset, no stun and no slow; threat and damage are untouched */
    KY_CREATURE_INVULNERABLE = 2, /* a hit does no damage (no hp loss, no damage number); the hit still counts */
    KY_CREATURE_ROOTED = 4        /* add-skills-unleashed-multiplayer 10.3: does not walk, is not knocked back, does not act; its AI stays */
} KyubuCreatureFlags;

/* add-skills-unleashed-multiplayer 10.3 (design D12): the movement and combat state of a creature, as creature_state_get reads it
   and creature_state_set writes the parts `fields` names. The caller sets `size`. */
typedef struct KyubuCreatureState {
    uint32_t size;          /* sizeof(KyubuCreatureState), set by the caller */
    uint32_t fields;        /* creature_state_set: the KyubuStateFields to write; creature_state_get sets all it read */
    int64_t  pos[3];        /* world units: 65,536 per block; x east, y north, z up */
    float    velocity[3];   /* blocks per second */
    float    knock[3];      /* the knockback (cube::CreatureData.extra_vel), blocks per second, added to the move and faded */
    uint32_t mode;          /* the activity (cube::CreatureData.current_mode), 0 for none */
    int32_t  mode_ms;       /* ms since the mode started (mode_start_time) */
    int32_t  stun_ms;       /* the stun left; at or below 0 is none */
    int32_t  slowed_ms;     /* the slow left */
    float    charge;        /* the charged mp (charged_mp) */
    float    aim[3];        /* where the aim ray meets the world, blocks from pos (ray_hit) */
    float    box[3];        /* 10.7, read only: the hitbox (cube::AppearanceData.scale), blocks, centred on pos; its feet are
                               pos z - box[2] / 2. Filled when `size` reaches it; creature_state_set never writes it */
} KyubuCreatureState;

typedef enum KyubuStateFields {
    KY_STATE_POS = 1, KY_STATE_VELOCITY = 2, KY_STATE_KNOCK = 4, KY_STATE_MODE = 8, KY_STATE_MODE_MS = 16,
    KY_STATE_STUN = 32, KY_STATE_SLOWED = 64, KY_STATE_CHARGE = 128, KY_STATE_AIM = 256,
    KY_STATE_ALL = 511
} KyubuStateFields;

/* add-skills-unleashed-multiplayer 10.9 (design D16): one projectile packet a joined client sent (packet 9), as Server.exe received
   it. `data` is the game's cube::Projectile, byte for byte: the shooter's id (int64 at 0), the start (three int64 world units at
   0x18), the velocity (three floats, blocks per second, at 0x3C), the damage the client computed (float at 0x48), the kind (int32
   at 0x60) and the rest, as the mapping's cube::Projectile names them. The caller sets `size`. */
#define KY_SHOT_DATA_SIZE 0x70u
typedef struct KyubuShot {
    uint32_t size;          /* sizeof(KyubuShot), set by the caller */
    uint32_t connection;    /* the connection it came on (the channel id: the player's creature id + 1); 0 when not known */
    uint8_t  data[KY_SHOT_DATA_SIZE];
} KyubuShot;

/* 1.4: creature bodies (docs/game-api.md, "Creature bodies"; capability creature-body of add-dragon-boss). A body is a rig of model
   pieces posed by clips; attached to a creature it is drawn in place of the creature's own parts. */
typedef uint32_t KyubuBody;   /* a body definition the loader issued (0 = none) */
typedef struct KyubuBodyPiece {
    const char* node;         /* the rig node that carries the model */
    uint32_t    model_slot;   /* the model slot it is drawn with (Mod::model_slot) */
} KyubuBodyPiece;
typedef struct KyubuBodyClip {
    const char* name;         /* 1 to 64 of a-z, 0-9, '_', '-', '.' */
    const char* text;         /* the text of an .anim.toml */
} KyubuBodyClip;
typedef struct KyubuBodyDesc {
    uint32_t size;            /* sizeof(KyubuBodyDesc) */
    const char* name;
    const char* rig_text;     /* the text of a .rig.toml (the Animator's format) */
    uint32_t piece_count;
    const KyubuBodyPiece* pieces;
    uint32_t clip_count;
    const KyubuBodyClip* clips;
} KyubuBodyDesc;
#define KY_BODY_LOOP 1u       /* creature_body_play: loop the clip; without it the clip holds its last pose */
typedef struct KyubuBodyState {
    uint32_t size;            /* sizeof(KyubuBodyState), set by the caller */
    int32_t  clip;            /* index of the playing clip, -1 for none */
    float    time_ms;         /* the playing clip's time */
    float    fade_left_ms;    /* what is left of the cross-fade, 0 for none */
    uint32_t flags;          /* KY_BODY_LOOP when looping */
    uint32_t reserved;       /* 0 */
} KyubuBodyState;
typedef struct KyubuBodyMarker {
    int32_t clip;             /* index of the clip the marker belongs to */
    char    name[32];         /* the marker's name */
} KyubuBodyMarker;

/* 1.1: a node of the UI's scene tree, as ui_node_find, ui_node_get and ui_node_child read it (docs/game-api.md). The caller
   sets `size`; the loader fills what fits, and later versions append fields. */
typedef struct KyubuUiNode {
    uint32_t  size;         /* sizeof(KyubuUiNode), set by the caller; the loader fills what fits */
    uint32_t  children;     /* the number of children */
    uint32_t  depth;        /* 0 for the root of the tree */
    uint32_t  reserved;     /* 0 */
    uintptr_t node;         /* the node's address (a plasma::Node*): its identity, to compare with the node a hooked draw call is
                               given. Valid only while the node exists: pass it back to ui_node_get to find out */
    uintptr_t parent;       /* the parent's address; 0 for the root */
    float     colour[4];    /* RGBA: the row of the node's display colour table that the game multiplies into every shape of the
                               node; 1, 1, 1, 1 when the node has none */
    char      name[64];     /* UTF-8, cut and terminated; "" for a node with no name (names repeat) */
} KyubuUiNode;

/* 1.1: what creature_spawn builds. The caller sets `size`; later versions append fields. */
#define KY_SPAWN_STILL 0u           /* KyubuSpawn.behavior: none, the creature stands still */
#define KY_SPAWN_MONSTER 1u         /* the zone default of a monster: it fights, looks at the player and wanders (the
                                       game's own combat, look-at-player and random-walk behaviours) */
#define KY_SPAWN_FRIENDLY 2u        /* it looks at the player, wanders and fights the creatures hostile to it when they
                                       come close (combat, look-at-player and random-walk; the hostile type says who) */
#define KY_SPAWN_COMPANION 3u       /* it follows `owner` and fights what its owner fights (the pet and summon shape);
                                       `owner` is required and the creature keeps its id. Hostile type 3 is a named
                                       summon, 2 a nameless creature; 5 (the game's pet) is removed by the game */
#define KY_SPAWN_GUARD 4u           /* it fights and walks the waypoints of `path` in turn; `path` is required */
#define KY_SPAWN_DEFAULT 5u         /* what the game gives a creature nobody chose a behaviour for: the zone default
                                       (KY_SPAWN_MONSTER), and nothing for a target (hostile type 6) */
#define KY_SPAWN_PATH_MAX 64u       /* the most waypoints a spawn takes */
typedef struct KyubuSpawn {
    uint32_t size;          /* sizeof(KyubuSpawn), set by the caller */
    uint32_t entity_type;   /* a cube::EntityType value (game_enum lists them) */
    uint8_t  hostile_type;  /* a cube::HostileType value other than 0 (a player): 1 hostile, 2 friendly, 3 named
                               friendly, 5 pet, 6 target */
    uint8_t  behavior;      /* one of the KY_SPAWN_ kinds above */
    uint16_t reserved;      /* 0 */
    int32_t  level;         /* 1 or more */
    int64_t  pos[3];        /* world units: 65,536 per block; x east, y north, z up */
    float    body_yaw;      /* facing, degrees counter-clockwise from north (+y) */
    uint32_t path_count;    /* KY_SPAWN_GUARD: the number of waypoints in `path`, 1 to KY_SPAWN_PATH_MAX; else ignored */
    KyubuCreature owner;    /* KY_SPAWN_COMPANION: a handle of the spawning side's creature; else ignored */
    const int64_t (*path)[3];   /* KY_SPAWN_GUARD: path_count waypoints, world units, x east, y north, z up */
} KyubuSpawn;

/* 1.1: an item for creature_give_item (the game's cube::ItemData without its upgrades, which are left empty). */
#define KY_ITEM_TYPE_MAX 0x19u        /* the highest cube::ItemData.type the API gives out */
#define KY_ITEM_TYPE_COINS 0x0Cu      /* coins: `material` 10 copper, 12 silver, 11 gold; `level` is the amount */
typedef struct KyubuItem {
    uint32_t size;        /* sizeof(KyubuItem), set by the caller */
    uint8_t  type;        /* cube::ItemData.type, 1 to KY_ITEM_TYPE_MAX (3 is a weapon) */
    uint8_t  sub_type;    /* cube::ItemData.sub_type: the kind within the type */
    uint8_t  rarity;      /* 0 to 4 */
    uint8_t  material;    /* cube::ItemData.material */
    uint32_t modifier;    /* cube::ItemData.modifier: the seed of the item's stat roll */
    int32_t  level;       /* 1 to 32767: the stored level (the game shows its power). For coins and the stackable types it is
                             the quantity, and the game stores level 1 */
} KyubuItem;

/* 1.1: one of the game's own particle effects, for particle_spawn: `count` small cubes (a spray, a puff, a splash) that fly out of `pos`
   and fall. It is the game's cube::ParticleData (mapping/types/world.toml); vanilla uses it for hits, bombs and a few spells. The caller
   sets `size`; later versions append fields. */
#define KY_PARTICLE_SOLID 0u            /* KyubuParticle.type: the types cuwo names; the game's own sites use 1 (bomb) and 2 */
#define KY_PARTICLE_BOMB 1u
#define KY_PARTICLE_NO_SPREAD 2u
#define KY_PARTICLE_NO_ACCELERATION 3u
#define KY_PARTICLE_NO_GRAVITY 4u
#define KY_PARTICLE_COUNT_MAX 1000u     /* the most particles one call may make (the game's own sites make 3 to 20) */
typedef struct KyubuParticle {
    uint32_t size;          /* sizeof(KyubuParticle), set by the caller */
    uint32_t type;          /* KY_PARTICLE_* */
    int64_t  pos[3];        /* where the effect starts, world units: 65,536 per block; x east, y north, z up */
    float    accel[3];      /* the acceleration the particles get, blocks per second squared: the game's hit effect uses 0, 0, 10 */
    float    color[4];      /* red, green, blue and alpha, each 0 to 1 */
    float    scale;         /* the size of one particle, blocks: the game's sites use 0.1 to 0.5. Above 0 and at most 16 */
    uint32_t count;         /* how many particles, 1 to KY_PARTICLE_COUNT_MAX */
    float    spreading;     /* how far the particles scatter, blocks per second: the sites use 3 and 8. 0 to 64 */
} KyubuParticle;

/* 1.1: a point light for light_spawn and light_set: it lights the models, the particles and the ground round `pos`, as the game's
   own lamps, projectiles and spell flashes do (it is a cube::Light of the frame's light vector, mapping/types/world.toml). Daylight
   hides it: it shows at night and in dark places. The caller sets `size`; later versions append fields. */
#define KY_LIGHT_MAX 16u                /* the lights alive at once, all mods together: the game lights a terrain region with 16 at most */
#define KY_LIGHT_RADIUS_MAX 64.0f       /* blocks */
#define KY_LIGHT_COLOR_MAX 16.0f
typedef struct KyubuLight {
    uint32_t size;          /* sizeof(KyubuLight), set by the caller */
    uint32_t flags;         /* 0 */
    int64_t  pos[3];        /* the light's place, world units: 65,536 per block; x east, y north, z up */
    float    radius;        /* blocks: how far it reaches. Above 0, at most KY_LIGHT_RADIUS_MAX. A lamp of rarity 4 reaches 42 */
    float    color[3];      /* red, green, blue, 0 to KY_LIGHT_COLOR_MAX: the strength is in it. The game's lamps give 0.45 to 1.05 */
    uint32_t life_ms;       /* it goes by itself this many ms after light_spawn (or the light_set that gave it); 0 lives until
                               light_remove, the world is left or its mod is unloaded */
    uint32_t fade_ms;       /* the last fade_ms of its life dim it to nothing; at most life_ms. 0 with life_ms 0 */
} KyubuLight;

/* add-skills-unleashed-multiplayer 10.1: one block of the terrain, for world_block: the game's cube::Block (mapping/types/world.toml).
   The kind is type & KY_BLOCK_KIND_MASK: 0 air, 1 solid, 2 water, 3 wet ground, 4 and up the ground kinds (grass, rock, sand, ...). */
#define KY_BLOCK_KIND_MASK 0x1Fu
#define KY_BLOCK_AIR 0u
#define KY_BLOCK_WATER 2u
#define KY_BLOCK_INTERIOR 0x40u        /* type bit: the block is inside a building */
typedef struct KyubuBlock {
    uint8_t color[3];       /* red, green, blue */
    uint8_t type;           /* the kind in bits 0 to 4 (KY_BLOCK_KIND_MASK); bit 5 destructible, bit 6 KY_BLOCK_INTERIOR, bit 7 under water */
} KyubuBlock;

/* add-skills-unleashed-multiplayer 10.2: the payload of sim.world_step, raised once per World step on both exes, after the hits and
   buffs the clients sent and before the zone items and the creatures, on each side whose World it is. The pointers are good until
   the step ends; creature_hit uses them, so a mod passes none. */
typedef struct KyubuWorldStepData {
    void* world;            /* the cube::World being stepped */
    int32_t dt;             /* the step's length in ms */
    void* step_out;         /* the step's output: its hit list carries the damage numbers to the players */
    void* effects;          /* the step's set of changed zones, the last pointer of cube::World::hitCreature and applyHit */
} KyubuWorldStepData;

/* add-skills-unleashed-multiplayer 10.2: one hit for creature_hit. The caller sets `size`. */
#define KY_HIT_MAGIC 1u              /* a spell: the target's resistance is taken off, not its armor */
#define KY_HIT_STUN_MAX 60000        /* ms */
typedef struct KyubuHit {
    uint32_t size;          /* sizeof(KyubuHit) */
    float damage;           /* before armor; negative heals */
    int32_t stun_ms;        /* exactly this stun, 0 to KY_HIT_STUN_MAX; 0 gives none */
    float knock[3];         /* the hit's direction, x east, y north, z up, blocks per second: the knockback the target takes */
    uint32_t flags;         /* KY_HIT_MAGIC */
} KyubuHit;

/* 1.1: an ability definition for ability_define. The caller sets `size`; every field below is read, and a zero is a
   value, not a default (duration_ms and max_rank of 0 are refused). The text ids are ids in kyubu:text; they are
   looked up when the HUD draws, and a missing one shows the string id. */
/* Why an ability ended: KyubuAbilityCtx.reason in on_end (-1 in on_start and on_step). */
#define KY_ABILITY_END_NONE (-1)
#define KY_ABILITY_END_DONE 0           /* its duration passed */
#define KY_ABILITY_END_CANCELLED 1      /* its code or the game API ended it, or a callback failed */
#define KY_ABILITY_END_STUNNED 2        /* the creature was stunned */
#define KY_ABILITY_END_DIED 3           /* the creature's hp reached 0 */
#define KY_ABILITY_END_INTERRUPTED 4    /* the game replaced it with another activity (also a stun, where the stun field is unknown) */
#define KY_ABILITY_END_GONE 5           /* the creature was removed or the world was left */
/* What a callback gets, valid during the call. `steps` counts the World steps of this activation, 1 on the first. */
typedef struct KyubuAbilityCtx {
    uint32_t size;          /* sizeof(KyubuAbilityCtx) of the loader */
    KyubuCreature creature; /* the creature that runs it, a handle of the side that calls */
    uint32_t number;        /* the ability's number in kyubu:abilities */
    const char* id;         /* its string id */
    uint32_t elapsed_ms;    /* the creature's mode time: about 0 on the first step */
    uint32_t dt_ms;         /* elapsed_ms less the last step's; on the first step, elapsed_ms */
    uint32_t steps;
    int32_t reason;         /* KY_ABILITY_END_*, in on_end only */
    uint32_t rank;          /* the character's rank in the ability (1 while ranks are not mapped); 1 for a creature that is not the local player */
    int32_t side;           /* KyubuSide the callback runs on */
    /* add-ability-animation 7.8, appended: the markers of the ability's bound animation clip crossed since the previous step, in the order
       crossed. Set in on_step only (NULL and 0 in on_start and on_end, and when the ability has no clip or the clip has no marker in
       the span). The first step covers the time from the ability's start, so a marker at t = 0 is told once. A mod reads these
       members only when `size` covers them (an older loader's struct ends at `side`). The names are valid during the call. */
    const char* const* markers;
    uint32_t marker_count;
    /* add-skills-unleashed-multiplayer 6.2, appended: 1 when the call is one of the watch callbacks (KyubuAbilityDef.watch_*), a
       bystander client's copy of another creature's run; else 0. Read it only when `size` covers it. */
    uint32_t watch;
    /* add-skills-unleashed-multiplayer 8.1, appended: how many times this run's mode time has gone back to about 0 as a restart
       (KyubuAbilityDef.restarts); 0 before the first. Read it only when `size` covers it. */
    uint32_t restarts;
} KyubuAbilityCtx;
typedef void (KYUBU_CALL* KyubuAbilityFn)(void* user, const KyubuAbilityCtx* ctx);
/* What ability_get reads. The caller sets `size`. `number` 0 is "in no ability" (then `id` is empty and `elapsed_ms` 0). */
#define KY_ABILITY_ID_MAX 100                 /* the longest ability id (namespace:name) and its terminator */
typedef struct KyubuAbilityInfo {
    uint32_t size;          /* sizeof(KyubuAbilityInfo), set by the caller */
    uint32_t number;        /* the ability's number in kyubu:abilities (vanilla 1 to 110, a mod's from 111), or 0 */
    uint32_t elapsed_ms;    /* the creature's mode time: the milliseconds since the ability started */
    char id[KY_ABILITY_ID_MAX];   /* its string id ("cubeworld:smash", "demo:burst"); empty when the registry has no name for the number */
} KyubuAbilityInfo;
#define KY_ABILITY_DURATION_MAX 60000u        /* ms */
#define KY_ABILITY_COOLDOWN_MAX 3600000u      /* ms */
#define KY_ABILITY_RANK_MAX 255u              /* the tree cannot reach it: it takes 255 skill points on one ability (was 20) */
#define KY_ABILITY_SLOT_NONE 0                /* KyubuAbilityDef.slot: no claim */
#define KY_ABILITY_SLOT_FOURTH 4              /* KyubuAbilityDef.slot: claims the fourth skill-bar slot */
#define KY_ABILITY_SPEC_ANY (-1)              /* KyubuAbilityDef.specialization: either one */
typedef struct KyubuAbilityDef {
    uint32_t size;                 /* sizeof(KyubuAbilityDef), set by the caller */
    const char* name;              /* text id of the display name, or NULL */
    const char* description;       /* text id of the description, or NULL */
    const char* icon;              /* id of the vanilla ability whose icon to show ("cubeworld:smash"), or NULL */
    uint32_t duration_ms;          /* 1 to KY_ABILITY_DURATION_MAX */
    float    mp_cost;              /* a fraction of the character's mp, 0 to 1 */
    uint32_t max_rank;             /* 1 to KY_ABILITY_RANK_MAX */
    const uint32_t* cooldown_ms;   /* max_rank + 1 values, for the ranks 0 to max_rank, each 0 to KY_ABILITY_COOLDOWN_MAX;
                                      NULL: all 0 */
    int32_t  slot;                 /* KY_ABILITY_SLOT_NONE or KY_ABILITY_SLOT_FOURTH */
    int32_t  class_type;           /* with the claim: cube::ClassType 1 to 4; else ignored */
    int32_t  specialization;       /* with the claim: KY_ABILITY_SPEC_ANY, 0 or 1; else ignored */
    KyubuAbilityFn on_start;       /* any of the three may be NULL */
    KyubuAbilityFn on_step;
    KyubuAbilityFn on_end;         /* the reason comes in the context */
    void* user;                    /* first argument of the callbacks */
    /* 1.1, appended (a caller built without it has a smaller `size` and no custom icon): the key of a PNG in the mod's
       assets/icons/ folder (data3.db), such as "burst.png", drawn as the ability's icon on the bar and in the skill tree.
       NULL or empty: none. `icon` is the fallback when the file cannot be loaded. */
    const char* icon_file;
    /* add-skills-unleashed-multiplayer 6.2, appended (a caller built without them has a smaller `size` and is not watched): the
       watch callbacks. On a Cube.exe joined to a server, another creature (a remote player, or a monster) whose mode is this
       ability runs them instead of on_start / on_step / on_end, with ctx.watch 1, so that the bystander sees the ability: they may
       draw, light and play sounds, and must not hit, push, write threat or set a mode (the server does that). The run starts
       when the mode becomes the number, its steps follow the mode time the server sends (a step back of up to 100 ms is held,
       with dt 0), and it ends only when the mode leaves the number (done, interrupted, stunned, died, cancelled when a watch
       callback failed, gone). No mp is taken and no cooldown starts; ctx.rank is the creature's skills[9] as sent, from 1 to
       max_rank. All NULL: the ability is not watched. Never called on Server.exe or in single-player. */
    KyubuAbilityFn watch_start;
    KyubuAbilityFn watch_step;
    KyubuAbilityFn watch_end;
    void* watch_user;              /* first argument of the watch callbacks */
    /* add-skills-unleashed-multiplayer 8.1, appended (a smaller `size` reads as 0): how many times one run may write its own mode
       time back to about 0 (a charge followed by a release clip played from its start). Where the run is another game's report (a
       watched creature, or a remote player on Server.exe), a step back of more than 100 ms to a mode time of 250 ms or less is then
       a restart, up to this many times: the run goes on (no new admission, no mp, no cooldown, no end), ctx.restarts counts it, the
       step has dt 0, and the markers are told again from the clip's start. Past the count, or with 0, it is a new start as before.
       The creature's own run (single-player, the local player) is unchanged: an `interrupted` end, then a start. 0 to 15. */
    uint32_t restarts;
} KyubuAbilityDef;

/* ------------------------------------------------------------------ mod management (1.2) */
/* ModState of a package in mods\ (KyubuModInfo.state). `failed` and `faulted` differ: failed is decided at boot
   (a bad manifest, an unmet requirement, a missing dependency), faulted is a mod that ran and crashed.
   `new` (5) is a package added to mods\ after a mods_rescan and never loaded; `missing` (6) is a package that was
   there at boot but whose folder or zip is gone since the rescan. */
typedef enum {
    KY_MOD_STATE_LOADED = 0,
    KY_MOD_STATE_DISABLED = 1,
    KY_MOD_STATE_FAILED = 2,
    KY_MOD_STATE_FAULTED = 3,
    KY_MOD_STATE_SKIPPED = 4,
    KY_MOD_STATE_NEW = 5,
    KY_MOD_STATE_MISSING = 6,
} KyubuModState;

#define KY_MOD_DEPS_MAX 16
typedef struct KyubuModDep {
    char id[33];                /* mod id */
    char req[48];               /* the mod.toml requirement text ("^1.2"), "" when there is no requirement */
    uint32_t optional;          /* declared `optional = true` */
} KyubuModDep;

/* What mods_get reads, one package of mods\ whatever its state. The caller sets `size` (a struct built without
   the dependency arrays ends at `path`, so mods_get leaves them alone; `dependency_count` and `dependent_count`
   are always written, the arrays only when `size` covers them). */
typedef struct KyubuModInfo {
    uint32_t size;              /* sizeof(KyubuModInfo), set by the caller */
    uint32_t state;             /* KyubuModState */
    uint32_t is_zip;            /* the package is a .zip file in mods\, not a folder */
    uint64_t package_size;      /* bytes of the zip file, or of every file of the folder */
    char id[33];                /* from mod.toml; of a failed package, the folder or zip name (its id may not parse) */
    char name[96];              /* from mod.toml; empty when the manifest does not parse */
    char version[40];           /* semver text ("1.2.0-beta"); empty when the manifest does not parse */
    char kyubu_req[48];         /* the `kyubu` requirement ("^1.2"); empty when the manifest does not parse */
    char trust[16];             /* "full" or "restricted" */
    char authors[160];          /* all authors joined with ", " */
    char description[256];
    char reason[192];           /* why it is not loaded ("disabled in kyubu/config.toml", a manifest error) */
    char path[260];             /* folder or zip, with backslashes */
    uint32_t dependency_count;  /* declared mods\ IDs it needs */
    uint32_t dependent_count;   /* installed packages that declare it */
    KyubuModDep dependencies[KY_MOD_DEPS_MAX];
    KyubuModDep dependents[KY_MOD_DEPS_MAX];
} KyubuModInfo;

/* What mods_option_get reads: one declared option of a package, plus the value in use. The texts clip when an
   entry is longer than its char array. */
typedef struct KyubuOptionInfo {
    char key[33];               /* the option's key */
    char type[8];               /* "bool", "int", "float" or "string" */
    char label[48];             /* settings-form text (at most 40 characters), empty when config.toml gives none */
    char group[64];             /* settings-form group, empty when config.toml gives none */
    char description[256];
    char def[96];               /* the declared default, as TOML text ("0.8", "true", "fire") */
    char value[96];             /* the value in use now, as TOML text (the player's file value or the default) */
    char min[96];               /* lower limit of an int or float option, empty when not declared */
    char max[96];               /* upper limit, empty when not declared */
    char choices[240];          /* for a string option, the choices joined with ", "; empty when none */
    uint32_t restart;           /* the option says it takes effect at the next start (D14) */
} KyubuOptionInfo;

/* What a queued change is (D2). The loader applies the queue at the next boot, before the mod scan: installs,
   then enables and disables, then deletes, then options, whatever order they were queued in. */
typedef enum {
    KY_MOD_OP_INSTALL = 0,      /* a staged package into mods\; a package of the same id goes to kyubu\backup\ */
    KY_MOD_OP_ENABLE = 1,
    KY_MOD_OP_DISABLE = 2,
    KY_MOD_OP_DELETE = 3,       /* the package into kyubu\backup\, optionally with its settings and data */
    KY_MOD_OP_CONFIG = 4,       /* one option of a mod, written at boot (a mod that stays disabled too) */
} KyubuModOpKind;

/* One queued change: mods_queue takes it, mods_queue_get reads it back. The fields a kind does not use are empty. */
typedef struct KyubuModOp {
    uint32_t kind;              /* KyubuModOpKind */
    char id[33];                /* every kind: the package's id from its mod.toml */
    char version[40];           /* install: the version the staged package must contain ("1.3.0") */
    char from[200];             /* install: the package to move, relative to the game folder with '/'
                                   ("kyubu/staging/emtweaks-1.3.0.zip"). Only kyubu/staging or kyubu/backup. */
    char key[33];               /* config: the option's key */
    char value[96];             /* config: the value as TOML text ("0.6", "true", "fire") */
    uint32_t keep_settings;     /* delete: 1 keeps kyubu\config\<id>.toml and kyubu\data\<id>\ (0 removes them) */
} KyubuModOp;

/* ------------------------------------------------------------------ the API table */
typedef struct KyubuApi {
    uint32_t size;              /* sizeof(KyubuApi) as built into the loader */
    uint32_t version;           /* KYUBU_API_VERSION of the loader */

    /* core */
    void        (KYUBU_CALL* log)(KyubuModHandle self, KyubuLogLevel level, const char* message);
    KyubuPhase  (KYUBU_CALL* phase)(void);
    KyubuSide   (KYUBU_CALL* current_side)(void);                  /* side of the running callback */
    KyubuResult (KYUBU_CALL* host_info)(KyubuHostInfo* out);
    KyubuResult (KYUBU_CALL* mod_id)(KyubuModHandle self, char* buf, uint32_t cap);
    KyubuResult (KYUBU_CALL* game_dir)(char* buf, uint32_t cap);
    KyubuResult (KYUBU_CALL* mod_data_dir)(KyubuModHandle self, char* buf, uint32_t cap);
    KyubuResult (KYUBU_CALL* read_package_file)(KyubuModHandle self, const char* path,
                                                void* buf, uint32_t cap, uint32_t* out_len);
    KyubuResult (KYUBU_CALL* is_mod_loaded)(const char* id, char* version_buf, uint32_t cap);
    void*       (KYUBU_CALL* buffer_alloc)(uint32_t size);          /* for provider outputs */
    void        (KYUBU_CALL* buffer_free)(void* p);
    KyubuResult (KYUBU_CALL* run_on_main_thread)(KyubuModHandle self, KyubuTaskFn fn, void* user);

    /* symbols & memory */
    KyubuResult (KYUBU_CALL* resolve_symbol)(const char* name, uintptr_t* out_addr);
    uintptr_t   (KYUBU_CALL* rva_to_addr)(uint32_t rva);
    uint32_t    (KYUBU_CALL* addr_to_rva)(uintptr_t addr);
    KyubuResult (KYUBU_CALL* mem_read)(uintptr_t addr, void* out, uint32_t len);
    KyubuResult (KYUBU_CALL* mem_write)(KyubuModHandle self, uintptr_t addr, const void* data, uint32_t len);
    void*       (KYUBU_CALL* game_alloc)(uint32_t size);            /* game's operator new */
    void        (KYUBU_CALL* game_free)(void* p);                   /* game's operator delete */

    /* hooks & patches */
    KyubuResult (KYUBU_CALL* hook_function)(KyubuModHandle self, uintptr_t target, void* handler,
                                            int32_t priority, void** next_slot, KyubuId* out_id);
    KyubuResult (KYUBU_CALL* hook_symbol)(KyubuModHandle self, const char* symbol, void* handler,
                                          int32_t priority, void** next_slot, KyubuId* out_id);
    KyubuResult (KYUBU_CALL* hook_mid)(KyubuModHandle self, uintptr_t address, KyubuMidHookFn fn,
                                       void* user, KyubuId* out_id);
    KyubuResult (KYUBU_CALL* hook_import)(KyubuModHandle self, const char* dll, const char* function,
                                          void* handler, int32_t priority, void** next_slot, KyubuId* out_id);
    KyubuResult (KYUBU_CALL* hook_vtable)(KyubuModHandle self, void* object, uint32_t index, void* handler,
                                          int32_t priority, void** next_slot, KyubuId* out_id);
    KyubuResult (KYUBU_CALL* patch_bytes)(KyubuModHandle self, uintptr_t address, const void* bytes,
                                          uint32_t len, KyubuId* out_id);
    KyubuResult (KYUBU_CALL* unhook)(KyubuModHandle self, KyubuId id);   /* hooks and patches */

    /* events */
    KyubuResult (KYUBU_CALL* subscribe)(KyubuModHandle self, const char* event, KyubuSide side,
                                        int32_t priority, KyubuEventFn fn, void* user, KyubuId* out_id);
    KyubuResult (KYUBU_CALL* unsubscribe)(KyubuModHandle self, KyubuId id);
    KyubuResult (KYUBU_CALL* emit)(KyubuModHandle self, const char* event, KyubuSide side,
                                   void* data, uint32_t data_size, int cancellable, int* out_cancelled);
    void        (KYUBU_CALL* event_cancel)(KyubuEvent* ev);

    /* assets */
    KyubuResult (KYUBU_CALL* asset_provider)(KyubuModHandle self, const char* db, const char* key_glob,
                                             KyubuAssetProviderFn fn, void* user, KyubuId* out_id);
    KyubuResult (KYUBU_CALL* asset_read)(const char* db, const char* key, void* buf, uint32_t cap,
                                         uint32_t* out_len);          /* resolved through all layers */
    KyubuResult (KYUBU_CALL* asset_served_by)(const char* db, const char* key, char* buf, uint32_t cap);

    /* registries */
    KyubuResult (KYUBU_CALL* registry_create)(KyubuModHandle self, const char* registry,
                                              uint32_t schema_version, int persistent);
    KyubuResult (KYUBU_CALL* registry_add)(KyubuModHandle self, const char* registry, const char* id,
                                           const void* payload, uint32_t payload_len, uint32_t* out_number);
    KyubuResult (KYUBU_CALL* registry_remap)(KyubuModHandle self, const char* registry, const char* id,
                                             const void* payload, uint32_t payload_len);
    KyubuResult (KYUBU_CALL* registry_lookup)(const char* registry, const char* id, uint32_t* out_number);
    KyubuResult (KYUBU_CALL* registry_name)(const char* registry, uint32_t number, char* buf, uint32_t cap);
    KyubuResult (KYUBU_CALL* registry_payload)(const char* registry, const char* id, void* buf,
                                               uint32_t cap, uint32_t* out_len);
    KyubuResult (KYUBU_CALL* registry_count)(const char* registry, uint32_t* out_count);
    KyubuResult (KYUBU_CALL* registry_entry)(const char* registry, uint32_t index, char* id_buf,
                                             uint32_t cap, uint32_t* out_number);
    KyubuResult (KYUBU_CALL* registry_hash)(const char* registry, char* hex_buf, uint32_t cap);

    /* channels & sessions */
    KyubuResult (KYUBU_CALL* channel_register)(KyubuModHandle self, const char* channel, KyubuSide side,
                                               KyubuChannelFn fn, void* user);
    KyubuResult (KYUBU_CALL* channel_send)(KyubuModHandle self, const char* channel, uint32_t connection,
                                           const void* data, uint32_t len);
    KyubuResult (KYUBU_CALL* session_manifest)(char* buf, uint32_t cap, uint32_t* out_len);

    /* mod-to-mod interfaces */
    KyubuResult (KYUBU_CALL* publish_interface)(KyubuModHandle self, const char* name, uint32_t version,
                                                void* iface);
    KyubuResult (KYUBU_CALL* get_interface)(const char* name, uint32_t min_version, void** out_iface,
                                            uint32_t* out_version);

    /* console & overlay UI (client side) */
    KyubuResult (KYUBU_CALL* register_command)(KyubuModHandle self, const char* name, const char* help,
                                               KyubuCommandFn fn, void* user);
    void*       (KYUBU_CALL* imgui_context)(void);   /* ImGuiContext* (Dear ImGui v1.91.5) or NULL */
    int         (KYUBU_CALL* ui_begin)(const char* title, int* open);
    void        (KYUBU_CALL* ui_end)(void);
    void        (KYUBU_CALL* ui_text)(const char* text);
    int         (KYUBU_CALL* ui_button)(const char* label);
    int         (KYUBU_CALL* ui_checkbox)(const char* label, int* value);
    int         (KYUBU_CALL* ui_slider_float)(const char* label, float* value, float min, float max);
    int         (KYUBU_CALL* ui_input_text)(const char* label, char* buf, uint32_t cap);
    void        (KYUBU_CALL* ui_same_line)(void);
    void        (KYUBU_CALL* ui_separator)(void);
    int         (KYUBU_CALL* ui_want_input)(void);   /* overlay currently captures input */

    /* diagnostics */
    KyubuResult (KYUBU_CALL* mod_stats)(const char* mod_id, double* out_ms_per_frame, uint32_t* out_hooks);
    KyubuResult (KYUBU_CALL* mod_state)(const char* mod_id, char* buf, uint32_t cap);

    /* ---- game API ---- (docs/game-api.md; appended from 1.1 on, check api->size first). A new member follows the
       checklist in docs/game-api.md ("Adding a function"): a GAME_API line in loader/src/game/entries.inc, a row in
       kSideRules, a Lua binding and stub, and an entry in the docs. Creatures are passed as KyubuCreature handles. */
    /* 1.1: whether a game-API primitive ("world", "handles", ...) is available on this build. KY_OK, or
       KY_E_UNAVAILABLE with the reason in `why` (may be NULL); KY_E_NOT_FOUND for an unknown primitive, with the
       reason in `why`; KY_E_INVALID_ARG when `primitive` is NULL. Any side, any thread, any phase. */
    KyubuResult (KYUBU_CALL* game_available)(const char* primitive, char* why, uint32_t cap);
    /* 1.1: the index-th value (from 0, in value order) of a verified mapped enum ("cube::EntityType", ...) and its
       name in `buf`. KY_OK; KY_E_NOT_FOUND past the last value, or for an unknown enum (the reason in `buf`);
       KY_E_UNAVAILABLE when the enum is candidate (the reason in `buf`); KY_E_BUFFER when the name does not fit;
       KY_E_INVALID_ARG when `name` is NULL. Any side, any thread, any phase. */
    KyubuResult (KYUBU_CALL* game_enum)(const char* name, uint32_t index, int64_t* value, char* buf, uint32_t cap);
    /* 1.1: a creature's verified state in `out` (set out->size first). Either side, for a handle it was given.
       KY_E_GONE when the creature no longer exists, KY_E_SIDE for a handle of the other side, KY_E_INVALID_ARG when
       `out` is NULL or out->size is below 8; and the guard's (docs/game-api.md, "Result codes"). */
    KyubuResult (KYUBU_CALL* creature_get)(KyubuModHandle self, KyubuCreature c, KyubuCreatureInfo* out);
    /* 1.1: the handle of the client side's own player (cube::World.local_player). Client side only (KY_E_SIDE from
       the server side); KY_E_PHASE before a world is loaded; and the guard's. */
    KyubuResult (KYUBU_CALL* creature_player)(KyubuModHandle self, KyubuCreature* out);
    /* 1.1: builds a creature the way the game builds its run-time ones; its (server-side) handle in `out`. Server
       side only (KY_E_SIDE from the client side); Cube.exe only for now (KY_E_UNAVAILABLE on Server.exe). Inside a
       World step the creature enters the map after the step. KY_E_INVALID_ARG when `spawn` or `out` is NULL, or a
       field is out of range, or `owner` or `path` is missing for a kind that needs it (the log names it; nothing is
       built); KY_E_GONE for an `owner` that no longer exists; KY_E_UNAVAILABLE for a behaviour kind whose primitive
       (`behavior`, and `behavior_companion` or `behavior_path` for those kinds) is; and the guard's. */
    KyubuResult (KYUBU_CALL* creature_spawn)(KyubuModHandle self, const KyubuSpawn* spawn, KyubuCreature* out);
    /* 1.1: a handle, on the calling side, of the creature with the game id `id` (creature_get's `id`), in `out`.
       Either side; it is how the server side reaches the player, which only the client side has a handle of (a
       `KyubuSpawn.owner`, for one). KY_E_NOT_FOUND when this side's World has no such creature (a creature spawned
       during the current World step enters the map after it); KY_E_INVALID_ARG when `out` is NULL; and the guard's. */
    KyubuResult (KYUBU_CALL* creature_find)(KyubuModHandle self, int64_t id, KyubuCreature* out);
    /* 1.1: moves a creature to `pos` (three int64_t: x east, y north, z up, in units, 65,536 to a block) and zeroes its
       velocity. Either side, for a creature the side owns (on the client side, the local player). Nothing checks the
       ground: the target zone may not be loaded, and the creature falls until it is. KY_E_INVALID_ARG when `pos` is
       NULL or a coordinate is beyond 2^47 units; KY_E_GONE, KY_E_SIDE (a creature the other side owns); and the
       guard's. */
    KyubuResult (KYUBU_CALL* creature_teleport)(KyubuModHandle self, KyubuCreature c, const int64_t* pos);
    /* 1.1: puts an item into a creature's bags with the game's own insert (set item->size first). Client side only (the
       local player's bags, in single-player and on a client joined to a server); Cube.exe only for now (KY_E_UNAVAILABLE on Server.exe). Coins go to the coin counter, the
       rest into the first empty slot of a bag; a bag that is full grows. An inventory window that is already open
       shows the item when it is opened again. KY_E_INVALID_ARG when `item` is NULL, item->size is below the
       struct's, or a field is out of range (the log names it; nothing is added); KY_E_GONE, KY_E_SIDE (a creature the
       other side owns); and the guard's. */
    KyubuResult (KYUBU_CALL* creature_give_item)(KyubuModHandle self, KyubuCreature c, const KyubuItem* item);

    /* ---- mod options ---- (docs/config.md; 1.1, check api->size first) */
    /* Read one of the calling mod's options (declared in its config.toml). KY_E_NOT_FOUND for an undeclared key,
       KY_E_INVALID_ARG when the option has another type, KY_E_BUFFER when a string does not fit. */
    KyubuResult (KYUBU_CALL* config_get_bool)(KyubuModHandle self, const char* key, int* out);
    KyubuResult (KYUBU_CALL* config_get_int)(KyubuModHandle self, const char* key, int64_t* out);
    KyubuResult (KYUBU_CALL* config_get_float)(KyubuModHandle self, const char* key, double* out);
    KyubuResult (KYUBU_CALL* config_get_string)(KyubuModHandle self, const char* key, char* buf, uint32_t cap);
    /* Sets one of the calling mod's options from text ("16", "true", "fire"), validated like the player's file,
       and saves it to kyubu/config/<id>.toml. KY_E_INVALID_ARG when the value is not allowed (nothing changes). */
    KyubuResult (KYUBU_CALL* config_set)(KyubuModHandle self, const char* key, const char* value);

    /* ---- abilities ---- (docs/abilities.md; 1.1, check api->size first). Any trust level, either side. */
    /* 1.1: defines the ability `id` (in the caller's namespace) in the registry kyubu:abilities; Register phase only. The
       loader gives it a number from 112 at the freeze. KY_E_INVALID_ARG when `id` or `def` is NULL, def->size is below
       the struct's, or a field is out of range (the log names it; nothing is added); KY_E_PHASE outside Register;
       KY_E_NAMESPACE for an id of another namespace; KY_E_DUPLICATE for an id already defined with other data.
       The same definition again (typically once per side) is KY_OK, and keeps the callbacks of that side. */
    KyubuResult (KYUBU_CALL* ability_define)(KyubuModHandle self, const char* id, const KyubuAbilityDef* def);
    /* 1.1: the number of the ability `id` ("cubeworld:smash", "demo:burst"). KY_E_NOT_FOUND for an id that is not in
       kyubu:abilities; KY_E_PHASE for a mod's ability before the freeze (the loader numbers them there; vanilla ones
       and kyubu:placeholder have their numbers at once). */
    KyubuResult (KYUBU_CALL* ability_number)(const char* id, uint32_t* out_number);
    /* 1.1: the id of the ability with `number`. KY_E_NOT_FOUND for a number nobody has (a mod's number before the
       freeze too), KY_E_BUFFER when the id does not fit (it is cut and terminated). */
    KyubuResult (KYUBU_CALL* ability_name)(uint32_t number, char* buf, uint32_t cap);
    /* 1.1: the ability at `index`, from 0, in number order: its id into id_buf and its number into out_number (either may
       be NULL). out_count (may be NULL) gets the number of abilities, whatever the index. KY_E_PHASE before the freeze,
       KY_E_NOT_FOUND at or past the end, KY_E_BUFFER when the id does not fit. */
    KyubuResult (KYUBU_CALL* ability_list)(uint32_t index, char* id_buf, uint32_t cap, uint32_t* out_number,
                                           uint32_t* out_count);
    /* 1.1: the ability that holds the fourth slot of a class (cube::ClassType 1 to 4) and specialization (0 or 1): its id
       into id_buf and its number into out_number (either may be NULL). It is the specialization's claim, else the
       class's KY_ABILITY_SPEC_ANY claim, else kyubu:placeholder unless [abilities] placeholder is off.
       KY_E_INVALID_ARG for other values, KY_E_PHASE before the freeze, KY_E_NOT_FOUND when nobody holds the slot,
       KY_E_BUFFER when the id does not fit. */
    KyubuResult (KYUBU_CALL* ability_slot_owner)(int32_t class_type, int32_t specialization, char* id_buf, uint32_t cap,
                                                 uint32_t* out_number);
    /* ---- game API ---- (abilities: docs/abilities.md; 1.1, appended after ability_slot_owner so that no earlier offset moves; the
       game API's rules are docs/game-api.md's, and the tools count these members with the first section's) */
    /* 1.1: the ability the creature `c` is in, in `out` (set out->size first): its number, id and the milliseconds since it
       started; number 0 and an empty id when it is in none. Either side, for a handle it was given. KY_E_INVALID_ARG when
       `out` is NULL or out->size is below 8; KY_E_GONE, KY_E_SIDE (a handle of the other side); and the guard's. */
    KyubuResult (KYUBU_CALL* ability_get)(KyubuModHandle self, KyubuCreature c, KyubuAbilityInfo* out);
    /* 1.1: the milliseconds of cooldown the creature `c` has left for the ability `number` (any number, vanilla or a mod's),
       in `out_ms`: 0 when it has none. A cooldown starts at the ability's first World step, so it reads 0 until then.
       Either side. KY_E_INVALID_ARG when `out_ms` is NULL or `number` is above 255; KY_E_GONE, KY_E_SIDE; and the guard's. */
    KyubuResult (KYUBU_CALL* ability_cooldown)(KyubuModHandle self, KyubuCreature c, uint32_t number, uint32_t* out_ms);
    /* 1.1: starts the **mod** ability `number` on the creature `c`, with the checks of a press of key 4 except the key: off
       cooldown, enough mp, an activity that may be interrupted, and the game's canUseAbility. The step runner then runs it:
       on_start, on_step, on_end, its mp taken once. Only for a creature the calling side owns (on the client side, the local
       player). KY_E_INVALID_ARG for a number that is not a defined mod ability (a vanilla one too; the creature is
       unchanged); KY_E_REFUSED when the game state refuses it (the reason is in the log and, in Lua, the second result:
       on cooldown, not enough mp, busy); KY_E_UNAVAILABLE where the fourth slot is not available (an entry that is not
       verified). On Server.exe the slot exists but the game's modeInterruptible and canUseAbility are not checked (no twin): only
       a creature already in a mode is refused. The server also admits a remote player's own start by itself (rank, cooldown, mp); KY_E_GONE, KY_E_SIDE (a creature the other side owns); and the guard's. */
    KyubuResult (KYUBU_CALL* ability_start)(KyubuModHandle self, KyubuCreature c, uint32_t number);
    /* 1.1: ends the creature's current mod ability at the next safe point after the World step, with the reason
       KY_ABILITY_END_CANCELLED (the ability's mode is set to 0 and on_end runs). KY_E_NOT_FOUND when the creature is in no
       mod ability, or only in a watched one (KyubuAbilityDef.watch_*); KY_E_UNAVAILABLE as for ability_start; KY_E_GONE, KY_E_SIDE; and the guard's. */
    KyubuResult (KYUBU_CALL* ability_end)(KyubuModHandle self, KyubuCreature c);
    /* 1.1: the local player's rank in the mod ability `number`, in `out_rank`: the one kept next to the character save, 1 when
       none was set, never above the ability's max_rank. Client side only (KY_E_SIDE from the server side). KY_E_INVALID_ARG
       for a number that is not a defined mod ability (a vanilla one too) or a NULL `out_rank`; KY_E_UNAVAILABLE when the
       ranks are not mapped for this build (the log and Lua's second result name the blocking entry; every rank then reads 1);
       KY_E_REFUSED when no character is selected yet; and the guard's. */
    KyubuResult (KYUBU_CALL* ability_rank_get)(KyubuModHandle self, uint32_t number, uint32_t* out_rank);
    /* 1.1: sets that rank, clamped to 0 up to the ability's max_rank, writes it to the ranks file at once and shows it on
       slot 4 within one frame (rank 0: the slot is empty). `out_rank` (may be NULL) gets the rank stored. Errors as for
       ability_rank_get; KY_E_REFUSED also when the file could not be written (the rank then holds until the game exits). */
    KyubuResult (KYUBU_CALL* ability_rank_set)(KyubuModHandle self, uint32_t number, uint32_t rank, uint32_t* out_rank);
    /* ---- game API ---- (the animation editor's calls: docs/game-api.md; 1.1, appended after ability_rank_set). Client side only (KY_E_SIDE
       from the server side), full trust only (KY_E_TRUST), dev mode only (KY_E_DEV_ONLY). They draw nothing themselves: the pose
       provider reads what they set. */
    /* 1.1: validates the clip `text` as the loader validates a clip file and publishes it as the clip "<caller's id>:<name>" in a
       new registry snapshot, with no file written. A clip of that id that a file gives is replaced for as long as the game runs
       (the file is read again if the game restarts). `name` is the part of the id after the colon: 1 to 64 characters of a-z,
       0-9, '_', '-' and '.'. A bad `text` or `name` is KY_E_INVALID_ARG, nothing changes, and `err` (may be NULL; `err_cap` bytes,
       cut and terminated) holds the errors, one per line, as the loader logs them: "<source>:<line>: <path>: <message>". */
    KyubuResult (KYUBU_CALL* animation_define_from_text)(KyubuModHandle self, const char* name, const char* text, char* err,
                                                         uint32_t err_cap);
    /* 1.1: the preview clock. The local player is drawn in the pose of the clip `clip_id` at `t_ms` (0 or more, finite) milliseconds,
       whatever its real mode, until animation_preview_clear. No ability starts. Calling it again moves the clock. KY_E_NOT_FOUND for a
       clip that is not in the registry, KY_E_INVALID_ARG for a NULL id or a `t_ms` below 0 or not finite. */
    KyubuResult (KYUBU_CALL* animation_preview)(KyubuModHandle self, const char* clip_id, double t_ms);
    /* 1.1: turns the preview clock off: the local player is drawn in the game's pose again. KY_OK when it was off already. */
    KyubuResult (KYUBU_CALL* animation_preview_clear)(KyubuModHandle self);
    /* ---- game API ---- (the animation binding: docs/game-api.md; 1.1, appended after animation_preview_clear). Any trust level, either side. */
    /* 1.1: binds the clip `clip_id` ("demo:slam") to the ability `ability_id`; the ability is drawn with the clip while it runs. One clip
       per ability: binding again replaces the first. The ability must be the caller's own and a mod's (define it first, with ability_define):
       KY_E_NAMESPACE for another mod's, KY_E_INVALID_ARG for a vanilla ability or a NULL argument, KY_E_NOT_FOUND for an ability or a clip that
       does not exist (the reason names the id). Works in any phase after the ability exists. A server side only records the binding, so that
       the registry can give the clip's duration and markers; it draws nothing. On the client side, KY_E_UNAVAILABLE when the pose provider's
       entries (game_available("animation_pose")) are not verified for the game build. */
    KyubuResult (KYUBU_CALL* ability_animation_bind)(KyubuModHandle self, const char* ability_id, const char* clip_id);
    /* ---- game API ---- (the UI scene tree and the render device: docs/game-api.md; 1.1, appended after ability_animation_bind). Client
       side only (KY_E_SIDE from the server side), any trust level, on the main thread (KY_E_UNSAFE from another), and KY_E_UNAVAILABLE
       when the `ui` primitive's entries are not verified for the game build. They read; none of them changes the game. */
    /* 1.1: the `nth` (0 first) node named `name` in a walk of the scene tree from its root, in the order the nodes are drawn in.
       Names repeat (every layer of the title logo has a C), so a caller that wants the one it means counts, or walks down from a
       parent. KY_E_NOT_FOUND when fewer than nth + 1 nodes have that name, KY_E_PHASE before the engine exists, KY_E_INVALID_ARG for a
       NULL name or a `size` of `out` below 8. */
    KyubuResult (KYUBU_CALL* ui_node_find)(KyubuModHandle self, const char* name, uint32_t nth, KyubuUiNode* out);
    /* 1.1: describes the node at `node`, an address that ui_node_find, ui_node_child or a draw hook gave (0: the root). The address is
       accepted only when the tree holds it (its parent links lead to the root and each node on the way is in its parent's child list):
       KY_E_GONE for one it does not, such as a node that was freed. Errors as for ui_node_find. */
    KyubuResult (KYUBU_CALL* ui_node_get)(KyubuModHandle self, uintptr_t node, KyubuUiNode* out);
    /* 1.1: the `index`th (0 first) child of `node` (0: the root), in drawing order. KY_E_NOT_FOUND past the last child (the first 256 of
       a node are reachable); otherwise as for ui_node_get. */
    KyubuResult (KYUBU_CALL* ui_node_child)(KyubuModHandle self, uintptr_t node, uint32_t index, KyubuUiNode* out);
    /* 1.1: the game's Direct3D 9 device (an IDirect3DDevice9*, not reference counted: do not Release it) and the size in pixels that
       the engine draws into, which is the window's client area. The same values as the `render.*` events carry, for a mod that is not
       inside one (a hooked draw call, a task on the main thread). KY_E_PHASE before the device exists, KY_E_INVALID_ARG for a NULL
       `out`. */
    KyubuResult (KYUBU_CALL* render_get)(KyubuModHandle self, KyubuRenderData* out);
    /* ---- game API ---- (the game's particles: docs/game-api.md; 1.1, appended after render_get). Client side only (KY_E_SIDE from the
       server side), any trust level, and KY_E_UNAVAILABLE when the `particles` primitive's entries are not verified for the game build. */
    /* 1.1: starts one of the game's own particle effects at `p->pos`. The effect is put on the output of the next World step, the way the
       game's hits and spells put theirs, so it plays with the game's own particles. It is local: a joined client's effect is not sent to the
       server or the other players. KY_E_INVALID_ARG for a NULL `p`, a `size` below the fields up to `spreading`, a number that is not
       finite, a `count`, `scale`, `spreading`, `type` or colour out of its range (the log names the field); KY_E_REFUSED when too many
       are waiting for the next step (512; a mod that makes more than that in one step asks again after it); and the game API's guard
       (docs/game-api.md, "Result codes"). */
    KyubuResult (KYUBU_CALL* particle_spawn)(KyubuModHandle self, const KyubuParticle* p);
    /* ---- game API ---- (lights: docs/game-api.md; 1.1, appended after particle_spawn). Client side only (KY_E_SIDE from the server
       side), any trust level, and KY_E_UNAVAILABLE when the `lights` primitive's entries are not verified for the game build. A light
       is local: it is drawn by the client that made it and sent to no one. The lights of a mod come before the game's own when the
       game picks which to use (the models take 10, each terrain region 16). */
    /* 1.1: starts a light (KyubuLight) and puts its id (1 or more) in `*out_id`. It shows from the next frame on. KY_E_INVALID_ARG
       for a NULL `l` or `out_id`, a `size` below the fields up to `fade_ms`, flags other than 0, a number that is not finite, or a
       radius, colour or fade out of its range (the log names the field); KY_E_REFUSED when KY_LIGHT_MAX lights are alive; and the
       game API's guard (docs/game-api.md, "Result codes"). */
    KyubuResult (KYUBU_CALL* light_spawn)(KyubuModHandle self, const KyubuLight* l, uint32_t* out_id);
    /* 1.1: gives the light `id` of this mod all the fields of `*l` (move it every frame to make it follow something); its life, when
       `life_ms` is not 0, counts again from now. KY_E_NOT_FOUND when no light of this mod has that id (it ended, or another mod's);
       the checks of light_spawn otherwise. */
    KyubuResult (KYUBU_CALL* light_set)(KyubuModHandle self, uint32_t id, const KyubuLight* l);
    /* 1.1: ends the light `id` of this mod now. KY_E_NOT_FOUND when no light of this mod has that id. */
    KyubuResult (KYUBU_CALL* light_remove)(KyubuModHandle self, uint32_t id);

    /* ---- mod management ---- (1.2, appended after light_remove; openspec/changes/add-mod-manager). Listing and
       hashing what was found in mods\ at boot. None of them load, unload or change anything, and all three work on
       both sides. They return KY_E_DENIED unless the caller has full trust and manage_mods = true in its mod.toml
       (the refusal is logged once per mod and function). */
    /* 1.2: how many packages mods_get describes. 0 when the caller is denied. */
    uint32_t (KYUBU_CALL* mods_count)(KyubuModHandle self);
    /* 1.2: fills `out` with the `index`th package of mods\, one per package found at boot, in discovery order.
       KY_E_NOT_FOUND past the last one, KY_E_INVALID_ARG for a NULL `out` or a `size` below the fields up to
       `path` (dependency_count and dependent_count are always written, the arrays when `size` covers them). */
    KyubuResult (KYUBU_CALL* mods_get)(KyubuModHandle self, uint32_t index, KyubuModInfo* out);
    /* 1.2: SHA-256 of the package of `id` into `out` (32 bytes). A zip hashes its whole file. A folder hashes a
       canonical listing: each file's relative path with `/`, its size and its own SHA-256, in path order, one line
       each (`"<path> <size> <hex>\n"`), the digest of that text. The same rule kcat uses, with a shared test
       vector. Computed on the first request and cached by path, size and modification time. KY_E_NOT_FOUND when no
       package in mods\ has that id, KY_E_INVALID_ARG for a NULL argument. */
    KyubuResult (KYUBU_CALL* mods_package_hash)(KyubuModHandle self, const char* id, uint8_t out[32]);
    /* 1.2: how many options `id` declares in its config.toml. 0 for a package with no config.toml, 0 when the
       caller is denied. The declaration of a package that is not loaded is parsed on request. */
    uint32_t (KYUBU_CALL* mods_option_count)(KyubuModHandle self, const char* id);
    /* 1.2: fills `out` with the `index`th declared option of `id`, in file order: the declaration and the value
       in use. KY_E_NOT_FOUND past the last option, when `id` names no package or declares no options;
       KY_E_INVALID_ARG for a NULL argument; KY_E_INTERNAL when the declaration itself does not parse. */
    KyubuResult (KYUBU_CALL* mods_option_get)(KyubuModHandle self, const char* id, uint32_t index, KyubuOptionInfo* out);
    /* 1.2: checks a value given as text by the same rules as the player's options file (parse_value_text, then
       the declaration's limits). Nothing is saved. KY_E_NOT_FOUND for an unknown `id` or `key`;
       KY_E_INVALID_ARG when the text is not a value of the option (the reason goes to the log, one line). */
    KyubuResult (KYUBU_CALL* mods_option_check)(KyubuModHandle self, const char* id, const char* key, const char* value);
    /* 1.2 (D14): sets an option of any package now. For a loaded mod exactly as its own config_set: validated,
       saved, `config.changed` on the next frame. For a package that is not loaded the options file is written
       (created with every default when it does not exist), and the mod stays as it was. */
    KyubuResult (KYUBU_CALL* mods_option_set)(KyubuModHandle self, const char* id, const char* key, const char* value);
    /* 1.2 (D15): rereads mods\: a package added since boot is listed by mods_get with the state `new` (or
       `failed`, with the error), a package whose folder or zip is gone with the state `missing`. Nothing is
       loaded, unloaded or changed. KY_E_DENIED unless the caller has full trust and manage_mods = true. */
    KyubuResult (KYUBU_CALL* mods_rescan)(KyubuModHandle self);
    /* 1.2 (D2): queues one change and rewrites kyubu\pending.toml in full; no other file changes. A config value
       is checked when queued (KY_E_INVALID_ARG, nothing changes); a `from` outside kyubu\staging or kyubu\backup,
       an id that is not a mod id or a version that is not semver are KY_E_INVALID_ARG too. KY_E_NOT_FOUND for a
       config op naming no installed package and no staged install in the queue; KY_E_TOO_LARGE past 64 ops. */
    KyubuResult (KYUBU_CALL* mods_queue)(KyubuModHandle self, const KyubuModOp* op);
    /* 1.2: empties the queue and deletes kyubu\pending.toml. KY_OK when there was nothing to clear. */
    KyubuResult (KYUBU_CALL* mods_queue_clear)(KyubuModHandle self);
    /* 1.2: how many changes are queued now. 0 when the caller is denied. */
    uint32_t (KYUBU_CALL* mods_queue_count)(KyubuModHandle self);
    /* 1.2: fills `out` with the `index`th queued change, in the order it was queued; KY_E_NOT_FOUND past the
       last one, KY_E_INVALID_ARG for a NULL `out`. */
    KyubuResult (KYUBU_CALL* mods_queue_get)(KyubuModHandle self, uint32_t index, KyubuModOp* out);
    /* 1.2 (D3): restarts the game: starts the same Cube.exe with the same arguments and working folder, then asks
       the game to quit by its own quit flag, so Shutdown runs as for a normal exit; the new process waits up to
       10 s for this one to end before it opens the log. Returns KY_OK when the restart is under way (the caller
       is about to be shut down; a second call changes nothing). KY_E_DENIED unless the caller has full trust
       and manage_mods = true; KY_E_SIDE on Server.exe; KY_E_PHASE before GameReady, in a joined game or with a
       world loaded (nothing is started); KY_E_UNAVAILABLE when the game controller is unknown; KY_E_INTERNAL
       when the process cannot be started. */
    KyubuResult (KYUBU_CALL* game_restart)(KyubuModHandle self);

    /* ---- game API ---- (creature flags: docs/game-api.md; 1.3, appended after game_restart; openspec/changes/add-dragon-boss, D3) */
    /* 1.3: sets a creature's KyubuCreatureFlags (all of them at once; 0 clears). The creature must be one the calling
       side owns. The flags are the loader's: they end with the
       creature or the world. A mod that applies a status (snare, freeze, launch, pull) must check KY_CREATURE_UNSTOPPABLE
       in creature_get and do nothing. KY_CREATURE_ROOTED (10.3) works on both exes; the other two on Cube.exe only, so on
       Server.exe a call with them is KY_E_UNAVAILABLE. KY_E_INVALID_ARG for a bit that is not defined (nothing changes); KY_E_GONE,
       KY_E_SIDE (a creature the other side owns); and the guard's. */
    KyubuResult (KYUBU_CALL* creature_set_flags)(KyubuModHandle self, KyubuCreature c, uint32_t flags);

    /* ---- game API ---- (creature bodies: docs/game-api.md; 1.4, appended after creature_set_flags; Cube.exe only for now) */
    /* Defines a body from a rig text, one model slot per rig node that has a model, and clips. KY_OK with the id in *out;
       KY_E_INVALID_ARG when refused, the reason in `why` (cut to why_cap; may be NULL): a rig the Animator's check refuses, a
       piece that has no model or whose slot is not loaded, a clip that fails the clip check or writes a pose value that no rig
       node reads. KY_E_UNAVAILABLE on a build without the primitive. Any side; no world is needed. */
    KyubuResult (KYUBU_CALL* body_define)(KyubuModHandle self, const KyubuBodyDesc* desc, KyubuBody* out, char* why, uint32_t why_cap);
    /* The name of piece (kind 0) or clip (kind 1) number `index` of a body in `buf`. KY_E_NOT_FOUND past the last. */
    KyubuResult (KYUBU_CALL* body_name)(KyubuModHandle self, KyubuBody body, uint32_t kind, uint32_t index, char* buf, uint32_t cap);
    /* Attaches a body to a creature with a scale (blocks per voxel), or detaches (body 0). While attached the creature's own parts
       are not drawn and the body's pieces are, in every pass; the body ends with the creature. KY_E_GONE, KY_E_NOT_FOUND (no such
       body), KY_E_INVALID_ARG (scale not above 0). */
    KyubuResult (KYUBU_CALL* creature_body_attach)(KyubuModHandle self, KyubuCreature c, KyubuBody body, float scale);
    /* Plays a clip by name from start_ms; KY_BODY_LOOP in `flags` loops it; fade_ms cross-fades from the clip before (0: at once).
       KY_E_NOT_FOUND for no body attached or no such clip. */
    KyubuResult (KYUBU_CALL* creature_body_play)(KyubuModHandle self, KyubuCreature c, const char* clip, float start_ms, uint32_t flags, float fade_ms);
    /* Sets the offset of a pose value (part 0 to 5: root, body, head, hand_r, hand_l, tail; channel 0 pos, 1 rot), added to the clip's,
       in weight group `group` (0 to 7). v NULL clears every offset. */
    KyubuResult (KYUBU_CALL* creature_body_offset)(KyubuModHandle self, KyubuCreature c, uint32_t part, uint32_t channel, const float* v, uint32_t group);
    /* Sets the weight (0 to 1) of an offset group; every group starts at 1. */
    KyubuResult (KYUBU_CALL* creature_body_weight)(KyubuModHandle self, KyubuCreature c, uint32_t group, float weight);
    /* Reads the playing clip into *state (may be NULL) and the markers passed since the last read into `markers` (cap entries; *n
       gets how many were returned; the rest stay for the next read). The clock runs with the creature's draw. */
    KyubuResult (KYUBU_CALL* creature_body_read)(KyubuModHandle self, KyubuCreature c, KyubuBodyState* state, KyubuBodyMarker* markers, uint32_t cap, uint32_t* n);
    /* The world position in blocks (x east, y north, z up) of the point p (voxels of the piece's model) on a piece, for the playing pose
       (clip NULL) or for a clip alone at t_ms, at the creature's place and yaw. Either side. */
    KyubuResult (KYUBU_CALL* creature_body_point)(KyubuModHandle self, KyubuCreature c, const char* piece, const float* p, const char* clip, float t_ms, float* out);
    /* 1.4, appended last: sets a creature's body yaw (degrees, counter-clockwise from north; the position is creature_teleport's). Either
       side, for a creature the side owns. KY_E_INVALID_ARG when yaw is not finite; KY_E_GONE, KY_E_SIDE; and the guard's. */
    KyubuResult (KYUBU_CALL* creature_set_yaw)(KyubuModHandle self, KyubuCreature c, float yaw);
    /* add-skills-unleashed-multiplayer 8.1, appended last: binds the clip `clip_id` to the ability `ability_id` for one creature,
       named by its game id (KyubuCreatureInfo.id): that creature is drawn with it, and its steps' markers come from it, while it runs
       the ability; every other creature keeps the ability's own clip (ability_animation_bind). NULL `clip_id` drops the creature's
       binding. A binding lasts until it is dropped, the creature is destroyed or the world is left. The checks and refusals are
       ability_animation_bind's; KY_E_INVALID_ARG for creature id 0. Any trust level, either side, any thread. */
    KyubuResult (KYUBU_CALL* ability_animation_bind_for)(KyubuModHandle self, const char* ability_id, uint64_t creature_id,
                                                          const char* clip_id);
    /* add-skills-unleashed-multiplayer 8.1, appended last: sets the milliseconds of cooldown the creature `c` has left for the
       ability `number` (1 to 255): 0 gives the cooldown back. The game's cooldown map is written when it has an entry for the
       number (the game makes one at the ability's first World step; none is made here). On Server.exe it also sets when the
       server admits a remote player's start of the ability again, so a server module that gives a cooldown back (a fizzled charge,
       as its client does) keeps the next start from being refused. Either side, for a creature the side owns (on Server.exe,
       every creature). KY_E_INVALID_ARG for number 0 or above 255 or ms above KY_ABILITY_COOLDOWN_MAX; KY_E_GONE, KY_E_SIDE;
       and the guard's. */
    KyubuResult (KYUBU_CALL* ability_cooldown_set)(KyubuModHandle self, KyubuCreature c, uint32_t number, uint32_t ms);
    /* add-skills-unleashed-multiplayer 10.1, appended last: the block at block x, y, z (x east, y north, z up; one block is 65,536
       units) of the calling side's World, in `out`: the stored block of a loaded zone, the game's solid block under a column, its
       water block above a column at z 0 and below, else air (cube::World::getBlock's rules). KY_E_NOT_FOUND when the point is in
       no loaded zone ("unknown": never read it as air), outside 0 to 2^24 - 1 in x or y included; KY_E_INVALID_ARG when `out` is
       NULL; and the guard's. Either side, on the game thread (a World step, frame.begin, sim.tick). */
    KyubuResult (KYUBU_CALL* world_block)(KyubuModHandle self, int32_t x, int32_t y, int32_t z, KyubuBlock* out);
    /* add-skills-unleashed-multiplayer 10.2, appended last: `attacker` (0 for none) hits `target` through the game's own
       cube::World::hitCreature, as a melee hit does: armor (resistance with KY_HIT_MAGIC), dodge, block, mana shield, the hit
       flash, the damage number on the step's output, kills, XP and threat. The stun and knock of `hit` replace the game's own.
       `*landed` (may be NULL) is 1 when the hit went through, 0 when it was void (a dead target), dodged or blocked. Only inside a
       World step (sim.world_step, or later in the same step): else KY_E_PHASE. On a joined client the hit is not applied there
       but sent to the server, like the game's own. KY_E_INVALID_ARG for a bad `hit`; KY_E_GONE; and the guard's. Either side. */
    KyubuResult (KYUBU_CALL* creature_hit)(KyubuModHandle self, KyubuCreature attacker, KyubuCreature target, const KyubuHit* hit,
                                           int* landed);
    /* add-skills-unleashed-multiplayer 10.3, appended last: reads a creature's KyubuCreatureState into `out` (out->size set by the
       caller; out->fields gets KY_STATE_ALL). Any creature of the calling side's World. KY_E_INVALID_ARG for a NULL or short `out`;
       KY_E_GONE; and the guard's. Either side, on the game thread. */
    KyubuResult (KYUBU_CALL* creature_state_get)(KyubuModHandle self, KyubuCreature c, KyubuCreatureState* out);
    /* add-skills-unleashed-multiplayer 10.3, appended last: writes the parts of `in` that in->fields names, for a creature the side
       owns. A rooted creature (KY_CREATURE_ROOTED) is still held where it is at the next move step, whatever is written here.
       KY_E_INVALID_ARG for a NULL or short `in`, a field bit that is not defined, or a value that is not finite; KY_E_GONE, KY_E_SIDE;
       and the guard's. Either side, on the game thread. */
    KyubuResult (KYUBU_CALL* creature_state_set)(KyubuModHandle self, KyubuCreature c, const KyubuCreatureState* in);
    /* add-skills-unleashed-multiplayer 10.9, appended last: moves up to `max` of the projectile packets the joined clients sent
       since the last take into `out` (each out[i].size set by the caller), oldest first; `*count` gets how many. The game's own
       packet is untouched: the server still relays it to the other clients. Packets are kept only from a mod's first call on, at
       most 256 (past that the oldest go). KY_E_INVALID_ARG for a NULL `count`, a NULL `out` with `max` above 0, or a short
       out[i]; and the guard's. Server.exe only, on the game thread. */
    KyubuResult (KYUBU_CALL* shot_take)(KyubuModHandle self, KyubuShot* out, uint32_t max, uint32_t* count);
    /* add-skills-unleashed-multiplayer 10.9, appended last: copies one cube::Projectile (`len` KY_SHOT_DATA_SIZE bytes) into the
       server World's projectile list, with the game's own push. The World's projectile step flies it from the next step on, stops
       it on blocks and makes its hits; it is not sent to the clients (send nothing twice: the client's own packet is relayed).
       Only inside a World step (sim.world_step, or later in the same step): else KY_E_PHASE. KY_E_INVALID_ARG for a NULL `data`
       or another `len`; and the guard's. Server.exe only. */
    KyubuResult (KYUBU_CALL* projectile_push)(KyubuModHandle self, const uint8_t* data, uint32_t len);
} KyubuApi;

/* ------------------------------------------------------------------ mod entry points */
typedef struct KyubuModQuery {
    uint32_t size;              /* set by loader: sizeof(KyubuModQuery) */
    uint32_t api_version;       /* set by mod: KYUBU_API_VERSION it was built against */
    char id[33];                /* set by mod: must equal mod.toml id */
    uint32_t sides;             /* set by mod: KyubuSide bit mask of sides it has code for */
} KyubuModQuery;

/* Exported by every native mod DLL:
 *   KYUBU_EXPORT int  KYUBU_CALL kyubu_mod_query(KyubuModQuery* q);
 *   KYUBU_EXPORT int  KYUBU_CALL kyubu_mod_init(const KyubuApi* api, KyubuModHandle self, KyubuSide side);
 *   KYUBU_EXPORT void KYUBU_CALL kyubu_mod_shutdown(KyubuSide side);            (optional)
 * kyubu_mod_init is called once per logical side the mod's entry point applies to.
 */
typedef int  (KYUBU_CALL* KyubuModQueryFn)(KyubuModQuery* q);
typedef int  (KYUBU_CALL* KyubuModInitFn)(const KyubuApi* api, KyubuModHandle self, KyubuSide side);
typedef void (KYUBU_CALL* KyubuModShutdownFn)(KyubuSide side);

#ifdef __cplusplus
}
#endif
#endif /* KYUBU_API_H */
