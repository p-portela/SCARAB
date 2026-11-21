# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
# shader-bridge: the FSSM -> RDNA2 -> DXIL pipeline.
#
# See docs/ROADMAP.md section 0 for why the target is DXIL (LLVM bitcode, shader
# model 6.x) inside a DXBC container.
#
# The extractor side is Python (fssm_*.py, run from the CLI); this builds the
# C++ side, which is what has to talk to DXC and to the upstream shader recompiler.

# DXC's own headers. Vendored because the header is the ABI contract and pinning
# it is safer than tracking upstream; it carries no binaries.
set(_dxc_inc "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dxc")

# Proves we can produce real DXIL locally: HLSL -> dxcompiler.dll -> a DXBC
# container whose DXIL part holds LLVM bitcode. This is the emitter half of the
# pipeline, and it is the cheapest way to de-risk the whole shader workstream.
add_executable(dxc_emit_test src/dxc_emit_test.cpp)
target_include_directories(dxc_emit_test PRIVATE "${_dxc_inc}")
scarab_configure_exe(dxc_emit_test)
if(NOT SCARAB_MSVC_LIKE)
  # dxcapi.h annotates interfaces with __declspec(uuid(...)), which clang ignores.
  # Harmless (dxc_iids.h supplies the IIDs) but noisy.
  target_compile_options(dxc_emit_test PRIVATE -Wno-unknown-attributes)
endif()
