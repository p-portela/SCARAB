# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# xcrdapi: the console's DirectStorage, as the title loads it.
#
# A DLL, not a static library: the title imports XCRDAPI.dll by name, so the
# artifact must exist on the search path. It runs no console code; it is the
# layer's own shim over the real host.

add_library(xcrdapi SHARED src/xcrdapi.cpp)
target_link_libraries(xcrdapi PRIVATE kernel32)
scarab_configure_shared(xcrdapi "${CMAKE_CURRENT_SOURCE_DIR}/xcrdapi.def")
scarab_strict_warnings(xcrdapi)
