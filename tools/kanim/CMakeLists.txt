# kanim: the clip checker. It links the loader's own evaluator and validator source (no copy, no Python twin), and nothing of the
# game or the loader's Windows code, so it also builds and runs on a machine with no game.
add_executable(kanim main.cpp)
target_include_directories(kanim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/loader/src/game
  ${CMAKE_SOURCE_DIR}/loader/src/core ${CMAKE_SOURCE_DIR}/third_party/toml ${CMAKE_SOURCE_DIR}/third_party/json)
target_link_libraries(kanim PRIVATE kyubu_rig ${KYUBU_STATIC_LINK})   # no MinGW runtime DLLs beside it
