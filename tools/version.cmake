# Writes the mod's version, as git tells it, into a header for the build. CMakeLists.txt runs it at every build:
#   cmake -DSOURCE_DIR=<repository> -DOUT=<header> -P tools/version.cmake
# A release is built at its tag (v1.6.14 gives "1.6.14"). Any other build says how far past the last release it is
# and whether its files are changed ("1.6.14-3-g1a2b3c4-dirty": 3 commits after v1.6.14, at commit 1a2b3c4, with
# changes not committed). Without git or its repository (the source as an archive): "unknown".
# The header is written only when the version changes, so that nothing is compiled again for it otherwise.
set(version "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --dirty --match "v[0-9]*"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE described OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE failed)
  if(NOT failed AND described)
    string(REGEX REPLACE "^v" "" version "${described}")
  endif()
endif()

set(text "// The mod's version (tools/version.cmake writes this at every build).\n#define FO2VR_VERSION \"${version}\"\n")
set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT text STREQUAL old)
  file(WRITE "${OUT}" "${text}")
endif()
message(STATUS "FO2VR ${version}")
