# kyubu-animator: the Animator, a standalone program (openspec add-ability-animation, section 6). It is not a mod: it needs no game and
# no loader. It links the loader's own evaluator and validator source, as tools/kanim does, so the Animator, kanim and the game give
# the same numbers for a clip. The pure code (model, rig, camera, cubmesh, posing, cliptext, timeline, keyinspector, checks, poselib, sheet, parity, particles, effects, sounds) is also built into kyubu_tests (tests/CMakeLists.txt).
set(KYUBU_ANIMATOR_CORE ${CMAKE_CURRENT_SOURCE_DIR}/model.cpp ${CMAKE_CURRENT_SOURCE_DIR}/rig.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/camera.cpp ${CMAKE_CURRENT_SOURCE_DIR}/cubmesh.cpp ${CMAKE_CURRENT_SOURCE_DIR}/posing.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cliptext.cpp ${CMAKE_CURRENT_SOURCE_DIR}/timeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/checks.cpp ${CMAKE_CURRENT_SOURCE_DIR}/poselib.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sheet.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/parity.cpp ${CMAKE_CURRENT_SOURCE_DIR}/particles.cpp ${CMAKE_CURRENT_SOURCE_DIR}/effects.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sounds.cpp ${CMAKE_CURRENT_SOURCE_DIR}/keyinspector.cpp PARENT_SCOPE)

# kyubu_rig: the rig solver (rig.cpp) and the clip reader and evaluator (the loader's animation_* and kanim_core sources), one static
# library. The Animator, kanim and kyubu-voxedit link it, and the loader's creature bodies use the same code, so a creature body
# in the game poses exactly as the Animator previews it. kyubu_tests still compiles the sources itself (kyubu_core has the evaluator).
set(G ${CMAKE_SOURCE_DIR}/loader/src/game)
add_library(kyubu_rig STATIC ${CMAKE_CURRENT_SOURCE_DIR}/rig.cpp ${G}/animation_eval.cpp ${G}/animation_load.cpp ${G}/animation_expr.cpp ${G}/kanim_core.cpp)
target_include_directories(kyubu_rig PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${G} ${CMAKE_SOURCE_DIR}/loader/src/core ${CMAKE_SOURCE_DIR}/third_party/toml ${CMAKE_SOURCE_DIR}/third_party/json)

add_executable(kyubu-animator WIN32 main.cpp app.cpp model.cpp camera.cpp cubmesh.cpp posing.cpp cliptext.cpp timeline.cpp keyinspector.cpp checks.cpp poselib.cpp sheet.cpp parity.cpp particles.cpp effects.cpp sounds.cpp audio.cpp video.cpp)
target_include_directories(kyubu-animator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/loader/src/game
  ${CMAKE_SOURCE_DIR}/loader/src/core ${CMAKE_SOURCE_DIR}/third_party/toml ${CMAKE_SOURCE_DIR}/third_party/json
  ${CMAKE_SOURCE_DIR}/mods-dev/anim_capture/src)   # capture_sheet.h: the sheet's layout, font and PNG, one source with `anim capture`
target_compile_definitions(kyubu-animator PRIVATE KYUBU_ANIMATOR_VERSION="${PROJECT_VERSION}"
  KYUBU_ANIMATOR_RIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/rigs")   # the stand-in rig, when no rigs/ folder is beside the program
target_link_libraries(kyubu-animator PRIVATE kyubu_rig imgui miniz d3d9 gdi32 dwmapi imm32 comdlg32 ${KYUBU_STATIC_LINK})   # no MinGW runtime DLLs beside it
# The stand-in rig goes beside the program, so a copied build finds it.
add_custom_command(TARGET kyubu-animator POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
  ${CMAKE_CURRENT_SOURCE_DIR}/rigs $<TARGET_FILE_DIR:kyubu-animator>/rigs)
