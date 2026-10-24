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
  src/controllers.cpp
  src/d3dcreate.cpp
  src/dialog.cpp
  src/dllmain.cpp
  src/eyeshare.cpp
  src/fo2vr.manifest
  src/largeaddress.cpp
  src/pacing.cpp
  src/profile.cpp
  src/record.cpp
  src/render.cpp
  src/screenshot.cpp
  src/sound.cpp
  src/util.cpp
  src/xr.cpp
)
set_target_properties(FO2VR PROPERTIES SUFFIX ".asi")
target_include_directories(FO2VR PRIVATE "${OPENXR_DIR}/include" "${VULKAN_DIR}/include")
target_compile_definitions(FO2VR PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE VK_NO_PROTOTYPES)
target_compile_options(FO2VR PRIVATE /W4 /permissive- /utf-8)
# The OpenXR loader sits next to the ASI (scripts\), not next to the game: it is loaded by full path and its
# functions are taken through xrGetInstanceProcAddr (src/xr.cpp, which may load the runtime without it).
target_link_options(FO2VR PRIVATE /SAFESEH:NO)

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

# Other copies of the game kept on the current build too (e.g. a Steam copy beside the GOG one): their folders, one a
# line, in extra-game-dirs.txt (not in git; # starts a comment). Each gets what a player installs from a release (not
# the PDB); its FO2VR.ini stays as it is.
set(FO2_EXTRA_LIST "${CMAKE_SOURCE_DIR}/extra-game-dirs.txt")
set(FO2_EXTRA_GAME_DIRS)
if(EXISTS "${FO2_EXTRA_LIST}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FO2_EXTRA_LIST}")
  file(STRINGS "${FO2_EXTRA_LIST}" FO2_EXTRA_GAME_DIRS)
endif()
set(extra_install)
foreach(dir IN LISTS FO2_EXTRA_GAME_DIRS)
  string(STRIP "${dir}" dir)
  if(dir STREQUAL "" OR dir MATCHES "^#")
    continue()
  endif()
  file(TO_CMAKE_PATH "${dir}" dir)
  if(NOT EXISTS "${dir}/FlatOut2.exe")
    message(WARNING "extra-game-dirs.txt: ${dir} has no FlatOut2.exe - the build is not installed there")
    continue()
  endif()
  list(APPEND extra_install
    COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}/scripts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:FO2VR>" "$<TARGET_FILE:wineopenxr>"
            "${OPENXR_DIR}/bin/openxr_loader.dll" "${dir}/scripts/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/README.md" "${dir}/scripts/FO2VR-README.md")
endforeach()
if(extra_install)
  add_custom_target(install_extra ALL ${extra_install} COMMENT "Installing FO2VR.asi into the other copies of the game")
  add_dependencies(install_extra FO2VR)
endif()
