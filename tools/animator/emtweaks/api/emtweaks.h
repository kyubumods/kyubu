/* emtweaks.h - the interfaces of the EMTweaks mod: `emtweaks:shockwave`, `emtweaks:particles`, `emtweaks:beam`,
 * `emtweaks:ghost`, `emtweaks:circle` and `emtweaks:swirl`.
 *
 * A mod that wants a shockwave (a flat ring that expands, fades and is drawn in the 3D world, not a billboard) asks for the
 * interface once and calls it from the game's main thread on the client side:
 *
 *     EmShockwaveApi* sw = NULL; uint32_t version = 0;
 *     if (api->get_interface(EM_SHOCKWAVE_INTERFACE, EM_SHOCKWAVE_VERSION, (void**)&sw, &version) == KY_OK) {
 *         EmShockwaveDef d = {0};
 *         d.size = sizeof d;
 *         d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;          // world units, 65,536 to a block
 *         d.facing[2] = 1.0f;                                // the ring lies flat (its normal points up)
 *         d.inner_radius = 1.0f; d.thickness = 0.5f; d.expansion = 0.25f;
 *         d.life_ticks = 40; d.fade_on_tick = 20; d.fade_speed = 0.05f;
 *         d.color[0] = 1.0f; d.color[1] = 0.6f; d.color[2] = 0.1f; d.color[3] = 0.9f;
 *         uint32_t id = sw->spawn(&d);
 *     }
 *
 * The mod that owns the interface must be a dependency of the caller, so that it loads first. An interface is append-only: a
 * later version only adds members at the end, and `size` tells how many the mod that owns it has.
 *
 * Units: positions are the game's world units (65,536 to a block; x east, y north, z up); lengths are blocks; a tick is 20 ms of
 * game time (the game's own World step).
 */
#ifndef EMTWEAKS_H
#define EMTWEAKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EM_SHOCKWAVE_INTERFACE "emtweaks:shockwave"
#define EM_SHOCKWAVE_VERSION 1u

/* What one shockwave is. The caller sets `size` and every field it uses; a zero field means what its comment says. */
typedef struct EmShockwaveDef {
    uint32_t size;          /* sizeof(EmShockwaveDef), set by the caller */
    uint32_t flags;         /* 0 */
    int64_t  pos[3];        /* the centre, in world units */
    float    facing[3];     /* the direction the ring faces (its normal), any length; all zero means up (0, 0, 1) */
    float    inner_radius;  /* blocks: the radius of the empty circle in the middle, at birth. 0 makes a disc */
    float    thickness;     /* blocks: the width of the ring, outer radius minus inner radius. Below 0.01 it is 0.01 */
    float    expansion;     /* blocks per tick: how fast both radii grow. Negative shrinks the ring (it ends when the
                               outer radius reaches 0). 0 keeps its size */
    uint32_t life_ticks;    /* how many ticks the shockwave lives, 1 to 6000 (0 means 1) */
    uint32_t fade_on_tick;  /* the tick on which the fade starts; 0 fades from the first tick. Ignored when fade_speed is 0 */
    float    fade_speed;    /* the part of the starting alpha lost per tick once fading: 0.05 takes 20 ticks to vanish.
                               0 never fades: the shockwave is cut off at life_ticks */
    float    color[4];      /* red, green, blue and alpha, each 0 to 1 (a value outside it is clamped). The alpha is the
                               opacity at birth */
    /* Added after the first release; a caller whose `size` ends before it gets the mod's default. */
    float    pixel_size;    /* blocks: the ring is drawn as square cells of this size, like the game's cubes. 0 uses the mod's
                               default (its `pixel_size` option, 0.25); a negative value draws a smooth ring */
} EmShockwaveDef;

typedef struct EmShockwaveApi {
    uint32_t size;          /* sizeof(EmShockwaveApi) */
    uint32_t version;       /* the interface's own version (EM_SHOCKWAVE_VERSION) */
    /* Starts a shockwave. Returns its id (1 or more), or 0 when it was refused: a bad definition (a size below the
       first 4 fields, a value that is not a number), or too many shockwaves alive (256). The reason is in the log. */
    uint32_t (*spawn)(const EmShockwaveDef* def);
    /* Ends a shockwave now. Returns 1 when it was alive, else 0. */
    int      (*cancel)(uint32_t id);
    /* How many shockwaves are alive. */
    uint32_t (*count)(void);
    /* Ends every shockwave. */
    void     (*clear)(void);
} EmShockwaveApi;

/* ---- the game's own particles: `emtweaks:particles` ------------------------------------------------------------------------
 *
 * One of the game's particle effects is `count` small cubes that fly out of a point and fall (a hit's spray, a bomb's puff). The
 * game's own types of them are the five EM_PARTICLE_*. This interface starts them (the same as Kyubu's `particle_spawn`, client
 * side only) and keeps a table of named presets, so a mod says "hit" and not eight numbers, and a preset can be tuned in one place.
 *
 *     EmParticleApi* pa = NULL; uint32_t version = 0;
 *     if (api->get_interface(EM_PARTICLES_INTERFACE, EM_PARTICLES_VERSION, (void**)&pa, &version) == KY_OK) {
 *         int64_t at[3] = { x, y, z };
 *         pa->spawn_preset("vanilla.hit", at, NULL, 1.0f);
 *     }
 */
