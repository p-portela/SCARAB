# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# game-input: the GDK input runtime the title loads by ordinal.
#
# The title imports GameInputCreate by ordinal 1 (GameInput.dll!#1), so the
# artifact must exist on the search path and pin that ordinal. It is a shim over
# nothing: it reports one phantom always-connected gamepad, and refuses every
# slot it has no real backing for.

add_library(gameinput SHARED src/gameinput.cpp)
target_link_libraries(gameinput PRIVATE kernel32)
scarab_configure_shared(gameinput "${CMAKE_CURRENT_SOURCE_DIR}/gameinput.def")
scarab_strict_warnings(gameinput)
