/* Compiles the generated cubeworld headers (kmap export) as C and checks the __thiscall typedef form and layout.h. */
#include "cubeworld/functions.h"
#include "cubeworld/cubeworld.h"
#include "cubeworld/layout.h"

static void CW_FASTCALL on_update(cube_GameController* self, void* edx, int32_t dt_ms) {
    (void)self; (void)edx; (void)dt_ms;
}

cw_cube_GameController_update_fn cubeworld_check_update = on_update;
const char* cubeworld_check_name = CW_SYM_cube_GameController_update;
size_t cubeworld_check_hp = offsetof(cube_CreatureData, hp);
const CwLayoutType* cubeworld_check_layout = cw_layout_types;
const CwLayoutEnum* cubeworld_check_enums = cw_layout_enums;
size_t cubeworld_check_layout_count = CW_LAYOUT_TYPE_COUNT + CW_LAYOUT_ENUM_COUNT;
