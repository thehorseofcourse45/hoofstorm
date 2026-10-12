#include "llshadercompiler.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <map>
#include <memory>

namespace
{
EShLanguage languageOf(LLShaderTranslator::Stage s)
{
    switch (s)
    {
    case LLShaderTranslator::Stage::Vertex: return EShLangVertex;
    case LLShaderTranslator::Stage::Geometry: return EShLangGeometry;
    default: return EShLangFragment;
    }
}

std::string firstError(const std::string& log)
{
    size_t pos = log.find("ERROR");
    if (pos == std::string::npos)
    {
        return log.substr(0, 200);
    }
    return log.substr(pos, log.find('\n', pos) - pos).substr(0, 200);
}

void initGlslang()
{
    static const bool once = (glslang::InitializeProcess(), true);
    (void) once;
}
} // namespace

LLShaderTranslator::Options LLShaderCompiler::viewerOptions()
{
    LLShaderTranslator::Options options;
    // order of LLShaderMgr::initAttribsAndUniforms (must match LLVertexBuffer's attribute enum)
    options.attribNames = { "position", "normal", "texcoord0", "texcoord1", "texcoord2", "texcoord3", "diffuse_color",
                            "emissive", "tangent", "weight", "weight4", "clothing", "joint", "texture_index" };
    // LLGLSLShader::UniformBlock
    options.blockBindings = { { "ReflectionProbes", 0 }, { "GLTFJoints", 1 }, { "GLTFNodes", 2 }, { "GLTFMaterials", 3 } };
    return options;
}

LLShaderCompiler::Result LLShaderCompiler::compileProgram(const std::vector<Stage>& stages, const LLShaderTranslator::Options& options,
                                                          bool checkLayout)
{
    initGlslang();
    Result result;

    // 1. preprocess every object (resolve #if/#define) with the OpenGL semantics the source was written for
    std::vector<LLShaderTranslator::StageInput> inputs;
    for (const Stage& s : stages)
    {
        LLShaderTranslator::StageInput in;
        in.stage = s.stage;
        for (const std::string& object : s.objects)
        {
            glslang::TShader shader(languageOf(s.stage));
            const char* text = object.c_str();
            shader.setStrings(&text, 1);
            glslang::TShader::ForbidIncluder includer;
            std::string out;
            if (!shader.preprocess(GetDefaultResources(), 110, ENoProfile, false, false, EShMsgDefault, &out, includer))
            {
                result.error = "preprocess: " + firstError(shader.getInfoLog());
                return result;
            }
            in.objects.push_back(out);
        }
        inputs.push_back(std::move(in));
        result.stages.push_back(s.stage);
    }

    // 2. merge and translate
    result.translation = LLShaderTranslator::translateProgram(inputs, options);
    if (!result.translation.ok())
    {
        result.error = "translate: " + result.translation.errors[0];
        return result;
    }
    result.stageSources = result.translation.stageSources;
    result.layout = LLShaderTranslator::computeBlockLayout(result.translation.uniforms);
    if (!result.layout.error.empty())
    {
        result.error = "layout: " + result.layout.error;
        return result;
    }

    // 3. compile every stage for Vulkan 1.3 and link them (varying locations are matched by name)
    std::vector<std::unique_ptr<glslang::TShader>> shaders;
    glslang::TProgram program;
    for (size_t i = 0; i < stages.size(); ++i)
    {
        const EShLanguage lang = languageOf(stages[i].stage);
        auto shader = std::make_unique<glslang::TShader>(lang);
        const char* text = result.stageSources[i].c_str();
        shader->setStrings(&text, 1);
        shader->setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
        shader->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
        shader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
        shader->setAutoMapLocations(true);
        shader->setAutoMapBindings(true);
        if (!shader->parse(GetDefaultResources(), 450, false, (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules)))
        {
            result.error = "compile: " + firstError(shader->getInfoLog());
            return result;
        }
        program.addShader(shader.get());
        shaders.push_back(std::move(shader));
    }
    if (!program.link((EShMessages)(EShMsgSpvRules | EShMsgVulkanRules)) || !program.mapIO())
    {
        result.error = "link: " + firstError(std::string(program.getInfoLog()) + program.getInfoDebugLog());
        return result;
    }

    for (size_t i = 0; i < stages.size(); ++i)
    {
        std::vector<unsigned int> spirv;
        glslang::GlslangToSpv(*program.getIntermediate(languageOf(stages[i].stage)), spirv);
        result.spirv.push_back(std::move(spirv));
    }

    // 4. the layout the CPU side will write must be the one the compiler gave the block
    if (checkLayout && !result.layout.members.empty())
    {
        program.buildReflection();
        std::map<std::string, int> offsets;
        for (int i = 0; i < program.getNumUniformVariables(); ++i)
        {
            std::string name = program.getUniform(i).name;
            const size_t dot = name.rfind('.');
            if (dot != std::string::npos)
            {
                name = name.substr(dot + 1);
            }
            const size_t bracket = name.find('[');
            if (bracket != std::string::npos)
            {
                name = name.substr(0, bracket);
            }
            offsets[name] = program.getUniform(i).offset;
        }
        for (const LLShaderTranslator::BlockMember& m : result.layout.members)
        {
            // glslang's reflection only lists the members some stage actually uses
            auto it = offsets.find(m.name);
            if (it != offsets.end() && it->second != m.offset)
            {
                if (++result.layoutMismatches == 1)
                {
                    result.error = "layout mismatch for " + m.name + ": ours " + std::to_string(m.offset) + ", glslang " +
                                   std::to_string(it->second);
                }
            }
        }
        // glslang reports the end of the last member; ours is rounded up to 16 (std140 block size)
        // (-1 = no stage uses any member, glslang drops the block)
        const int raw_size = program.getUniformBlockSize(0);
        const int glslang_size = (raw_size + 15) / 16 * 16;
        if (raw_size >= 0 && glslang_size != result.layout.size && result.error.empty())
        {
            result.error = "layout block size mismatch: ours " + std::to_string(result.layout.size) + ", glslang " +
                           std::to_string(program.getUniformBlockSize(0));
            ++result.layoutMismatches;
        }
    }
    return result;
}
