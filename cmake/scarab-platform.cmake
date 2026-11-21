# scarab-platform.cmake -- platform and toolchain abstraction.
#
# SCARAB targets two host environments and must build on both:
#
#   Windows (primary)   native MSVC / clang-cl / LLVM-mingw. Tests run natively.
#   Linux (secondary)   cross-compiled with LLVM-mingw; tests run under Wine, and
#                       the D3D12 backend is vkd3d-proton instead of Microsoft's
#                       runtime.
#
# Everything that differs is resolved here so the component files stay
# declarative and a component never needs to ask "am I on MSVC".
#
# Provided:
#   SCARAB_MSVC_LIKE        link/compile flags are MSVC-style
#   SCARAB_PYTHON           python interpreter (Windows has no `python3`)
#   SCARAB_TEST_LAUNCHER    prefix that runs a Windows exe on this host
#                         (empty on Windows, `wine` on Linux)
#   scarab_configure_shared(target def_file)   apply the shared DLL settings
#   scarab_configure_exe(target)               apply the shared exe settings

# --- toolchain flavour -----------------------------------------------------
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  set(SCARAB_MSVC_LIKE ON)
else()
  set(SCARAB_MSVC_LIKE OFF)
endif()

# A drop-in DLL and a test exe must not depend on a redistributable C++ runtime:
# the title's process may not have one, and on Wine it definitely should not.
# MSVC-style: static CRT. GNU-style: -static.
if(SCARAB_MSVC_LIKE)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # fopen/strcat/vsnprintf are used deliberately and are safe here
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  set(SCARAB_GNU_LINK_FLAGS -static)
endif()

# --- python ---------------------------------------------------------------
# `python3` is not guaranteed on Windows, and hard-coding it in a build step has
# already broken one CMake custom command on this project. Resolve it once.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(SCARAB_PYTHON "${Python3_EXECUTABLE}")
message(STATUS "SCARAB: python      = ${SCARAB_PYTHON}")

# --- test execution ------------------------------------------------------
# The test executables are always Windows PEs. Whether they can run as-is depends
# on the HOST, not the target: note that WIN32 is true when cross-compiling *to*
# Windows, so it is the wrong question to ask. CMAKE_CROSSCOMPILING is the right
# one. Getting this backwards silently runs a PE through the shell.
if(CMAKE_CROSSCOMPILING)
  set(_wine "")
  if(EXISTS "$ENV{HOME}/opt/wine/bin/wine")
    set(_wine "$ENV{HOME}/opt/wine/bin/wine")
  else()
    find_program(_wine_found NAMES wine wine64)
    if(_wine_found)
      set(_wine "${_wine_found}")
    endif()
  endif()
  if(_wine)
    message(STATUS "SCARAB: cross-compiling; tests will run under ${_wine}")
  else()
    message(WARNING
      "SCARAB: cross-compiling but no Wine found on the host. The build and the "
      "python gates still work; the G3/G4 gates need Wine (or run the exes on "
      "Windows). Override with -DSCARAB_TEST_LAUNCHER=/path/to/wine.")
  endif()
else()
  set(_wine "")
endif()
set(SCARAB_TEST_LAUNCHER "${_wine}" CACHE STRING
    "Prefix used to launch test executables (empty when they run natively)")
message(STATUS "SCARAB: test runner = '${SCARAB_TEST_LAUNCHER}'")

# --- shared target settings ----------------------------------------------
function(scarab_configure_shared target def_file)
  if(SCARAB_MSVC_LIKE)
    target_link_options(${target} PRIVATE "/DEF:${def_file}")
  else()
    # GNU-style: the .def is a linker input, and symbol decoration is stripped
    # (a no-op on x64, kept so a 32-bit build would be correct too)
    target_link_options(${target} PRIVATE ${SCARAB_GNU_LINK_FLAGS} "-Wl,--kill-at" "${def_file}")
  endif()
  set_target_properties(${target} PROPERTIES PREFIX "")
endfunction()

function(scarab_configure_exe target)
  if(SCARAB_MSVC_LIKE)
    target_link_options(${target} PRIVATE "/ENTRY:mainCRTStartup")
  else()
    target_link_options(${target} PRIVATE ${SCARAB_GNU_LINK_FLAGS})
  endif()
endfunction()

# Warning flags differ per compiler family; keep them out of the components.
function(scarab_strict_warnings target)
  if(SCARAB_MSVC_LIKE)
    target_compile_options(${target} PRIVATE /W4 /wd4100 /wd4189)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type)
  endif()
endfunction()
