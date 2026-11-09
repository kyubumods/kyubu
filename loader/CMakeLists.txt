file(GLOB_RECURSE KYUBU_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

# Embed the Lua prelude as a C string.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/lua/prelude.lua KYUBU_PRELUDE)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/prelude_lua.h.tmp
  "// Generated from src/lua/prelude.lua\nstatic const char kPreludeLua[] = R\"KYUBUPRELUDE(${KYUBU_PRELUDE})KYUBUPRELUDE\";\n")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/gen/prelude_lua.h.tmp ${CMAKE_CURRENT_BINARY_DIR}/gen/prelude_lua.h COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/lua/prelude.lua)

set(KYUBU_SYSTEM_LIBS dbghelp psapi shell32 user32 gdi32 ws2_32 d3d9 ole32 imm32 dwmapi)

# Everything lives in a static library (tests link it); the proxy DLL links it whole.
add_library(kyubu_core STATIC ${KYUBU_CORE_SOURCES})
target_include_directories(kyubu_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src/hook ${CMAKE_CURRENT_SOURCE_DIR}/src/mods
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api ${CMAKE_CURRENT_SOURCE_DIR}/src/events ${CMAKE_CURRENT_SOURCE_DIR}/src/assets
  ${CMAKE_CURRENT_SOURCE_DIR}/src/registry ${CMAKE_CURRENT_SOURCE_DIR}/src/lua ${CMAKE_CURRENT_SOURCE_DIR}/src/sides
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dev ${CMAKE_CURRENT_SOURCE_DIR}/src/game ${CMAKE_CURRENT_BINARY_DIR}/gen
  ${CMAKE_SOURCE_DIR}/third_party/minhook/src)
target_link_libraries(kyubu_core PUBLIC kyubu_headers minhook miniz luajit imgui ${KYUBU_SYSTEM_LIBS})
target_compile_definitions(kyubu_core PRIVATE KYUBU_VERSION_STRING="${PROJECT_VERSION}")

# The rig solver of the Animator (tools/animator/rig.cpp): creature bodies pose with it (openspec add-dragon-boss task 3.3).
target_sources(kyubu_core PRIVATE ${CMAKE_SOURCE_DIR}/tools/animator/rig.cpp)
target_include_directories(kyubu_core PUBLIC ${CMAKE_SOURCE_DIR}/tools/animator)

# The proxy: zlib1.dll with all zlib exports forwarded to zlib1_vanilla.dll.
add_library(kyubu_proxy SHARED ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zlib1.def)
set_target_properties(kyubu_proxy PROPERTIES OUTPUT_NAME zlib1 PREFIX "" LINKER_LANGUAGE CXX)
if(MSVC)
  target_link_libraries(kyubu_proxy PRIVATE kyubu_core)
  target_link_options(kyubu_proxy PRIVATE /WHOLEARCHIVE:kyubu_core)
  # link.exe only emits a forwarder "name = zlib1_vanilla.name" when "name" resolves to some symbol.
  # An import library for zlib1_vanilla with the same names provides that; the DLL still gets pure
  # forwarders and no load-time import of zlib1_vanilla (checked on Windows with MSVC 19.35).
  include(${CMAKE_SOURCE_DIR}/cmake/zlib_vanilla_implib.cmake)
  kyubu_zlib_vanilla_implib(${CMAKE_CURRENT_SOURCE_DIR}/src/core/zlib1.def ${CMAKE_CURRENT_BINARY_DIR} ZLIB_VANILLA_LIB)
  add_custom_target(zlib1_vanilla_implib DEPENDS ${ZLIB_VANILLA_LIB})
  add_dependencies(kyubu_proxy zlib1_vanilla_implib)
  target_link_libraries(kyubu_proxy PRIVATE ${ZLIB_VANILLA_LIB})
else()
  target_link_libraries(kyubu_proxy PRIVATE -Wl,--whole-archive kyubu_core -Wl,--no-whole-archive
    kyubu_headers minhook miniz luajit imgui ${KYUBU_SYSTEM_LIBS} ${KYUBU_STATIC_LINK})
  target_link_options(kyubu_proxy PRIVATE -Wl,--enable-stdcall-fixup)
endif()
add_dependencies(kyubu_core luajit_ext)