#define EM_PARTICLES_INTERFACE "emtweaks:particles"
#define EM_PARTICLES_VERSION 1u

#define EM_PARTICLE_SOLID 0u            /* EmParticleDef.type: cuwo's names; their looks are catalogued in docs/emtweaks.md */
#define EM_PARTICLE_BOMB 1u
#define EM_PARTICLE_NO_SPREAD 2u
#define EM_PARTICLE_NO_ACCELERATION 3u
#define EM_PARTICLE_NO_GRAVITY 4u
#define EM_PARTICLE_COUNT_MAX 1000u

typedef struct EmParticleDef {
    uint32_t size;          /* sizeof(EmParticleDef), set by the caller */
    uint32_t type;          /* EM_PARTICLE_*, 0 to 4 */
    int64_t  pos[3];        /* where the effect starts, world units. Ignored by define_preset (a preset has no place) */
    float    accel[3];      /* the acceleration the particles get, blocks per second squared (the game's hit uses 0, 0, 10) */
    float    color[4];      /* red, green, blue and alpha, each 0 to 1 */
    float    scale;         /* the size of one particle, blocks: above 0, at most 16 */
    uint32_t count;         /* how many particles, 1 to EM_PARTICLE_COUNT_MAX */
    float    spreading;     /* how far the particles scatter, blocks per second: 0 to 64 */
} EmParticleDef;

/* A ring of particles: `points` effects placed round `pos` in the plane whose normal is `facing`, each accelerating towards the
   centre (time above 0) or away from it (time below 0), so that a particle that starts at rest covers `radius` in |time| seconds
   (accel = 2 * radius / time^2, if the acceleration is in blocks per second squared: the unit is still to be confirmed). */
typedef struct EmParticleRingDef {
    uint32_t size;          /* sizeof(EmParticleRingDef), set by the caller */
    uint32_t type;          /* EM_PARTICLE_*: every point has it */
    int64_t  pos[3];        /* the centre, world units */
    float    facing[3];     /* the ring's normal, any length; all zero means up (a ring lying flat) */
    float    radius;        /* blocks, 0.1 to 64: how far from the centre the particles start (or end, for an outward ring) */
    uint32_t points;        /* how many effects the ring is made of, 3 to 64 */
    float    time;          /* seconds, 0.05 to 30 in magnitude: above 0 gathers to the centre, below 0 spreads out from it */
    float    accel[3];      /* added to every point's acceleration (to cancel gravity, for example) */
    float    color[4];      /* RGBA, 0 to 1 */
    float    scale;         /* the size of one particle, blocks: above 0, at most 16 */
    uint32_t count;         /* particles at each point, 1 to 1000 */
    float    spreading;     /* the scatter at each point, blocks per second: 0 to 64; 0 keeps the ring tidy */
} EmParticleRingDef;

typedef struct EmParticleApi {
    uint32_t size;          /* sizeof(EmParticleApi) */
    uint32_t version;       /* EM_PARTICLES_VERSION */
    /* Starts the effect of `def`. Returns 1 when it was queued, 0 when it was refused (a bad field, no world, the server side, or
       a Kyubu without `particle_spawn`); the reason is in the log. */
    int         (*spawn)(const EmParticleDef* def);
    /* Starts the preset `name` at `pos` (world units). `color` (4 floats) replaces the preset's colour when not NULL;
       `scale_mul` multiplies the preset's scale (0 or less means 1). Returns 1 or 0 like spawn. */
    int         (*spawn_preset)(const char* name, const int64_t pos[3], const float* color, float scale_mul);
    /* Saves a preset under `name` (a mod's own prefix and a dot or colon: "my_mod.sparkle"; 1 to 63 characters, no spaces),
       replacing one of that name. `def->pos` is ignored. The built-in "vanilla.*" presets are read only. Returns 1 or 0. */
    int         (*define_preset)(const char* name, const EmParticleDef* def);
    /* Copies a preset to `out` (set out->size first; pos is 0). Returns 1 when it exists, else 0. */
    int         (*preset_get)(const char* name, EmParticleDef* out);
    /* How many presets there are, and the name of the one at `index` (NULL out of range; valid until the next define_preset). */
    uint32_t    (*preset_count)(void);
    const char* (*preset_name)(uint32_t index);
    /* Starts a ring of particles (EmParticleRingDef). Returns how many effects were queued: `points`, or 0 when it was refused
       (a bad field, no world, the server side; the reason is in the log). Nothing is queued when it is refused. */
    int         (*spawn_ring)(const EmParticleRingDef* def);
} EmParticleApi;

