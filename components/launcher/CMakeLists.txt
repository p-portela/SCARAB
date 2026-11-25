# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# launcher: map a console title's PE, bind its imports to this layer, run it.
#
# One executable and no library: nothing else links the loader yet. The
# reference loader (VIctory's) is a crate because it is embedded in a Rust
# runtime; here the Windows loader does the mapping and the job is main().

add_executable(scarab_launch src/main.cpp)
scarab_strict_warnings(scarab_launch)
target_link_libraries(scarab_launch PRIVATE kernel32 user32 shell32)
scarab_configure_exe(scarab_launch)
