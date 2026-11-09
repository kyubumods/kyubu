# UI (plasma)

Atlas page for the `ui` subsystem: plasma, the retained-mode UI and 2D graphics library the game is built on; the
`.plx` files that hold its scenes; and the game's own `cube::*Widget` classes. Reference pages:
[docs/game/ui.md](../../docs/game/ui.md).

## Purpose

Every screen and HUD element is a plasma scene: a tree of nodes (`plasma::Node`), each with an optional shape, a
transformation, a display and a widget. The scenes are drawn in an editor and saved as `.plx` files (`gui.plx`,
`start.plx`, `help.plx`, `quest-tag.plx`, `cursor.plx`, `interface.plx` in the game folder). At start-up the game
controller loads them and attaches its own widget subclasses (inventory, chat, skills, options, the start menu ...) to
nodes found by name. plasma also draws text with its own fonts and wraps Direct3D 9 (`plasma::D3D9Engine`), so the
[rendering](rendering.md) page shares its engine.

## Key classes

All RTTI classes, `verified` as classes (`rtti:`), Cube.exe only.

The framework:

| Class | What it is |
|---|---|
| [`plasma::Widget`](../../docs/game/types/plasma_Widget.md) | **the widget base class**. Bases `plasma::NamedObject`, `plasma::Object` and `plasma::Deformer` (a second vtable at +0x24). 43 virtual slots: slot 0 the deleting destructor, slot 40 `clone`. Probably 352 bytes (the most common `operator new` size next to its vtable) |
| [`plasma::Node`](../../docs/game/types/plasma_Node.md) | a scene-tree node: name, shape, transformation, display, widget, children, flags (the `Node.*` attributes of a `.plx`) |
| [`plasma::Engine`](../../docs/game/types/plasma_Engine.md), [`plasma::D3D9Engine`](../../docs/game/types/plasma_D3D9Engine.md) | the engine that owns the scenes, the fonts and the device; the game keeps a pointer at GameController+0x800708 |
| `plasma::Button`, `plasma::ListWidget`, `plasma::Edit`, `plasma::ScrollButton`, `plasma::ScrollSlider`, `plasma::PopUpButton` | the stock widgets; each has a reader in the `.plx` loader |
| `plasma::Transformation`, `plasma::Display`, `plasma::TextShape`, `plasma::MeshShape`, `plasma::CurveShape` | the per-node parts and shapes; their values are attribute templates (plasma::ContinuousAttribute and others, animatable) |
| `plasma::Widget::Connection` | the base of the callbacks: plasma::Widget::MemberFunctionConnection for the game controller, the inventory, the options and the character-style widget |
| `plasma::InvalidFileFormatException`, `plasma::InvalidVersionException`, `plasma::InvalidDemoLicenseException` | thrown by the `.plx` reader |

The game's widgets, each a direct subclass of `plasma::Widget` (size from `operator new`):

| Class | Size | Notes |
|---|---:|---|
| [`cube::InventoryWidget`](../../docs/game/types/cube_InventoryWidget.md) | 440 | three instances; children upbutton, downbutton, scrollbutton |
| [`cube::OptionsWidget`](../../docs/game/types/cube_OptionsWidget.md) | 496 | connects its controls to its own methods |
| [`cube::CharacterStyleWidget`](../../docs/game/types/cube_CharacterStyleWidget.md) | 424 | character creation |
| [`cube::SkillWidget`](../../docs/game/types/cube_SkillWidget.md) | 404 | the skill tree |
| [`cube::ChatWidget`](../../docs/game/types/cube_ChatWidget.md) | 400 | |
| [`cube::SpeechWidget`](../../docs/game/types/cube_SpeechWidget.md) | 480 | two instances (villager speech?) |
| [`cube::VoxelWidget`](../../docs/game/types/cube_VoxelWidget.md) | 1088 | the largest; draws voxel models (items?) |
| [`cube::MapOverlayWidget`](../../docs/game/types/cube_MapOverlayWidget.md) | 356 | the map ([travel](travel.md), 6.5) |
| `cube::AdaptionWidget`, `cube::BlueprintPreviewWidget`, `cube::CharacterWidget`, `cube::EnchantWidget`, `cube::ObjectiveWidget`, `cube::PreviewWidget`, `cube::SpriteWidget`, `cube::StartMenuWidget`, `cube::StatisticsWidget`, `cube::SystemWidget` | 356–384 | the other screens and HUD parts |
| `cube::CharacterPreviewWidget`, `cube::WorldPreviewWidget` | 360, 364 | one per saved character and per saved world, in the start menu's lists |