/* ---- rays of light: `emtweaks:beam` -----------------------------------------------------------------------------------------
 *
 * A beam is a see-through ray of light from `pos` along `dir`: a band that widens from `radius_start` to `radius_end`, turned
 * about its own line towards the camera, opaque `color[3]` along its middle and `edge_alpha` at its rim, drawn as square cells
 * like the shockwave's. It can be moved at each step (`move`), so a ring of rays can drift.
 *
 *     EmBeamApi* bm = NULL; uint32_t version = 0;
 *     if (api->get_interface(EM_BEAM_INTERFACE, EM_BEAM_VERSION, (void**)&bm, &version) == KY_OK) {
 *         EmBeamDef d = {0};
 *         d.size = sizeof d;
 *         d.pos[0] = x; d.pos[1] = y; d.pos[2] = z + 8 * 65536;   // 8 blocks up
 *         d.dir[2] = -1.0f; d.length = 8.0f;                      // straight down to the ground
 *         d.radius_start = 0.4f; d.radius_end = 0.8f;
 *         d.color[0] = d.color[1] = d.color[2] = 1.0f; d.color[3] = 0.5f; d.edge_alpha = 0.05f;
 *         d.fade_in_ticks = 25; d.life_ticks = 250; d.fade_on_tick = 200; d.fade_speed = 0.02f;
 *         uint32_t id = bm->spawn(&d);
 *     }
 */
#define EM_BEAM_INTERFACE "emtweaks:beam"
#define EM_BEAM_VERSION 1u

typedef struct EmBeamDef {
    uint32_t size;          /* sizeof(EmBeamDef), set by the caller */
    uint32_t flags;         /* 0 */
    int64_t  pos[3];        /* where the ray starts, world units */
    float    dir[3];        /* the way it goes, any length; all zero means straight down (0, 0, -1) */
    float    length;        /* blocks, 0.01 to 256 */
    float    radius_start;  /* blocks: the half-width at `pos`, 0.01 to 64 */
    float    radius_end;    /* blocks: the half-width at the far end, 0.01 to 64 (a ray can widen towards the ground) */
    float    color[4];      /* red, green, blue and alpha, each 0 to 1. The alpha is the opacity along the ray's middle */
    float    edge_alpha;    /* the opacity at the rim, 0 to 1; the cells between go evenly from one to the other */
    float    pixel_size;    /* blocks: the size of a square cell. 0 uses the mod's `pixel_size` option (0.25); a negative value
                               draws a smooth band */
    uint32_t fade_in_ticks; /* ticks to rise from nothing to full opacity; 0 starts at full */
    uint32_t life_ticks;    /* how many ticks the beam lives, 1 to 6000 (0 means 1) */
    uint32_t fade_on_tick;  /* the tick on which the fade out starts; ignored when fade_speed is 0 */
    float    fade_speed;    /* the part of the opacity lost per tick once fading out; 0 never fades (cut off at life_ticks) */
    uint32_t additive;      /* 1 adds its light to what is behind it (a glow), 0 blends over it like the shockwave */
} EmBeamDef;

typedef struct EmBeamApi {
    uint32_t size;          /* sizeof(EmBeamApi) */
    uint32_t version;       /* EM_BEAM_VERSION */
    /* Starts a beam. Returns its id (1 or more), or 0 when it was refused: a bad definition (a size below the whole of version 1,
       a value that is not a number) or 64 beams alive. The reason is in the log. */
    uint32_t (*spawn)(const EmBeamDef* def);
    /* Moves a beam: `pos` (world units) is its new start and `dir` its new way (all zero is down); NULL keeps either. A `length`
       below 0 keeps it. Its age, fade and look are kept. Returns 1 when it was alive, else 0. */
    int      (*move)(uint32_t id, const int64_t pos[3], const float dir[3], float length);
    /* Ends a beam now. Returns 1 when it was alive, else 0. */
    int      (*cancel)(uint32_t id);
    /* How many beams are alive. */
    uint32_t (*count)(void);
    /* Ends every beam. */
    void     (*clear)(void);
} EmBeamApi;

/* ---- see-through models: `emtweaks:ghost` ---------------------------------------------------------------------------------
 *
 * A ghost is a voxel model (the bytes of a .cub file) drawn see-through at any angle: the game draws its own models opaque and
 * turns them by yaw only. Only its outer faces are built, once, at the spawn; each step draws the ones turned towards the camera,
 * shaded by the way they face, so no inner or far face shows through. It lives until `cancel` or `clear`.
 *
 *     EmGhostApi* gh = NULL; uint32_t version = 0;
 *     if (api->get_interface(EM_GHOST_INTERFACE, EM_GHOST_VERSION, (void**)&gh, &version) == KY_OK) {
 *         EmGhostDef d = {0};
 *         d.size = sizeof d;
 *         d.cub = bytes; d.cub_size = n;                   // copied by spawn
 *         d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;        // the middle of the model's box, world units
 *         d.rotation[2] = 90.0f;                           // yaw, pitch, roll in degrees
 *         d.voxel_size = 0.06f; d.alpha = 0.0f;           // starts invisible
 *         uint32_t id = gh->spawn(&d);
 *         ...each step: gh->set(id, NULL, NULL, a);         // a from 0 to 1 over the fade
 *     }
 */
