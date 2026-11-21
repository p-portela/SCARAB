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

scarab_configure_shared(d3d12_xs "${CMAKE_CURRENT_SOURCE_DIR}/d3d12_xs.def")
set_target_properties(d3d12_xs PROPERTIES OUTPUT_NAME "d3d12_xs")
