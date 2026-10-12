// Corpus test for LLShaderTranslator (standalone, no GPU).
//
// Reads the shader dump written by the viewer (FS_SHADER_DUMP_DIR: manifest.txt + one file per shader object) and, for every
// program of the manifest: preprocesses each object with glslang, merges and translates the stages with LLShaderTranslator,
// compiles every stage for Vulkan 1.3, links the stages of the program (interfaces matched by name), generates SPIR-V and runs
// the SPIR-V validator on it.
//
//   translator_test <dump_dir> [out_dir]      out_dir: write the translated sources and SPIR-V there
//
// Exit code 0 only if every stage compiled, every module validated and every program linked.

#include "llshadertranslator.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv-tools/libspirv.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{

struct ManifestProgram
{
    std::string name;
    std::vector<std::string> objects;   // attached objects, attach order, no duplicates
};

std::vector<ManifestProgram> readManifest(const std::string& dir)
{
    std::vector<ManifestProgram> programs;
    std::ifstream in(dir + "/manifest.txt");
    std::string line;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        {
            line.pop_back();
        }
        if (line.compare(0, 8, "PROGRAM ") == 0)
        {
            programs.push_back({ line.substr(8), {} });
        }
        else if (line.compare(0, 7, "ATTACH ") == 0 && !programs.empty() && line.substr(7) != "?")
        {
            auto& objs = programs.back().objects;
            const std::string file = line.substr(7);
            if (std::find(objs.begin(), objs.end(), file) == objs.end())
            {
                objs.push_back(file);
            }
        }
    }
    return programs;
}

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

EShLanguage languageOf(const std::string& file)
{
    const std::string ext = file.substr(file.rfind('.') + 1);
    if (ext == "vert") return EShLangVertex;
    if (ext == "frag") return EShLangFragment;
    if (ext == "geom") return EShLangGeometry;
    return EShLangCount;
}

LLShaderTranslator::Stage stageOf(EShLanguage lang)
{
    switch (lang)
    {
    case EShLangVertex: return LLShaderTranslator::Stage::Vertex;
    case EShLangGeometry: return LLShaderTranslator::Stage::Geometry;
    default: return LLShaderTranslator::Stage::Fragment;
    }
}

const char* nameOf(EShLanguage lang)
{
    return lang == EShLangVertex ? "vert" : lang == EShLangFragment ? "frag" : "geom";
}

// What the viewer's GL path does for you: preprocess one object (resolve #if/#define) with the OpenGL semantics it was written for.
bool preprocess(const std::string& source, EShLanguage lang, std::string& out, std::string& log)
{
    glslang::TShader shader(lang);
    const char* text = source.c_str();
    shader.setStrings(&text, 1);
    glslang::TShader::ForbidIncluder includer;
    bool ok = shader.preprocess(GetDefaultResources(), 110, ENoProfile, false, false, EShMsgDefault, &out, includer);
    log = shader.getInfoLog();
    return ok;
}