#define EM_GHOST_INTERFACE "emtweaks:ghost"
#define EM_GHOST_VERSION 1u

typedef struct EmGhostDef {
    uint32_t    size;       /* sizeof(EmGhostDef), set by the caller */
    uint32_t    flags;      /* 0 */
    int64_t     pos[3];     /* the middle of the model's box, world units */
    const char* cub;        /* the model's bytes in the .cub layout (docs/assets.md); copied by spawn, so they can go after it */
    uint32_t    cub_size;   /* how many bytes */
    float       rotation[3];/* degrees: yaw (about up; 0 faces the model's +y north, 90 west, like a creature's yaw), then pitch
                               (about the model's +x; positive lifts its +y), then roll (about the model's +y; positive lowers
                               its +x). The model is turned by roll first, then pitch, then yaw */
    float       voxel_size; /* blocks: the size of one voxel, above 0, at most 4 */
    float       color[4];   /* multiplies each voxel's colour and the opacity, RGBA 0 to 1; all four zero means 1, 1, 1, 1 */
    float       alpha;      /* the opacity now, 0 to 1 (drawn: color[3] * alpha); `set` changes it */
} EmGhostDef;

typedef struct EmGhostApi {
    uint32_t size;          /* sizeof(EmGhostApi) */
    uint32_t version;       /* EM_GHOST_VERSION */
    /* Starts a ghost. Returns its id (1 or more), or 0 when it was refused: a bad definition (a size below the whole of version
       1, a value that is not a number, bytes that are not a model) or 16 ghosts alive. The reason is in the log. The model is read
       at the call (the bytes may go after it) and its faces are built on a worker thread: the ghost is drawn when they are done,
       and a model of more than 65,536 outer faces drops the ghost then, with the reason in the log. */
    uint32_t (*spawn)(const EmGhostDef* def);
    /* Places a ghost: `pos` (world units), `rotation` (yaw, pitch, roll in degrees) and `alpha` (0 to 1). NULL keeps `pos` or
       `rotation`; an `alpha` below 0 keeps it. Returns 1 when it is alive, else 0. */
    int      (*set)(uint32_t id, const int64_t pos[3], const float rotation[3], float alpha);
    /* Ends a ghost now. Returns 1 when it was alive, else 0. */
    int      (*cancel)(uint32_t id);
    /* How many ghosts are alive. */
    uint32_t (*count)(void);
    /* Ends every ghost. */
    void     (*clear)(void);
} EmGhostApi;

/* ---- magic circles: `emtweaks:circle` ---------------------------------------------------------------------------------------
 *
 * A circle is a ring of fire on the ground with a star inside, built as voxel models at the spawn from a radius and a voxel size,
 * so a larger circle has more voxels, not larger ones (a ghost scaled up would grow its voxels, its line and its flames with it).
 * It is drawn see-through like a ghost. Its flames can rise out of the ground, its star grow in after them, and it can turn, pulse,
 * fade in and fade out by itself. It has its own limit (16), apart from the ghosts'. It can be summoned: points with trails come
 * down onto it turning on its ring, and spin fast as it rises (the summon_* fields).
 *
 *     EmCircleApi* ci = NULL; uint32_t version = 0;
 *     if (api->get_interface(EM_CIRCLE_INTERFACE, EM_CIRCLE_VERSION, (void**)&ci, &version) == KY_OK) {
 *         EmCircleDef d = {0};
 *         d.size = sizeof d;
 *         d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;        // the centre on the ground, world units
 *         d.radius = 4.0f; d.voxel_size = 0.0625f;          // 4 blocks, 16 voxels to a block, at any radius
 *         d.flame_height = 4; d.star_points = 5;           // a pentagram (star_step 0 means 2)
 *         d.ring_color[0] = 0.8f; d.ring_color[1] = 0.1f; d.ring_color[2] = 0.05f;
 *         d.flame_color[0] = 1.0f; d.flame_color[1] = 0.8f; d.flame_color[2] = 0.3f;
 *         d.star_color[0] = 1.0f; d.star_color[1] = 0.6f; d.star_color[2] = 0.1f;
 *         d.rise_ticks = 20; d.star_delay_ticks = 20; d.star_in_ticks = 6;
 *         d.life_ticks = 500; d.fade_out_ticks = 25;
 *         uint32_t id = ci->spawn(&d);
 *     }
 */
#define EM_CIRCLE_INTERFACE "emtweaks:circle"
#define EM_CIRCLE_VERSION 1u

