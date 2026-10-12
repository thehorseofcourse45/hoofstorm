/**
 * @file llshadertranslator.cpp
 * @brief Turns the viewer's OpenGL GLSL into Vulkan GLSL, a pure text transform
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llshadertranslator.h"

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <set>

namespace
{

// Remove // and /* */ comments, keeping the newlines so that line structure survives.
std::string stripComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size();)
    {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/')
        {
            while (i < src.size() && src[i] != '\n')
            {
                ++i;
            }
        }
        else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/'))
            {
                if (src[i] == '\n')
                {
                    out += '\n';
                }
                ++i;
            }
            i = std::min(i + 2, src.size());
        }
        else
        {
            out += src[i++];
        }
    }
    return out;
}

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
    {
        return "";
    }
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// whitespace runs become one space; used to compare declarations
std::string collapse(const std::string& s)
{
    std::string out;
    bool space = false;
    for (char c : s)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            space = !out.empty();
        }
        else
        {
            if (space)
            {
                out += ' ';
                space = false;
            }
            out += c;
        }
    }
    return out;
}

struct Chunk
{
    bool preprocessor = false;
    std::string text;
};

// Split into top-level chunks: preprocessor lines, and declarations (a statement ending in ';' or a body closed back to depth 0).
std::vector<Chunk> chunksOf(const std::string& src)
{
    std::vector<Chunk> out;
    std::string cur;
    int depth = 0;
    size_t pos = 0;
    while (pos <= src.size())
    {
        size_t eol = src.find('\n', pos);
        if (eol == std::string::npos)
        {
            eol = src.size();
        }
        const std::string line = src.substr(pos, eol - pos);
        pos = eol + 1;

        const std::string stripped = trim(line);
        if (cur.empty())
        {
            if (stripped.empty())
            {
                continue;
            }
            if (stripped[0] == '#')
            {
                out.push_back({ true, stripped });
                continue;
            }
        }
        cur += line;
        cur += '\n';
        depth += (int)std::count(line.begin(), line.end(), '{') - (int)std::count(line.begin(), line.end(), '}');
        if (depth == 0 && !stripped.empty() && (stripped.back() == ';' || stripped.back() == '}'))
        {
            out.push_back({ false, trim(cur) });
            cur.clear();
        }
    }
    if (!trim(cur).empty())
    {
        out.push_back({ false, trim(cur) });
    }
    return out;
}

// structs are identified by name (every object carries its own copy of GBufferInfo), everything else by its text
std::string dedupeKey(const std::string& decl)
{
    static const std::regex sStruct(R"(^struct\s+(\w+))");
    const std::string c = collapse(decl);
    std::smatch m;
    if (std::regex_search(c, m, sStruct))
    {
        return "struct " + m[1].str();
    }
    return c;
}

bool isLegacyExtension(const std::string& line)
{
    static const std::regex sLegacy(R"(^#\s*extension\s+(GL_ARB_shader_texture_lod|GL_EXT_gpu_shader4|GL_ARB_texture_rectangle)\b)");
    return std::regex_search(line, sLegacy);
}

bool isOpaqueType(const std::string& type)
{
    static const std::regex sOpaque(R"(^(?:[iu]?sampler|[iu]?image|[iu]?texture|atomic_uint|subpassInput))");
    return std::regex_search(type, sOpaque);
}

// split "a, b[2] = x, c" at top-level commas
std::vector<std::string> splitDeclarators(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s)
    {
        if (c == '(' || c == '[' || c == '{')
        {
            ++depth;
        }
        else if (c == ')' || c == ']' || c == '}')
        {
            --depth;
        }
        if (c == ',' && depth == 0)
        {
            out.push_back(trim(cur));
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    out.push_back(trim(cur));
    return out;
}

// number of interface locations one array element of a varying of this type takes (0 = unknown type)
int locationSlots(const std::string& type)
{
    static const std::regex sMat(R"(^mat([234])(?:x[234])?$)");
    static const std::regex sSimple(R"(^(?:float|int|uint|bool|[iub]?vec[234])$)");
    std::smatch m;
    if (std::regex_match(type, m, sMat))
    {
        return m[1].str()[0] - '0';
    }
    return std::regex_match(type, sSimple) ? 1 : 0;
}

struct StageBody
{
    std::vector<std::string> extensions;
    std::vector<std::string> lines;     // top-level chunks in order
};

} // namespace

