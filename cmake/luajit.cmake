# Builds LuaJIT 2.1 as a static library for 32-bit Windows.
#  - MSVC: runs src/msvcbuild.bat static (from a "x86 Native Tools" prompt).
#  - MinGW cross-compile on Linux: make HOST_CC="gcc -m32" CROSS=<prefix> TARGET_SYS=Windows.
#  - MinGW on Windows (e.g. WinLibs): the same, with the compiler's folder as the prefix and a `make` on PATH.
include(ExternalProject)
set(LJ_SRC ${CMAKE_SOURCE_DIR}/third_party/luajit)
set(LJ_BUILD ${CMAKE_BINARY_DIR}/luajit-build)

if(MSVC)
  set(LJ_LIB ${LJ_BUILD}/src/lua51.lib)
  ExternalProject_Add(luajit_ext
    SOURCE_DIR ${LJ_SRC}
    DOWNLOAD_COMMAND ${CMAKE_COMMAND} -E copy_directory ${LJ_SRC} ${LJ_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND cmd /c "cd /d ${LJ_BUILD}/src && msvcbuild.bat static"
    INSTALL_COMMAND ""
    BUILD_IN_SOURCE 0
    BUILD_BYPRODUCTS ${LJ_LIB})
else()
  string(REGEX REPLACE "gcc(-posix)?(\\.exe)?$" "" LJ_CROSS "${CMAKE_C_COMPILER}")
  set(LJ_LIB ${LJ_BUILD}/src/libluajit.a)
  ExternalProject_Add(luajit_ext
    SOURCE_DIR ${LJ_SRC}
    DOWNLOAD_COMMAND ${CMAKE_COMMAND} -E copy_directory ${LJ_SRC} ${LJ_BUILD}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make -C ${LJ_BUILD} -j4 HOST_CC=gcc\ -m32 CROSS=${LJ_CROSS} TARGET_SYS=Windows BUILDMODE=static amalg
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${LJ_LIB})
endif()

add_library(luajit STATIC IMPORTED GLOBAL)
set_target_properties(luajit PROPERTIES IMPORTED_LOCATION ${LJ_LIB})
file(MAKE_DIRECTORY ${LJ_BUILD}/src)
target_include_directories(luajit INTERFACE ${LJ_BUILD}/src)
add_dependencies(luajit luajit_ext)
