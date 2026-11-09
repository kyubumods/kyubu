# kyubu-voxedit: a voxel model editor beside the Animator (tools/animator). It opens a .cub, or every model of a rig at its rest
# pose, and edits voxels: add, remove, paint, fill, box, mirror, slice. It reuses the Animator's camera, .cub mesher and rig solver.
# The pure code (voxel, doc) is also built into kyubu_tests (tests/CMakeLists.txt).
set(KYUBU_VOXEDIT_CORE ${CMAKE_CURRENT_SOURCE_DIR}/voxel.cpp ${CMAKE_CURRENT_SOURCE_DIR}/doc.cpp PARENT_SCOPE)

set(A ${CMAKE_SOURCE_DIR}/tools/animator)
add_executable(kyubu-voxedit WIN32 main.cpp app.cpp voxel.cpp doc.cpp ${A}/camera.cpp ${A}/cubmesh.cpp)
target_include_directories(kyubu-voxedit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${A} ${CMAKE_SOURCE_DIR}/loader/src/game
  ${CMAKE_SOURCE_DIR}/loader/src/core ${CMAKE_SOURCE_DIR}/third_party/toml ${CMAKE_SOURCE_DIR}/third_party/json)
target_compile_definitions(kyubu-voxedit PRIVATE KYUBU_VOXEDIT_VERSION="${PROJECT_VERSION}")
target_link_libraries(kyubu-voxedit PRIVATE kyubu_rig imgui d3d9 gdi32 dwmapi imm32 comdlg32 shell32 ${KYUBU_STATIC_LINK})   # no MinGW runtime DLLs beside it
