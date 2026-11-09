// Compiles the generated cubeworld headers (kmap export) as C++ and checks the __thiscall typedef form and layout.h.
#include "cubeworld/functions.h"
#include "cubeworld/cubeworld.h"
#include "cubeworld/layout.h"

static void CW_FASTCALL on_update(cube_GameController* self, void* edx, int32_t dt_ms) { (void)self; (void)edx; (void)dt_ms; }

extern cw_cube_GameController_update_fn cubeworld_check_update;
cw_cube_GameController_update_fn cubeworld_check_update = on_update;
static_assert(sizeof(cube_CreatureData) == 0x1168, "cube::CreatureData size");
extern const CwLayoutType* cubeworld_check_layout;
const CwLayoutType* cubeworld_check_layout = cw_layout_types;
extern const CwLayoutEnum* cubeworld_check_enums;
const CwLayoutEnum* cubeworld_check_enums = cw_layout_enums;
static_assert(CW_LAYOUT_TYPE_COUNT > 0, "layout.h describes the structs");