std::string firstError(const std::string& log)
{
    std::istringstream in(log);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.find("ERROR") != std::string::npos)
        {
            return line.substr(0, 200);
        }
    }
    return log.substr(0, 200);
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

    LLShaderTranslator::Options options;
    // order of LLShaderMgr::initAttribsAndUniforms (must match LLVertexBuffer's attribute enum)
    options.attribNames = { "position", "normal", "texcoord0", "texcoord1", "texcoord2", "texcoord3", "diffuse_color",
                            "emissive", "tangent", "weight", "weight4", "clothing", "joint", "texture_index" };
    // LLGLSLShader::UniformBlock
    options.blockBindings = { { "ReflectionProbes", 0 }, { "GLTFJoints", 1 }, { "GLTFNodes", 2 }, { "GLTFMaterials", 3 } };

    if (!selfCheck(options))
    {
        return 1;
    }
    printf("self-check                        : ok\n");

    const std::vector<ManifestProgram> programs = readManifest(dir);
    if (programs.empty())
    {
        fprintf(stderr, "no programs in %s/manifest.txt\n", dir.c_str());
        return 2;
    }

    glslang::InitializeProcess();
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);

    std::map<std::string, std::string> preprocessed;    // object file -> preprocessed text
    int stages_total = 0, stages_ok = 0, spirv_valid = 0, programs_linked = 0, translate_errors = 0;
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
        const ManifestProgram& prog = programs[p];

        // group objects by stage, keeping attach order
        std::vector<EShLanguage> langs;
        std::map<EShLanguage, std::vector<std::string>> by_stage;
        for (const std::string& obj : prog.objects)
        {
            const EShLanguage lang = languageOf(obj);
            if (lang == EShLangCount)
            {
                continue;
            }
            if (!by_stage.count(lang))
            {
                langs.push_back(lang);
            }
            by_stage[lang].push_back(obj);
        }
        std::sort(langs.begin(), langs.end());

        std::vector<LLShaderTranslator::StageInput> inputs;
        bool prep_ok = true;
        for (EShLanguage lang : langs)
        {
            LLShaderTranslator::StageInput in;
            in.stage = stageOf(lang);
            for (const std::string& obj : by_stage[lang])
            {
                auto it = preprocessed.find(obj);
                if (it == preprocessed.end())
                {
                    std::string text, log;
                    if (!preprocess(readFile(dir + "/" + obj), lang, text, log))
                    {
                        fail("preprocess: " + firstError(log), prog.name + " / " + obj);
                        prep_ok = false;
                    }
                    it = preprocessed.emplace(obj, text).first;
                }
                in.objects.push_back(it->second);
            }
            inputs.push_back(std::move(in));
        }
        stages_total += (int)langs.size();
        if (!prep_ok)
        {
            continue;
        }

        const LLShaderTranslator::Result tr = LLShaderTranslator::translateProgram(inputs, options);
        uniforms_total += tr.uniforms.size();
        samplers_total += tr.samplers.size();
        attribs_total += tr.attributesLocated;
        legacy_removed += tr.legacyExtensionsRemoved;
        initializers_removed += tr.uniformInitializersRemoved;
        if (!tr.ok())
        {
            ++translate_errors;
            fail("translate: " + tr.errors[0], prog.name);
            continue;
        }

        // compile every stage, then link them as one program
        std::vector<std::unique_ptr<glslang::TShader>> shaders;
        std::vector<std::string> sources = tr.stageSources;     // keep the text alive for glslang
        glslang::TProgram program;
        bool compiled = true;
        for (size_t i = 0; i < langs.size(); ++i)
        {
            auto shader = std::make_unique<glslang::TShader>(langs[i]);
            const char* text = sources[i].c_str();
            shader->setStrings(&text, 1);
            shader->setEnvInput(glslang::EShSourceGlsl, langs[i], glslang::EShClientVulkan, 100);
            shader->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
            shader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
            shader->setAutoMapLocations(true);      // varyings: matched by name across the stages
            shader->setAutoMapBindings(true);       // anything we did not bind explicitly
            const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
            if (shader->parse(GetDefaultResources(), 450, false, messages))
            {
                ++stages_ok;
            }
            else
            {
                compiled = false;
                fail("compile: " + firstError(shader->getInfoLog()), prog.name + " [" + nameOf(langs[i]) + "]");
            }
            program.addShader(shader.get());
            shaders.push_back(std::move(shader));
            if (!out_dir.empty())
            {
                char name[64];
                snprintf(name, sizeof(name), "/%03zu.%s.vk.glsl", p, nameOf(langs[i]));
                std::ofstream(out_dir + name, std::ios::binary) << sources[i];
            }
        }
        if (!compiled)
        {
            continue;
        }

        const EShMessages link_messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
        if (!program.link(link_messages) || !program.mapIO())
        {
            fail("link: " + firstError(std::string(program.getInfoLog()) + program.getInfoDebugLog()), prog.name);
            continue;
        }
        ++programs_linked;

        for (size_t i = 0; i < langs.size(); ++i)
        {
            std::vector<unsigned int> spirv;
            glslang::GlslangToSpv(*program.getIntermediate(langs[i]), spirv);
            if (validator.Validate(spirv))
            {
                ++spirv_valid;
            }
            else
            {
                fail("spirv-val failed", prog.name + " [" + nameOf(langs[i]) + "]");
            }
            if (!out_dir.empty())
            {
                char name[64];
                snprintf(name, sizeof(name), "/%03zu.%s.spv", p, nameOf(langs[i]));
                std::ofstream(out_dir + name, std::ios::binary).write((const char*)spirv.data(), spirv.size() * 4);
            }
        }
    }

    printf("programs                          : %zu\n", programs.size());
    printf("program stages                    : %d\n", stages_total);
    printf("stages compiled for Vulkan        : %d / %d\n", stages_ok, stages_total);
    printf("SPIR-V modules valid (spirv-val)  : %d / %d\n", spirv_valid, stages_total);
    printf("programs linked across stages     : %d / %zu\n", programs_linked, programs.size());
    printf("translation: %zu uniform block members, %zu samplers, %zu attribute locations, %d legacy #extension removed, %d initializers removed\n",
           uniforms_total, samplers_total, attribs_total, legacy_removed, initializers_removed);
    for (const auto& f : failures)
    {
        printf("FAIL x%d  %s   e.g. %s\n", f.second, f.first.c_str(), failure_example[f.first].c_str());
    }

    glslang::FinalizeProcess();
    const bool pass = stages_ok == stages_total && spirv_valid == stages_total && programs_linked == (int)programs.size();
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
