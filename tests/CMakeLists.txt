file(GLOB KYUBU_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/hook/*.cpp)
# kyubu_probe's tools are tested in-process, against the real API table (mods-dev/CMakeLists.txt).
add_executable(kyubu_tests ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp ${KYUBU_TEST_SOURCES} ${KYUBU_PROBE_CORE} ${KYUBU_ANIMATOR_CORE} ${KYUBU_VOXEDIT_CORE})
target_link_libraries(kyubu_tests PRIVATE kyubu_core monocypher ${KYUBU_STATIC_LINK})
target_include_directories(kyubu_tests PRIVATE ${CMAKE_SOURCE_DIR}/mods-dev/kyubu_probe/src
                           ${CMAKE_SOURCE_DIR}/mods-dev/kyubu_probe/include
                           ${CMAKE_SOURCE_DIR}/mods-dev/anim_capture/src
                           ${CMAKE_SOURCE_DIR}/tools/animator
                           ${CMAKE_SOURCE_DIR}/tools/voxedit)
target_compile_definitions(kyubu_tests PRIVATE KYUBU_SOURCE_DIR="${CMAKE_SOURCE_DIR}")   # loader/lua/cubeworld.lua
if(NOT MSVC)
  # dummy hook targets must keep their frame-pointer prologues
  target_compile_options(kyubu_tests PRIVATE -O1 -fno-omit-frame-pointer -fno-inline-functions)
endif()
add_test(NAME kyubu_tests COMMAND kyubu_tests)

# Fixture mods used by in-game checks (see tests/fixtures/mods/*/mod.toml descriptions).
set(FX ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mods)
kyubu_native_mod(fixture-mods ${FX} fx_fault_frame ${FX}/fx_fault_frame/src/main.cpp)
kyubu_native_mod(fixture-mods ${FX} fx_crash ${FX}/fx_crash/src/main.cpp)
foreach(m fx_script_error fx_restricted fx_missing_dep fx_requires_server fx_patch_a fx_patch_b fx_dup_a fx_dup_b fx_hot)
  kyubu_stage_mod(fixture-mods ${FX} ${m})
endforeach()
kyubu_stage_mod(fixture-mods ${FX} fx_features)
kyubu_stage_mod(fixture-mods ${FX} fx_parity)
kyubu_stage_mod(fixture-mods ${FX} fx_slow)
kyubu_stage_mod(fixture-mods ${FX} fx_input)
foreach(m fx_globals_a fx_globals_b fx_asset_watch fx_net)
  kyubu_stage_mod(fixture-mods ${FX} ${m})
endforeach()