typedef struct EmCircleDef {
    uint32_t size;          /* sizeof(EmCircleDef), set by the caller */
    uint32_t flags;         /* 0 */
    int64_t  pos[3];        /* the centre, on the ground (the bottom of the ring), world units */
    float    rotation[3];   /* degrees, as a ghost's: yaw, pitch, roll. All zero lies flat on the ground; pitch 90 stands it up */
    float    radius;        /* blocks: from the centre to the middle of the ring's line. The models are built for it */
    float    voxel_size;    /* blocks: the size of one voxel, above 0, at most 1. The circle is at most 256 voxels across */
    uint32_t line_width;    /* voxels: the width of the ring's line, 1 to 16; 0 means 3 */
    uint32_t flame_height;  /* voxels: the tallest flame on the line, 0 to 32; 0 is a flat line one voxel high */
    uint32_t star_points;   /* a star inside the ring with this many points, 3 to 12; 0 draws none */
    uint32_t star_step;     /* each point is joined to the one this many further on: 5 points and 2 make a pentagram, 6 and 2 two
                               triangles, 1 a polygon. 0 means 2 (1 for 3 or 4 points); at most star_points / 2 */
    float    inner_radius;  /* blocks: a thin ring inside, drawn with the star; 0 draws none */
    float    ring_color[3]; /* red, green, blue, 0 to 1: the line at the ground */
    float    flame_color[3];/* the flames' tips; the voxels between go from ring_color to it */
    float    star_color[3]; /* the star and the inner ring */
    uint32_t seed;          /* the flames' ragged pattern: the same fields and seed build the same circle on every client */
    float    alpha;         /* the opacity at full, 0 to 1; 0 means 1. `set` changes it */
    uint32_t rise_ticks;    /* the flames rise out of the ground over this many ticks; 0 shows them whole at once */
    uint32_t star_delay_ticks; /* ticks from the spawn before the star (and the inner ring) appears */
    uint32_t star_in_ticks; /* ticks over which the star grows from the centre to its size, a little past it and back; 0 at once */
    float    spin;          /* degrees a second about the circle's own up; positive turns counter-clockwise seen from above */
    float    pulse;         /* 0 to 1: the flames' height swells and sinks by this part of it, pulse_hz times a second */
    float    pulse_hz;      /* 0 to 10 */
    uint32_t fade_in_ticks; /* ticks to rise from nothing to full opacity; 0 starts at full */
    uint32_t life_ticks;    /* ticks it lives, up to 360,000 (2 hours); 0 lives until `end`, `cancel` or `clear` */
    uint32_t fade_out_ticks;/* the last ticks of its life fade it out; ignored when life_ticks is 0 */
    /* Added after version 1's first release; a caller whose `size` ends before them gets no summon. */
    uint32_t summon_ticks;  /* 0: none. Before the circle forms, white-hot points with long trails (a swirl of EM_SWIRL_SPARKS)
                               grow in summon_height above it and come down onto it over this many ticks, turning on its ring;
                               landed, they spin fast while its flames rise (over rise_ticks, or 10 ticks when that is 0) and shrink
                               away as it forms. The circle's own times (rise, star, fade in, life) count from the landing */
    uint32_t summon_points; /* how many points, 1 to 8; 0 means 2 */
    float    summon_height; /* blocks above the centre where they appear, 0 to 64; 0 means twice the radius */
    float    summon_radius; /* blocks: the ring they turn on, 0 to 64; 0 means the circle's radius */
    float    summon_spin;   /* degrees a second at the top, the way as spin's, up to 900: 7/3 as fast landing, ten times on the
                               ground. 0 means -300 (clockwise seen from above) */
    float    summon_voxel;  /* blocks: the points' voxels, 0 to 1; 0 means the circle's voxel_size */
    float    summon_color[3]; /* the points' cores; all zero means flame_color brought near white. The rest of their colours run
                               from flame_color to ring_color and darker */
    float    summon_tilt;   /* degrees, -80 to 80: the points' tails climb at this slope behind them, so they lean down onto the
                               circle (the swirl's tilt_degrees); 0 lies flat. Added later still: a shorter `size` gets 0 */
    uint32_t summon_vanish; /* 1: the points shrink away as they reach the ground and end there, instead of spinning on it while the
                               flames rise; 0: the first look. Added last: a shorter `size` gets 0 */
} EmCircleDef;

typedef struct EmCircleApi {
    uint32_t size;          /* sizeof(EmCircleApi) */
    uint32_t version;       /* EM_CIRCLE_VERSION */
    /* Builds a circle's models and starts it. Returns its id (1 or more), or 0 when it was refused: a bad definition (a size below
       the whole of version 1, a value that is not a number or out of its range, a circle more than 256 voxels across, models of
       more than 65,536 outer faces) or 16 circles alive. The reason is in the log. */
    uint32_t (*spawn)(const EmCircleDef* def);
    /* Places a circle: `pos` (world units), `rotation` (yaw, pitch, roll in degrees) and `alpha` (its opacity at full, 0 to 1). NULL
       keeps `pos` or `rotation`; an `alpha` below 0 keeps it. Its age, spin and look are kept. Returns 1 when it is alive, else 0. */
    int      (*set)(uint32_t id, const int64_t pos[3], const float rotation[3], float alpha);
    /* Fades a circle out over `fade_ticks` and ends it (0 ends it now). Returns 1 when it was alive, else 0. */
    int      (*end)(uint32_t id, uint32_t fade_ticks);
    /* Ends a circle now. Returns 1 when it was alive, else 0. */
    int      (*cancel)(uint32_t id);
    /* How many circles are alive. */
    uint32_t (*count)(void);
    /* Ends every circle. */
    void     (*clear)(void);
} EmCircleApi;

