# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# runtime-bridge: d3d12_xs.dll
#
# A drop-in replacement for the console D3D12 runtime's 116 exports. It forwards
# the standard D3D12 API to a PC D3D12 runtime and emulates the console-only free
# functions. Deliberately header-free: it declares ABI-exact prototypes and
# resolves the backend through GetProcAddress, so it cannot drift from the PC ABI
# at compile time and needs no D3D12 headers.
#
# The export contract is enforced by tests/ (gate G1), not by this file: a build
# that links but exports the wrong names must fail the gate.

add_library(d3d12_xs SHARED
  src/d3d12_xs_bridge.cpp
  src/iid_table.cpp
)

target_include_directories(d3d12_xs PRIVATE src)
scarab_strict_warnings(d3d12_xs)

# version.lib: the backend resolver reports which d3d12.dll it actually loaded,
# including its version resource, so "which runtime am I on?" is answerable from
# the log alone.
target_link_libraries(d3d12_xs PRIVATE user32 advapi32 kernel32 version)

# The export contract (.def) is *generated* from the console runtime the user
# supplies, not committed: it is the vendor's export table. tools/gen_def.py
# emits it, aliasing the vendor's console-only export names onto our own source
# symbols (the vendor token is derived from the two runtimes, never hardcoded, and
# the mapping is verified against the symbols this repo defines). Outcome is
# checked by gate G1 against the same artifact.
set(SCARAB_DEF "${CMAKE_CURRENT_BINARY_DIR}/d3d12_xs.def")
set(SCARAB_REF_DLL "${SCARAB_VENDOR}/x64_dx12/d3d12_xs.dll")
set(SCARAB_REF_SHARED_DLL "${SCARAB_VENDOR}/d3d12/os/d3d12.dll")
add_custom_command(
  OUTPUT "${SCARAB_DEF}"
  COMMAND "${SCARAB_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/gen_def.py"
          "${SCARAB_REF_DLL}" "${SCARAB_REF_SHARED_DLL}" "${SCARAB_DEF}"
          "${CMAKE_CURRENT_BINARY_DIR}/exports_console.csv"
  DEPENDS "${SCARAB_REF_DLL}" "${SCARAB_REF_SHARED_DLL}"
          "${CMAKE_SOURCE_DIR}/tools/gen_def.py"
  COMMENT "Generating the d3d12_xs export contract from the supplied runtime")
add_custom_target(d3d12_xs_contract DEPENDS "${SCARAB_DEF}")
add_dependencies(d3d12_xs d3d12_xs_contract)

scarab_configure_shared(d3d12_xs "${SCARAB_DEF}")
set_target_properties(d3d12_xs PROPERTIES OUTPUT_NAME "d3d12_xs")
