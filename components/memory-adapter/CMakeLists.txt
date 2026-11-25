# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# memory-adapter: console memory semantics.
#
# Static library, not a DLL: the bridge and the wrapper load it as the
# layer's memory-semantic provider. Nothing here runs a console library;
# this is the layer's own honest shim over the real Windows APIs.

add_library(scarab_memory_adapter STATIC
  src/memory_adapter.cpp
  src/xmem.cpp
)

target_include_directories(scarab_memory_adapter PUBLIC include)
scarab_strict_warnings(scarab_memory_adapter)

# windows.h / CRT; no D3D12 headers needed.
target_link_libraries(scarab_memory_adapter PRIVATE kernel32 user32 psapi)

# The same shim as the DLL the title's import table names. Same source, so the
# self-test and the loaded shim cannot drift.
add_library(xmem SHARED src/xmem.cpp)
target_include_directories(xmem PRIVATE include)
target_link_libraries(xmem PRIVATE kernel32 psapi)
scarab_configure_shared(xmem "${CMAKE_CURRENT_SOURCE_DIR}/xmem.def")
scarab_strict_warnings(xmem)
