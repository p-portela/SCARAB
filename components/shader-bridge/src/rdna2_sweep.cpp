// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// rdna2_sweep.cpp
//
// Diagnostic for: why does the upstream recompiler's decoder reject the target title's RDNA2 streams?
// Walks the instruction stream using the decoder's own wordCount, catches the
// rejection per instruction, and for VOP3 encodings decodes the modifier fields
// so the exact trigger is visible.
//
// Built the same way as rdna2_recompile.cpp. Read-only on the recompiler repo.

#include "RdnaDecoder/RdnaInstructionDecoder.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace ShaderRecompiler;

namespace {

std::vector<std::uint32_t> ReadWords(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<std::uint32_t> words(size / 4);
    in.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    return words;
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, int> tally;
    std::map<std::string, int> vop3Field;
    int shown = 0;
    const int showLimit = 40;

    for (int a = 1; a < argc; ++a) {
        const std::string path = argv[a];
        std::vector<std::uint32_t> words;
        try {
            words = ReadWords(path);
        } catch (const std::exception&) {
            continue;
        }

        std::size_t w = 0;
        while (w < words.size()) {
            try {
                const auto instruction = DecodeRdnaInstruction(static_cast<std::uint32_t>(w * 4u), words,
                                                               static_cast<std::uint32_t>(w));
                const std::uint32_t count = instruction.wordCount != 0u ? instruction.wordCount : 1u;
                w += count;
            } catch (const std::exception& ex) {
                const std::string message = ex.what();
                tally[message] += 1;

                const std::uint32_t word0 = words[w];
                const std::uint32_t word1 = (w + 1 < words.size()) ? words[w + 1] : 0u;
                std::string detail;
                if ((word0 >> 26u) == 0x35u) {
                    const std::uint32_t opcode = (word0 >> 16u) & 0x3ffu;
                    const std::uint32_t abs = (word0 >> 8u) & 0x7u;
                    const std::uint32_t opSel = (word0 >> 11u) & 0xfu;
                    const std::uint32_t clamp = (word0 >> 15u) & 0x1u;
                    const std::uint32_t omod = (word1 >> 27u) & 0x3u;
                    const std::uint32_t neg = (word1 >> 29u) & 0x7u;
                    char buf[192];
                    std::snprintf(buf, sizeof(buf),
                                  "VOP3 opcode=0x%03x abs=0x%x opSel=0x%x clamp=%u omod=0x%x neg=0x%x",
                                  opcode, abs, opSel, clamp, omod, neg);
                    detail = buf;

                    // Identify the mnemonic by re-decoding with the modifier bit
                    // fields cleared (bits 8..15 of word0).
                    std::vector<std::uint32_t> patched(words);
                    patched[w] = word0 & ~((0x7u << 8u) | (0xfu << 11u) | (1u << 15u));
                    try {
                        const auto clean = DecodeRdnaInstruction(static_cast<std::uint32_t>(w * 4u), patched,
                                                                 static_cast<std::uint32_t>(w));
                        std::string text = RdnaInstructionToString(clean);
                        const auto newline = text.find('\n');
                        if (newline != std::string::npos) text.resize(newline);
                        detail += " | unmasked: " + text;
                    } catch (const std::exception& inner) {
                        detail += std::string(" | unmasked also failed: ") + inner.what();
                    }
                    vop3Field[detail] += 1;
                } else {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "non-VOP3 word0=0x%08x", word0);
                    detail = buf;
                }

                if (shown < showLimit) {
                    std::printf("%s word %zu: %s | %s\n", path.c_str(), w, message.c_str(), detail.c_str());
                    ++shown;
                }
                w += 1u;
            }
        }
    }

    std::printf("\n=== rejection reasons ===\n");
    for (const auto& [message, count] : tally) {
        std::printf("%8d  %s\n", count, message.c_str());
    }
    std::printf("\n=== VOP3 modifier fields on rejected words ===\n");
    for (const auto& [detail, count] : vop3Field) {
        std::printf("%8d  %s\n", count, detail.c_str());
    }
    return 0;
}
