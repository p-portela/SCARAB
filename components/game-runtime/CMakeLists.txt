# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# game-runtime: the GDK system runtime, as the title loads it.
#
# A DLL, not a static library: the title asks for `xgameruntime.dll` by name,
# so the artifact must exist on the search path. It runs no console code; it
# is the layer's own shim over the real host.

add_library(xgameruntime SHARED src/xgameruntime.cpp)
target_link_libraries(xgameruntime PRIVATE kernel32)
scarab_configure_shared(xgameruntime "${CMAKE_CURRENT_SOURCE_DIR}/xgameruntime.def")
scarab_strict_warnings(xgameruntime)
