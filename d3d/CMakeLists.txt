cmake_minimum_required(VERSION 3.25)

project(atmosphere_d3d12 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "This sample requires Windows and Direct3D 12.")
endif()

find_package(directx-headers CONFIG REQUIRED)
find_package(directx-dxc CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# The reusable part: the atmosphere model and the minimal D3D12 helpers.
add_library(atmosphere_d3d12 STATIC
  src/atmosphere/constants.h
  src/atmosphere/model.cc
  src/atmosphere/model.h
  src/d3d12/common.cc
  src/d3d12/common.h
  src/d3d12/descriptor_heap.cc
  src/d3d12/descriptor_heap.h
  src/d3d12/device.cc
  src/d3d12/device.h
  src/d3d12/gpu_timer.cc
  src/d3d12/gpu_timer.h
  src/d3d12/shader_compiler.cc
  src/d3d12/shader_compiler.h
  src/d3d12/shader_watcher.cc
  src/d3d12/shader_watcher.h
  src/d3d12/texture.cc
  src/d3d12/texture.h
)
target_include_directories(atmosphere_d3d12 PUBLIC src)
target_link_libraries(atmosphere_d3d12 PUBLIC
  Microsoft::DirectX-Headers
  Microsoft::DirectX-Guids
  Microsoft::DirectXShaderCompiler
  d3d12
  dxgi
)
target_compile_definitions(atmosphere_d3d12 PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
                           UNICODE _UNICODE)

# The demo application.
set(SHADER_FILES
  shaders/atmosphere/atmosphere.hlsli
  shaders/atmosphere/definitions.hlsli
  shaders/atmosphere/functions.hlsli
  shaders/atmosphere/precompute.hlsl
  shaders/demo/demo.hlsl
  shaders/demo/lut_view.hlsl
)
# The shaders are compiled at runtime, so they must not be compiled by the
# Visual Studio HLSL build rule.
set_source_files_properties(${SHADER_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
source_group("Shaders" FILES ${SHADER_FILES})

add_executable(atmosphere_demo WIN32
  src/demo/demo.cc
  src/demo/demo.h
  src/demo/main.cc
  src/demo/ui.cc
  ${SHADER_FILES}
)
target_link_libraries(atmosphere_demo PRIVATE atmosphere_d3d12 imgui::imgui
                      windowscodecs)
# Shaders are loaded from the source directory if it exists (so that they can
# be edited and reloaded at runtime), or from the "shaders" directory next to
# the executable otherwise.
target_compile_definitions(atmosphere_demo PRIVATE
  ATMOSPHERE_SHADER_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set_target_properties(atmosphere_demo PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:atmosphere_demo>")
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY
  VS_STARTUP_PROJECT atmosphere_demo)

# Copy the shaders and the DXC runtime libraries (dxil.dll is loaded
# dynamically by dxcompiler.dll, to sign the compiled shaders) next to the
# executable.
add_custom_command(TARGET atmosphere_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "$<TARGET_FILE_DIR:atmosphere_demo>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:Microsoft::DirectXShaderCompiler>"
    "$<TARGET_FILE_DIR:Microsoft::DirectXShaderCompiler>/dxil.dll"
    "$<TARGET_FILE_DIR:atmosphere_demo>"
)

foreach(target atmosphere_d3d12 atmosphere_demo)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /MP)
  endif()
endforeach()
