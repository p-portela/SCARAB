#!/usr/bin/env python3
"""RDNA2 (gfx10) instruction-family classifier + length-aware walker.

Encoding rules taken from the AMD RDNA2 ISA machine-readable XML via
the upstream shader recompiler core/shader/recompiler/RdnaDecoder/src/RdnaInstructionDecoder.cpp
(GetRdnaInstructionFamily) and the per-family decoders in the same directory.

Stdlib only.
"""

# (word >> 26) -> family, for the "top bits != 00/10" space
_SW26 = {
    0x32: "VINTRP", 0x33: "VOP3P", 0x35: "VOP3", 0x36: "DS",
    0x37: "FLAT", 0x38: "MUBUF", 0x3A: "MTBUF", 0x3C: "MIMG",
    0x3D: "SMEM", 0x3E: "EXP",
}
# families that occupy two dwords in their base form
TWO_WORD = {"VOP3", "VOP3P", "SMEM", "MUBUF", "MTBUF", "FLAT", "DS", "EXP"}


def family(word):
    """Return the RDNA2 instruction family for a 32-bit code word (or 'UNKNOWN')."""
    if (word & 0x80000000) == 0:
        sel = (word >> 25) & 0x3F
        if sel == 0x3E:
            return "VOPC"
        if sel == 0x3F:
            return "VOP1"
        return "VOP2"
    if (word & 0xC0000000) == 0x80000000:
        op = (word >> 23) & 0x7F
        if op == 0x7D:
            return "SOP1"
        if op == 0x7E:
            return "SOPC"
        if op == 0x7F:
            return "SOPP"
        return "SOPK" if op >= 0x60 else "SOP2"
    return _SW26.get(word >> 26, "UNKNOWN")


def sopp_opcode(word):
    return (word >> 16) & 0x7F


def vop1_opcode(word):
    return (word >> 9) & 0xFF


def vop2_opcode(word):
    return (word >> 25) & 0x3F


def sop1_opcode(word):
    return (word >> 8) & 0xFF


def sop2_opcode(word):
    return (word >> 23) & 0x7F


def sopk_opcode(word):
    return (word >> 23) & 0x1F


def mimg_nsa(word):
    # nsa field is in dword1 for MIMG
    return None


def instr_length(words, i):
    """Best-effort instruction length in dwords at words[i]. Returns (len, family)."""
    w = words[i]
    fam = family(w)
    if fam == "MIMG":
        return 2, fam
    if fam in TWO_WORD:
        return 2, fam
    if fam == "UNKNOWN":
        return 1, fam
    return 1, fam


def walk(words, start=0, end=None):
    """Yield (index, length, family) walking the stream."""
    if end is None:
        end = len(words)
    i = start
    while i < end:
        n, fam = instr_length(words, i)
        yield i, n, fam
        i += n