## Entry points

All candidate, Cube.exe:

- **The widget base.** `plasma::Widget::Widget()` (0x625B60) is called by every `cube::*Widget` constructor;
  `plasma::Widget::~Widget()` (0x625FA0) by the deleting destructors; `plasma::Widget::clone()` is slot 40.
- **The game's widgets.** One constructor per class, from `cube::AdaptionWidget::AdaptionWidget()` to
  `cube::WorldPreviewWidget::WorldPreviewWidget()`; all but two are called only by
  `cube::GameController::GameController()`. The exceptions are `cube::CharacterPreviewWidget::CharacterPreviewWidget()`
  and `cube::WorldPreviewWidget::WorldPreviewWidget()`, which the start menu's list builders (0x49D810 and 0x4A2590)
  call once per saved character or world.
- **Attaching.** `plasma::create_node()` (a new node on the engine), `plasma::clone_node()` (a copy of a template
  subtree under a parent) and `plasma::set_node_widget()` (stores a widget on the node of a given name, searching the
  children).
- **Callbacks.** `plasma::connect_widget()` adds a member-function connection to a widget; the makers are 0x457C70
  (the game controller's), 0x4C17B0 (the inventory's), 0x4CEE70 (the options') and 0x427C90 (character style).
- **Loading `.plx` files.** `plasma::load_plx()` (path in, prints `read <path>`), which calls `plasma::read_plx()`
  (the file format: header, then objects by type name), which calls `plasma::read_plx_node()` and, through the
  per-type readers, `plasma::read_plx_widget()`. `cube::load_interface_plx()` reads `interface.plx` through a
  second reader (0x40CEE0).
- **The engine.** `plasma::Engine::Engine()` and `plasma::D3D9Engine::D3D9Engine()`, created by 0x4C85C0 (the
  Direct3D set-up under [WinMain](../../docs/game/core.md)).
- **The look-at name** (2026-09-30). `cube::GameController::lookAtNameSite` (0x49B016, a mid-function site in
  `cube::GameController::update()`) is where the name of the creature being looked at is appended to the label, with
  the name string at [esp+4]. It is the hook site of ToufouMaster's CreatureLevelDisplay mod, which shows
  "LV &lt;level&gt; &lt;name&gt;" there. Its 8 bytes occur once in Cube.exe; that mod was written for another build of
  the game (see `kyubu-mapping/evidence/toufoumaster-crossmatch-2026-09-30/`).
- **Which creature is looked at** (2026-10-12). The label above belongs to the creature under the crosshair, and that creature
  comes from a list `cube::GameController::update()` fills once a frame: the creatures near the player whose hp is above 0.
  `cube::GameController::lookListFillSite` and `lookListFilledSite` are the two ends of the loop that fills it; the
  [creatures page](creatures.md) ("A model drawn at a point of the mod's choice") says how a mod keeps a creature out of it.
- **The character sheet** (0x435620, probably the draw slot of `cube::CharacterWidget`, whose constructor is just
  before it; not named yet) prints its stat lines from the creature stat functions on the [combat](combat.md) page.

## Data flow

1. WinMain's device set-up (0x4C85C0) creates the Direct3D 9 device and a `plasma::D3D9Engine`, whose base
   constructor `plasma::Engine::Engine()` makes the root node and the font engine.
2. `cube::GameController::GameController()` loads the scenes with `plasma::load_plx()`: `start.plx`, `help.plx`,
   `quest-tag.plx` and `cursor.plx` by name (and probably `gui.plx`, whose name is only reached through a pointer).
   `cube::load_interface_plx()` loads `interface.plx`.
3. `plasma::read_plx()` checks the header (the string PlasmaGraphics, a version and, going by the exceptions, a
   demo-license check; failures throw `plasma::InvalidVersionException` and `plasma::InvalidDemoLicenseException`),
   then reads each object by its type name. Nodes (`plasma::read_plx_node()`) link their shape, transformation,
   display, widget and children; stock widgets (Button, ListWidget, Edit ...) read their `Widget.*` part with
   `plasma::read_plx_widget()`.
4. **How a `cube::*Widget` is created and attached** (read in 0x49D810, the character list): the game clones a
   template subtree with `plasma::clone_node()` or makes a node with `plasma::create_node()`, allocates the widget
   with `operator new`, calls its constructor with (engine, node, game controller), which calls
   `plasma::Widget::Widget()`, and then calls `plasma::set_node_widget()` on the subtree with the node's name. The
   game controller keeps the widget pointers in its own fields.
5. Widget constructors look up their child nodes by name (upbutton, scrollbutton ...) and connect their events to
   member functions with `plasma::connect_widget()`; the connection's only virtual slot calls the member function.
6. Each frame the widgets are updated and drawn with the scene (`cube::GameController::render()`, see
   [rendering](rendering.md)); the details are open.

## Client and server

- The UI is client-only. Server.exe has no plasma classes, no widget classes and no `.plx` strings.
- Nothing to correlate; a server-side mod never sees this page's entries.

## Hook points of interest

- `plasma::set_node_widget()` (candidate): every place the game attaches a widget. A mod can see or replace the
  widget that goes on a named node.
- `plasma::load_plx()` (candidate): every scene file the game loads. A mod could load its own `.plx`, or change a
  scene after it is read.
- `cube::GameController::GameController()` (verified, [core](core.md)): after it returns, all HUD widgets exist; the
  place to add a mod's own widgets.
- The `cube::*Widget` constructors (candidate), for example `cube::InventoryWidget::InventoryWidget()`, to change one
  screen.
- `plasma::connect_widget()` (candidate): to add a callback to an existing button.
- `cube::GameController::lookAtNameSite` (candidate): change the name shown for the creature being looked at, for
  example to add its level. A level rework that changes what levels mean would show the new level here.
- `cube::GameController::abilityBarBuiltSite` (verified, [combat](combat.md)): the HUD's ability slots are drawn from
  `cube::GameController.ability_bar` and `cube::GameController.ability_icons` in the same `update` call, so an id
  pushed at this site gets a slot. The HUD loop is bounded by the 7 slot widgets, not by the bar; an id with no icon
  draws a blank grey one (probe `ability-slot4`). The icon map's values are never freed, so a mod's icon object is the
  mod's to keep.

## Open questions

- The fields of `plasma::Widget` (name, node, the connection list, flags) and where each `cube::*Widget` is stored
  in the game controller. Static reading of `plasma::Widget::Widget()` and of the constructor calls in
  `cube::GameController::GameController()`, then the inspector.
- The meaning of the 43 slots of `plasma::Widget` (drawing, input, layout); only 0 and 40 are named.
- How the engine is stepped and drawn each frame, and which side (the game or plasma) sends input to the widgets
  ([input](input.md)).
- The owners of the namespaced free functions: `plasma::create_node()` is probably a `plasma::Engine` method, and
  `plasma::set_node_widget()` and `plasma::clone_node()` are probably `plasma::Node` methods. Rename them (keeping
  aliases) once confirmed.
- What `cube::load_interface_plx()` does with the names button:click, widget:show, window:close ... (UI sounds?), and
  why `interface.plx` has its own reader and a different header.
- The `.plx` format itself (a documented reader would let mods build scenes): the version and demo-license fields,
  and the object records.
- Where `gui.plx` is loaded, and what 0x6565F0 and 0x65E780 (the other callers of `plasma::load_plx()`) load.

## Depth

reconnaissance, reviewed 2026-10-10

## The title screen and the scene tree

Read on 2026-10-10 in the running game at the start menu (probe [title-scene](../probes/title-scene.toml); the full tree
and the draw order from a logging build of a mod). Counts are children; a name in quotes is a `plasma::Node.name`.

**The tree.** `plasma::D3D9Engine.root` is a node named `root` with 18 children, every node of every screen the game
has, as the .plx files at start-up left them.

| Child of root | What it holds |
|---|---|
| an unnamed group, 64 children | the HUD: `minimap`, `landscape`, `info`, `inventory`, `lifebar` (several), `mpbar`, `hpbar`, `castbar`, `experiencebar`, `staminabar`, `ridingbar`, `manacubebar`, `questtag`, `smallquesttag`, `abilitybutton` (6), `menubutton` (6), `equipmentboxleft`/`equipmentboxright` (6 pairs), `crosshair`, `zoomcrosshair`, `speech` (4), `lockedenemy`, `lockedfriend`, and `blackwidget` panels |
| five unnamed groups of 2 to 4 children | the menu panels: each has a `button` or `button2`, a `smallbutton`, a `blackwidget` background, or an edit field (`charactername`, `worldName`, `worldSeed`, `serverName`) with its `text`, `clip` and `Shape` parts, and `connectionError` |
| an unnamed group, 4 children: **the start scene** | `cubeworld` (the logo), `picroma` (the credit), an empty node, and a `blackwidget` of 21 children (nine `leftbutton`/`rightbutton` pairs and three `button2`, probably character creation) |
| `textFX`, `wait`, `nameError` | one top-level node each (`wait` and `nameError` have one child) |
| four `button` nodes | top-level buttons, each a `Shape`, a `text` and a `clip`: the clones Back, Multiplayer Worlds..., Connect to server and Connect of the world and connect screens. The title screen's three rows are not nodes (see The start menu below) |
| a group of 1 child, and three small groups | not read |

A button is always the same four-part pattern: a `Shape` (the plate), a `text` (whose one child is the glyph run) and a
`clip`. The game finds a node by name and puts its widget on it with `plasma::set_node_widget()`.

**Where the game keeps nodes.** `cube::GameController` holds about 80 node pointers from +0x800748 to +0x8009F8, the
nodes its constructor looked up by name (the ones with a unique name are mapped: `cube::GameController.logo`,
`cube::GameController.picroma`, `cube::GameController.minimap`, `cube::GameController.wait`, ...), and the engine at
`cube::GameController.engine` (+0x800708). The rest are repeats of `lifebar`, `bar`, `blackwidget`, `button`.

**The logo** (`cube::GameController.logo`, the node `cubeworld` of `start.plx`, 49 nodes; numbered here depth first from
0):

| Nodes | What |
|---|---|
| 1 to 10, 11 to 20, 21 to 30 | three groups of the nine letters `C U B E W O L R D` (the outline, the side, the inline); each letter a leaf with its own shape |
| 31 to 44 | a fourth group (the faces), made of four `Text` nodes (`CUBE`, `W`, `ORL`, `D`) whose children are the glyphs |
| 45, 46, 47 to 48 | a `Text` with no children, a `Shape`, and a `Text` with one glyph child named `D` (node 48): the alpha sign is at the end |

Every node has colour row 0 of `plasma::Display.colour_table` and white (1, 1, 1, 1) at rest, so the logo's colours come
from its shapes, not from the nodes. The art is one ramp of blues: a dark navy for the sides, a vivid blue line round
each face, an azure face that is lighter at the top, and cyan-white where the bevel catches the light.

**The draw order** of one frame is 0, 1 ... 30, then 32 ... 44, then 31, then 45 ... 48, and almost every node is drawn
twice in a row (a shape has a fill drawing and a stroke drawing). Node 31 is a group with a mask: its children are drawn
first, into a second render target of the window's size, and node 31's own call then lays that target over the screen,
cut to the letters and coloured by the group's gradient (the faces' top-to-bottom shading comes from there). Whatever else
is drawn into the target in between (nodes 32 to 44) is taken for letter faces: it is coloured by the gradient and cut
to the box of the letters. Draw over the logo only after node 48, with the back buffer bound.

