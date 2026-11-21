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
# the ABI. Regenerated on every build that touches the header or the generator.
add_custom_command(
  OUTPUT "${_gen}/gen_device.inc" "${_gen}/gen_clist.inc"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/gen_wrappers.py" ID3D12Device4
          > "${_gen}/gen_device.inc"
  COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/gen_wrappers.py" ID3D12GraphicsCommandList6
          > "${_gen}/gen_clist.inc"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_wrappers.py"
          "${XTL_VENDOR}/d3d12/include/d3d12.h"
  COMMENT "Generating ABI-exact forwarding stubs from d3d12.h"
  VERBATIM
)

# Object library so the self-test can exercise the same code the DLL ships.
# The generated stubs are listed as HEADER_FILE_ONLY sources: that is what makes
# CMake actually run the custom command above. Without listing them they are
# never built and a stale copy somewhere on the include path gets used instead -
# which is exactly the silent drift this generator exists to prevent.
set_source_files_properties(
  "${_gen}/gen_device.inc" "${_gen}/gen_clist.inc"
  PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(xtl_wrapper_impl OBJECT
  src/wrap.cpp
  src/guid_defs.cpp
  "${_gen}/gen_device.inc"
  "${_gen}/gen_clist.inc"
)
target_include_directories(xtl_wrapper_impl PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${_gen}"
  "${XTL_VENDOR}/d3d12/include"
)
target_compile_options(xtl_wrapper_impl PRIVATE
  -Wno-unused-parameter -Wno-cast-function-type)

add_library(d3d12_xs_wrap SHARED
  $<TARGET_OBJECTS:xtl_wrapper_impl>
)
target_include_directories(d3d12_xs_wrap PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${_gen}"
  "${XTL_VENDOR}/d3d12/include"
)
target_link_libraries(d3d12_xs_wrap PRIVATE ole32 oleaut32 uuid user32 kernel32)
target_link_options(d3d12_xs_wrap PRIVATE
  -static
  -Wl,--kill-at
  "${CMAKE_CURRENT_SOURCE_DIR}/d3d12_xs_wrap.def"
)
set_target_properties(d3d12_xs_wrap PROPERTIES PREFIX "" OUTPUT_NAME "d3d12_xs_wrap")