/* ---- rings of fiery points: `emtweaks:swirl` -------------------------------------------------------------------------------
 *
 * A swirl is a ring of `points` fiery points round an axis through `pos`, each with a tail of sparks behind it, built as one voxel
 * model at the spawn (for its radius, with the voxel size kept) and turned round the axis, drawn see-through like a ghost. Two looks,
 * the Fire Mage's: EM_SWIRL_EMBERS (small points in alternating colours riding a wave, three sparks behind each: embers round raised
 * hands) and EM_SWIRL_SPARKS (white-hot points, each with a long trail that thins, cools and breaks up: points coming down onto a
 * spot). Over travel_ticks its height along the axis and its spin can go from one value to another; it can grow in and shrink out.
 * The mod moves it with `set` (to the hands at each step).
 *
 *     EmSwirlApi* sw = NULL; uint32_t version = 0;
 *     if (api->get_interface(EM_SWIRL_INTERFACE, EM_SWIRL_VERSION, (void**)&sw, &version) == KY_OK) {
 *         EmSwirlDef d = {0};
 *         d.size = sizeof d;
 *         d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;        // between the hands, world units
 *         d.style = EM_SWIRL_EMBERS; d.radius = 0.69f;     // 8 embers of the player's voxels (voxel_size 0 means 0.086)
 *         d.spin = -750.0f;                                // clockwise seen from above
 *         d.grow = 1; d.fade_in_ticks = 22;                // grows in over the hands' rise
 *         uint32_t id = sw->spawn(&d);                     // then sw->set(id, hands, NULL, -1, -1) each step, sw->end(id, 6)
 *     }
 */
#define EM_SWIRL_INTERFACE "emtweaks:swirl"
#define EM_SWIRL_VERSION 1u

#define EM_SWIRL_EMBERS 0u          /* EmSwirlDef.style: points of 2 voxels, by turns yellow, orange, amber and red, on a wave */
#define EM_SWIRL_SPARKS 1u          /* points of 3 voxels, white at the core, with long trails */

#define EM_SWIRL_EASE_SMOOTH 0u     /* EmSwirlDef.ease: slow, fast, slow */
#define EM_SWIRL_EASE_LINEAR 1u
#define EM_SWIRL_EASE_IN 2u         /* slow, then faster */
#define EM_SWIRL_EASE_OUT 3u        /* fast, then slower */

typedef struct EmSwirlDef {
    uint32_t size;          /* sizeof(EmSwirlDef), set by the caller */
    uint32_t flags;         /* 0 */
    int64_t  pos[3];        /* the ring's centre, world units; `set` moves it */
    float    axis[3];       /* the axis the ring turns round (its "up"), any length; all zero means up. Down turns the model
                               upside down */
    uint32_t style;         /* EM_SWIRL_EMBERS or EM_SWIRL_SPARKS */
    uint32_t points;        /* 1 to 16, evenly round the ring; 0 means 8 embers or 2 sparks */
    float    radius;        /* blocks from the axis to the points, above 0, at most 64. The model is built for it */
    float    voxel_size;    /* blocks, above 0, at most 1; 0 means 0.086 (the player model's own). At most 256 voxels across */
    float    height;        /* blocks along the axis from pos, -256 to 256 */
    float    height_end;    /* the height at the end of travel_ticks */
    float    spin;          /* degrees a second, -3600 to 3600: counter-clockwise seen from the axis's tip when positive. The
                               tails lie behind the points as it turns */
    float    spin_end;      /* the spin at the end of travel_ticks: it goes there straight */
    uint32_t travel_ticks;  /* ticks from the spawn to the end values, which then hold; 0: the start values hold */
    uint32_t ease;          /* EM_SWIRL_EASE_*: how the height goes to its end value */
    float    ramp[6][3];    /* the colours, hot to cold (RGB 0 to 1): the core, yellow, amber, orange, red, dark red; all zero
                               means that fire */
    float    tail_degrees;  /* how far round the ring a tail reaches behind its point, 0 to 180; 0 means 33 (embers) or 75 (sparks) */
    uint32_t seed;          /* where a sparks trail breaks up: the same on every client for the same seed */
    uint32_t additive;      /* 1 adds its light to what is behind it (a glow), 0 blends over it */
    uint32_t grow;          /* 1: the fades grow it in from nothing and shrink it out, at full opacity; 0: they fade its opacity */
    float    alpha;         /* the opacity at full, 0 to 1; 0 means 1. `set` changes it */
    float    scale;         /* its size, 0 to 8 (a flare above 1); 0 means 1. `set` changes it */
    uint32_t fade_in_ticks; /* ticks to grow (or fade) in; 0 starts whole */
    uint32_t life_ticks;    /* ticks it lives, up to 360,000; 0 lives until `end`, `cancel` or `clear` */
    uint32_t fade_out_ticks;/* the last ticks of its life shrink (or fade) it out; ignored when life_ticks is 0 */
    /* Added after version 1's first release; a caller whose `size` ends before it gets no tilt. */
    float    tilt_degrees;  /* -80 to 80: each tail climbs along the axis at this slope as it goes back from its point, so the points
                               lean down as they turn (towards the ground for an axis up); negative lowers the tails. 0 lies flat.
                               The model is at most 256 voxels high */
} EmSwirlDef;