LLShaderTranslator::Result LLShaderTranslator::translateProgram(const std::vector<StageInput>& stages, const Options& options)
{
    static const std::regex sBlock(R"(^(?:layout\s*\(([^)]*)\)\s*)?uniform\s+(\w+)\s*\{)");
    static const std::regex sUniform(R"(^(?:layout\s*\(([^)]*)\)\s*)?uniform\s+(?:(?:highp|mediump|lowp)\s+)?(\w+(?:\s*\[[^\]]*\])?)\s+([^;]*);$)");
    static const std::regex sTypeArray(R"(^(\w+)\s*(?:\[([^\]]*)\])?$)");
    static const std::regex sDeclarator(R"(^(\w+)\s*(?:\[(.*)\])?$)");
    static const std::regex sVertexIn(R"(^in\s+(.+?)\s+(\w+)\s*;$)");
    static const std::regex sVarying(R"(^((?:(?:flat|smooth|noperspective|centroid)\s+)*)(in|out)\s+(\w+(?:\s*\[[^\]]*\])?)\s+(\w+)\s*(?:\[([^\]]*)\])?\s*;$)");

    Result result;
    std::vector<StageBody> bodies(stages.size());
    std::map<std::string, size_t> uniformIndex;
    std::map<std::string, size_t> samplerIndex;
    std::map<std::string, std::pair<int, int>> varyingLocation;     // name -> (first location, slots)
    int nextVaryingLocation = 0;

    for (size_t s = 0; s < stages.size(); ++s)
    {
        StageBody& body = bodies[s];
        std::set<std::string> seen;
        std::set<std::string> seenExt;

        for (const std::string& object : stages[s].objects)
        {
            for (const Chunk& chunk : chunksOf(stripComments(object)))
            {
                if (chunk.preprocessor)
                {
                    const std::string pp = collapse(chunk.text);
                    if (pp.compare(0, 8, "#version") == 0)
                    {
                        continue;
                    }
                    if (pp.compare(0, 10, "#extension") == 0 || pp.compare(0, 11, "# extension") == 0)
                    {
                        if (isLegacyExtension(pp))
                        {
                            ++result.legacyExtensionsRemoved;
                        }
                        else if (seenExt.insert(pp).second)
                        {
                            body.extensions.push_back(pp);
                        }
                        continue;
                    }
                    if (seen.insert(pp).second)
                    {
                        body.lines.push_back(pp);
                    }
                    continue;
                }

                if (!seen.insert(dedupeKey(chunk.text)).second)
                {
                    continue;
                }
                const std::string decl = collapse(chunk.text);
                std::smatch m;

                // uniform block that already exists in the source: give it a binding
                if (std::regex_search(decl, m, sBlock))
                {
                    const std::string qualifiers = m[1].str();
                    const std::string name = m[2].str();
                    if (qualifiers.find("binding") != std::string::npos)
                    {
                        body.lines.push_back(chunk.text);
                        continue;
                    }
                    auto it = options.blockBindings.find(name);
                    if (it == options.blockBindings.end())
                    {
                        result.errors.push_back("uniform block '" + name + "' has no binding in Options::blockBindings");
                        body.lines.push_back(chunk.text);
                        continue;
                    }
                    std::string layout = "layout(" + (qualifiers.empty() ? std::string() : trim(qualifiers) + ", ") +
                                         "set = " + std::to_string(options.blockSet) + ", binding = " + std::to_string(it->second) + ") ";
                    // keep the block body as written (it may span lines); replace only the header
                    const size_t header_end = chunk.text.find("uniform");
                    body.lines.push_back(layout + chunk.text.substr(header_end));
                    continue;
                }

                // loose uniform
                if (std::regex_search(decl, m, sUniform))
                {
                    const std::string layoutq = m[1].str();
                    // "uniform vec4[2] name" puts the array size on the type
                    std::string type = m[2].str();
                    std::string type_array;
                    {
                        std::smatch tm;
                        const std::string full_type = type;
                        if (std::regex_match(full_type, tm, sTypeArray))
                        {
                            type = tm[1].str();
                            type_array = tm[2].str();
                        }
                    }
                    const bool opaque = isOpaqueType(type);
                    if (!layoutq.empty())
                    {
                        result.errors.push_back("uniform with a layout qualifier is not supported: " + decl);
                        body.lines.push_back(chunk.text);
                        continue;
                    }
                    for (std::string declarator : splitDeclarators(m[3].str()))
                    {
                        const size_t eq = declarator.find('=');
                        if (eq != std::string::npos)
                        {
                            declarator = trim(declarator.substr(0, eq));     // initializers are not allowed in blocks
                            ++result.uniformInitializersRemoved;
                        }
                        std::smatch dm;
                        if (!std::regex_match(declarator, dm, sDeclarator))
                        {
                            result.errors.push_back("cannot parse uniform declarator '" + declarator + "' in: " + decl);
                            continue;
                        }
                        const std::string name = dm[1].str();
                        std::string array = dm[2].str();
                        if (!type_array.empty())
                        {
                            if (!array.empty())
                            {
                                result.errors.push_back("multi-dimensional uniform arrays are not supported: " + decl);
                            }
                            array = type_array;
                        }

                        if (opaque)
                        {
                            auto it = samplerIndex.find(name);
                            if (it == samplerIndex.end())
                            {
                                SamplerBinding sb;
                                sb.type = type;
                                sb.name = name;
                                sb.arraySize = array;
                                sb.binding = (int)result.samplers.size();
                                it = samplerIndex.emplace(name, result.samplers.size()).first;
                                result.samplers.push_back(sb);
                            }
                            else if (result.samplers[it->second].type != type || result.samplers[it->second].arraySize != array)
                            {
                                result.errors.push_back("sampler '" + name + "' is declared with different types in one program");
                            }
                            const SamplerBinding& sb = result.samplers[it->second];
                            body.lines.push_back("layout(set = " + std::to_string(options.samplerSet) + ", binding = " +
                                                 std::to_string(sb.binding) + ") uniform " + type + " " + name +
                                                 (array.empty() ? "" : "[" + array + "]") + ";");
                        }
                        else
                        {
                            auto it = uniformIndex.find(name);
                            if (it == uniformIndex.end())
                            {
                                uniformIndex.emplace(name, result.uniforms.size());
                                result.uniforms.push_back({ type, name, array });
                            }
                            else if (result.uniforms[it->second].type != type || result.uniforms[it->second].arraySize != array)
                            {
                                result.errors.push_back("uniform '" + name + "' is declared with different types in one program");
                            }
                        }
                    }
                    continue;
                }

                // vertex attribute: fixed location by name
                if (stages[s].stage == Stage::Vertex && std::regex_match(decl, m, sVertexIn))
                {
                    const std::string name = m[2].str();
                    auto it = std::find(options.attribNames.begin(), options.attribNames.end(), name);
                    if (it != options.attribNames.end())
                    {
                        body.lines.push_back("layout(location = " + std::to_string(it - options.attribNames.begin()) + ") in " +
                                             m[1].str() + " " + name + ";");
                        ++result.attributesLocated;
                        continue;
                    }
                }

                // stage interface variable (vertex out, geometry in/out, fragment in): the same name gets the same location in
                // every stage. The compiler's own auto-mapping assigns them per stage in declaration order, which only agrees
                // when every stage declares its varyings in the same order.
                const bool isVarying = std::regex_match(decl, m, sVarying) &&
                                       !(stages[s].stage == Stage::Vertex && m[2].str() == "in") &&
                                       !(stages[s].stage == Stage::Fragment && m[2].str() == "out");
                if (isVarying)
                {
                    const std::string qualifiers = m[1].str();
                    const std::string direction = m[2].str();
                    // "out vec4[2] name" puts the array on the type, "in vec4 name[]" on the name
                    std::string type = m[3].str();
                    const std::string name = m[4].str();
                    std::string array = m[5].str();
                    bool has_array = m[5].matched;
                    {
                        std::smatch tm;
                        const std::string full_type = type;
                        if (std::regex_match(full_type, tm, sTypeArray))
                        {
                            type = tm[1].str();
                            if (tm[2].matched || full_type.find('[') != std::string::npos)
                            {
                                if (has_array)
                                {
                                    result.errors.push_back("multi-dimensional varying arrays are not supported: " + decl);
                                }
                                array = tm[2].str();
                                has_array = true;
                            }
                        }
                    }
                    int slots = locationSlots(type);
                    if (slots == 0)
                    {
                        result.errors.push_back("cannot size varying '" + name + "' of type " + type);
                        body.lines.push_back(chunk.text);
                        continue;
                    }
                    // an array dimension multiplies the slots, except the per-vertex dimension of geometry shader inputs
                    const bool per_vertex = stages[s].stage == Stage::Geometry && direction == "in";
                    if (has_array && !per_vertex)
                    {
                        const int count = atoi(array.c_str());
                        if (count <= 0)
                        {
                            result.errors.push_back("varying array '" + name + "' has no plain size");
                        }
                        slots *= count > 0 ? count : 1;
                    }
                    auto it = varyingLocation.find(name);
                    if (it == varyingLocation.end())
                    {
                        it = varyingLocation.emplace(name, std::make_pair(nextVaryingLocation, slots)).first;
                        nextVaryingLocation += slots;
                    }
                    else if (it->second.second != slots)
                    {
                        result.errors.push_back("varying '" + name + "' has a different size in two stages");
                    }
                    body.lines.push_back("layout(location = " + std::to_string(it->second.first) + ") " + qualifiers + direction + " " +
                                         type + " " + name + (has_array ? "[" + array + "]" : "") + ";");
                    ++result.varyingsLocated;
                    continue;
                }

                body.lines.push_back(chunk.text);
            }
        }
    }

    std::string block;
    if (!result.uniforms.empty())
    {
        block = "layout(std140, set = " + std::to_string(options.uniformSet) + ", binding = " + std::to_string(options.uniformBinding) +
                ") uniform " + programBlockName() + "\n{\n";
        for (const UniformMember& u : result.uniforms)
        {
            block += "    " + u.type + " " + u.name + (u.arraySize.empty() ? "" : "[" + u.arraySize + "]") + ";\n";
        }
        block += "};";
    }

    for (const StageBody& body : bodies)
    {
        std::string src = "#version 450\n";
        for (const std::string& e : body.extensions)
        {
            src += e + "\n";
        }
        if (!block.empty())
        {
            src += block + "\n";
        }
        for (const std::string& l : body.lines)
        {
            src += l + "\n";
        }
        result.stageSources.push_back(src);
    }
    return result;
}

