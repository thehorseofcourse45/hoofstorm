#include "vkshaderbackend.h"
#include "llstd140pack.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{
// GL constants (the viewer's vocabulary); the Windows gl.h stops at GL 1.1
constexpr U32 kVertexShader = 0x8B31, kFragmentShader = 0x8B30, kGeometryShader = 0x8DD9;

struct GLTypeName
{
    const char* glsl;
    U32 gl;
};
const GLTypeName kTypes[] = {
    { "float", 0x1406 },  { "vec2", 0x8B50 },   { "vec3", 0x8B51 },   { "vec4", 0x8B52 },
    { "int", 0x1404 },    { "ivec2", 0x8B53 },  { "ivec3", 0x8B54 },  { "ivec4", 0x8B55 },
    { "uint", 0x1405 },   { "uvec2", 0x8DC6 },  { "uvec3", 0x8DC7 },  { "uvec4", 0x8DC8 },
    { "bool", 0x8B56 },   { "bvec2", 0x8B57 },  { "bvec3", 0x8B58 },  { "bvec4", 0x8B59 },
    { "mat2", 0x8B5A },   { "mat3", 0x8B5B },   { "mat4", 0x8B5C },
    { "mat2x3", 0x8B65 }, { "mat2x4", 0x8B66 }, { "mat3x2", 0x8B67 }, { "mat3x4", 0x8B68 },
    { "mat4x2", 0x8B69 }, { "mat4x3", 0x8B6A },
    { "sampler2D", 0x8B5E },       { "sampler3D", 0x8B5F },        { "samplerCube", 0x8B60 },
    { "sampler2DShadow", 0x8B62 }, { "sampler2DRect", 0x8B63 },    { "sampler2DRectShadow", 0x8B64 },
    { "sampler2DArray", 0x8DC1 },  { "sampler2DMS", 0x9108 },      { "samplerCubeArray", 0x900C },
};

U32 glTypeOf(const std::string& glsl)
{
    for (const GLTypeName& t : kTypes)
    {
        if (glsl == t.glsl)
        {
            return t.gl;
        }
    }
    return 0;
}

// a tiny byte stream for the SPIR-V blob
struct Writer
{
    std::vector<U8>& out;
    void u32(U32 v) { out.insert(out.end(), (const U8*) &v, (const U8*) &v + 4); }
    void str(const std::string& s) { u32((U32) s.size()); out.insert(out.end(), s.begin(), s.end()); }
    void words(const std::vector<uint32_t>& w) { u32((U32) w.size()); out.insert(out.end(), (const U8*) w.data(), (const U8*) (w.data() + w.size())); }
};