typedef struct EmSwirlApi {
    uint32_t size;          /* sizeof(EmSwirlApi) */
    uint32_t version;       /* EM_SWIRL_VERSION */
    /* Starts a swirl. Returns its id (1 or more), or 0 when it was refused: a bad definition (a size below the whole of version
       1, a value that is not a number or out of its range) or 32 swirls alive. The reason is in the log. The model is built on a
       worker thread: the swirl's clock runs at once and it is drawn when they are done; a build that fails (more than 256 voxels
       across, too many faces) drops the swirl and the log says why. */
    uint32_t (*spawn)(const EmSwirlDef* def);
    /* Moves a swirl: `pos` (world units), `axis` (all zero is up), `scale` (0 to 8) and `alpha` (its opacity at full, 0 to 1). NULL
       keeps `pos` or `axis`; a `scale` or an `alpha` below 0 keeps it. Its age and way are kept. Returns 1 when it is alive, else 0. */
    int      (*set)(uint32_t id, const int64_t pos[3], const float axis[3], float scale, float alpha);
    /* Shrinks (or fades) a swirl out over `fade_ticks` and ends it (0 ends it now). Returns 1 when it was alive, else 0. */
    int      (*end)(uint32_t id, uint32_t fade_ticks);
    /* Ends a swirl now. Returns 1 when it was alive, else 0. */
    int      (*cancel)(uint32_t id);
    /* How many swirls are alive. */
    uint32_t (*count)(void);
    /* Ends every swirl. */
    void     (*clear)(void);
} EmSwirlApi;

/* ---- the pulse ------------------------------------------------------------------------------------------------------------
 * The Water Mage's pulse, as its Animator preview drew it: a thin ring of cells running out over the ground from a centre, with a
 * crest of blue drops riding just behind it. It is built at the spawn as one voxel model for each step of its radius (each step
 * about 1.18 times the last, the voxel size kept) and each is drawn stretched up to the next, so the cells stay small as it grows;
 * it fades as it goes out and is gone when it reaches its radius. At tick t of travel_ticks its radius is
 * radius_start + (radius - radius_start) * t / travel_ticks (blocks): a mod's hitbox can grow with it, or ask `radius`.
 *
 *     EmPulseApi* pu = NULL; uint32_t version = 0;
 *     if (api->get_interface(EM_PULSE_INTERFACE, EM_PULSE_VERSION, (void**)&pu, &version) == KY_OK) {
 *         EmPulseDef d = {0};
 *         d.size = sizeof d;
 *         d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;        // the caster's feet, world units
 *         d.radius = 5.33f;                                // blocks; travel_ticks 0: at the preview's speed (9.6 blocks a second)
 *         uint32_t id = pu->spawn(&d);                     // pu->radius(id) at each step for the hitbox
 *     }
 */
#define EM_PULSE_INTERFACE "emtweaks:pulse"
#define EM_PULSE_VERSION 1u

typedef struct EmPulseDef {
    uint32_t size;          /* sizeof(EmPulseDef), set by the caller */
    uint32_t flags;         /* 0 */
    int64_t  pos[3];        /* the centre, on the ground, world units */
    float    axis[3];       /* the ring's normal, any length; all zero means up */
    float    radius;        /* blocks the ring runs out to, above radius_start; the model is at most 1024 voxels across */
    float    radius_start;  /* blocks at the spawn, 0 or more; 0 means 2.5 voxels */
    float    voxel_size;    /* blocks, above 0, at most 1; 0 means 0.086 (the player model's own) */
    uint32_t travel_ticks;  /* ticks to reach `radius`, up to 3000; 0 means at the preview's 9.63 blocks a second */
    float    lag;           /* blocks the crest of drops rides behind the ring; 0 means 5 voxels; below 0: no drops */
    float    ramp[6][3];    /* the colours (RGB 0 to 1): white, foam, cyan, azure, blue, deep; all zero means the preview's water */
    float    alpha;         /* the opacity at full, 0 to 1; 0 means 1 */
    uint32_t additive;      /* 1 adds its light to what is behind it (a glow), 0 blends over it */
    uint32_t seed;          /* where the cells sparkle and the drops fall: the same on every client for the same seed; 0 is the preview's */
} EmPulseDef;

