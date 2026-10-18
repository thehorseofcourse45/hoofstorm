// Compiles the FSR EASU and RCAS shaders with the viewer's translator and glslang, without a GPU.
// usage: llvulkan_fsr_shader_test <directory holding fsrEasuF.glsl and fsrRcasF.glsl>
#include "llshadercompiler.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static bool readFile(const std::string& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: llvulkan_fsr_shader_test <shader dir>\n"); return 2; }
    const std::string dir = argv[1];
    int checks = 0, failed = 0;
    const char* vertex = "#version 330\nin vec3 position;\nout vec2 vary_fragcoord;\n"
                         "void main(){gl_Position=vec4(position,1.0);vary_fragcoord=position.xy*0.5+0.5;}\n";
    const char* library = "#version 330\nvec3 clampHDRRange(vec3 c){return clamp(c,vec3(0.0),vec3(1.0));}\n";
    for (const char* name : {"fsrEasuF.glsl", "fsrRcasF.glsl"})
    {
        ++checks;
        std::string body;
        if (!readFile(dir + "/" + name, body)) { ++failed; printf("FAIL %s: cannot read\n", name); continue; }
        LLShaderCompiler::Stage vs, fs;
        vs.stage = LLShaderTranslator::Stage::Vertex;
        vs.objects = {vertex};
        fs.stage = LLShaderTranslator::Stage::Fragment;
        fs.objects = {"#version 330\n#define HAS_NOISE 1\n" + body, library};
        const auto result = LLShaderCompiler::compileProgram({vs, fs}, LLShaderCompiler::viewerOptions(), false);
        std::string error;
        if (!result.ok()) { ++failed; printf("FAIL %s: %s\n", name, result.error.c_str()); continue; }
        if (!LLShaderCompiler::validateSpirv(result.spirv.back(), error)) { ++failed; printf("FAIL %s: spirv %s\n", name, error.c_str()); continue; }
        printf("PASS %s\n", name);
    }
    printf("FSR_SHADER_CHECKS=%d %s\n", checks, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
