// Build-time tool, not part of AMD's SDK. Compiles the vendored FSR 3.1 upscaler GLSL to SPIR-V with the repo's glslang and
// writes the shader-blob table that the SDK's Vulkan backend asks for through ffxGetPermutationBlobByIndex().
// It replaces AMD's FidelityFX_SC output: same permutation options, same resource names, bindings read back from the SPIR-V.
//   usage: ffx_blobgen <ffx directory> <output .cpp>
// Permutations (see ffx/README): HDR input and render-resolution motion vectors are always on, inverted depth, display-size
// and jitter-cancelled motion vectors always off. Free choices: Lanczos LUT, FP16, sharpening (accumulate pass only).
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// FSR3UPSCALER_SHADER_PERMUTATION_* in src/components/fsr3upscaler/ffx_fsr3upscaler_private.h (static_asserted in the output)
enum : uint32_t { kLut = 1, kHdr = 2, kLowResMv = 4, kJitterMv = 8, kInverted = 16, kSharpen = 32, kWave64 = 64, kFp16 = 128 };

struct PassDef { const char* ffxPass; const char* file; bool half; uint32_t extra; };      // 'half': the SDK has a 16-bit variant
static const PassDef kPasses[] = {
    {"FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS", "prepare_inputs", false, 0},
    {"FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID", "luma_pyramid", false, 0},
    {"FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID", "shading_change_pyramid", false, 0},
    {"FFX_FSR3UPSCALER_PASS_SHADING_CHANGE", "shading_change", true, 0},
    {"FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY", "prepare_reactivity", true, 0},
    {"FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY", "luma_instability", false, 0},
    {"FFX_FSR3UPSCALER_PASS_ACCUMULATE", "accumulate", true, 0},
    {"FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN", "accumulate", true, kSharpen},
    {"FFX_FSR3UPSCALER_PASS_RCAS", "rcas", false, 0},
    {"FFX_FSR3UPSCALER_PASS_DEBUG_VIEW", "debug_view", false, 0},
    {"FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE", "autogen_reactive", true, 0},
};

struct Includer : glslang::TShader::Includer
{
    std::vector<fs::path> dirs;
    IncludeResult* find(const char* name, const char* from)
    {
        std::vector<fs::path> candidates;
        if (from && *from) candidates.push_back(fs::path(from).parent_path() / name);
        for (const auto& d : dirs) candidates.push_back(d / name);
        for (const auto& c : candidates)
        {
            std::ifstream in(c, std::ios::binary);
            if (!in) continue;
            auto* text = new std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return new IncludeResult(c.string(), text->data(), text->size(), text);
        }
        return nullptr;
    }
    IncludeResult* includeSystem(const char* name, const char* from, size_t) override { return find(name, from); }
    IncludeResult* includeLocal(const char* name, const char* from, size_t) override { return find(name, from); }
    void releaseInclude(IncludeResult* r) override
    {
        if (r) { delete static_cast<std::string*>(r->userData); delete r; }
    }
};

struct Resource { std::string name; uint32_t binding = 0; };
struct Blob
{
    std::vector<uint32_t> spirv;
    std::vector<Resource> cbv, srvTex, uavTex;
};

static std::string preambleFor(uint32_t options, bool half)
{
    std::ostringstream o;
    o << "#define FFX_GPU 1\n#define FFX_GLSL 1\n#define FFX_HALF " << (half ? 1 : 0) << "\n"
      << "#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF 0\n"
      << "#define FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF 0\n"
      << "#define FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF 1\n"
      << "#define FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF 0\n"
      << "#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 2\n"
      << "#define FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE " << ((options & kLut) ? 1 : 0) << "\n"
      << "#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT " << ((options & kHdr) ? 1 : 0) << "\n"
      << "#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS " << ((options & kLowResMv) ? 1 : 0) << "\n"
      << "#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS " << ((options & kJitterMv) ? 1 : 0) << "\n"
      << "#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH " << ((options & kInverted) ? 1 : 0) << "\n"
      << "#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING " << ((options & kSharpen) ? 1 : 0) << "\n";
    return o.str();
}

