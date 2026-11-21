# shader-bridge

The console feeds the driver pre-compiled **RDNA2/gfx10 machine code** wrapped in an FSSM
container (itself zlib-compressed in an XBCH stream inside an XDAT container that sits
inside a DXIL-shaped outer container). PC drivers accept DXIL, or SPIR-V under Vulkan.
This component is that translation.

```
FSSM blob -> raw RDNA2 stream      fssm_to_rdna2.py                         done, 25,321 shaders
          -> DXIL                  the upstream shader recompiler front-end + a DXIL emitter          in progress
          -> PC shader blob        include/shader_bridge.h                  this seam
```

**The target is DXIL, not SPIR-V.** vkd3d-proton consumes DXIL and so does Microsoft's
D3D12 runtime, so DXIL is the one output that serves both backends — and Windows is the
primary target. the upstream shader recompiler already emits SPIR-V; that is worth keeping as a Linux-only
cross-check, but making it primary would lock this component to the Linux path.

## Contents

| file | role |
|---|---|
| `fssm_lib.py` | container parsing: FSSM header, XBCH/zlib unwrap |
| `fssm_parse.py` | inspection / triage over FSSM blobs |
| `fssm_to_rdna2.py` | the extractor: FSSM → raw instruction streams + index |
| `extract_shaders.py` | splits an RPF shader library into per-shader files |
| `rdna2_classify.py` | corpus-wide classification of the extracted streams |
| `src/rdna2_recompile.cpp` | drives the upstream shader recompiler's recompiler on a raw stream |
| `src/rdna2_sweep.cpp` | walks the instruction stream to locate the first decoder rejection |
| `include/shader_bridge.h` | the interface the runtime-bridge calls from `D3D12UploadCustomMicrocodeX` |

## State of the work

Extraction is complete and verified: 25,321 streams, nothing skipped, and the FSSM
sub-header invariants hold across the corpus (`code_end == code_off + code_size` in
99.9%; `s_endpgm` in 25,782/25,782 with a control showing 0/3000). Recompilation is
blocked on decoder coverage, and there is no Vulkan validation layer available on this
host to prove the output — see `../../the shader-target notes`.

One decoder question is unresolved and is worth reading before touching this:
`../../docs/isa/shader-isa-notes.md` §3–4. The short version is that the vendor's
own compiler text confirms VOP3 uses base-plus-offset sub-encodings, the upstream shader recompiler implements
that convention, and therefore the remaining sweep failures are more likely
*stream alignment* than a wrong opcode table.

## The other half of the job: bindings

console takes shader bindings from the **PSO root signature**, not from a per-shader header,
so any shader that reads buffers or images needs an SRT/BDA equivalent synthesised before
a PC runtime will accept it. See `../../docs/shader-abi-notes.md`. This is why
`shader_bridge.h` exposes `scarab_shader_synthesise_bindings` separately from translation.

## A free oracle

`xbsc_xs.dll` — the console shader compiler in the vendor stack — also exports
`XBSC_DisassembleRawShader` and `XBSC_AssembleShader`, and it **loads under Wine on
Linux** (43/43 exports resolve). So the 25,321 streams do not have to be decoded by
inference: the vendor ships the tool that decodes them, and the call is now recovered
and working — `tools/xbsc_probe.{c,sh}` + `tools/xbsc_probe_notes.md`. Run
`XBSC_DisassembleRawShader(code, size_bytes, &marker, NULL)` and read the text back
with `XBSC_GetDisassembly`; that output is the oracle for every decoder question
below.
