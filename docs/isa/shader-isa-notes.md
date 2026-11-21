# console shader ISA evidence recovered from `xbsc_xs.dll`

`xbsc_xs.dll` (16,802,360 B, sha256 `38611e0c7d5bfce734fbe5facbd526ec45f616412be1119d294dfcf652b29e28`)
is the console shader compiler shipped in the console stack redist. It is not just a
compiler: **it embeds an ISA reference and the vendor's own encoding rules.**

Extraction tool: `tools/peinfo.py`. Outputs:
- `docs/isa/xbsc-isa-mnemonics.txt` — 1,542 ISA mnemonics
- `docs/isa/xbsc-isa-docs.txt` — 856 specification/doc sentences

## 1. What "FSSM" is, confirmed from the compiler side

Magic-byte scan (`tools/peinfo.py::MAGICS`) over the shader-toolchain DLLs:

| DLL | `FSSM` | `XBCH` | `XDAT` | `DXIL` | `DXBC` |
|---|---|---|---|---|---|
| `xbsc_xs.dll` | **5** | 3 | 3 | 6 | 6 |
| `dxcompiler_xs.dll` | **5** | 3 | 3 | 6 | 6 |
| `xgs12_pc_xs.dll` | 0 | 6 | 6 | 6 | 6 |
| `dxcompiler.dll` (PC) | **0** | 1 | 0 | 6 | 6 |
| `dxil.dll` (PC validator) | 0 | 1 | 0 | 6 | 6 |

The console compilers contain `FSSM` and `XBCH`/`XDAT`; the PC compiler does not
contain `FSSM` at all. The `FSSM` occurrences sit in `.text` immediately after
comparison instructions (`...rS.:FSSMuK.` — a 4-byte compare against the magic),
i.e. the compiler *recognises* the container, it is not a coincidental substring.

This independently confirms the earlier finding that the game's 25,889 shader
payloads are `FSSM` v0x101 wrapped in an `XBCH` (zlib) stream inside an `XDAT`
container, and that `FSSM` is the console's compiled-shader format — the thing
`d3d12_xs.dll`'s `D3D12UploadCustomMicrocodeX` exists to deliver.

## 2. The console exposes a *disassembler* — we do not have to guess the ISA

`xbsc_xs.dll` exports 43 functions. The relevant ones:

| export | role |
|---|---|
| `SCCompileMultiShadersconsole` | compile for Scarlett |
| `XBSC_AssembleShader`, `XBSC_GetAssembledShader` | **assemble** text/IR → hardware microcode |
| `XBSC_DisassembleRawShader`, `XBSC_GetDisassembly` | **disassemble** hardware microcode → text |
| `XBSC_DisassembleHwPipeline`, `XBSC_DisassembleHwPipelineWithCallback` | disassemble a whole pipeline |
| `XBSC_CompressBuffer` / `DecompressBuffer` / `GetCompressedBuffer` / `GetDecompressedBuffer` | the `XBCH` zlib container |
| `XBSC_SerializePipelineCompileInput` / `...Output`, `XBSC_Deserialize…` | the cooked-PSO container |
| `SC_ConvertRootSignatureToSCInputs`, `XBSC_CompileRootSignature`, `XBSC_GetCompiledRootSignature` | root-signature → shader-binding ABI |
| `XBSC_GetShaderStats`, `XBSC_DumpShaderStats`, `XBSC_GetRegPressure` | analysis |
| `XBSC_CreatePixDwarfApi` | PIX debug info |
| `SCDump{,Merged,Raw}HwShader{,Data}` | dump hardware shaders |

Consequence: the 25,321 extracted RDNA2 streams do not need to be decoded by
inference. The vendor ships the tool that decodes them. Running it is the
decisive validation of `docs/fssm-analysis.md`.

## 3. The VOP3 encoding rule, stated by the vendor

Among the 856 embedded spec sentences:

> Offset to add to any **VOP1** opcodes when they need to use the VOP3 encoding.
> For example, `SQ_V_OP1_OFFSET + SQ_V_MOV_B32` generates the VOP3 version of MOV.

> Offset to add to any **VOP2** opcodes when they need to use the VOP3 encoding.
> For example, `SQ_V_OP2_OFFSET + SQ_V_ADD_F32` generates the VOP3 version of ADD.

> Offset to add to any **VOPC** opcodes when they need to use the VOP3 encoding.

> Offset to add to any **VINTRP** opcodes when they need to use the VOP3 encoding.

> Instruction `'%s'` in VOP3 encoding **cannot accept absolute value modifiers**.

> Cannot represent opcode `'%s'` in any VOPC/VOP1/VOP2/VOP3 encoding.

> Vector instruction cannot be promoted to the VOP3 encoding; it must use the
> VOP2/VOP1/VOPC encoding. As a consequence these instructions may not use the
> input modifiers `-x`, `abs(x)`, nor may they use the output modifiers `mul`,
> `div`, `clamp`.

This settles the shape of the question that blocked the decoder sweep: a VOP3
opcode field is *not* a flat opcode space — it is a base space plus offset-encoded
VOP1/VOP2/VOPC/VINTRP sub-spaces. the upstream shader recompiler already implements this convention:

```
RdnaVectorOpDecoder.cpp:641   return opcode >= 0x100u && opcode <= 0x13fu;   // VOP2 in VOP3
RdnaVectorOpDecoder.cpp:645   return opcode >= 0x180u && opcode <= 0x1ffu;   // VOP1 in VOP3
RdnaVectorOpDecoder.cpp:657   const auto vop2Encoding = opcode - 0x100u;
RdnaVectorOpDecoder.cpp:664   lookupVectorOpcode(vop3EncodedVop1Opcodes, opcode - 0x180u, ...)
```

So decoding `0x180` as `VNop` (VOP1 offset + V_NOP) is **the intended behaviour of
the upstream shader recompiler's tables, and the vendor's documentation confirms that convention exists.**
The earlier conclusion — "either the console `0x180` mapping differs, or my field
decode is wrong" — therefore resolves toward the second option.

## 4. What this does and does not prove

Proven: the FSSM container belongs to the console shader compiler; the vendor ships
a disassembler; the VOP3 offset-encoding convention is real and documented.

**Not proven:** the numeric values of `SQ_V_OP{1,2,C}_OFFSET` / `SQ_V_INTRP_OFFSET`.
They are compiler constants and do not appear as strings. the upstream shader recompiler's `0x100` / `0x180`
match the well-known published convention, and the vendor text confirms the
convention's existence, but the constants themselves are still taken on trust.

That distinction matters for one specific thing: an alternative reading of the
failure list is that the affected words are simply **not instruction boundaries**
at all — the walker is misaligned, and it is decoding operand or padding bytes as
opcodes. Under that reading, opcode `0x180` carrying `abs`/`op_sel`/`clamp` is
exactly what garbage looks like: `V_NOP` cannot take modifiers, so `0x180` *with*
modifiers is not a real instruction. The check to run is whether the failing words
sit after the end of the real program (the streams are padded with repeated
`s_code_end`, `0xBF9F0000`) or immediately after a literal-consuming instruction
whose word count was mis-decoded.

Either way the next move is measurement, not more table archaeology:

1. Run `XBSC_DisassembleRawShader` on real blobs (portable Wine, Linux) and
   compare against the the upstream shader recompiler decode. This answers the ISA questions outright and
   validates or refutes `docs/fssm-analysis.md`.
2. Re-run the sweep checking instruction-boundary alignment rather than opcode
   validity: for each rejected word, test whether decoding *stops* at a plausible
   program end, and whether the preceding instruction consumed a literal.