struct Reader
{
    const U8* p;
    const U8* end;
    bool bad = false;
    U32 u32()
    {
        if (end - p < 4) { bad = true; return 0; }
        U32 v;
        memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    std::string str()
    {
        const U32 n = u32();
        if (bad || (size_t) (end - p) < n) { bad = true; return ""; }
        std::string s((const char*) p, n);
        p += n;
        return s;
    }
    std::vector<uint32_t> words()
    {
        const U32 n = u32();
        if (bad || (size_t) (end - p) < (size_t) n * 4) { bad = true; return {}; }
        std::vector<uint32_t> w(n);
        memcpy(w.data(), p, (size_t) n * 4);
        p += (size_t) n * 4;
        return w;
    }
};

constexpr U32 kBlobMagic = 0x42534C4C;      // "LLSB"
constexpr U32 kBlobVersion = 1;             // bump when the translator or this format changes

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace

VulkanShaderBackend::~VulkanShaderBackend()
{
    for (auto& kv : mPrograms)
    {
        if (kv.second.vk)
        {
            if (mCtx->program == kv.second.vk)
            {
                mCtx->program = nullptr;
            }
            kv.second.vk->destroy();
        }
    }
}

// ----------------------------------------------------------------------------------------------------------------
// objects

U32 VulkanShaderBackend::createObject(U32 stage)
{
    if (stage != kVertexShader && stage != kFragmentShader && stage != kGeometryShader)
    {
        return 0;
    }
    const U32 id = mNext++;
    mObjects[id].stage = stage;
    return id;
}

bool VulkanShaderBackend::compileObject(U32 object, U32 count, const char* const* text, std::string& log)
{
    auto it = mObjects.find(object);
    if (it == mObjects.end())
    {
        log = "unknown shader object";
        return false;
    }
    // Vulkan compiles whole stages, so this is where the object's source is only kept; link() compiles.
    it->second.source.clear();
    for (U32 i = 0; i < count; ++i)
    {
        it->second.source += text[i];
    }
    return true;
}

void VulkanShaderBackend::deleteObject(U32 object)
{
    mObjects.erase(object);
}

std::string VulkanShaderBackend::getObjectLog(U32 object)
{
    (void) object;
    return "";
}

// ----------------------------------------------------------------------------------------------------------------
// programs

U32 VulkanShaderBackend::createProgram()
{
    const U32 id = mNext++;
    mPrograms[id];
    return id;
}

void VulkanShaderBackend::attachObject(U32 program, U32 object)
{
    if (mPrograms.count(program) && mObjects.count(object))
    {
        mPrograms[program].objects.push_back(object);
    }
}

void VulkanShaderBackend::deleteProgramAndObjects(U32 program)
{
    auto it = mPrograms.find(program);
    if (it == mPrograms.end())
    {
        return;
    }
    for (U32 o : it->second.objects)
    {
        mObjects.erase(o);
    }
    if (it->second.vk)
    {
        if (mCtx->program == it->second.vk)
        {
            mCtx->program = nullptr;
        }
        it->second.vk->destroy();
    }
    mPrograms.erase(it);
}

void VulkanShaderBackend::bindAttribLocation(U32 program, U32 index, const char* name)
{
    auto& names = mPrograms[program].attribNames;
    if (names.size() <= index)
    {
        names.resize(index + 1);
    }
    names[index] = name;
}

bool VulkanShaderBackend::finish(Program& p)
{
    if (p.spirv.size() != 2)
    {
        p.log = "geometry stages are not supported by the skeleton yet";
        return false;
    }
    if (p.vk)
    {
        p.vk->destroy();
    }
    p.vk = VkProgram::create(*mCtx, p.spirv[0], p.spirv[1], p.layout, p.samplers);
    return true;
}

bool VulkanShaderBackend::link(U32 program)
{
    auto it = mPrograms.find(program);
    if (it == mPrograms.end())
    {
        return false;
    }
    Program& p = it->second;
    const auto t0 = std::chrono::steady_clock::now();

    // group the attached objects into stages (vertex, fragment, geometry), keeping attach order
    std::map<int, LLShaderCompiler::Stage> by_stage;
    for (U32 o : p.objects)
    {
        const Object& obj = mObjects[o];
        const LLShaderTranslator::Stage stage = obj.stage == kVertexShader ? LLShaderTranslator::Stage::Vertex
                                              : obj.stage == kFragmentShader ? LLShaderTranslator::Stage::Fragment
                                                                             : LLShaderTranslator::Stage::Geometry;
        LLShaderCompiler::Stage& s = by_stage[(int) stage];
        s.stage = stage;
        s.objects.push_back(obj.source);
    }
    std::vector<LLShaderCompiler::Stage> stages;
    for (auto& kv : by_stage)
    {
        stages.push_back(std::move(kv.second));
    }

    LLShaderTranslator::Options options = LLShaderCompiler::viewerOptions();
    if (!p.attribNames.empty())
    {
        options.attribNames = p.attribNames;        // the locations the viewer asked for with bindAttribLocation()
    }
    const LLShaderCompiler::Result r = LLShaderCompiler::compileProgram(stages, options, false);
    if (!r.ok())
    {
        p.log = r.error;
        return false;
    }
    p.uniforms = r.translation.uniforms;
    p.samplers = r.translation.samplers;
    p.attributes = r.translation.attributeNames;
    p.layout = r.layout;
    p.spirv = r.spirv;
    p.blob.clear();
    p.log.clear();
    const bool ok = finish(p);
    lastLinkMs = msSince(t0);
    return ok;
}

std::string VulkanShaderBackend::getProgramLog(U32 program)
{
    auto it = mPrograms.find(program);
    return it == mPrograms.end() ? "" : it->second.log;
}

void VulkanShaderBackend::use(U32 program)
{
    mCtx->program = program ? vkProgram(program) : nullptr;
}

// ----------------------------------------------------------------------------------------------------------------
// reflection. Locations: 0 .. members-1 are the uniform block members, then one per sampler.

S32 VulkanShaderBackend::getAttribLocation(U32 program, const char* name)
{
    auto it = mPrograms.find(program);
    if (it == mPrograms.end() || std::find(it->second.attributes.begin(), it->second.attributes.end(), name) == it->second.attributes.end())
    {
        return -1;
    }
    const auto& table = it->second.attribNames.empty() ? LLShaderCompiler::viewerOptions().attribNames : it->second.attribNames;
    auto pos = std::find(table.begin(), table.end(), name);
    return pos == table.end() ? -1 : (S32) (pos - table.begin());
}

S32 VulkanShaderBackend::getUniformLocation(U32 program, const char* cname)
{
    auto it = mPrograms.find(program);
    if (it == mPrograms.end())
    {
        return -1;
    }
    std::string name = cname;
    if (name.size() > 3 && name.compare(name.size() - 3, 3, "[0]") == 0)
    {
        name.resize(name.size() - 3);
    }
    const Program& p = it->second;
    for (size_t i = 0; i < p.uniforms.size(); ++i)
    {
        if (p.uniforms[i].name == name)
        {
            return (S32) i;
        }
    }
    for (size_t i = 0; i < p.samplers.size(); ++i)
    {
        if (p.samplers[i].name == name)
        {
            return (S32) (p.uniforms.size() + i);
        }
    }
    return -1;
}

S32 VulkanShaderBackend::getActiveUniformCount(U32 program)
{
    auto it = mPrograms.find(program);
    return it == mPrograms.end() ? 0 : (S32) (it->second.uniforms.size() + it->second.samplers.size());
}

void VulkanShaderBackend::getActiveUniform(U32 program, S32 index, std::string& name, U32& type, S32& size)
{
    const Program& p = mPrograms[program];
    // samplers first, in binding order: the viewer gives sampler uniforms their texture channels in enumeration order
    const std::string* glsl_type;
    const std::string* array;
    if (index < (S32) p.samplers.size())
    {
        const auto& s = p.samplers[index];
        name = s.name;
        glsl_type = &s.type;
        array = &s.arraySize;
    }
    else
    {
        const auto& u = p.uniforms[index - p.samplers.size()];
        name = u.name;
        glsl_type = &u.type;
        array = &u.arraySize;
    }
    type = glTypeOf(*glsl_type);
    size = array->empty() ? 1 : atoi(array->c_str());
    if (!array->empty())
    {
        name += "[0]";      // GL reports arrays like this
    }
}

// ----------------------------------------------------------------------------------------------------------------
// SPIR-V blob (the program binary calls)

void VulkanShaderBackend::serialize(Program& p)
{
    p.blob.clear();
    Writer w{ p.blob };
    w.u32(kBlobMagic);
    w.u32(kBlobVersion);
    w.u32((U32) p.attribNames.size());
    for (const auto& a : p.attribNames) w.str(a);
    w.u32((U32) p.attributes.size());
    for (const auto& a : p.attributes) w.str(a);
    w.u32((U32) p.uniforms.size());
    for (size_t i = 0; i < p.uniforms.size(); ++i)
    {
        w.str(p.uniforms[i].type);
        w.str(p.uniforms[i].name);
        w.str(p.uniforms[i].arraySize);
        w.u32((U32) p.layout.members[i].offset);
        w.u32((U32) p.layout.members[i].size);
        w.u32((U32) p.layout.members[i].arrayStride);
    }
    w.u32((U32) p.layout.size);
    w.u32((U32) p.samplers.size());
    for (const auto& s : p.samplers)
    {
        w.str(s.type);
        w.str(s.name);
        w.str(s.arraySize);
        w.u32((U32) s.binding);
    }
    w.u32((U32) p.spirv.size());
    for (const auto& m : p.spirv) w.words(m);
}

S32 VulkanShaderBackend::getProgramBinaryLength(U32 program)
{
    auto it = mPrograms.find(program);
    if (it == mPrograms.end() || !it->second.vk)
    {
        return 0;
    }
    if (it->second.blob.empty())
    {
        serialize(it->second);
    }
    return (S32) it->second.blob.size();
}

bool VulkanShaderBackend::getProgramBinary(U32 program, std::vector<U8>& binary, U32& format)
{
    if (getProgramBinaryLength(program) == 0)
    {
        return false;
    }
    binary = mPrograms[program].blob;
    format = kBlobMagic;
    return true;
}

bool VulkanShaderBackend::loadProgramBinary(U32 program, U32 format, const void* data, S32 length)
{
    auto it = mPrograms.find(program);
    if (it == mPrograms.end() || format != kBlobMagic)
    {
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    Program& p = it->second;
    Reader r{ (const U8*) data, (const U8*) data + length };
    if (r.u32() != kBlobMagic || r.u32() != kBlobVersion)
    {
        return false;
    }
    p.attribNames.clear();
    for (U32 i = 0, n = r.u32(); i < n && !r.bad; ++i) p.attribNames.push_back(r.str());
    p.attributes.clear();
    for (U32 i = 0, n = r.u32(); i < n && !r.bad; ++i) p.attributes.push_back(r.str());
    p.uniforms.clear();
    p.layout = LLShaderTranslator::BlockLayout();
    for (U32 i = 0, n = r.u32(); i < n && !r.bad; ++i)
    {
        LLShaderTranslator::UniformMember u;
        u.type = r.str();
        u.name = r.str();
        u.arraySize = r.str();
        LLShaderTranslator::BlockMember m;
        m.name = u.name;
        m.offset = (int) r.u32();
        m.size = (int) r.u32();
        m.arrayStride = (int) r.u32();
        p.uniforms.push_back(u);
        p.layout.members.push_back(m);
    }
    p.layout.size = (int) r.u32();
    p.samplers.clear();
    for (U32 i = 0, n = r.u32(); i < n && !r.bad; ++i)
    {
        LLShaderTranslator::SamplerBinding s;
        s.type = r.str();
        s.name = r.str();
        s.arraySize = r.str();
        s.binding = (int) r.u32();
        p.samplers.push_back(s);
    }
    p.spirv.clear();
    for (U32 i = 0, n = r.u32(); i < n && !r.bad; ++i) p.spirv.push_back(r.words());
    if (r.bad || !finish(p))
    {
        return false;
    }
    p.blob.assign((const U8*) data, (const U8*) data + length);
    lastLoadMs = msSince(t0);
    return true;
}

// ----------------------------------------------------------------------------------------------------------------
// uniforms. The program keeps the block as a CPU shadow (VkProgram::shadow); every draw snapshots it.

void VulkanShaderBackend::writeVectors(U32 program, S32 loc, int srcComps, int count, const void* src)
{
    auto it = mPrograms.find(program);
    if (it != mPrograms.end() && it->second.vk && loc >= 0 && loc < (S32) it->second.uniforms.size())
    {
        Program& p = it->second;
        std140pack::writeVectors(p.vk->shadow.data(), p.layout.members[loc], p.uniforms[loc].type, srcComps, count, src);
    }
}

void VulkanShaderBackend::writeMatrices(U32 program, S32 loc, int cols, int rows, int count, bool transpose, const F32* src)
{
    auto it = mPrograms.find(program);
    if (it != mPrograms.end() && it->second.vk && loc >= 0 && loc < (S32) it->second.uniforms.size())
    {
        Program& p = it->second;
        std140pack::writeMatrices(p.vk->shadow.data(), p.layout.members[loc], p.uniforms[loc].type, cols, rows, count, transpose, src);
    }
}

void VulkanShaderBackend::uniform1i(U32 p, S32 loc, S32 v)
{
    auto it = mPrograms.find(p);
    if (it == mPrograms.end() || !it->second.vk)
    {
        return;
    }
    const S32 first_sampler = (S32) it->second.uniforms.size();
    if (loc >= first_sampler && loc < first_sampler + (S32) it->second.samplers.size())
    {
        it->second.vk->samplerUnit[loc - first_sampler] = v;       // the value of a sampler uniform is its texture unit
        return;
    }
    writeVectors(p, loc, 1, 1, &v);
}

void VulkanShaderBackend::uniform2i(U32 p, S32 loc, S32 x, S32 y) { const S32 v[2] = {x, y}; writeVectors(p, loc, 2, 1, v); }
void VulkanShaderBackend::uniform1f(U32 p, S32 loc, F32 v) { writeVectors(p, loc, 1, 1, &v); }
void VulkanShaderBackend::uniform2f(U32 p, S32 loc, F32 x, F32 y) { const F32 v[2] = {x, y}; writeVectors(p, loc, 2, 1, v); }
void VulkanShaderBackend::uniform3f(U32 p, S32 loc, F32 x, F32 y, F32 z) { const F32 v[3] = {x, y, z}; writeVectors(p, loc, 3, 1, v); }
void VulkanShaderBackend::uniform4f(U32 p, S32 loc, F32 x, F32 y, F32 z, F32 w) { const F32 v[4] = {x, y, z, w}; writeVectors(p, loc, 4, 1, v); }

void VulkanShaderBackend::uniform1iv(U32 p, S32 loc, S32 count, const S32* v)
{
    auto it = mPrograms.find(p);
    if (it != mPrograms.end() && it->second.vk)
    {
        const S32 first_sampler = (S32) it->second.uniforms.size();
        if (loc >= first_sampler && loc < first_sampler + (S32) it->second.samplers.size())
        {
            if (count > 0)
            {
                it->second.vk->samplerUnit[loc - first_sampler] = v[0];     // a sampler array is one descriptor binding here
            }
            return;
        }
    }
    writeVectors(p, loc, 1, count, v);
}

void VulkanShaderBackend::uniform4iv(U32 p, S32 loc, S32 count, const S32* v) { writeVectors(p, loc, 4, count, v); }
void VulkanShaderBackend::uniform1fv(U32 p, S32 loc, S32 count, const F32* v) { writeVectors(p, loc, 1, count, v); }
void VulkanShaderBackend::uniform2fv(U32 p, S32 loc, S32 count, const F32* v) { writeVectors(p, loc, 2, count, v); }
void VulkanShaderBackend::uniform3fv(U32 p, S32 loc, S32 count, const F32* v) { writeVectors(p, loc, 3, count, v); }
void VulkanShaderBackend::uniform4fv(U32 p, S32 loc, S32 count, const F32* v) { writeVectors(p, loc, 4, count, v); }
void VulkanShaderBackend::uniform4uiv(U32 p, S32 loc, S32 count, const U32* v) { writeVectors(p, loc, 4, count, v); }
void VulkanShaderBackend::uniformMatrix2fv(U32 p, S32 loc, S32 count, bool t, const F32* v) { writeMatrices(p, loc, 2, 2, count, t, v); }
void VulkanShaderBackend::uniformMatrix3fv(U32 p, S32 loc, S32 count, bool t, const F32* v) { writeMatrices(p, loc, 3, 3, count, t, v); }
void VulkanShaderBackend::uniformMatrix3x4fv(U32 p, S32 loc, S32 count, bool t, const F32* v) { writeMatrices(p, loc, 3, 4, count, t, v); }
void VulkanShaderBackend::uniformMatrix4fv(U32 p, S32 loc, S32 count, bool t, const F32* v) { writeMatrices(p, loc, 4, 4, count, t, v); }
