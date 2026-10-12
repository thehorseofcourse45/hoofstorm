// Corpus test for LLShaderTranslator (standalone, no GPU).
//
// Reads the shader dump written by the viewer (FS_SHADER_DUMP_DIR: manifest.txt + one file per shader object) and, for every
// program of the manifest, runs LLShaderCompiler: preprocess each object with glslang, merge and translate the stages, compile
// every stage for Vulkan 1.3 and link the stages (interfaces matched by name). Then it runs the SPIR-V validator on every
// module and compares the CPU-side std140 layout of the program uniform block with glslang's own reflection.
//
//   translator_test <dump_dir> [out_dir]      out_dir: write the translated sources and SPIR-V there
//
// Exit code 0 only if every stage compiled, every module validated, every program linked and every layout matched.

#include "shaderdump.h"

#include <spirv-tools/libspirv.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{

const char* nameOf(LLShaderTranslator::Stage s)
{
    return s == LLShaderTranslator::Stage::Vertex ? "vert" : s == LLShaderTranslator::Stage::Fragment ? "frag" : "geom";
}

// The interface between stages must agree: a varying has the same location and type in every stage that declares it, and no
// vertex output / geometry or fragment input is left without an explicit location (the compiler's own per-stage auto-mapping
// would pair them by declaration order). Returns an empty string if everything agrees.
std::string checkInterface(const LLShaderCompiler::Result& r)
{
    static const std::regex sLocated(R"(^layout\(location = (\d+)\) (?:(?:flat|smooth|noperspective|centroid) )*(in|out) (\w+) (\w+)(?:\[[^\]]*\])?;$)");
    static const std::regex sBare(R"(^(?:(?:flat|smooth|noperspective|centroid) )*(in|out) \S+ \w+(?:\[[^\]]*\])?;$)");
    std::map<std::string, std::pair<int, std::string>> seen;      // varying name -> (location, type)
    for (size_t i = 0; i < r.stageSources.size(); ++i)
    {
        const LLShaderTranslator::Stage stage = r.stages[i];
        std::istringstream in(r.stageSources[i]);
        std::string line;
        while (std::getline(in, line))
        {
            std::smatch m;
            if (std::regex_match(line, m, sBare))
            {
                const bool vertex_attribute = stage == LLShaderTranslator::Stage::Vertex && m[1].str() == "in";
                const bool fragment_output = stage == LLShaderTranslator::Stage::Fragment && m[1].str() == "out";
                if (!vertex_attribute && !fragment_output)
                {
                    return "stage interface variable without a location: " + line;
                }
            }
            else if (std::regex_match(line, m, sLocated))
            {
                const bool vertex_attribute = stage == LLShaderTranslator::Stage::Vertex && m[2].str() == "in";
                const bool fragment_output = stage == LLShaderTranslator::Stage::Fragment && m[2].str() == "out";
                if (vertex_attribute || fragment_output)
                {
                    continue;
                }
                const int location = atoi(m[1].str().c_str());
                auto it = seen.emplace(m[4].str(), std::make_pair(location, m[3].str())).first;
                if (it->second.first != location || it->second.second != m[3].str())
                {
                    return "varying '" + m[4].str() + "' differs between stages: " + line;
                }
            }
        }
    }
    return "";
}

