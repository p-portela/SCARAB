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
