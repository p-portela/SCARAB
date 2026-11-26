# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
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
#
# Staged as an explicit file-level rule rather than a POST_BUILD step on the test
# executables: a POST_BUILD only fires when *its own* target is out of date, so a
# change to the bridge alone would leave a stale DLL beside an up-to-date test -
# which silently tests the wrong binary.
set(SCARAB_TEST_STAGE_DIR "${CMAKE_BINARY_DIR}/tests")
set(_staged "")
foreach(_name d3d12_xs.dll d3d12_xs_wrap.dll)
  string(REPLACE ".dll" "" _tgt "${_name}")
  add_custom_command(
    OUTPUT "${SCARAB_TEST_STAGE_DIR}/${_name}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SCARAB_TEST_STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${_tgt}> "${SCARAB_TEST_STAGE_DIR}"
    DEPENDS ${_tgt}
    COMMENT "Staging ${_name} beside the WinPE tests")
  list(APPEND _staged "${SCARAB_TEST_STAGE_DIR}/${_name}")
endforeach()
add_custom_target(scarab_stage_tests ALL DEPENDS ${_staged})
add_dependencies(wrapper_selftest scarab_stage_tests)
add_dependencies(integration_test scarab_stage_tests)

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

# G7: the bridge must identify its own backend correctly. The Linux path *is*
# the D3D12 -> Vulkan bridge, and a path-based guess once labelled Wine's builtin
# (vkd3d-proton) as Microsoft's runtime and warned about a mismatch that did not
# exist. Runs the integration test and reads the bridge's log. The launcher is
# passed through so the test can tell which host it is on (empty -> native).
add_test(NAME G7_backend_identity
  COMMAND "${SCARAB_PYTHON}" "${_t}/test_backend_identity.py"
          "${_launch}" "$<TARGET_FILE:integration_test>")
set_tests_properties(G7_backend_identity PROPERTIES
  SKIP_RETURN_CODE 77
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:integration_test>")

# G2 is the build itself: the wrapper compiles only if `override` matches the
# real header for all 53 device and 76 command-list methods. There is nothing
# left to run, which is the point - it cannot pass while being wrong.

# ---------------------------------------------------------------------------
# memory-adapter: the layer's own semantics (flag translation, swizzle policy,
# the xmem shim) with no GPU. Honest by design: the self-test fails rather
# than passes when a shape drifts.
# ---------------------------------------------------------------------------
add_executable(memory_adapter_selftest "${_t}/memory_adapter_selftest.cpp")
target_include_directories(memory_adapter_selftest PRIVATE
  "${CMAKE_SOURCE_DIR}/components/memory-adapter/include")
target_link_libraries(memory_adapter_selftest PRIVATE scarab_memory_adapter kernel32 user32)
scarab_configure_exe(memory_adapter_selftest)

add_test(NAME G8_memory_adapter
  COMMAND "${_launch}" "$<TARGET_FILE:memory_adapter_selftest>")
set_tests_properties(G8_memory_adapter PROPERTIES
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:memory_adapter_selftest>")

# ---------------------------------------------------------------------------
# xg oracle: compare the shim against the vendor xg_xs.dll. The vendor library
# is the only answer this project has -- reading its decompiled C is not
# proof, calling it is. Exits 77 (skip) when the vendor DLL is absent.
# ---------------------------------------------------------------------------
add_executable(xg_oracle_test "${_t}/xg_oracle_test.cpp")
target_include_directories(xg_oracle_test PRIVATE
  "${CMAKE_SOURCE_DIR}/components/memory-adapter/include")
target_link_libraries(xg_oracle_test PRIVATE scarab_memory_adapter kernel32 user32)
scarab_configure_exe(xg_oracle_test)

add_test(NAME G9_xg_oracle
  COMMAND ${_launch} "$<TARGET_FILE:xg_oracle_test>"
          "${SCARAB_VENDOR}/x64_dx12/xg_xs.dll")
set_tests_properties(G9_xg_oracle PROPERTIES
  SKIP_RETURN_CODE 77
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:xg_oracle_test>")

# ---------------------------------------------------------------------------
# launcher: a fixture PE with one app-local import and one TLS callback. The
# loader must map it, bind the import, run the callback and reach main. The
# same test then points the loader at the title's own exe in --list mode, where
# our shims must resolve and the console DLLs we do not ship must be named.
# ---------------------------------------------------------------------------
add_library(fixture_dep SHARED "${_t}/fixture_dep.c")
scarab_configure_shared(fixture_dep "${_t}/fixture_dep.def")

add_executable(loader_fixture "${_t}/loader_fixture.c")
target_link_libraries(loader_fixture PRIVATE fixture_dep)
scarab_configure_exe(loader_fixture)

# The game-input shim is wired in by its own step; until the target exists the
# check below reports SKIP and the rest of G10 is unaffected.
set(_gdk_dlls "$<TARGET_FILE:xgameruntime>")
if (TARGET gameinput)
    list(APPEND _gdk_dlls "$<TARGET_FILE:gameinput>")
endif()
add_test(NAME G10_launcher
  COMMAND "${SCARAB_PYTHON}" "${_t}/test_loader.py" "${_launch}"
          "$<TARGET_FILE:scarab_launch>" "$<TARGET_FILE:loader_fixture>"
          "$<TARGET_FILE:xmem>" "$<TARGET_FILE:d3d12_xs>"
          "${SCARAB_ARTIFACTS}/early_xmas/game_scarlett_master.exe"
          "${CMAKE_SOURCE_DIR}/tools" ${_gdk_dlls})
set_tests_properties(G10_launcher PROPERTIES
  SKIP_RETURN_CODE 77
  WORKING_DIRECTORY "$<TARGET_FILE_DIR:loader_fixture>")

