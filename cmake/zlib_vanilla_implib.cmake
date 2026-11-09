# Builds an import library for zlib1_vanilla.dll from the proxy's .def file (MSVC only; see loader/CMakeLists.txt).
function(kyubu_zlib_vanilla_implib proxy_def out_dir out_var)
  file(STRINGS ${proxy_def} lines REGEX "^[ \t]+[A-Za-z0-9_]+[ \t]*=[ \t]*zlib1_vanilla\\.")
  set(content "LIBRARY zlib1_vanilla\nEXPORTS\n")
  foreach(line IN LISTS lines)
    string(REGEX REPLACE "^[ \t]+([A-Za-z0-9_]+).*" "\\1" name "${line}")
    string(APPEND content "  ${name}\n")
  endforeach()
  list(LENGTH lines count)
  if(count EQUAL 0)
    message(FATAL_ERROR "no forwarded exports found in ${proxy_def}")
  endif()
  file(WRITE ${out_dir}/zlib1_vanilla.def "${content}")
  set(lib ${out_dir}/zlib1_vanilla.lib)
  add_custom_command(OUTPUT ${lib}
    COMMAND ${CMAKE_AR} /nologo /machine:x86 /def:${out_dir}/zlib1_vanilla.def /out:${lib}
    DEPENDS ${proxy_def}
    COMMENT "Import library for zlib1_vanilla.dll (${count} names)"
    VERBATIM)
  set(${out_var} ${lib} PARENT_SCOPE)
endfunction()
