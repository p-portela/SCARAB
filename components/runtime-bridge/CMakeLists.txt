# runtime-bridge: d3d12_xs.dll
#
# A drop-in replacement for the console D3D12 runtime's 116 exports. It forwards
# the standard D3D12 API to the PC runtime and emulates the console-only free
# functions. Deliberately header-free: it declares ABI-exact prototypes and
# resolves the PC runtime through GetProcAddress, so it cannot drift from the PC
# ABI at compile time and needs no D3D12 headers.
#
# The export contract is enforced by tests/test_export_contract.py, not by this
# file: a build that links but exports the wrong names must fail the gate.

add_library(d3d12_xs SHARED
  src/d3d12_xs_bridge.cpp
  src/iid_table.cpp
)

target_link_libraries(d3d12_xs PRIVATE user32 advapi32 kernel32)
target_link_options(d3d12_xs PRIVATE
  -static                       # no redistributable C++ runtime for a drop-in DLL
  -Wl,--kill-at
  "${CMAKE_CURRENT_SOURCE_DIR}/d3d12_xs.def"
)

set_target_properties(d3d12_xs PROPERTIES
  PREFIX ""
  OUTPUT_NAME "d3d12_xs"
)