// ----------------------------------------------------------------------------------------------------------------
// std140 layout

namespace
{
// base alignment and size of one element of a basic type; matrices are handled by the caller as arrays of columns
struct TypeInfo
{
    int align = 0;
    int size = 0;
    bool known = false;
};

int roundUp(int v, int a)
{
    return (v + a - 1) / a * a;
}

TypeInfo std140Type(const std::string& t)
{
    // scalars
    if (t == "float" || t == "int" || t == "uint" || t == "bool") return { 4, 4, true };
    // vectors: vecN, ivecN, uvecN, bvecN
    static const std::regex sVec(R"(^[iub]?vec([234])$)");
    std::smatch m;
    if (std::regex_match(t, m, sVec))
    {
        const int n = m[1].str()[0] - '0';
        return { n == 2 ? 8 : 16, n * 4, true };
    }
    // matrices: matN or matCxR (C columns, R rows); each column is stored like a vector of R floats rounded up to vec4
    static const std::regex sMat(R"(^mat([234])(?:x([234]))?$)");
    if (std::regex_match(t, m, sMat))
    {
        const int cols = m[1].str()[0] - '0';
        return { 16, cols * 16, true };     // std140 rounds every matrix column up to a vec4
    }
    return {};
}
} // namespace

LLShaderTranslator::BlockLayout LLShaderTranslator::computeBlockLayout(const std::vector<UniformMember>& uniforms)
{
    BlockLayout layout;
    int offset = 0;
    for (const UniformMember& u : uniforms)
    {
        const TypeInfo t = std140Type(u.type);
        if (!t.known)
        {
            layout.error = "unknown uniform type '" + u.type + "' for " + u.name;
            return layout;
        }
        BlockMember m;
        m.name = u.name;
        if (u.arraySize.empty())
        {
            offset = roundUp(offset, t.align);
            m.offset = offset;
            m.size = t.size;
            offset += t.size;
        }
        else
        {
            const int count = atoi(u.arraySize.c_str());
            if (count <= 0 || u.arraySize.find_first_not_of("0123456789") != std::string::npos)
            {
                layout.error = "array size '" + u.arraySize + "' of " + u.name + " is not a plain number";
                return layout;
            }
            m.arrayStride = roundUp(t.size, 16);        // std140: array elements are aligned to vec4
            offset = roundUp(offset, 16);
            m.offset = offset;
            m.size = m.arrayStride * count;
            offset += m.size;
        }
        layout.members.push_back(m);
    }
    layout.size = roundUp(offset, 16);
    return layout;
}
