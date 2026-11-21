# Test executables. These are Windows PEs: on this host they are run under Wine
# by tests/run_tests.sh. They are also plain exes and run natively on Windows.
#
# The gates that matter (export contract, wrapper vtable layout, integration) are
# listed in docs/ARCHITECTURE.md section 5.

# ---------------------------------------------------------------------------
# wrapper self-test: exercises the wrapper's own logic with no GPU and no device.
# A null PC object is passed on purpose - QI routing, refcounting, validation and
# unknown-slot logging must all behave without one.
# ---------------------------------------------------------------------------
add_executable(wrapper_selftest
  wrapper_selftest_main.cpp
  $<TARGET_OBJECTS:xtl_wrapper_impl>
)
target_include_directories(wrapper_selftest PRIVATE
  "${CMAKE_SOURCE_DIR}/components/extension-wrapper"
  "${CMAKE_SOURCE_DIR}/components/extension-wrapper/src"
  "${CMAKE_BINARY_DIR}/components/extension-wrapper/generated"
  "${XTL_VENDOR}/d3d12/include"
)
target_link_libraries(wrapper_selftest PRIVATE ole32 oleaut32 uuid user32 kernel32)
target_link_options(wrapper_selftest PRIVATE -static)

# ---------------------------------------------------------------------------
# integration test: bridge -> wrapper -> real PC D3D12, end to end. Creates a
# console DMA command list and checks the object that comes back serves both the
# console IID and the standard PC interfaces.
# ---------------------------------------------------------------------------
add_executable(integration_test integration_test.cpp)
target_include_directories(integration_test PRIVATE
  "${CMAKE_SOURCE_DIR}/components/extension-wrapper"
  "${CMAKE_SOURCE_DIR}/components/extension-wrapper/src"
  "${XTL_VENDOR}/d3d12/include"
)
target_link_options(integration_test PRIVATE -static)