typedef struct EmPulseApi {
    uint32_t size;          /* sizeof(EmPulseApi) */
    uint32_t version;       /* EM_PULSE_VERSION */
    /* Starts a pulse. Returns its id (1 or more), or 0 when it was refused: a bad definition (a size below the whole of version
       1, a value that is not a number or out of its range, more than 1024 voxels across) or 16 pulses alive. The reason is in the
       log. The models are built on a worker thread: the call is quick, the pulse's clock runs at once and its ring is drawn when
       they are done; a build that fails drops the pulse and the log says why. */
    uint32_t (*spawn)(const EmPulseDef* def);
    /* The ring's radius now, blocks; -1 when the pulse has ended. */
    float    (*radius)(uint32_t id);
    /* Ends a pulse now. Returns 1 when it was alive, else 0. */
    int      (*cancel)(uint32_t id);
    /* How many pulses are alive. */
    uint32_t (*count)(void);
    /* Ends every pulse. */
    void     (*clear)(void);
} EmPulseApi;

/* ---- the chains -----------------------------------------------------------------------------------------------------------
 * The Water Mage's chains round a snared creature, as its Animator preview drew them: blue see-through chains at random places round
 * it, leaning in to it from the ground. They shoot up out of the ground and snap tight (snap_ticks, past their length by
 * `overshoot`), ease back a little (settle_ticks) and hold; at the end of their life (or at `end`) they fade in eight steps with blue
 * drops dripping off them. Their reach, count and links follow the creature's size. Built at the spawn; drawn like a ghost. The
 * snare itself (the freeze, the tint) is the calling mod's.
 *
 *     EmChainsApi* ch = NULL; uint32_t version = 0;
 *     if (api->get_interface(EM_CHAINS_INTERFACE, EM_CHAINS_VERSION, (void**)&ch, &version) == KY_OK) {
 *         EmChainsDef d = {0};
 *         d.size = sizeof d;
 *         d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;        // the creature's feet, world units
 *         d.target_radius = 0.77f; d.target_height = 2.9f; // blocks: half its width, its height
 *         d.seed = creature_id;
 *         uint32_t id = ch->spawn(&d);                     // ch->end(id) when the snare ends: the chains fade and go
 *     }
 */
#define EM_CHAINS_INTERFACE "emtweaks:chains"
#define EM_CHAINS_VERSION 1u

typedef struct EmChainsDef {
    uint32_t size;          /* sizeof(EmChainsDef), set by the caller */
    uint32_t flags;         /* 0 */
    int64_t  pos[3];        /* the creature's feet (the ground under its middle), world units; `set` moves it */
    float    target_radius; /* blocks: half the creature's width, above 0, at most 11; 0 means 0.52 (a slime's). The chains' anchors
                               are 1.6 times it out, their links grow with it */
    float    target_height; /* blocks, above 0, at most 16; 0 means 0.69. The chains reach up to 0.69 of it (at most 1.55 radii) */
    uint32_t count;         /* 1 to 12 chains; 0 means by size: the radius in voxels / 1.5, 4 to 8 */
    float    voxel_size;    /* blocks, above 0, at most 1; 0 means 0.086. At most 1024 voxels across */
    float    yaw;           /* degrees: turns the whole set about the vertical */
    uint32_t seed;          /* where the chains stand and the drops fall: the same on every client for the same seed */
    float    ramp[6][3];    /* the colours (RGB 0 to 1): glint, light links, links, the foot, drop, drop's tail; all zero means the
                               preview's blues */
    float    alpha;         /* the opacity at full, 0 to 1; 0 means 0.75 (see-through) */
    uint32_t additive;      /* 1 adds its light to what is behind it (a glow), 0 blends over it */
    uint32_t snap_ticks;    /* ticks to shoot up and snap tight; 0 means 10 (0.2 s) */
    uint32_t settle_ticks;  /* ticks to ease back after the snap; 0 means 18 */
    float    overshoot;     /* how far past their length they snap, 0 to 1; 0 means 0.12; below 0: none */
    uint32_t life_ticks;    /* ticks from the spawn to the fade's start, up to 360,000; 0: until `end` */
    uint32_t fade_ticks;    /* ticks of the fade (eight steps); 0 means 24 */
} EmChainsDef;

typedef struct EmChainsApi {
    uint32_t size;          /* sizeof(EmChainsApi) */
    uint32_t version;       /* EM_CHAINS_VERSION */
    /* Starts a set of chains. Returns its id (1 or more), or 0 when it was refused (a bad definition or 32 sets alive). The reason
       is in the log. The models are built on a worker thread: the call is quick, the set's clock runs at once and the chains are
       drawn when they are done; a build that fails (more than 1024 voxels across) drops the set and the log says why. */
    uint32_t (*spawn)(const EmChainsDef* def);
    /* Moves a set: `pos` (world units; NULL keeps it) and `alpha` (below 0 keeps it). Returns 1 when it is alive, else 0. */
    int      (*set)(uint32_t id, const int64_t pos[3], float alpha);
    /* Starts the fade now (if it has not started) and ends the set after it. Returns 1 when it was alive, else 0. */
    int      (*end)(uint32_t id);
    /* Ends a set now, without the fade. Returns 1 when it was alive, else 0. */
    int      (*cancel)(uint32_t id);
    /* How many sets are alive. */
    uint32_t (*count)(void);
    /* Ends every set. */
    void     (*clear)(void);
} EmChainsApi;

#ifdef __cplusplus
}
#endif

#endif /* EMTWEAKS_H */
