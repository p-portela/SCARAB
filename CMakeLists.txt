cmake_minimum_required(VERSION 3.20)
project(consoleTranslationLayer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Artifacts: the Microsoft binaries this project builds against and the game
# data it analyses. They are NEVER vendored into the repository (see
# docs/PROVENANCE.md); scripts/link-artifacts.sh symlinks them in from an
# artifacts directory.
# ---------------------------------------------------------------------------
set(XTL_ARTIFACTS "$ENV{XTL_ARTIFACTS}" CACHE PATH
    "Directory holding the vendor/ and corpora/ trees (default: ~/Documents/scarab-artifacts)")
if(NOT XTL_ARTIFACTS)
  set(XTL_ARTIFACTS "$ENV{HOME}/Documents/scarab-artifacts")
endif()
set(XTL_VENDOR "${CMAKE_SOURCE_DIR}/vendor" CACHE PATH "Vendor tree (Microsoft D3D12 + console SDK for PC)")
set(XTL_CORPORA "${CMAKE_SOURCE_DIR}/corpora" CACHE PATH "Extracted game corpora")

if(NOT EXISTS "${XTL_VENDOR}/x64_dx12/d3d12_xs.dll" AND NOT XTL_ALLOW_MISSING_ARTIFACTS)
  message(WARNING
    "Vendor tree not found at ${XTL_VENDOR}.\n"
    "  Run scripts/link-artifacts.sh first (it symlinks from XTL_ARTIFACTS=${XTL_ARTIFACTS}),\n"
    "  or configure with -DXTL_ALLOW_MISSING_ARTIFACTS=ON to build only what does not need it.")
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(components/runtime-bridge)
add_subdirectory(components/extension-wrapper)

# Optionally build the test executables that can run under Wine on this host.
option(XTL_BUILD_TESTS "Build the Wine-runnable test executables" ON)
if(XTL_BUILD_TESTS)
  add_subdirectory(tests)
endif()

message(STATUS "consoleTranslationLayer: build type  = ${CMAKE_BUILD_TYPE}")
message(STATUS "consoleTranslationLayer: artifacts   = ${XTL_ARTIFACTS}")
message(STATUS "consoleTranslationLayer: vendor      = ${XTL_VENDOR}")
