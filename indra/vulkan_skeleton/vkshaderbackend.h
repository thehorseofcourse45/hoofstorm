// Vulkan implementation of LLShaderBackend (llshaderbackend.h) for the skeleton. See doc/vulkan/shader-layer-design.md.
//
// It is driven exactly like the OpenGL one: objects are created and "compiled" (the source is stored, Vulkan compiles a whole
// stage, not an object), attached to a program, the reserved attribute locations are bound, and link() runs the real work:
// LLShaderCompiler (glslang preprocess, LLShaderTranslator, glslang compile and link) and the creation of the VkProgram.
// Reflection (active uniforms, locations, attribute locations) is answered from the translator's description of the program.
// The program binary calls store and load a "SPIR-V blob" (stages + layout + reflection), which is the SPIR-V cache of the design.
#pragma once

#include "llshaderbackend.h"
#include "vkprogram.h"
#include "llshadercompiler.h"

#include <map>
#include <string>
#include <vector>

class VulkanShaderBackend final : public LLShaderBackend
{
public:
    explicit VulkanShaderBackend(VkContext* ctx) : mCtx(ctx) {}
    ~VulkanShaderBackend() override;

    // timings of the last link() and the last loadProgramBinary()
    double lastLinkMs = 0.0;
    double lastLoadMs = 0.0;

    // objects
    U32 createObject(U32 stage) override;
    bool compileObject(U32 object, U32 count, const char* const* text, std::string& log) override;
    void deleteObject(U32 object) override;

    // programs
    U32 createProgram() override;
    void attachObject(U32 program, U32 object) override;
    void deleteProgramAndObjects(U32 program) override;
    void bindAttribLocation(U32 program, U32 index, const char* name) override;
    bool link(U32 program) override;
    bool validate(U32 program) override { return mPrograms.count(program) && mPrograms[program].vk; }
    std::string getObjectLog(U32 object) override;
    std::string getProgramLog(U32 program) override;
    void use(U32 program) override;

    // reflection
    S32 getAttribLocation(U32 program, const char* name) override;
    S32 getUniformLocation(U32 program, const char* name) override;
    S32 getActiveUniformCount(U32 program) override;
    void getActiveUniform(U32 program, S32 index, std::string& name, U32& type, S32& size) override;
    void bindUniformBlock(U32, const char*, U32) override { /* the translator already gave existing blocks their binding */ }

    // program binary cache
    bool supportsProgramBinary() const override { return true; }
    void setProgramBinaryRetrievable(U32) override {}
    bool loadProgramBinary(U32 program, U32 format, const void* data, S32 length) override;
    S32 getProgramBinaryLength(U32 program) override;
    bool getProgramBinary(U32 program, std::vector<U8>& binary, U32& format) override;

    // uniforms
    void uniform1i(U32 p, S32 loc, S32 v) override;
    void uniform2i(U32 p, S32 loc, S32 x, S32 y) override;
    void uniform1f(U32 p, S32 loc, F32 v) override;
    void uniform2f(U32 p, S32 loc, F32 x, F32 y) override;
    void uniform3f(U32 p, S32 loc, F32 x, F32 y, F32 z) override;
    void uniform4f(U32 p, S32 loc, F32 x, F32 y, F32 z, F32 w) override;
    void uniform1iv(U32 p, S32 loc, S32 count, const S32* v) override;
    void uniform4iv(U32 p, S32 loc, S32 count, const S32* v) override;
    void uniform1fv(U32 p, S32 loc, S32 count, const F32* v) override;
    void uniform2fv(U32 p, S32 loc, S32 count, const F32* v) override;
    void uniform3fv(U32 p, S32 loc, S32 count, const F32* v) override;
    void uniform4fv(U32 p, S32 loc, S32 count, const F32* v) override;
    void uniform4uiv(U32 p, S32 loc, S32 count, const U32* v) override;
    void uniformMatrix2fv(U32 p, S32 loc, S32 count, bool transpose, const F32* v) override;
    void uniformMatrix3fv(U32 p, S32 loc, S32 count, bool transpose, const F32* v) override;
    void uniformMatrix3x4fv(U32 p, S32 loc, S32 count, bool transpose, const F32* v) override;
    void uniformMatrix4fv(U32 p, S32 loc, S32 count, bool transpose, const F32* v) override;

    // the Vulkan program behind a handle (for the skeleton's own checks)
    VkProgram* vkProgram(U32 program) { return mPrograms.count(program) ? mPrograms[program].vk : nullptr; }

private:
    struct Object
    {
        U32 stage = 0;
        std::string source;
    };

    struct Program
    {
        std::vector<U32> objects;               // attach order
        std::vector<std::string> attribNames;   // index = attribute location, from bindAttribLocation()
        std::string log;
        VkProgram* vk = nullptr;

        // what link() learned (or loadProgramBinary() read back)
        std::vector<LLShaderTranslator::UniformMember> uniforms;    // same order as layout.members
        std::vector<LLShaderTranslator::SamplerBinding> samplers;
        std::vector<std::string> attributes;                        // attribute names the vertex stage reads
        LLShaderTranslator::BlockLayout layout;
        std::vector<std::vector<uint32_t>> spirv;                   // vertex, fragment
        std::vector<U8> blob;                                       // serialized form, built on demand
    };

    bool finish(Program& p);                    // create the VkProgram from the fields above
    void serialize(Program& p);
    void writeVectors(U32 program, S32 loc, int srcComps, int count, const void* src);
    void writeMatrices(U32 program, S32 loc, int cols, int rows, int count, bool transpose, const F32* src);

    VkContext* mCtx;
    std::map<U32, Object> mObjects;
    std::map<U32, Program> mPrograms;
    U32 mNext = 1;
};