// names and bindings of the shader's buffers and images (set 0 only; the two samplers at 1000/1001 belong to the backend)
static bool reflect(const std::vector<uint32_t>& w, Blob& out, std::string& error)
{
    struct Type { uint32_t op = 0, a = 0, b = 0, sampled = 0; bool block = false; };
    std::map<uint32_t, Type> types;
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, uint32_t> set, binding;
    struct Var { uint32_t id, type, storage; };
    std::vector<Var> vars;
    for (size_t i = 5; i < w.size();)
    {
        const uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > w.size()) { error = "malformed SPIR-V"; return false; }
        const uint32_t* p = &w[i + 1];
        switch (op)
        {
        case 5: names[p[0]] = reinterpret_cast<const char*>(p + 1); break;                          // OpName
        case 71:                                                                                    // OpDecorate
            if (p[1] == 34) set[p[0]] = p[2];
            else if (p[1] == 33) binding[p[0]] = p[2];
            else if (p[1] == 2 || p[1] == 3) { types[p[0]].block = true; if (p[1] == 3) types[p[0]].b = 1; }   // Block, BufferBlock
            break;
        case 25: types[p[0]].op = op; types[p[0]].sampled = p[6]; break;                            // OpTypeImage
        case 26: types[p[0]].op = op; break;                                                        // OpTypeSampler
        case 30: types[p[0]].op = op; break;                                                        // OpTypeStruct
        case 28: case 29: types[p[0]].op = op; types[p[0]].a = p[1]; break;                         // OpTypeArray, OpTypeRuntimeArray
        case 32: types[p[0]].op = op; types[p[0]].a = p[2]; break;                                  // OpTypePointer -> pointee
        case 59: vars.push_back({p[1], p[0], p[2]}); break;                                         // OpVariable
        }
        i += n;
    }
    for (const Var& v : vars)
    {
        if (v.storage != 0 && v.storage != 2 && v.storage != 12) continue;                          // UniformConstant, Uniform, StorageBuffer
        uint32_t t = types[v.type].a;
        while (types[t].op == 28 || types[t].op == 29) t = types[t].a;
        const Type& ty = types[t];
        if (ty.op == 26) continue;                                                                  // sampler
        const std::string name = names[v.id];
        if (set[v.id] != 0 || !binding.count(v.id) || name.empty()) { error = "unexpected resource " + name; return false; }
        const Resource r{name, binding[v.id]};
        if (ty.op == 25 && ty.sampled == 1) out.srvTex.push_back(r);
        else if (ty.op == 25 && ty.sampled == 2) out.uavTex.push_back(r);
        else if (ty.op == 30 && v.storage == 2 && ty.block && !ty.b) out.cbv.push_back(r);
        else { error = "unsupported resource kind: " + name; return false; }
    }
    return true;
}

