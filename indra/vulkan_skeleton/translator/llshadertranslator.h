/**
 * @file llshadertranslator.h
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

#ifndef LL_LLSHADERTRANSLATOR_H
#define LL_LLSHADERTRANSLATOR_H

// See doc/vulkan/shader-layer-design.md. Depends on the C++ standard library only, so it can be tested without a GPU,
// without GL and without glslang (the caller preprocesses and compiles).
//
// GL links several shader objects per program stage. A Vulkan stage is one SPIR-V module, so the objects of a stage are merged
// into one source. The translation then does what Vulkan GLSL requires:
//   - #version 450, legacy no-op #extension lines dropped, remaining #extension lines moved to the top
//   - loose (non-opaque) uniforms of ALL stages of the program become the members of ONE std140 uniform block,
//     declared identically in every stage
//   - samplers get layout(set = samplerSet, binding = N), N in order of first appearance in the program
//   - existing uniform blocks get layout(set = blockSet, binding = <table>)
//   - vertex shader inputs named in attribNames get layout(location = index of the name)
//   - stage interface variables (vertex outputs, geometry inputs/outputs, fragment inputs) get layout(location = N), the same N for
//     the same name in every stage of the program. Fragment outputs are left to the compiler.
//
// Every object passed in must already be preprocessed (no #if/#define/#include left; glslang's preprocessor output is fine).

#include <map>
#include <string>
#include <vector>

class LLShaderTranslator
{
public:
    enum class Stage
    {
        Vertex,
        Fragment,
        Geometry
    };

    struct Options
    {
        // vertex attribute i is bound to layout(location = i); matches LLShaderMgr::mReservedAttribs
        std::vector<std::string> attribNames;
        // uniform block name -> binding in descriptor set blockSet (LLGLSLShader::UniformBlock)
        std::map<std::string, int> blockBindings;
        int uniformSet = 0;     // set holding the program's uniform block, at binding uniformBinding
        int uniformBinding = 0;
        int samplerSet = 1;
        int blockSet = 2;
    };

    struct StageInput
    {
        Stage stage = Stage::Vertex;
        std::vector<std::string> objects;   // attach order
    };

    struct UniformMember
    {
        std::string type;       // "mat4", "vec4" ...
        std::string name;
        std::string arraySize;  // text between [ ] ("" if not an array)
    };

    struct SamplerBinding
    {
        std::string type;       // "sampler2D" ...
        std::string name;
        std::string arraySize;
        int binding = 0;
    };

    struct Result
    {
        std::vector<std::string> stageSources;      // same order as the input stages
        std::vector<UniformMember> uniforms;        // members of the program uniform block, in declaration order
        std::vector<SamplerBinding> samplers;
        std::vector<std::string> errors;            // non-empty = the program cannot be translated
        int legacyExtensionsRemoved = 0;
        int uniformInitializersRemoved = 0;
        int attributesLocated = 0;
        int varyingsLocated = 0;
        bool ok() const { return errors.empty(); }
    };

    static Result translateProgram(const std::vector<StageInput>& stages, const Options& options);

    // Where each member of the program uniform block lives, under std140 rules (the layout of the block that
    // translateProgram() declares). A CPU-side copy of the block is written at these offsets.
    struct BlockMember
    {
        std::string name;
        int offset = 0;         // bytes from the start of the block
        int size = 0;           // bytes the member occupies (for an array: all elements, including padding between them)
        int arrayStride = 0;    // 0 if not an array
    };
    struct BlockLayout
    {
        std::vector<BlockMember> members;
        int size = 0;           // total bytes, rounded up to 16
        std::string error;      // non-empty = a member has a type or array size the calculator does not know
        const BlockMember* find(const std::string& name) const
        {
            for (const BlockMember& m : members)
            {
                if (m.name == name)
                {
                    return &m;
                }
            }
            return nullptr;
        }
    };
    static BlockLayout computeBlockLayout(const std::vector<UniformMember>& uniforms);

    // The name of the uniform block that holds the program's loose uniforms.
    static const char* programBlockName() { return "LLProgramUniforms"; }
};

#endif // LL_LLSHADERTRANSLATOR_H
