cmake_minimum_required(VERSION 3.20)
project(fo2vr CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "FlatOut 2 is a 32-bit game: configure from an x86 developer environment")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Game folder to install into (scripts\ is loaded by the ASI loader). Defaults to the parent of
# this repository.
set(FO2_GAME_DIR "${CMAKE_SOURCE_DIR}/.." CACHE PATH "FlatOut 2 install folder")

set(OPENXR_DIR "${CMAKE_SOURCE_DIR}/third_party/openxr")
set(VULKAN_DIR "${CMAKE_SOURCE_DIR}/third_party/vulkan")

add_library(FO2VR SHARED
  src/background.cpp
  src/d3dcreate.cpp
  src/dllmain.cpp
  src/eyeshare.cpp
  src/pacing.cpp
  src/profile.cpp
  src/record.cpp
  src/render.cpp
  src/screenshot.cpp
  src/util.cpp
  src/xr.cpp
)
set_target_properties(FO2VR PROPERTIES SUFFIX ".asi")
target_include_directories(FO2VR PRIVATE "${OPENXR_DIR}/include" "${VULKAN_DIR}/include")
target_compile_definitions(FO2VR PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE VK_NO_PROTOTYPES)
target_compile_options(FO2VR PRIVATE /W4 /permissive- /utf-8)
# The OpenXR loader sits next to the ASI (scripts\), not next to the game, so it is delay-loaded
# and loaded by full path before the first call.
target_link_libraries(FO2VR PRIVATE "${OPENXR_DIR}/lib/openxr_loader.lib" delayimp)
target_link_options(FO2VR PRIVATE /SAFESEH:NO /DELAYLOAD:openxr_loader.dll)

# Keep the build stamp in the log current.
add_custom_command(TARGET FO2VR PRE_BUILD COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_SOURCE_DIR}/src/util.cpp")

# Tells DXVK which Vulkan extensions the OpenXR runtime needs (see src/wineopenxr/wineopenxr.cpp).
add_library(wineopenxr SHARED src/wineopenxr/wineopenxr.cpp src/wineopenxr/wineopenxr.def)
target_compile_definitions(wineopenxr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(wineopenxr PRIVATE /W4 /permissive-)
add_dependencies(FO2VR wineopenxr)

add_custom_command(TARGET FO2VR POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${FO2_GAME_DIR}/scripts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:FO2VR>" "${FO2_GAME_DIR}/scripts/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:FO2VR>" "${FO2_GAME_DIR}/scripts/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:wineopenxr>" "${FO2_GAME_DIR}/scripts/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENXR_DIR}/bin/openxr_loader.dll" "${FO2_GAME_DIR}/scripts/"
  COMMENT "Installing FO2VR.asi into ${FO2_GAME_DIR}/scripts"
)
