# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
cmake_minimum_required(VERSION 3.20)
project(SCARAB LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Everything platform-specific (MSVC vs GNU flags, static runtime, python
# discovery, how a test exe gets launched) lives in one module. Components stay
# declarative; see cmake/scarab-platform.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/scarab-platform.cmake)

# ---------------------------------------------------------------------------
# Artifacts: the Microsoft binaries this project builds against and the game
# data it analyses. They are NEVER vendored into the repository;
# scripts/link-artifacts.{sh,ps1} link them in from an artifacts directory.
# ---------------------------------------------------------------------------
set(SCARAB_ARTIFACTS "$ENV{SCARAB_ARTIFACTS}" CACHE PATH
    "Directory holding the vendor/ and corpora/ trees (default: ~/Documents/scarab-artifacts)")
if(NOT SCARAB_ARTIFACTS)
  set(SCARAB_ARTIFACTS "$ENV{USERPROFILE}/Documents/scarab-artifacts")
  if(NOT EXISTS "${SCARAB_ARTIFACTS}")
    set(SCARAB_ARTIFACTS "$ENV{HOME}/Documents/scarab-artifacts")
  endif()
endif()
set(SCARAB_VENDOR "${CMAKE_SOURCE_DIR}/vendor" CACHE PATH
    "Vendor tree (Microsoft D3D12 + console SDK for PC)")
set(SCARAB_CORPORA "${CMAKE_SOURCE_DIR}/corpora" CACHE PATH "Extracted game corpora")

if(NOT EXISTS "${SCARAB_VENDOR}/x64_dx12/d3d12_xs.dll" AND NOT SCARAB_ALLOW_MISSING_ARTIFACTS)
  message(WARNING
    "Vendor tree not found at ${SCARAB_VENDOR}.\n"
    "  Run scripts/link-artifacts.sh (or scripts/link-artifacts.ps1) first - it links from\n"
    "  SCARAB_ARTIFACTS=${SCARAB_ARTIFACTS} - or configure with\n"
    "  -DSCARAB_ALLOW_MISSING_ARTIFACTS=ON to build only what does not need it.")
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(components/runtime-bridge)
add_subdirectory(components/extension-wrapper)
add_subdirectory(components/shader-bridge)
add_subdirectory(components/memory-adapter)

option(SCARAB_BUILD_TESTS "Build the test executables and register the CTest gates" ON)
if(SCARAB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "SCARAB: build type  = ${CMAKE_BUILD_TYPE}")
message(STATUS "SCARAB: msvc-like   = ${SCARAB_MSVC_LIKE}")
message(STATUS "SCARAB: artifacts   = ${SCARAB_ARTIFACTS}")
message(STATUS "SCARAB: vendor      = ${SCARAB_VENDOR}")
