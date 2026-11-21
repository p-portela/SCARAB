# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# extension-wrapper: d3d12_xs_wrap.dll
#
# Owns the console (Scarlett) extension surface: the X-suffixed methods the console
# appends to the PC interfaces, and the console-only command-list type/IID. The
# bridge loads it dynamically, so this component may be absent and the standard
# API still works.
#
# Unlike the bridge, this component DELIBERATELY depends on the real Agility SDK
# header. It owns interfaces, so it inherits the genuine ones and marks every
# forwarded method `override` - the compiler then proves the PC-side vtable
# layout instead of us promising it. Code that owns an interface should inherit
# it; code that only forwards free functions should not need the header at all.

set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_gen}")

# Forwarding stubs are GENERATED from the real header so they cannot drift from
# the ABI. Note the -o form rather than shell redirection: `>` does not exist in
# cmd.exe, so a redirect here would make the Windows build impossible.
add_custom_command(
  OUTPUT "${_gen}/gen_device.inc" "${_gen}/gen_clist.inc"
  COMMAND "${SCARAB_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/gen_wrappers.py"
          ID3D12Device4 --output "${_gen}/gen_device.inc"
  COMMAND "${SCARAB_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/gen_wrappers.py"
          ID3D12GraphicsCommandList6 --output "${_gen}/gen_clist.inc"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_wrappers.py"
          "${SCARAB_VENDOR}/d3d12/include/d3d12.h"
  COMMENT "Generating ABI-exact forwarding stubs from d3d12.h"
  VERBATIM
)

# Listed as HEADER_FILE_ONLY sources: that is what makes CMake actually run the
# custom command. Without listing them they are never built and a stale copy
# somewhere on the include path gets used instead - exactly the silent drift this
# generator exists to prevent.
set_source_files_properties(
  "${_gen}/gen_device.inc" "${_gen}/gen_clist.inc"
  PROPERTIES HEADER_FILE_ONLY TRUE)

# Object library so the self-test can exercise the same code the DLL ships.
add_library(scarab_wrapper_impl OBJECT
  src/wrap.cpp
  src/guid_defs.cpp
  "${_gen}/gen_device.inc"
  "${_gen}/gen_clist.inc"
)
target_include_directories(scarab_wrapper_impl PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${_gen}"
  "${SCARAB_VENDOR}/d3d12/include"
)
scarab_strict_warnings(scarab_wrapper_impl)

add_library(d3d12_xs_wrap SHARED $<TARGET_OBJECTS:scarab_wrapper_impl>)
target_include_directories(d3d12_xs_wrap PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${_gen}"
  "${SCARAB_VENDOR}/d3d12/include"
)
target_link_libraries(d3d12_xs_wrap PRIVATE ole32 oleaut32 uuid user32 kernel32)
scarab_configure_shared(d3d12_xs_wrap "${CMAKE_CURRENT_SOURCE_DIR}/d3d12_xs_wrap.def")
set_target_properties(d3d12_xs_wrap PROPERTIES OUTPUT_NAME "d3d12_xs_wrap")
