// rdna2_recompile.cpp
//
// Bridge tool: takes a raw RDNA2 word stream (produced by
// tools/fssm_to_rdna2.py from the target title / Scarlett FSSM shaders) and runs it
// through the upstream shader recompiler's shader recompiler.
//
//   --disasm        disassemble via RdnaInstructionDecoder + RdnaProgramToString
//   --spv <path>    recompile to a SPIR-V binary module
//   --stage <s>     compute (default) | pixel | vertex | mesh
//   --wave <32|64>  wave size (default 32)
//
// the upstream shader recompiler findings this relies on (source-verified; see
// docs/the recompiler integration notes):
//   * the recompiler engine never dereferences ShaderBinary.header, so for an
//     console shader header may be an empty span and headerAddress 0;
//   * everything the Agc driver would derive from the PS5 header must instead be
//     hand-supplied in GuestContext;
//   * a raw word span needs no capture.
//
// Built against the existing the recompiler build tree; the recompiler repo is not modified.

#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "Recompiler.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <span>
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

bool ParseStage(const std::string& s, ShaderStage& out) {
    if (s == "compute") { out = ShaderStage::Compute; return true; }
    if (s == "pixel" || s == "fragment") { out = ShaderStage::Fragment; return true; }
    if (s == "vertex") { out = ShaderStage::Vertex; return true; }
    if (s == "mesh") { out = ShaderStage::Mesh; return true; }
    return false;
}

// The minimal compute request from docs/the recompiler integration notes Q2.
RecompileRequest BuildComputeRequest(std::span<const std::uint32_t> code, std::uint32_t wave) {
    static const std::uint32_t caps[] = {1u /*ImageGatherExtended*/, 11u /*Int64*/,
                                         5347u /*PhysicalStorageBufferAddresses*/};
    static const std::string_view exts[] = {"SPV_KHR_physical_storage_buffer"};

    RecompileRequest request{};
    request.shader.stage = ShaderStage::Compute;
    request.shader.codeAddress = 0x22000;
    request.shader.code = code;
    request.shader.headerAddress = 0;
    request.shader.header = {};

    request.context.waveSize = wave;
    request.context.userDataBaseRegister = 0;
    request.context.userData = {};
    request.context.memory = MemoryRegionView{};

    ShaderComputeStageInfo compute{};
    compute.numThreads = {64u, 1u, 1u};
    compute.ldsSizeDwords = 0u;
    compute.groupIdEnable = {false, false, false};
    compute.tgSizeEnable = false;
    compute.threadIdComponentCount = 1u;
    request.context.compute = compute;

    request.target.vulkanVersion = 0x00401000u;
    request.target.spirvVersion = 0x00010300u;
    request.target.subgroupSize = wave;
    request.target.bdaAbiVersion = 1u;
    request.target.supportedCapabilities = caps;
    request.target.supportedExtensions = exts;

    request.layout = BindingLayout{0u, 0u, 0u, 128u};
    request.useCache = false;
    return request;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> files;
    std::string spvOut;
    std::string stageName = "compute";
    std::uint32_t wave = 32;
    bool wantDisasm = false;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--disasm") wantDisasm = true;
        else if (a == "--spv" && i + 1 < argc) spvOut = argv[++i];
        else if (a == "--stage" && i + 1 < argc) stageName = argv[++i];
        else if (a == "--wave" && i + 1 < argc) wave = static_cast<std::uint32_t>(std::stoi(argv[++i]));
        else if (a == "--quiet") quiet = true;
        else files.push_back(a);
    }
    if (files.empty()) {
        std::cerr << "usage: rdna2_recompile <file.rdna2>... [--disasm] [--spv out.spv]"
                     " [--stage compute|pixel|vertex|mesh] [--wave 32|64]\n";
        return 2;
    }

    ShaderStage stage = ShaderStage::Compute;
    if (!ParseStage(stageName, stage)) {
        std::cerr << "unknown stage: " << stageName << "\n";
        return 2;
    }

    int ok = 0;
    int failed = 0;
    for (const auto& path : files) {
        try {
            const auto words = ReadWords(path);
            if (words.empty()) { std::cerr << path << ": empty\n"; ++failed; continue; }

            if (wantDisasm && !quiet) {
                const auto program = RdnaInstructionDecoder{}.Decode(words);
                std::cout << "=== " << path << " (" << words.size() << " words) ===\n";
                std::cout << RdnaProgramToString(program);
            }

            if (!spvOut.empty()) {
                const auto request = BuildComputeRequest(words, wave);
                const auto result = Recompile(request);
                const auto& image = result.spirv;
                std::ofstream out(spvOut, std::ios::binary);
                out.write(reinterpret_cast<const char*>(image.data()),
                          static_cast<std::streamsize>(image.size() * 4));
                if (!quiet) {
                    std::cout << path << ": SPIR-V " << image.size() << " words -> " << spvOut
                              << (image.size() > 5 &&
                                          image[0] == 0x07230203u ? " (magic OK)" : " (BAD MAGIC)")
                              << "\n";
                }
            }
            ++ok;
        } catch (const std::exception& ex) {
            ++failed;
            if (!quiet) std::cout << path << ": FAIL " << ex.what() << "\n";
        }
    }
    if (!quiet) {
        std::cerr << "done: " << ok << " ok, " << failed << " failed\n";
    }
    return failed == 0 ? 0 : 1;
}
