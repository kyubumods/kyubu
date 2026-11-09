set(IMGUI ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/imgui_demo.cpp
  ${IMGUI}/backends/imgui_impl_dx9.cpp ${IMGUI}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
if(NOT MSVC)
  target_compile_options(imgui PRIVATE -w)
endif()
