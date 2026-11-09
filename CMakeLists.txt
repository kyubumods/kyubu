cmake_minimum_required(VERSION 3.20)
project(kyubu VERSION 1.0.1 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Kyubu must be built for 32-bit x86 (the game is 32-bit). Use -A Win32 with MSVC or cmake/mingw-i686.cmake.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

option(KYUBU_BUILD_TESTS "Build unit and hook-engine tests" ON)

if(MSVC)
  add_compile_options(/W3 /utf-8 /EHsc)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT
  add_link_options(/SAFESEH:NO)
else()
  add_compile_options(-Wall -Wno-unused-function -Wno-cast-function-type -fno-omit-frame-pointer)
  set(KYUBU_STATIC_LINK -static -static-libgcc -static-libstdc++)
endif()
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 UNICODE _UNICODE)

set(TP ${CMAKE_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------- third-party
add_library(minhook STATIC
  ${TP}/minhook/src/buffer.c ${TP}/minhook/src/hook.c ${TP}/minhook/src/trampoline.c
  ${TP}/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${TP}/minhook/include PRIVATE ${TP}/minhook/src)

add_library(miniz STATIC ${TP}/miniz/miniz.c)
target_include_directories(miniz PUBLIC ${TP}/miniz)

# Monocypher: Ed25519 verification of the mod catalog (add-mod-manager D12). Signing is in tools/kcat.
add_library(monocypher STATIC ${TP}/monocypher/monocypher.c ${TP}/monocypher/monocypher-ed25519.c)
target_include_directories(monocypher PUBLIC ${TP}/monocypher)

add_library(kyubu_headers INTERFACE)
target_include_directories(kyubu_headers INTERFACE
  ${CMAKE_SOURCE_DIR}/sdk/include ${TP}/json ${TP}/toml ${TP}/doctest)

include(cmake/luajit.cmake)
include(cmake/imgui.cmake)
include(cmake/kyubu_mod.cmake)

add_subdirectory(loader)
add_subdirectory(tools/kanim)
add_subdirectory(tools/animator)
add_subdirectory(tools/voxedit)
add_subdirectory(mods-dev)
if(KYUBU_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
add_subdirectory(mods-examples)
add_subdirectory(mods-gameplay)
# Mods kept in their own repositories, built against this checkout: -DKYUBU_MODS="<mod repo>;<mod repo>".
# Each repository's CMakeLists.txt is added here, so it uses kyubu_native_mod, the SDK and third_party/ of this tree.
set(KYUBU_MODS "" CACHE STRING "folders of mod repositories to build with Kyubu")
foreach(dir IN LISTS KYUBU_MODS)
  get_filename_component(name "${dir}" NAME)
  add_subdirectory("${dir}" "${CMAKE_BINARY_DIR}/mod-repos/${name}")
endforeach()
