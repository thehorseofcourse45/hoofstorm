// Reader for the shader dump the viewer writes with FS_SHADER_DUMP_DIR (see ../shader_probe/README.md): manifest.txt plus one
// file per shader object. Used by translator_test and the Vulkan skeleton.
#pragma once

#include "llshadercompiler.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace shaderdump
{

struct ManifestProgram
{
    std::string name;
    std::vector<std::string> objects;   // attached objects, attach order, no duplicates
};

inline std::vector<ManifestProgram> readManifest(const std::string& dir)
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

inline std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline bool stageOf(const std::string& file, LLShaderTranslator::Stage& stage)
{
    const std::string ext = file.substr(file.rfind('.') + 1);
    if (ext == "vert") { stage = LLShaderTranslator::Stage::Vertex; return true; }
    if (ext == "frag") { stage = LLShaderTranslator::Stage::Fragment; return true; }
    if (ext == "geom") { stage = LLShaderTranslator::Stage::Geometry; return true; }
    return false;
}

// The stages of one manifest program with the source of their objects, in the order vertex, fragment, geometry.
inline std::vector<LLShaderCompiler::Stage> loadStages(const std::string& dir, const ManifestProgram& program)
{
    std::map<int, LLShaderCompiler::Stage> by_stage;
    for (const std::string& obj : program.objects)
    {
        LLShaderTranslator::Stage stage;
        if (!stageOf(obj, stage))
        {
            continue;
        }
        LLShaderCompiler::Stage& s = by_stage[(int) stage];
        s.stage = stage;
        s.objects.push_back(readFile(dir + "/" + obj));
    }
    std::vector<LLShaderCompiler::Stage> stages;
    for (auto& kv : by_stage)
    {
        stages.push_back(std::move(kv.second));
    }
    return stages;
}

} // namespace shaderdump
