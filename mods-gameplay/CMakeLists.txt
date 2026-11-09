# ModManager, the mod the player release carries (scripts/package.py; the installer puts it in mods/). The gameplay
# mods have their own repositories, built with -DKYUBU_MODS (CMakeLists.txt at the top).
kyubu_native_mod(gameplay-mods ${CMAKE_CURRENT_SOURCE_DIR} mod_manager mod_manager/src/main.cpp mod_manager/src/plan.cpp ${CMAKE_SOURCE_DIR}/loader/src/mods/semver.cpp ${CMAKE_SOURCE_DIR}/loader/src/core/sha256.cpp)
target_include_directories(mod_manager PRIVATE ${CMAKE_SOURCE_DIR}/loader/src)
target_link_libraries(mod_manager PRIVATE shell32 winhttp monocypher miniz)
target_include_directories(mod_manager PRIVATE ${TP}/json ${TP}/monocypher)
# The fixture catalog is signed with tests/fixtures/catalog-src/test.key, so replays and tests need KYUBU_MM_TEST_KEY (mod_manager/src/keys.h).
# Release guard (add-mod-manager D16): a release configure must say -DKYUBU_RELEASE=ON -DKYUBU_MM_TEST_KEY=OFF, or it stops here.
# `kcat release` and scripts/package.py also refuse a DLL that holds the test key's bytes.
option(KYUBU_RELEASE "Configure for a release: the test public key must be off" OFF)
option(KYUBU_MM_TEST_KEY "ModManager also trusts the fixture catalog's test public key" ON)
if(KYUBU_MM_TEST_KEY)
  if(KYUBU_RELEASE)
    message(FATAL_ERROR "KYUBU_RELEASE is on but KYUBU_MM_TEST_KEY is on too: configure with -DKYUBU_MM_TEST_KEY=OFF so ModManager does not trust the test key")
  endif()
  target_compile_definitions(mod_manager PRIVATE KYUBU_MM_TEST_KEY)
endif()

