# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# Cross-compile for 64-bit Windows from a Linux host.
#
# Uses LLVM-mingw (clang + lld targeting x86_64-w64-windows-gnu), which is a
# Linux-hosted toolchain. Note that the "winlibs" mingw builds are *Windows*
# executables and cannot be used here without Wine.
#
# Usage:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake
#
# Override the toolchain location with -DSCARAB_TOOLCHAIN=/path/to/llvm-mingw
# (default: $HOME/opt/llvm-mingw).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT SCARAB_TOOLCHAIN)
  if(DEFINED ENV{SCARAB_TOOLCHAIN})
    set(SCARAB_TOOLCHAIN "$ENV{SCARAB_TOOLCHAIN}")
  else()
    set(SCARAB_TOOLCHAIN "$ENV{HOME}/opt/llvm-mingw")
  endif()
endif()

set(_tc_bin "${SCARAB_TOOLCHAIN}/bin")
if(NOT EXISTS "${_tc_bin}")
  message(FATAL_ERROR
    "LLVM-mingw not found at ${SCARAB_TOOLCHAIN}. Set -DSCARAB_TOOLCHAIN=/path/to/llvm-mingw "
    "or the SCARAB_TOOLCHAIN environment variable.")
endif()

set(CMAKE_C_COMPILER   "${_tc_bin}/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${_tc_bin}/x86_64-w64-mingw32-clang++")
set(CMAKE_AR           "${_tc_bin}/llvm-ar")
set(CMAKE_RANLIB       "${_tc_bin}/llvm-ranlib")
set(CMAKE_STRIP        "${_tc_bin}/llvm-strip")
set(CMAKE_RC_COMPILER  "${_tc_bin}/x86_64-w64-mingw32-windres")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
