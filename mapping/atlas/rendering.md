# Rendering

Atlas page for the `rendering` subsystem: the Direct3D 9 device, the `plasma::` engine that draws on it, the voxel
shader, the models and the device reset. Reference pages: [docs/game/rendering.md](../../docs/game/rendering.md).

## Purpose

Cube.exe draws with Direct3D 9. One device is created at start-up and owned by `plasma::D3D9Engine`, the drawing
engine of the [ui](ui.md) framework; the 3D world is drawn by `cube::GameController::render()`. The voxel models
(`.cub`) are loaded into `Model` objects by `ModelLoader::loadAll()` and drawn with `cube::CubeShader`, a vertex and
pixel shader set (shader model 3). Server.exe draws nothing, but it still loads the models (6.2's question).

## Key classes

- [`plasma::D3D9Engine`](../../docs/game/types/plasma_D3D9Engine.md) (verified as a class, RTTI; listed under
  [ui](ui.md)): 0x2D0 bytes, derives from [`plasma::Engine`](../../docs/game/types/plasma_Engine.md). Holds the
  device; its textures, surfaces and drawing are `plasma::D3D9Texture`, `plasma::D3D9RenderSurface` and
  `plasma::D3D9Drawing`. Its destructor is 0x687640 (called by vtable slot 0).
- [`cube::CubeShader`](../../docs/game/types/cube_CubeShader.md) (verified as a class, RTTI): 0x74 bytes, one
  instance. The compiled shaders (one vs_3_0 and five ps_3_0, "Microsoft (R) HLSL Shader Compiler 9.30") sit in the
  data section; no function references them directly.
- `Model` and `ModelLoader` (verified functions, no RTTI): the model table, 2569 slots, filled from data1.db by
  `ModelLoader::loadAll()` and `Model::load()`.
- [`cube::Sprite`](../../docs/game/types/cube_Sprite.md) (96 bytes, 28 `operator new` sites) and
  [`cube::SpriteManager`](../../docs/game/types/cube_SpriteManager.md) (verified as classes, RTTI; in both builds):
  not read yet (particles or billboards?).
- The device globals, set in [render_init](../../docs/game/rendering.md) (mapped 2026-10-10): `d3d9` (0x7691B8, the
  `IDirect3D9`), `d3d9_device` (0x7691B4, the device), `plasma::d3d9_engine` (0x7691CC, the `plasma::D3D9Engine`),
  `cube::cube_shader` (0x7691BC, the `cube::CubeShader`) and `game_window` (0x7691C0, the window). The device is created
  windowed, with 4 samples, a D24S8 depth-stencil surface and immediate presentation, at 1024 x 768 until the window is
  sized (`plasma::D3D9Engine.width` and `plasma::D3D9Engine.height` follow it).

## Entry points

All candidate unless noted, Cube.exe:

- [render_init](../../docs/game/rendering.md) (0x4C85C0): called by `WinMain()` once the window exists.
  `Direct3DCreate9`, `CreateDevice` (hardware vertex processing, multithreaded), then
  `plasma::D3D9Engine::D3D9Engine()`, the font folder, and `cube::CubeShader::CubeShader()` with
  `cube::CubeShader::init()`. When it fails, `WinMain()` shows "Could not initialize Direct3D".
- `cube::CubeShader::CubeShader()` (0x448270), `cube::CubeShader::init()` (0x448460: returns bool, fills a
  vertex-element table) and `cube::CubeShader::~CubeShader()` (0x4482C0).
- [reset_device](../../docs/game/rendering.md) (0x4C87D0): called by `Game::frame()` (verified) when a reset is
  flagged (the byte 0x7691D1). It releases the engine's resources, calls IDirect3DDevice9::Reset until it succeeds
  (under the game controller's lock), passes the new size to `cube::GameController::onResize()` ([input](input.md))
  and recreates the resources.
- `cube::GameController::render()` (verified, vtable slot 11): the 3D draw of the world.
- `ModelLoader::loadAll()`, `Model::Model()` and `Model::load()` (verified): model loading, from the game
  controller's constructor.
- `ModelLoader::get()` (0x412190, task 8.4): a slot's `Model*`, or null when the slot is negative or past the
  vector's end. The ModelLoader is GameController + 0x2F8, its vector at + 4. Creatures and items reach the table
  through `draw_creature` and `ModelLoader::itemModel()` ([creatures](creatures.md), [items](items.md)).
- [matrix_transform_normal](../../docs/game/rendering.md) (0x4890A0, candidate): a vector times the 3×3 part of a
  4×4 float matrix (`this`), written to the first argument. 1.0.1 listed it as model resolver B; task 8.4 rejected
  that. Called by `cube::GameController::update()` and `render()`, `cube::World::update()` and others.

## Data flow

1. **Start-up.** `WinMain()` creates the window, then [render_init](../../docs/game/rendering.md) builds the
   device, the `plasma::D3D9Engine` and the `cube::CubeShader`. The engine is passed to
   `cube::GameController::GameController()`, which keeps it at +0x800708 (6.4) and loads the models
   (`ModelLoader::loadAll()`, data1.db).
2. **Each frame.** `WinMain()`'s message loop calls `Game::frame()`, which runs
   [reset_device](../../docs/game/rendering.md) when needed. Where the frame's draw starts is not confirmed: the
   engine draws the widget tree (each widget's vtable slot 1, 6.4 and 6.5), and `cube::GameController::render()` is
   reached only through its vtable. `render()` calls the map query 0x600ED0 ([travel](travel.md)) and
   [play_sound](../../docs/game/audio.md).
3. **Widgets.** Every widget draw slot read so far uses resource1.dat (a font or image archive?).
4. **Resize and lost device.** [reset_device](../../docs/game/rendering.md), then
   `cube::GameController::onResize()`, which lays the widgets out again.

## Client and server

- Only Cube.exe renders. Server.exe imports nothing from Direct3D and has no plasma engine and no
  `cube::CubeShader`.
- Server.exe still has `ModelLoader::loadAll()` and `Model::load()` (candidate, correlated), and `cube::Sprite` and
  `cube::SpriteManager` in RTTI: the models are probably needed for collision (an open question since 6.2).

## Hook points of interest

- [render_init](../../docs/game/rendering.md) (candidate): the device and the engine exist once it returns; a mod
  can create its own Direct3D resources after it.
- [reset_device](../../docs/game/rendering.md) (candidate): a mod that owns `D3DPOOL_DEFAULT` resources must
  release and recreate them around it.
- `cube::GameController::render()` (verified): draw before or after the world.
- `ModelLoader::loadAll()` (verified): the model table (the loader's model slots use it already).
- `ModelLoader::get()`: every slot look-up outside the inlined ones in `draw_creature` and `ModelLoader::itemModel()`.

## Open questions

- Which function calls `cube::GameController::render()` each frame, and in what order the world and the widgets are
  drawn. A tracer probe on `render()` would show the caller.
- Where the shaders are created (`CreateVertexShader` and `CreatePixelShader` go through the device's vtable, so the
  survey shows no import for them), and what each of the five pixel shaders is for.
- What `cube::Sprite` and `cube::SpriteManager` draw, and why Server.exe has them.
- The two engine calls in [reset_device](../../docs/game/rendering.md) (0x689450 before the reset, 0x6893F0 after):
  the release and re-creation of resources, to confirm.
- What resource1.dat holds.
- Where the anti-aliasing and render-distance options (options.cfg, read by 0x4CE450) are applied.

## Depth

reconnaissance, reviewed 2026-10-10

## How a plasma shape is drawn

Read on 2026-10-10 in `plasma::D3D9Drawing::draw()` (0x6898B0, vtable slot 2 of `plasma::D3D9Drawing`) and
`plasma::D3D9Drawing::updateBuffers()` (slot 1), and checked in the running game at the title screen (probe
[title-scene](../probes/title-scene.toml)). Every shape of the UI, the HUD and the title logo goes through them; the 3D
world does not.

- **One call, one indexed draw.** `plasma::D3D9Drawing::draw()` takes the node (or null) and an RGBA colour that the
  parent passes down. A shape without buffers (`plasma::D3D9Drawing.vertex_buffer` null) returns at once, which is why
  group nodes cost nothing. A shape has a fill drawing and a stroke drawing, so most nodes are drawn twice in a row.
- **It sets all its own state, every call.** The mask or texture (`SetTexture`), the vertex declaration, both shaders
  (`plasma::D3D9Engine.vertex_shader`, `plasma::D3D9Engine.pixel_shader`), the transform in the vertex shader's
  constants, the colour (`plasma::Display.colour_table` row `plasma::Display.colour_index` times the passed colour), the
  mask's four corner constants and the pixel shader's switches (a texture or not, the filter 0 to 3, and four numbers
  of the drawing: opacity, brightness, contrast and saturation). Anything a caller set before the call is replaced, so
  a shape cannot be changed by setting device state in front of it; swap the engine's shader field, or draw
  something else after it.
- **The transform** is `plasma::D3D9Engine.world_view`, a 4 x 4 whose x' is `x * m[0] + y * m[4] + m[12]`, in pixels
  of the client area, with the engine's vertex shader taking 0.135 pixel off (its half-texel fix). `draw` pushes a copy
  onto `plasma::D3D9Engine.matrix_stack` and restores it at the end, so a hook after `draw` sees the transform of the
  next shape, and a hook before it the one of this shape.
- **Depth.** When bit 3 of the engine's flags (+0x18C) is set and a node is given, `draw` first moves the transform's
  z by minus two times a global counter (at 0x76BF3C) and draws with the depth test on (`GREATER`), counting the
  counter up: later shapes are nearer. Otherwise the depth test is off. The depth-stencil surface is D24S8.
- **Antialiasing.** The device has 4 samples. `draw` switches the multisample state off for the indexed draw of a shape
  with an index buffer, and on again after it.
- **The vertices** are 48 bytes: a position (2 floats), 4 floats, 4 floats and 2 floats (the colour and the mask's
  coordinates). `plasma::D3D9Drawing::updateBuffers()` copies them from the shape's CPU arrays into the buffers when
  the shape changed, and keeps a CPU copy (probably to refill them after a device reset).
- **Render targets.** Some group nodes are drawn by first drawing their children into a second render target the size
  of the window, then drawing the group's own shape with that target as its mask, through the group's colour. The
  title logo's face layer is one (see [ui](ui.md), The title screen); which node setting asks for it is not read yet.
  To draw over a frame with your own triangles, wait until the back buffer is bound again, which is after the last
  node of the screen has been drawn.

## Hook points added by this reading

- `plasma::D3D9Drawing::draw` (verified): the one place every UI shape passes. Before the call, `plasma::D3D9Drawing.bounds`
  and `plasma::D3D9Engine.world_view` say where the shape lands; around it, a swapped `plasma::D3D9Engine.pixel_shader`
  recolours that one shape; after the last shape of a screen, the back buffer is free for the mod's own drawing.
- `d3d9_device`, `plasma::d3d9_engine`: the device and the engine without going through the controller.

## The world's camera and the order of a frame

Read on 2026-10-11 from the world vertex shader's compiled constant table and `cube::GameController::render()`, and checked in
the running game (probe [shockwave-ring](../probes/shockwave-ring.toml)). Evidence: `evidence/particles-2026-10-11/`.

- **The order inside `render`.** The sky and the world's models and terrain are drawn with `cube::CubeShader`; then the
  game clears the depth buffer only (the device's Clear with the flag D3DCLEAR_ZBUFFER alone, at 0x4BAA73), and then draws the
  interface with the plasma engine (the function at 0x64F5A0, called on the engine), then the models the HUD shows (item and character
  previews). So a draw at `Present` is not tested against the world, and a draw that wants the world's depth has to come
  before that clear.
- **The world's vertex shader** (vs_3_0, the blob at 0x6FC928) takes: `pointLightPositions` c0 to c9, `pointLightColors`
  c10 to c19, `worldViewProjMatrix` c20 to c23, `worldViewMatrix` c24 to c26, `worldMatrix` c27 to c29, `fogScale` c30,
  `fogGradientTranslation` c31, `materialColor` c32, `lightDirection` c33, `lightFrontColor` c34, `lightBackColor` c35,
  `cameraPosition` c36, `darknessColor` c37, `ambientColor` c38 and `white` c39. The UI's vertex shader (0x71F4A8) reuses
  c0 to c27, so a snapshot of c20 to c29 is the camera's only while the world's shader is bound. The world's pixel shaders
  take `skyColor1`, `skyColor2`, `fogColor`, `alpha` and `shininess` in c0 to c4.
- **`cube::CubeShader::setMatrices(world, view, projection)`** (0x4488F0, ret 0xC) writes c20 to c29 from three 4 x 4
  matrices in D3D's row-vector layout (a translation in the last row). The render function gives every world draw of a
  frame the same view and projection (65 of 66 calls in the replay), so the pair seen most often is the camera.
- **The render space.** A position is not the world's: `cube::GameController.render_origin` (+0x1D0, two int64: x and y)
  is added to the world position before it is divided by 65,536, so the middle of the world (x = y = 8,396,928 blocks)
  is 0. `cube::GameController.camera_position` (+0x140, int64 x 3) plus that origin is the shader's `cameraPosition`. The
  z is not moved. A creature at the middle of the world, 215 blocks up, is at (0, 0, 215) in the shader; a terrain chunk is
  a world matrix with its corner (for example -32, -32, 198).
- **The view matrix** looks from the camera to a point in front of it (the third-person camera's focus): its last row was
  (0, -86.6, 50) at the default zoom of a new character, a 30 degree pitch at a distance of 100 render units, and the
  projection's diagonal (-1.61, 2.15, 1.0, 0.0). Neither is needed to draw: the matrices go to the fixed-function
  pipeline as they are.

The EMTweaks mod (`mods-gameplay/emtweaks`, docs (in the `emtweaks` repository)) uses all
four: it hooks `cube::CubeShader::setMatrices` and the device's `Clear`, reads `render_origin`, and draws its rings
with the fixed-function pipeline between the world and the interface.