static std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv)
{
    if (argc != 3) { fprintf(stderr, "usage: ffx_blobgen <ffx directory> <output .cpp>\n"); return 2; }
    const fs::path root = argv[1];
    Includer includer;
    includer.dirs = {root / "include/FidelityFX/gpu", root / "include/FidelityFX/gpu/fsr3upscaler"};
    glslang::InitializeProcess();

    std::map<std::string, int> blobBySource;        // preprocessed GLSL -> blob index: identical permutations share one module
    std::vector<Blob> blobs;
    struct Entry { const char* pass; uint32_t options; int blob; };
    std::vector<Entry> entries;

    for (const PassDef& pass : kPasses)
    {
        const fs::path path = root / "src/backends/vk/shaders/fsr3upscaler" / (std::string("ffx_fsr3upscaler_") + pass.file + "_pass.glsl");
        const std::string text = readFile(path), pathName = path.string();
        if (text.empty()) { fprintf(stderr, "ffx_blobgen: cannot read %s\n", pathName.c_str()); return 1; }
        for (uint32_t lut = 0; lut < 2; ++lut)
            for (uint32_t fp16 = 0; fp16 < 2; ++fp16)
            {
                const uint32_t options = kHdr | kLowResMv | pass.extra | (lut ? kLut : 0) | (fp16 ? kFp16 : 0);
                const bool half = fp16 && pass.half;
                const std::string preamble = preambleFor(options, half);
                const char* src = text.data();
                const int len = (int) text.size();
                const char* name = pathName.c_str();

                auto makeShader = [&](glslang::TShader& s) {
                    s.setStringsWithLengthsAndNames(&src, &len, &name, 1);
                    s.setPreamble(preamble.c_str());
                    s.setEntryPoint("main");
                    s.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
                    s.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
                    s.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
                };
                const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
                std::string preprocessed;
                {
                    glslang::TShader shader(EShLangCompute);
                    makeShader(shader);
                    if (!shader.preprocess(GetDefaultResources(), 450, ENoProfile, false, false, messages, &preprocessed, includer))
                    {
                        fprintf(stderr, "ffx_blobgen: preprocess %s: %s\n", pass.file, shader.getInfoLog());
                        return 1;
                    }
                }
                auto found = blobBySource.find(preprocessed);
                if (found == blobBySource.end())
                {
                    glslang::TShader shader(EShLangCompute);
                    makeShader(shader);
                    glslang::TProgram program;
                    if (!shader.parse(GetDefaultResources(), 450, ENoProfile, false, false, messages, includer))
                    {
                        fprintf(stderr, "ffx_blobgen: compile %s (options %u): %s\n", pass.file, options, shader.getInfoLog());
                        return 1;
                    }
                    program.addShader(&shader);
                    if (!program.link(messages))
                    {
                        fprintf(stderr, "ffx_blobgen: link %s: %s\n", pass.file, program.getInfoLog());
                        return 1;
                    }
                    Blob blob;
                    spv::SpvBuildLogger logger;
                    glslang::SpvOptions spvOptions;
                    spvOptions.validate = true;
                    spvOptions.disableOptimizer = false;
                    spvOptions.optimizeSize = true;       // FidelityFX_SC builds with -Os
                    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), blob.spirv, &logger, &spvOptions);
                    std::string error;
                    if (blob.spirv.empty() || !reflect(blob.spirv, blob, error))
                    {
                        fprintf(stderr, "ffx_blobgen: %s (options %u): %s %s\n", pass.file, options, error.c_str(), logger.getAllMessages().c_str());
                        return 1;
                    }
                    found = blobBySource.emplace(preprocessed, (int) blobs.size()).first;
                    blobs.push_back(std::move(blob));
                }
                entries.push_back({pass.ffxPass, options, found->second});
            }
    }

    std::ostringstream o;
    o << "// Generated by ffx/tools/ffx_blobgen.cpp at build time. Do not edit.\n"
         "#include <FidelityFX/host/ffx_fsr3upscaler.h>\n#include <ffx_shader_blobs.h>\n#include \"fsr3upscaler/ffx_fsr3upscaler_private.h\"\n#include <string.h>\n\n"
         "static_assert(FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE == 1 && FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT == 2 &&\n"
         "              FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS == 4 && FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING == 32 &&\n"
         "              FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64 == 64 && FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16 == 128,\n"
         "              \"permutation bits changed: update ffx_blobgen.cpp\");\n\nnamespace {\n";
    for (size_t b = 0; b < blobs.size(); ++b)
    {
        o << "const uint32_t spv" << b << "[] = {";
        for (size_t i = 0; i < blobs[b].spirv.size(); ++i) o << (!i ? "\n" : i % 12 ? "," : ",\n") << "0x" << std::hex << blobs[b].spirv[i] << std::dec;
        o << "};\n";
        auto group = [&](const char* tag, const std::vector<Resource>& rs) {
            if (rs.empty()) return;
            o << "const char* " << tag << "N" << b << "[] = {";
            for (const auto& r : rs) o << "\"" << r.name << "\",";
            o << "};\nconst uint32_t " << tag << "B" << b << "[] = {";
            for (const auto& r : rs) o << r.binding << ",";
            o << "};\nconst uint32_t " << tag << "C" << b << "[] = {";
            for (size_t i = 0; i < rs.size(); ++i) o << "1,";
            o << "};\nconst uint32_t " << tag << "S" << b << "[] = {";
            for (size_t i = 0; i < rs.size(); ++i) o << "0,";
            o << "};\n";
        };
        group("cb", blobs[b].cbv);
        group("srv", blobs[b].srvTex);
        group("uav", blobs[b].uavTex);
    }
    o << "\nstruct Entry { FfxFsr3UpscalerPass pass; uint32_t options; int blob; };\n";
    o << "const FfxShaderBlob kBlobs[] = {\n";
    for (size_t b = 0; b < blobs.size(); ++b)
    {
        auto ptrs = [&](const char* tag, const std::vector<Resource>& rs) {
            std::ostringstream s;
            if (rs.empty()) return std::string("nullptr, nullptr, nullptr, nullptr,");
            for (const char* part : {"N", "B", "C", "S"}) s << tag << part << b << ", ";
            return s.str();
        };
        const Blob& x = blobs[b];
        o << "  { reinterpret_cast<const uint8_t*>(spv" << b << "), sizeof(spv" << b << "), " << x.cbv.size() << ", " << x.srvTex.size() << ", "
          << x.uavTex.size() << ", 0, 0, 0, 0,\n    " << ptrs("cb", x.cbv) << " " << ptrs("srv", x.srvTex) << " " << ptrs("uav", x.uavTex)
          << "\n    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr },\n";
    }
    o << "};\nconst Entry kEntries[] = {\n";
    for (const Entry& e : entries) o << "  {" << e.pass << ", " << e.options << ", " << e.blob << "},\n";
    o << "};\n}  // namespace\n\n"
         "extern \"C\" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effectId, FfxPass passId, FfxBindStage, uint32_t permutationOptions, FfxShaderBlob* outBlob)\n{\n"
         "    if (effectId != FFX_EFFECT_FSR3UPSCALER || !outBlob) return FFX_ERROR_INVALID_ARGUMENT;\n"
         "    permutationOptions &= ~(uint32_t) FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64;      // same SPIR-V on Vulkan\n"
         "    for (const Entry& e : kEntries)\n"
         "        if ((uint32_t) e.pass == (uint32_t) passId && e.options == permutationOptions)\n"
         "        {\n"
         "            memcpy(outBlob, &kBlobs[e.blob], sizeof(FfxShaderBlob));\n"
         "            return FFX_OK;\n"
         "        }\n"
         "    return FFX_ERROR_INVALID_ENUM;      // a permutation the viewer does not build\n"
         "}\n\n"
         "extern \"C\" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool& isWave64)\n{\n    isWave64 = false;\n    return FFX_OK;\n}\n";

    std::ofstream out(argv[2], std::ios::binary);
    out << o.str();
    if (!out) { fprintf(stderr, "ffx_blobgen: cannot write %s\n", argv[2]); return 1; }
    printf("ffx_blobgen: %zu entries, %zu distinct SPIR-V modules\n", entries.size(), blobs.size());
    glslang::FinalizeProcess();
    return 0;
}