**What the screen does on its way up** (a native run, Windows 10): the logo's first draw is about 4.4 s after the
process starts, on a plain blue gradient; at about 10 s the 3D world fades in behind it, and the menu buttons are
then drawn over the world. The engine's width and height (`plasma::D3D9Engine.width`, `plasma::D3D9Engine.height`) are
the window's client area, which can differ from what `options.cfg` asks for.

## The start menu

Read on 2026-10-19 (REA on Ghidra, then objdump) for task 2.1 of `add-mod-manager`; probe [start-menu](../probes/start-menu.toml).

**No buttons.** Start Game, Options and Exit are not plasma nodes. `cube::GameController.start_menu` is a
`cube::StartMenuWidget` on an unnamed node (`cube::GameController.start_menu_node`) in the start scene, and its draw
(`cube::StartMenuWidget::draw`, vtable slot 1, the only slot it overrides) writes the three rows as text each frame,
in the font `resource1.dat` at size 18, white with a black 4 px outline. The row under the cursor turns cyan.

**Where.** The widget is 200 x 100; `cube::GameController::onResize` puts it at ((window_width - 200) / 2,
window_height / 2 + 50). Inside it the rows are centred at x 100 and y 30, 80 and 130, and the cursor is tested over
the whole width at y 10 to 40, 60 to 90 and 110 to 140 (past the widget's own height). At 1024 x 768 that is
y 444 to 474, 494 to 524 and 544 to 574 on screen.

**The click.** The draw stores the row under the cursor in `cube::StartMenuWidget.hover` (-1 for none). A left
mouse-down in `cube::GameController::onMouseDown`, while `cube::GameController.start_scene` and `start_menu_node` are
both visible, acts on it: 0 hides the start scene and shows `cube::GameController.character_select`, 1 shows
`cube::GameController.options_panel`, 2 sets `cube::GameController.quit_requested`. Any other value is ignored.

**When it shows.** A node is shown or hidden by one word: `plasma::Display.visible_values` at
`plasma::Display.key`. Each update, while the start scene is visible, `start_menu_node` and the logo are visible
exactly while `options_panel` is not. In character creation the start scene itself is hidden.

**Sound.** Neither the draw nor the click path fires an interface event or plays a sound. The names `button:click`
and `button:hover` belong to `cube::load_interface_plx`, which registers handlers that queue events for the
interface (0x585920 adds a record to a list); the start menu does not use them.

**How a row is drawn.** The draw gets the font with `plasma::FontEngine::getFont` (on the engine's font engine at
+0x34, name `resource1.dat`; it skips everything when that is null), brackets the rows with the engine's vtable
+0x40 and +0x44 (given the widget's +0x148), and draws each row with two calls of `plasma::ScalableFont::drawText`,
a thiscall on the font that pops its 13 stack arguments itself:

| # | Argument | The start menu passes |
|---:|---|---|
| 1 | text, a `std::wstring*` | the row's name |
| 2, 3 | x, y (floats, screen pixels) | widget width / 2, then 30, 80 or 130 in the widget's frame |
| 4 | size | 18 |
| 5 | outline width (0 draws none) | 4, then 0 |
| 6 | extra space after each glyph | 0 |
| 7 | extra space between lines | 0 |
| 8 | wrap width, used with flag 0x10 | 0 |
| 9 | fill colour, `float[4]` RGBA | white, then the row's colour |
| 10 | outline colour | opaque black, then zero |
| 11 | a third colour, used only by vector fonts | zero |
| 12 | flags: 1 centre on x, 2 right-align, 0x10 wrap | 1 |
| 13 | snap to whole pixels (bool) | 1 |

So each row is the text with a black 4 px outline, then the fill again on top in white, or (0.2, 1, 1, 1) when
hovered. The x and y are in the widget's frame because the engine's +0x40 call sets the widget's transform.

**Text width (2026-10-24, probe [mod-text](../probes/mod-text.toml)).** `drawText` finds a line's width with `plasma::ScalableFont::measureText` (0x65C8E0),
a thiscall on the font with six stack arguments (`ret 0x18`): the text, `float out_min[2]`, `float out_max[2]`, align flags (0), a start index
(-1 for the whole string) and a bool (true). `out_max.x` comes back as the pen advance of the longest line in font units; the width in pixels
is `out_max.x * size / *(float*)(font + 0xE0)`. A mod that draws a bar at that x after a left-aligned `drawText` sees it land on the last glyph's
edge (replay, size 18: "A VERY LONG MOD NAME THAT MUST BE CLIPPED" is 599 px, "SHORT" 80 px).

**A Mods row between Options and Exit (2026-10-19).** The "Exit" label is the wide literal at 0x71917C that only this draw uses: a mod that rewrites it to "Mods" makes the third row Mods with its own band and hover colour, then draws Exit at y 180 from the end site and turns the game's hover 2 into 3 (Mods) and the new Exit band (y 160 to 190) into 4; a click on 4 sets quit_requested. See the next paragraph for the plain fourth row.

**A fourth row (for a mod).** Because the menu is drawn, not built from nodes, a native extra row needs no cloned
node. `cube::StartMenuWidget::drawEndSite` is after the Exit row, with the font still bound and esi the widget: a mod
that makes the same two calls there for "Mods" at y 180 (hit band 160 to 190, over x 0 to the widget's width) and
sets `hover` to 3 gets the game's font, outline and hover colour. The font pointer is not kept in a register at the
site; call `getFont` again. The game ignores hover 3 on a click, so the mod handles that click itself (a left
mouse-down while the menu is visible and hover is 3). The row reaches y 190, so it fits while window_height / 2 +
50 + 190 is inside the window.

**Tried in a mod (2026-10-19, probes [start-menu-row](../probes/start-menu-row.toml), [-options](../probes/start-menu-row-options.toml), [-exit](../probes/start-menu-row-exit.toml)).** `mods-dev/start_menu_row` hooks the end site by name and draws a row as described (the final build puts Mods third, as above): edi is the font at the site (no second `getFont` needed), the text can be a hand-built `std::wstring` (8 wide chars, size, capacity 7; "Mods" fits), and calling `getCursor` and `getWidth` from the hook works. A click is the `input.mouse_button` event with hover 3 and a draw less than 0.5 s ago (hover keeps its last value while the menu is hidden). On the options panel and in character creation the draw does not run, so nothing is drawn and the click is not counted ([start-menu-row-options](../probes/start-menu-row-options.toml)).

**The cursor and the width.** The draw finds the row under the cursor with two plasma widget calls, both thiscall
on the widget. `plasma::Widget::getCursor` (0x627DD0) writes the cursor in the widget's frame to a `float[2]`: the
engine's `plasma::D3D9Engine.cursor_x` and `cursor_y` (client pixels) minus the widget's world position.
`plasma::Widget::getWidth` (0x626650) returns the width in ST0 (200 here). A mod's fourth row can call both, or
compute the same from the engine's cursor and the widget's place given under Where.

## A screen of a mod's own (read 2026-10-24)

What a mod needs to replace the title screen with a page of its own, the way Options does, built from the game's own
nodes so it looks like the game. Tried in a replay with `mods-dev/start_menu_row` 0.2.0
([mods-page](../probes/mods-page.toml)).

**Hiding the title.** `cube::GameController::update` shows `start_menu_node` and `logo` again on every update while
the start scene is visible and `options_panel` is hidden, so a mod cannot hide them once. It can at
`cube::GameController::startMenuShownSite`, the join right after that block ([esp+0x10] is the controller): a mid hook
that writes both visible values to 0 there runs before the frame is drawn, so the menu's draw never runs (0 draws in
766 updates) and nothing flickers. With `start_menu_node` hidden the game also ignores clicks on the menu rows
(`onMouseDown` needs it visible). Writing nothing restores the title on the next update.

**Game widgets from templates.** The constructor looks about 60 templates up by name with `plasma::Node::findChild` in
`cube::GameController.templates`, then detaches most of them, so after start-up they are only reachable through what
it kept: `cube::GameController.blackwidget_template` (the dark panel with the white stepped edge) and the game's own
clones, such as `cube::GameController.creation_back_button` (a `button`) and the arrow pickers on `options_panel`
(clones keep the template's name, so `findChild` finds `leftbutton` and `rightbutton` there, 19 x 21 each).
`plasma::Node::clone(parent)` copies a node, its children and its widget under any parent:

- the clone keeps the source's visible value, so clones of a hidden node start hidden and must be shown;
- the widget's copy starts with no click connection, so a clone of a connected button does nothing when clicked, and
  its own hover still works (the game's grey gradient);
- `plasma::Widget::setSize` and `plasma::Widget::setPosition` (thiscall, x, y, then 1) size and place the clone's
  widget in its parent's frame, so a button cloned under a panel moves with it;
  `plasma::Widget::getSizeWidth` and `getSizeHeight` read the size back;
- `plasma::Node::setText` (a `std::wstring*`, short labels fit the 8-character buffer) changes a button's label.

**Clicks.** A mod tests its buttons itself: on `input.mouse_button` it queues the click, and at the update site it
compares `plasma::Widget::getCursor` of each button with its size. Keeping every scene change (clone, show, hide) in
the update site keeps it out of the draw and out of the input handler.

## Level rework sites

Sites on the skill and respec screens that were mapped for `add-level-rework`. The mod does not hook them. Addresses are `Cube.exe`.

- `cube::SkillWidget::pointsLimitSite` (0x4DCCDF) and `cube::GameController::skillPointLimitSite` (0x47BE34): the points from levels are
  `2 * level - 2`, so a level-N character has as many skill points as a vanilla level-N character, as the `level-progression` spec says.
  The mana cubes add one per four (Creature+0x1164 / 4). The two sites are the display and the click check of that limit (probe
  `skill-points`).
- `cube::SkillWidget::respecCostLevelSite` (0x4DEAF3) and `cube::SkillWidget::respecCostSpecLevelSite` (0x4DEB72): the player's level as a
  float before `stat_scale`, in the respec price. The price is not an item stat, so the item-levels spec and decision 7 of the design keep
  it as vanilla computes it: it takes the mod level N as a vanilla level, and a level-50 character pays a respec as power 72. Static only.

## The skill tree and a fourth ability

Read for `add-ability-api` (task 8.1, M9; probe [ability-skilltree](../probes/ability-skilltree.toml), 2026-10-05). The vanilla tree shows three
ability buttons; the game has a fourth, hidden one.

- The panel has 11 buttons, `cube::GameController.skill_buttons`, one per `cube::CreatureData.skills` entry. Buttons 0 to 5 are the pet, climbing
  and swimming skills, 6 to 8 are ABILITY1 to ABILITY3 and 9 is ABILITY4 (10 is ABILITY5). They are laid out as four rows: 6, 7, 8 and 9 in one
  row, then the pairs 0 and 1, 2 and 3, 4 and 5. Button 9 sits one step to the right of button 8.
- The per-button loop of `cube::GameController::update` (0x48E6B0 to 0x48ECDC) gives each button its icon: buttons 6, 7 and 8 take it from
  `cube::GameController.ability_icons` by class (and, for 8, by specialization), every other button from `cube::GameController.skill_icons`
  by its index. A null icon draws nothing. The loop shows buttons 0 to 9 and hides 10; at 0x48ED0E the game then hides button 9 on every frame
  (`cube::GameController::skillButton9HideSite` is the point after it). So the fourth ability is hidden twice: no icon in the map, and the flag.
- "By specialization" means the panel's working specialization, the int32 at SkillWidget+0x18C (`cube::GameController.skill_widget`), not the
  player's: a click on a specialization button sets it, LEARN copies it to Creature+0x141, CANCEL and the panel's opening put the player's back.
  Key 9 of `skill_icons` is one node for every specialization, so Kyubu replaces the value it wrote there when the shown specialization's
  fourth ability changes (probe su-spec-switch-0720, 2026-11-09: Guardian's icon, then Berserker's on the click, before LEARN).
- With an icon in the node of key 9 and 1 written into the flag at the site, button 9 draws in the ability row. A click adds a rank to the working
  copy (the click handler already treats index 9: it needs skill 8 at rank 5, and its remove branch needs skill 9 at 0 before skill 8 can drop),
  a rank counts against the points, and LEARN copies all 11 ranks to the creature, so `skills[9]` is written by the panel. Probe: 0 to 3, then
  rank 3 again after closing and reopening. The other writers of the 11 ranks are block copies (save and network read, creature copy), found
  statically.
- The click handler (0x47BB30, add branch) has no ceiling for any button: a rank is added while the points allow and the
  prerequisite (skill 8 at 5 for button 9) holds. A mod ability with `max_rank` 3 showed LVL 12 in the play-test of 2026-10-12.
  The loader's per-frame hook clamps `SkillWidget+0x160+9*4` (the working copy) to `max_rank`.
- Still missing for a usable ability: nothing reads `skills[9]` on the skill bar (the learned gate reads skills 6 to 8).
- The tooltip of button 9 shows the level and no text until a name is put into the World's skill-name map (below).

### Skill tooltip text (read 2026-10-12, decompile of 0x4A58D0, 0x5A4380, 0x4E3C90, 0x478D30 and the World constructor)

- `0x4A58D0` builds the tooltip for a skill index. Buttons 6, 7 and 8 take a name from a table by class (the ability-name map at
  World+0x80011C, read by 0x593440). Every other index reads the skill-name map at World+0x800114, a `std::map<int, wstring>`
  that the World constructor (0x58D370) fills with indexes 0 to 5 (SkillPetTaming to SkillBoatDriving) and 10, and with
  nothing for 9. The names are the suffixes of the dictionary keys.
- With the name, 0x4A58D0 asks the text engine (GameController+0x30C, `0x4E3C90`) for `description:<name>`, `skill:level`,
  `details:<name>` and `skill:nextlevel`, with a map of @placeholders from `skill_description_fill` (0x478D30, arguments: the
  skill index, a rank, a map object whose string map is at +8). The fill is called twice: for the current rank and for the next.
  It fills every placeholder of every vanilla ability whatever the index is; a placeholder nobody fills is left as it is.
- The text engine's templates are the entries of `dict_en.xml` in `data4.db`, read once at startup by 0x4E0B30 (the first
  read the game makes). Kyubu merges a mod's `assets/text/dict_en.xml` into it by key, so a mod adds `description:<name>` and
  `details:<name>` by file; `[Title $stress]`, `%` between paragraphs and `[@placeholder $number]` are the markup.
- A mod therefore needs three things: a name at index 9 (`wstring_map_index` 0x4E1FE0 returns the entry of a key, inserting an
  empty one; `wstring_assign_cstr` 0x468560 assigns it), the dictionary entries, and, for per-rank numbers, a wrapper of
  `skill_description_fill` that stores its own placeholders through `wstring_map_index_ws` (0x468DD0) with keys made by
  `wstring_from_cstr` (0x40EDA0). SkillsUnleashed does all three (`src/lib/tooltip.lua`); seen working 2026-10-12.
- **A placeholder name of the mod's own is not resolved.** With `@uppercutcooldown` and two more in the map (a dump of the
  map in the log showed them with the right values, in order, next to the game's 39) the text showed them blank, while
  `@smashcooldown` in the same text showed its number. The cause was not found by reading (the map copy 0x4DF3F0 is a
  structural clone; the lookup in 0x4E35C0 is the same call for every name). The working way: write the values under
  placeholder names the game fills itself, and use those names in the dictionary text. The map is built again for every
  tooltip, so this changes no other tooltip.
- **The box has a fixed height.** The longest vanilla ability text is about 18 rows of 30 characters; a text of 25 rows
  ran below the box. Keep a mod's text under that, the numbers included.
- The skill tree lets the rank go past the ability's own maximum (the log shows ranks 6 to 10); the numbers clamp.
- The per-level point grant is not a grant: the budget is `2 * level - 2`, computed (probe `skill-points`, above).