// Paths the corpus does not reach: an existing uniform block, multi-declarator uniforms, initializers, shared samplers.
bool selfCheck(const LLShaderTranslator::Options& options)
{
    LLShaderTranslator::StageInput vs, fs;
    vs.stage = LLShaderTranslator::Stage::Vertex;
    vs.objects = { "#version 420\n"
                   "uniform mat4 mvp;\n"
                   "uniform float a = 1.0, b[3];\n"
                   "in vec3 position;\n"
                   "void main() { gl_Position = mvp * vec4(position, a + b[1]); }\n" };
    fs.stage = LLShaderTranslator::Stage::Fragment;
    fs.objects = { "#version 420\n"
                   "#extension GL_EXT_gpu_shader4 : enable\n"
                   "layout (std140) uniform ReflectionProbes { vec4 probe[4]; };\n"
                   "uniform sampler2D diffuseMap;\n"
                   "uniform float a;\n"
                   "out vec4 c;\n"
                   "void main() { c = texture(diffuseMap, vec2(0)) * a + probe[1]; }\n" };
    const LLShaderTranslator::Result r = LLShaderTranslator::translateProgram({ vs, fs }, options);
    bool ok = r.ok() && r.uniforms.size() == 3 && r.samplers.size() == 1 && r.uniformInitializersRemoved == 1 &&
              r.legacyExtensionsRemoved == 1 && r.attributesLocated == 1 && r.stageSources.size() == 2;
    if (ok)
    {
        ok = r.stageSources[1].find("layout(std140, set = 2, binding = 0) uniform ReflectionProbes") != std::string::npos &&
             r.stageSources[1].find("layout(set = 1, binding = 0) uniform sampler2D diffuseMap;") != std::string::npos &&
             r.stageSources[0].find("layout(location = 0) in vec3 position;") != std::string::npos &&
             r.stageSources[0].find("float b[3];") != std::string::npos && r.stageSources[1].find("float b[3];") != std::string::npos;
    }
    // mat4 (64) at 0, float a at 64, float b[3] (stride 16) at 80, size 128
    const LLShaderTranslator::BlockLayout layout = LLShaderTranslator::computeBlockLayout(r.uniforms);
    ok = ok && layout.error.empty() && layout.members.size() == 3 && layout.members[0].offset == 0 && layout.members[1].offset == 64 &&
         layout.members[2].offset == 80 && layout.members[2].size == 48 && layout.size == 128;
    if (!ok)
    {
        fprintf(stderr, "self-check FAILED\n");
        for (const std::string& src : r.stageSources)
        {
            fprintf(stderr, "---\n%s", src.c_str());
        }
        for (const std::string& e : r.errors)
        {
            fprintf(stderr, "error: %s\n", e.c_str());
        }
    }
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: translator_test <dump_dir> [out_dir]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string out_dir = argc > 2 ? argv[2] : "";
    const LLShaderTranslator::Options options = LLShaderCompiler::viewerOptions();

    if (!selfCheck(options))
    {
        return 1;
    }
    printf("self-check                        : ok\n");

    const std::vector<shaderdump::ManifestProgram> programs = shaderdump::readManifest(dir);
    if (programs.empty())
    {
        fprintf(stderr, "no programs in %s/manifest.txt\n", dir.c_str());
        return 2;
    }

    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);

    int stages_total = 0, stages_ok = 0, spirv_valid = 0, programs_ok = 0, layout_mismatches = 0;
    std::map<std::string, int> failures;                // first error -> count
    std::map<std::string, std::string> failure_example;
    size_t uniforms_total = 0, samplers_total = 0, attribs_total = 0;
    int legacy_removed = 0, initializers_removed = 0;

    auto fail = [&](const std::string& what, const std::string& where)
    {
        const std::string key = what.substr(0, 160);
        if (failures[key]++ == 0)
        {
            failure_example[key] = where;
        }
    };

    for (size_t p = 0; p < programs.size(); ++p)
    {
        const shaderdump::ManifestProgram& prog = programs[p];

        const std::vector<LLShaderCompiler::Stage> stages = shaderdump::loadStages(dir, prog);
        stages_total += (int) stages.size();

        const LLShaderCompiler::Result r = LLShaderCompiler::compileProgram(stages, options, true);
        layout_mismatches += r.layoutMismatches;
        if (!r.ok())
        {
            // failures in the first three phases leave no SPIR-V
            fail(r.error, prog.name);
            continue;
        }
        const std::string interface_error = checkInterface(r);
        if (!interface_error.empty())
        {
            fail("interface: " + interface_error, prog.name);
            continue;
        }
        ++programs_ok;
        stages_ok += (int) stages.size();
        uniforms_total += r.translation.uniforms.size();
        samplers_total += r.translation.samplers.size();
        attribs_total += r.translation.attributesLocated;
        legacy_removed += r.translation.legacyExtensionsRemoved;
        initializers_removed += r.translation.uniformInitializersRemoved;

        for (size_t i = 0; i < stages.size(); ++i)
        {
            if (validator.Validate(r.spirv[i]))
            {
                ++spirv_valid;
            }
            else
            {
                fail("spirv-val failed", prog.name + " [" + nameOf(stages[i].stage) + "]");
            }
            if (!out_dir.empty())
            {
                char name[64];
                snprintf(name, sizeof(name), "/%03zu.%s.vk.glsl", p, nameOf(stages[i].stage));
                std::ofstream(out_dir + name, std::ios::binary) << r.stageSources[i];
                snprintf(name, sizeof(name), "/%03zu.%s.spv", p, nameOf(stages[i].stage));
                std::ofstream(out_dir + name, std::ios::binary).write((const char*) r.spirv[i].data(), r.spirv[i].size() * 4);
            }
        }
    }

    printf("programs                          : %zu\n", programs.size());
    printf("program stages                    : %d\n", stages_total);
    printf("programs compiled and linked      : %d / %zu\n", programs_ok, programs.size());
    printf("stages in those programs          : %d / %d\n", stages_ok, stages_total);
    printf("SPIR-V modules valid (spirv-val)  : %d / %d\n", spirv_valid, stages_ok);
    printf("uniform block layout mismatches   : %d (our std140 offsets vs glslang's reflection)\n", layout_mismatches);
    printf("translation: %zu uniform block members, %zu samplers, %zu attribute locations, %d legacy #extension removed, %d initializers removed\n",
           uniforms_total, samplers_total, attribs_total, legacy_removed, initializers_removed);
    for (const auto& f : failures)
    {
        printf("FAIL x%d  %s   e.g. %s\n", f.second, f.first.c_str(), failure_example[f.first].c_str());
    }

    const bool pass = programs_ok == (int) programs.size() && stages_ok == stages_total && spirv_valid == stages_total && layout_mismatches == 0;
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
