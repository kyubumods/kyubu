# Helpers to build and stage mods. A staged mod lives in ${CMAKE_BINARY_DIR}/<root>/<id>/ and can be copied
# straight into a game's mods/ folder.
function(kyubu_stage_mod root src id)
  set(dst ${CMAKE_BINARY_DIR}/${root}/${id})
  add_custom_target(stage_${id} ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${dst}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${src}/${id} ${dst}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${dst}/src)
endfunction()

function(kyubu_native_mod root src id)
  add_library(${id} SHARED ${ARGN})
  set_target_properties(${id} PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${root}/${id}/bin
                                           RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${root}/${id}/bin)
  target_include_directories(${id} PRIVATE ${CMAKE_SOURCE_DIR}/sdk/include)
  target_compile_features(${id} PRIVATE cxx_std_20)
  if(NOT MSVC)
    target_link_options(${id} PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
  # Native mods do their float math with SSE2, not the i686 default x87 (MSVC's x86 default is SSE2 already). Direct3D 9
  # puts the x87 unit in single precision, which made a timer step in sixteenths of a second (title_plus, 2026-10-25),
  # and each x87 float-to-int conversion switches the rounding mode (EMTweaks was 4 to 7 times slower, 2026-10-19).
  # MinGW's i686 target assumes 4-byte stack alignment on entry and realigns where SSE needs 16, so the game can call in.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${id} PRIVATE -msse2 -mfpmath=sse)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${id} PRIVATE -msse2)
  endif()
  kyubu_stage_mod(${root} ${src} ${id})
  add_dependencies(${id} stage_${id})
endfunction()
