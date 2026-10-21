cmake_minimum_required(VERSION 3.20)
project(CircleRoom C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- GLFW (vendored in third_party/glfw) ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)

# --- Dear ImGui (vendored in third_party/imgui) ---
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC glfw)

# --- App ---
file(GLOB APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/ai/*.cpp)
set(APP_RC "")
if(WIN32)
  set(APP_RC ${CMAKE_SOURCE_DIR}/res/circleroom.rc)  # icon + version info
endif()
add_executable(CircleRoom WIN32 ${APP_SOURCES} ${APP_RC})
target_include_directories(CircleRoom PRIVATE src third_party/stb third_party/onnxruntime/include)
target_link_libraries(CircleRoom PRIVATE imgui glfw comdlg32 shell32 ole32 oleaut32 uuid windowscodecs psapi)
if(MINGW)
  target_compile_options(CircleRoom PRIVATE -Wall -Wno-unused-function)
  target_link_options(CircleRoom PRIVATE -static -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
endif()
