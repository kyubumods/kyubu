# Native Kyubu mod template. Rename "my_mod" here, in mod.toml and in src/main.cpp.
#
#   MSVC (Developer prompt):  cmake -B build -A Win32 && cmake --build build --config Release
#   MinGW-w64 (Windows):      cmake -B build -G "MinGW Makefiles" && cmake --build build
#   MinGW-w64 (Linux cross):  cmake -B build -DCMAKE_TOOLCHAIN_FILE=<absolute path to the SDK>/cmake/mingw-i686.cmake && cmake --build build
#
# The staged mod ends up in build/my_mod/ (mod.toml, bin/my_mod.dll, assets/): copy that folder into mods/.
cmake_minimum_required(VERSION 3.20)
project(my_mod CXX)

set(MOD_ID my_mod)
set(KYUBU_SDK "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Kyubu SDK folder (the one containing include/kyubu_api.h)")
if(NOT EXISTS "${KYUBU_SDK}/include/kyubu_api.h")
  message(FATAL_ERROR "kyubu_api.h not found under KYUBU_SDK=${KYUBU_SDK}; pass -DKYUBU_SDK=<path to the SDK>")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Cube World is a 32-bit game: configure with -A Win32 (MSVC) or a 32-bit (i686) MinGW toolchain")
endif()

set(STAGE ${CMAKE_BINARY_DIR}/${MOD_ID})
add_library(${MOD_ID} SHARED src/main.cpp)
set_target_properties(${MOD_ID} PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY ${STAGE}/bin
                      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${STAGE}/bin RUNTIME_OUTPUT_DIRECTORY_DEBUG ${STAGE}/bin)
target_include_directories(${MOD_ID} PRIVATE ${KYUBU_SDK}/include)
target_compile_features(${MOD_ID} PRIVATE cxx_std_20)
if(MSVC)
  # Static CRT: the mod does not depend on a VC++ redistributable (the game itself uses VC110).
  set_property(TARGET ${MOD_ID} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_link_options(${MOD_ID} PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Stage mod.toml, config.toml and assets next to bin/.
add_custom_command(TARGET ${MOD_ID} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/mod.toml ${STAGE}/mod.toml
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/config.toml ${STAGE}/config.toml
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets ${STAGE}/assets)
