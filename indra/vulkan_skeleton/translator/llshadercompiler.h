// Viewer GLSL (as the viewer assembles it for OpenGL) to Vulkan SPIR-V: preprocess each object with glslang, translate with
// LLShaderTranslator, compile and link the stages with glslang. Prototype of the Vulkan shader backend's compile step
// (doc/vulkan/shader-layer-design.md); used by translator_test and the Vulkan skeleton. Needs glslang.
#pragma once

#include "llshadertranslator.h"

#include <cstdint>
#include <string>
#include <vector>

class LLShaderCompiler
{
public:
    struct Stage
    {
        LLShaderTranslator::Stage stage = LLShaderTranslator::Stage::Vertex;
        std::vector<std::string> objects;       // complete GL source of each shader object, attach order
    };

    struct Result
    {
        std::string error;                                      // empty = success
        std::vector<LLShaderTranslator::Stage> stages;          // same order as the input
        std::vector<std::vector<uint32_t>> spirv;               // one module per stage
        std::vector<std::string> stageSources;                  // the translated GLSL, for debugging
        LLShaderTranslator::Result translation;
        LLShaderTranslator::BlockLayout layout;                 // std140 layout of the program uniform block
        int layoutMismatches = 0;                               // members whose offset differs from glslang's reflection
        bool ok() const { return error.empty(); }
    };

    // 'checkLayout': compare LLShaderTranslator::computeBlockLayout() with glslang's own reflection of the block.
    static Result compileProgram(const std::vector<Stage>& stages, const LLShaderTranslator::Options& options, bool checkLayout);

    // The reserved attribute names and uniform block bindings of the viewer (LLShaderMgr, LLGLSLShader::UniformBlock).
    static LLShaderTranslator::Options viewerOptions();
};
