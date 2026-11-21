# The verification gates, registered with CTest so the command is identical on
# every host:
#
#     ctest --test-dir build --output-on-failure
#
# On Windows the executables run natively; elsewhere they are Windows PEs run
# through SCARAB_TEST_LAUNCHER (Wine). That decision is made once, in
# cmake/scarab-platform.cmake. tests/run_tests.sh is a thin wrapper that also
# builds first, for the Linux cross workflow.

set(_t "${CMAKE_SOURCE_DIR}/tests")
set(_inc
  "${CMAKE_SOURCE_DIR}/components/extension-wrapper"
  "${CMAKE_SOURCE_DIR}/components/extension-wrapper/src"
  "${CMAKE_BINARY_DIR}/components/extension-wrapper/generated"
  "${SCARAB_VENDOR}/d3d12/include")

# ---------------------------------------------------------------------------
# wrapper self-test: the wrapper's own logic with no GPU and no device.
# A null PC object is passed on purpose - QI routing, refcounting, validation and
# unknown-slot logging must all behave without one.
# ---------------------------------------------------------------------------
add_executable(wrapper_selftest "${_t}/wrapper_selftest_main.cpp"
               $<TARGET_OBJECTS:scarab_wrapper_impl>)
target_include_directories(wrapper_selftest PRIVATE ${_inc})
target_link_libraries(wrapper_selftest PRIVATE ole32 oleaut32 uuid user32 kernel32)
scarab_configure_exe(wrapper_selftest)

# ---------------------------------------------------------------------------
# integration test: bridge -> wrapper -> real PC D3D12, end to end. Creates a
# console DMA command list and checks the object that comes back serves both the
# console IID and the standard PC interfaces.
# ---------------------------------------------------------------------------
add_executable(integration_test "${_t}/integration_test.cpp")
target_include_directories(integration_test PRIVATE ${_inc})
scarab_configure_exe(integration_test)

# The integration test loads d3d12_xs.dll by name and the bridge then looks for
# d3d12_xs_wrap.dll beside it. Put all three together in the build tree so the
# test works the same natively on Windows and under Wine, with no PATH games.
foreach(_tgt wrapper_selftest integration_test)
  add_custom_command(TARGET ${_tgt} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:d3d12_xs> "$<TARGET_FILE_DIR:${_tgt}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:d3d12_xs_wrap> "$<TARGET_FILE_DIR:${_tgt}>"
    COMMENT "Staging the DLLs next to ${_tgt}")
endforeach()

# ---------------------------------------------------------------------------
# gates
# ---------------------------------------------------------------------------
# G5: tooling unit tests
add_test(NAME G5_tools
  COMMAND "${SCARAB_PYTHON}" "${_t}/test_tools.py")
set_tests_properties(G5_tools PROPERTIES ENVIRONMENT "SCARAB_VENDOR=${SCARAB_VENDOR}")

# G1: export contract, byte-exact against the genuine console runtime
add_test(NAME G1_export_contract
  COMMAND "${SCARAB_PYTHON}" "${_t}/test_export_contract.py"
          "$<TARGET_FILE:d3d12_xs>" "${SCARAB_VENDOR}/x64_dx12/d3d12_xs.dll")

# G3 / G4: the Windows executables, launched however this host launches them
set(_launch ${SCARAB_TEST_LAUNCHER})
add_test(NAME G3_wrapper_selftest
  COMMAND ${_launch} "$<TARGET_FILE:wrapper_selftest>")
add_test(NAME G4_integration
  COMMAND ${_launch} "$<TARGET_FILE:integration_test>")
set_tests_properties(G3_wrapper_selftest G4_integration PROPERTIES
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:integration_test>")

# G6: the shader emitter half of the pipeline. Produces real DXIL with DXC and
# verifies the container part-by-part. SKIPs (77) when the vendor DXC is not
# linked, so the suite stays meaningful on a machine without the artifacts.
if(TARGET dxc_emit_test)
  add_test(NAME G6_dxil_emitter
    COMMAND ${_launch} "$<TARGET_FILE:dxc_emit_test>"
            "${SCARAB_VENDOR}/x64_dx12/dxcompiler.dll" "test.dxil")
  set_tests_properties(G6_dxil_emitter PROPERTIES
    SKIP_RETURN_CODE 77
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:dxc_emit_test>")
endif()

# G2 is the build itself: the wrapper compiles only if `override` matches the
# real header for all 53 device and 76 command-list methods. There is nothing
# left to run, which is the point - it cannot pass while being wrong.