SOPP_NAMES = {
    0: "S_NOP", 1: "S_ENDPGM", 2: "S_BRANCH", 3: "S_WAKEUP",
    4: "S_CBRANCH_SCC0", 5: "S_CBRANCH_SCC1", 6: "S_CBRANCH_VCCZ",
    7: "S_CBRANCH_VCCNZ", 8: "S_CBRANCH_EXECZ", 9: "S_CBRANCH_EXECNZ",
    10: "S_BARRIER", 11: "S_SETKILL", 12: "S_WAITCNT", 13: "S_SETHALT",
    14: "S_SLEEP", 15: "S_SETPRIO", 16: "S_SENDMSG", 17: "S_SENDMSGHALT",
    18: "S_TRAP", 19: "S_ICACHE_INV", 20: "S_INCPERFLEVEL",
    21: "S_DECPERFLEVEL", 22: "S_TTRACEDATA", 23: "S_CBRANCH_CDBGSYS",
    24: "S_CBRANCH_CDBGUSER", 25: "S_CBRANCH_CDBGSYS_OR_USER",
    26: "S_CBRANCH_CDBGSYS_AND_USER", 27: "S_ENDPGM_SAVED",
    30: "S_ENDPGM_ORDERED_PS_DONE", 31: "S_CODE_END", 32: "S_INST_PREFETCH",
    33: "S_CLAUSE", 35: "S_WAITCNT_DEPCTR", 36: "S_ROUND_MODE",
    37: "S_DENORM_MODE", 40: "S_TTRACEDATA_IMM",
}
SOP1_NAMES = {
    0: "S_MOV_B32", 1: "S_MOV_B64", 2: "S_CMOV_B32", 3: "S_CMOV_B64",
    4: "S_NOT_B32", 5: "S_NOT_B64", 6: "S_WQM_B32", 7: "S_WQM_B64",
    8: "S_BREV_B32", 9: "S_BREV_B64", 10: "S_BCNT0_I32_B32",
    11: "S_BCNT0_I32_B64", 12: "S_BCNT1_I32_B32", 13: "S_BCNT1_I32_B64",
    16: "S_FF0_I32_B32", 17: "S_FF0_I32_B64", 18: "S_FF1_I32_B32",
    19: "S_FF1_I32_B64", 20: "S_FLBIT_I32_B32", 21: "S_FLBIT_I32_B64",
    22: "S_FLBIT_I32", 23: "S_FLBIT_I32_I64", 24: "S_SEXT_I32_I8",
    25: "S_SEXT_I32_I16", 28: "S_BITSET0_B32", 29: "S_BITSET0_B64",
    30: "S_BITSET1_B32", 31: "S_BITSET1_B64", 32: "S_GETPC_B64",
    33: "S_SETPC_B64", 34: "S_SWAPPC_B64", 35: "S_RFE_B64",
    36: "S_AND_SAVEEXEC_B32", 37: "S_AND_SAVEEXEC_B64", 38: "S_OR_SAVEEXEC_B32",
    39: "S_OR_SAVEEXEC_B64", 40: "S_XOR_SAVEEXEC_B32", 41: "S_XOR_SAVEEXEC_B64",
    42: "S_NAND_SAVEEXEC_B32", 43: "S_NAND_SAVEEXEC_B64", 44: "S_NOR_SAVEEXEC_B32",
    45: "S_NOR_SAVEEXEC_B64", 46: "S_XNOR_SAVEEXEC_B32", 47: "S_XNOR_SAVEEXEC_B64",
}
VOP1_NAMES = {
    0: "V_NOP", 1: "V_MOV_B32", 2: "V_READFIRSTLANE_B32", 3: "V_CVT_I32_F64",
    4: "V_CVT_F64_I32", 5: "V_CVT_F32_I32", 6: "V_CVT_F32_U32",
    7: "V_CVT_U32_F32", 8: "V_CVT_I32_F32", 11: "V_CVT_F16_F32",
    12: "V_CVT_F32_F16", 13: "V_CVT_RPI_I32_F32", 14: "V_CVT_FLR_I32_F32",
    15: "V_CVT_OFF_F32_I4", 16: "V_CVT_F32_F64", 17: "V_CVT_F64_F32",
    19: "V_CVT_F32_UBYTE0", 20: "V_CVT_F32_UBYTE1", 21: "V_CVT_F32_UBYTE2",
    22: "V_CVT_F32_UBYTE3", 23: "V_CVT_U32_F64", 24: "V_CVT_F64_U32",
    32: "V_FRACT_F32", 33: "V_TRUNC_F32", 34: "V_CEIL_F32", 35: "V_RNDNE_F32",
    36: "V_FLOOR_F32", 37: "V_EXP_F32", 38: "V_LOG_F32", 41: "V_SQRT_F32",
    47: "V_SIN_F32", 48: "V_COS_F32", 49: "V_NOT_B32", 50: "V_BFREV_B32",
    51: "V_FREXP_EXP_I32_F32", 52: "V_FREXP_MANT_F32", 53: "V_CLREXCP",
    54: "V_MOVRELD_B32", 55: "V_MOVRELS_B32", 56: "V_MOVRELSD_B32",
    57: "V_CVT_F16_U16", 58: "V_CVT_F16_I16", 59: "V_CVT_U16_F16",
    60: "V_CVT_I16_F16", 61: "V_RCP_F16", 62: "V_SQRT_F16", 63: "V_RSQ_F16",
    64: "V_LOG_F16", 65: "V_EXP_F16", 66: "V_FREXP_MANT_F16",
    67: "V_FREXP_EXP_I16_F16", 68: "V_FLOOR_F16", 69: "V_CEIL_F16",
    70: "V_TRUNC_F16", 71: "V_RNDNE_F16", 72: "V_FRACT_F16", 73: "V_SIN_F16",
    74: "V_COS_F16", 75: "V_EXP_LEGACY_F32", 76: "V_LOG_LEGACY_F32",
    77: "V_CVT_NORM_I16_F16", 78: "V_CVT_NORM_U16_F16",
    79: "V_SAT_PK_U8_I16", 80: "V_SCREEN_PARTITION_4SE_B32",
    81: "V_CVT_F32_FP8", 82: "V_CVT_F32_BF8", 83: "V_CVT_PK_F32_FP8",
    84: "V_CVT_PK_F32_BF8", 85: "V_CVT_PK_F16_FP8", 86: "V_CVT_PK_F16_BF8",
    87: "V_CVT_F32_BF16",
}
VOP2_NAMES = {
    0: "V_CNDMASK_B32", 1: "V_ADD_F32", 2: "V_SUB_F32", 3: "V_SUBREV_F32",
    4: "V_MUL_LEGACY_F32", 5: "V_MUL_F32", 6: "V_MUL_I32_I24", 7: "V_MUL_HI_I32_I24",
    8: "V_MUL_U32_U24", 9: "V_MUL_HI_U32_U24", 10: "V_MIN_F32", 11: "V_MAX_F32",
    12: "V_MIN_I32", 13: "V_MAX_I32", 14: "V_MIN_U32", 15: "V_MAX_U32",
    16: "V_LSHLREV_B32", 17: "V_LSHRREV_B32", 18: "V_ASHRREV_I32",
    19: "V_AND_B32", 20: "V_OR_B32", 21: "V_XOR_B32", 22: "V_MAC_F32",
    23: "V_MADMK_F32", 24: "V_ADD_I32", 25: "V_SUB_I32", 26: "V_SUBREV_I32",
    27: "V_ADDC_U32", 28: "V_SUBB_U32", 29: "V_SUBBREV_U32",
    30: "V_ADD_F16", 31: "V_SUB_F16", 32: "V_SUBREV_F16", 33: "V_MUL_F16",
    34: "V_MAC_F16", 35: "V_MADMK_F16", 36: "V_ADD_U16", 37: "V_SUB_U16",
    38: "V_SUBREV_U16", 39: "V_MUL_LO_U16", 40: "V_LSHLREV_B16",
    41: "V_LSHRREV_B16", 42: "V_ASHRREV_I16", 43: "V_MAX_F16", 44: "V_MIN_F16",
    45: "V_MAX_U16", 46: "V_MAX_I16", 47: "V_MIN_U16", 48: "V_MIN_I16",
    49: "V_LDEXP_F16", 50: "V_ADD_U32", 51: "V_SUB_U32", 52: "V_SUBREV_U32",
    53: "V_DOT2C_F32_F16", 54: "V_DOT2C_I32_I16", 55: "V_DOT4C_I32_I8",
    59: "V_DOT8C_I32_I4", 60: "V_FMAC_F32", 61: "V_XNOR_B32",
}


def name_of(fam, word, second=None):
    if fam == "SOPP":
        return SOPP_NAMES.get(sopp_opcode(word), "SOPP_%d" % sopp_opcode(word))
    if fam == "SOP1":
        return SOP1_NAMES.get(sop1_opcode(word), "SOP1_%d" % sop1_opcode(word))
    if fam == "VOP1":
        return VOP1_NAMES.get(vop1_opcode(word), "VOP1_%d" % vop1_opcode(word))
    if fam == "VOP2":
        return VOP2_NAMES.get(vop2_opcode(word), "VOP2_%d" % vop2_opcode(word))
    return fam
