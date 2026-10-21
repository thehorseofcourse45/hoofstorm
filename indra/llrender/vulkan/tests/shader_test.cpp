// GPU checks for geometry-stage execution and versioned, failure-safe SPIR-V caches.
#include "vkshaderbackend.h"
#include "vkbackends.h"
#include "vkframebufferbackend.h"
#include "glconsts.h"
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>

// The viewer's class1/deferred shader directory (CMake: LL_VIEWER_DEFERRED_SHADERS).
static bool readViewerShader(const char* name, std::string& text)
{
    std::ifstream in(std::string(LL_VIEWER_DEFERRED_SHADERS) + "/" + name, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

bool test_shaders(VkContext& ctx, VulkanRenderStateBackend& st, VulkanVertexBufferBackend& vb,
                  VulkanShaderBackend& shaders, VulkanFramebufferBackend& fb)
{
    bool passed = true;
    int checks = 0;
    auto expect = [&](const char* label, bool value) {
        printf("  [%s] shader: %s\n", value ? "PASS" : "FAIL", label);
        ++checks;
        passed &= value;
    };
    printf("scene: shader geometry/cache\n");
    const char* sources[] = {
        "#version 330\nin vec4 position;\nout vec4 vertex_color_input;\n"
        "void main(){gl_Position=position;vertex_color_input=vec4(1);}",
        "#version 330\nin vec4 vertex_color;\nout vec4 color;\nvoid main(){color=vertex_color;}",
        "#version 330\nlayout(triangles) in;\nlayout(triangle_strip,max_vertices=3) out;\n"
        "in vec4 vertex_color_input[];\nout vec4 vertex_color;\nuniform vec4 geometry_color;\n"
        "void main(){for(int i=0;i<3;++i){gl_Position=gl_in[i].gl_Position+vec4(0.7,0,0,0);"
        "vertex_color=geometry_color*vertex_color_input[i];EmitVertex();}EndPrimitive();}"
    };
    const U32 stageTypes[] = {0x8B31,0x8B30,0x8DD9};
    U32 program = shaders.createProgram();
    for (int i=0;i<3;++i)
    {
        U32 object = shaders.createObject(stageTypes[i]);
        std::string log;
        expect("shader source accepted", shaders.compileObject(object,1,&sources[i],log));
        shaders.attachObject(program,object);
    }
    shaders.bindAttribLocation(program,0,"position");
    const bool linked = shaders.link(program);
    expect("three-stage program links", linked);
    if (!linked)
    {
        fprintf(stderr,"%s\n",shaders.getProgramLog(program).c_str());
        shaders.deleteProgramAndObjects(program);
        return false;
    }
    expect("geometry module exists", shaders.vkProgram(program)->gs != VK_NULL_HANDLE);
    expect("compiler identity salts cache keys", shaders.programBinaryCacheSalt() == LLShaderCompiler::cacheIdentity());
    std::vector<U8> binary;
    U32 format=0;
    expect("geometry program serializes", shaders.getProgramBinary(program,binary,format));
    U32 cached=shaders.createProgram();
    expect("geometry program restores", shaders.loadProgramBinary(cached,format,binary.data(),(S32)binary.size()));
    expect("restored geometry module exists", shaders.vkProgram(cached) && shaders.vkProgram(cached)->gs);
    auto* original=shaders.vkProgram(cached);
    shaders.use(cached);
    expect("loading a bound program updates the active pointer",
        shaders.loadProgramBinary(cached,format,binary.data(),(S32)binary.size()) &&
        shaders.vkProgram(cached)!=original && ctx.program==shaders.vkProgram(cached));
    original=shaders.vkProgram(cached);
    auto rejects = [&](const std::vector<U8>& blob) {
        return !shaders.loadProgramBinary(cached,format,blob.data(),(S32)blob.size()) &&
            shaders.vkProgram(cached)==original && ctx.program==original && shaders.validate(cached);
    };
    auto put32 = [](std::vector<U8>& blob,size_t at,U32 value) { memcpy(blob.data()+at,&value,4); };
    auto get32 = [&](size_t at) { U32 value; memcpy(&value,binary.data()+at,4); return value; };
    auto altered=binary;
    put32(altered,4,1);
    expect("old blob schema rejected without replacing program",rejects(altered));
    auto changeIdentity = [&](const char* token) {
        altered=binary;
        const std::string identity=LLShaderCompiler::cacheIdentity();
        const auto at=identity.find(token);
        if (at==std::string::npos) return false;
        altered[12+at]^=1;
        return rejects(altered);
    };
    expect("translator identity mismatch rejected",changeIdentity("translator-"));
    expect("glslang identity mismatch rejected",changeIdentity("glslang-"));
    expect("SPIRV-Tools identity mismatch rejected",changeIdentity("SPIRV-Tools"));
    expect("compiler package identity mismatch rejected",changeIdentity("package-"));
    altered=binary;
    const size_t attrs=12+get32(8);
    put32(altered,attrs,0xffffffffu);
    expect("oversized reflection count rejected",rejects(altered));
    size_t offset=attrs;
    auto skipStrings = [&]() {
        const U32 count=get32(offset);offset+=4;
        for(U32 i=0;i<count;++i){offset+=4+get32(offset);}
    };
    skipStrings();skipStrings();
    const U32 uniforms=get32(offset);offset+=4;
    expect("geometry-only uniform reflected",uniforms==1);
    for(int i=0;i<3;++i) offset+=4+get32(offset);
    altered=binary;put32(altered,offset,4);
    expect("invalid uniform offset rejected",rejects(altered));
    offset+=12;
    altered=binary;put32(altered,offset,0x7fffffff);
    expect("oversized uniform block rejected",rejects(altered));
    altered=binary;altered.push_back(0);
    expect("trailing bytes rejected",rejects(altered));
    const U32 magic=0x07230203;
    size_t module=0;
    for(size_t i=0;i+4<binary.size();++i) if(get32(i)==magic){module=i;break;}
    altered=binary;put32(altered,module,0);
    expect("invalid SPIR-V rejected before driver module creation",module && rejects(altered));
    bool truncated=true;
    for(size_t length : {size_t(0),size_t(1),size_t(7),size_t(12),attrs,offset,module,binary.size()-1})
        truncated &= !shaders.loadProgramBinary(cached,format,binary.data(),(S32)length) && shaders.vkProgram(cached)==original;
    expect("truncated cache payloads rejected",truncated);
    expect("null and negative-length payloads rejected",
        !shaders.loadProgramBinary(cached,format,nullptr,10) &&
        !shaders.loadProgramBinary(cached,format,binary.data(),-1));
    expect("other API binary format rejected",!shaders.loadProgramBinary(cached,format^1,binary.data(),(S32)binary.size()));
    const bool supported=ctx.geometryShader;
    ctx.geometryShader=false;
    expect("missing geometry feature rejected",rejects(binary));
    ctx.geometryShader=supported;

    BufferHandle vertices;
    U8* cpu=nullptr;
    vb.allocate(BufferKind::Vertex,48,vertices,cpu);
    const float positions[3][4]={{-0.9f,-0.3f,0.5f,1},{-0.5f,-0.3f,0.5f,1},{-0.7f,0.3f,0.5f,1}};
    memcpy(cpu,positions,sizeof(positions));
    vb.upload(BufferKind::Vertex,vertices,0,47,cpu);
    U32 offsets[14]{};
    for(U32 id : {program,cached})
    {
        const float clear[]={0,0,0,1};
        if(!ctx.beginFrame(clear)){passed=false;break;}
        fb.bind(0);
        st.setViewport(0,0,(S32)ctx.extent.width,(S32)ctx.extent.height);
        st.setCapability(0x0B44,false);
        st.setCapability(0x0BE2,false);
        st.setCapability(0x0C11,false);
        st.setDepthTest(false);
        st.setColorMask(true,true,true,true);
        shaders.use(id);
        shaders.uniform4f(id,shaders.getUniformLocation(id,"geometry_color"),0,1,0,1);
        vb.setBuffers(vertices,{},1,offsets);
        vb.drawArrays(0,0,3);
        U8 center[4]{},left[4]{};
        fb.readPixels((S32)ctx.extent.width/2,(S32)ctx.extent.height/2,1,1,glc::RGBA,glc::UNSIGNED_BYTE,center);
        fb.readPixels((S32)(ctx.extent.width*0.15f),(S32)ctx.extent.height/2,1,1,glc::RGBA,glc::UNSIGNED_BYTE,left);
        expect(id==program ? "geometry transforms vertices and reads its uniform" : "cached geometry renders identically",
            center[0]==0 && center[1]==255 && center[2]==0 && left[0]==0 && left[1]==0 && left[2]==0);
        ctx.endFrame(false);
    }
    shaders.use(0);
    vb.free(BufferKind::Vertex,48,vertices,cpu);
    shaders.deleteProgramAndObjects(program);
    shaders.deleteProgramAndObjects(cached);

    // FSR 2 motion vectors: the viewer's fsr2MotionF.glsl, with the version and stage lines LLShaderMgr puts in front of it,
    // compiles and links with the viewer's post vertex shader and exposes the uniforms LLPipeline::generateFsr2Motion sets.
    {
        std::string vertexBody, fragmentBody;
        const bool found = readViewerShader("postDeferredNoTCV.glsl", vertexBody) && readViewerShader("fsr2MotionF.glsl", fragmentBody);
        expect("fsr2MotionF.glsl and its vertex shader are readable", found);
        if (found)
        {
            const std::string vertexSource = "#version 400\n#define VERTEX_SHADER 1\n" + vertexBody;
            const std::string fragmentSource = "#version 400\n#define FRAGMENT_SHADER 1\n" + fragmentBody;
            const char* stageSources[] = {vertexSource.c_str(), fragmentSource.c_str()};
            U32 motion = shaders.createProgram();
            bool compiled = true;
            for (int i = 0; i < 2; ++i)
            {
                U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31);
                std::string log;
                if (!shaders.compileObject(object, 1, &stageSources[i], log))
                {
                    compiled = false;
                    fprintf(stderr, "%s\n", log.c_str());
                }
                shaders.attachObject(motion, object);
            }
            expect("fsr2MotionF.glsl compiles", compiled);
            shaders.bindAttribLocation(motion, 0, "position");
            const bool motionLinked = compiled && shaders.link(motion);
            expect("FSR 2 motion program links", motionLinked);
            bool uniforms = motionLinked;
            for (const char* name : {"depthMap", "inv_view_proj_cur", "view_proj_cur", "view_proj_prev", "motion_y_sign"})
                uniforms = motionLinked && shaders.getUniformLocation(motion, name) >= 0 && uniforms;
            expect("FSR 2 motion program exposes its uniforms", uniforms);
            shaders.deleteProgramAndObjects(motion);
        }
    }

    // FSR 2 avatar velocity: the rigged and body vertex shaders with the shared fragment shader. MAX_JOINTS_PER_MESH_OBJECT is a define
    // LLViewerShaderMgr puts in front of every shader; the test supplies it the same way.
    for (const char* vertexFile : {"fsr2AvatarVelocityV.glsl", "fsr2AvatarBodyVelocityV.glsl"})
    {
        const bool rigged = std::string(vertexFile) == "fsr2AvatarVelocityV.glsl";
        std::string vertexBody, fragmentBody;
        const bool found = readViewerShader(vertexFile, vertexBody) && readViewerShader("fsr2AvatarVelocityF.glsl", fragmentBody);
        expect((std::string(vertexFile) + " and fsr2AvatarVelocityF.glsl are readable").c_str(), found);
        if (!found) continue;
        const std::string vertexSource = "#version 400\n#define VERTEX_SHADER 1\n#define MAX_JOINTS_PER_MESH_OBJECT 110\n" + vertexBody;
        const std::string fragmentSource = "#version 400\n#define FRAGMENT_SHADER 1\n" + fragmentBody;
        const char* stageSources[] = {vertexSource.c_str(), fragmentSource.c_str()};
        U32 velocity = shaders.createProgram();
        bool compiled = true;
        for (int i = 0; i < 2; ++i)
        {
            U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31);
            std::string log;
            if (!shaders.compileObject(object, 1, &stageSources[i], log))
            {
                compiled = false;
                fprintf(stderr, "%s\n", log.c_str());
            }
            shaders.attachObject(velocity, object);
        }
        expect((std::string(vertexFile) + " program compiles").c_str(), compiled);
        // the attribute table LLShaderMgr::initAttribsAndUniforms binds for every viewer program
        const char* viewerAttribs[] = {"position", "normal", "texcoord0", "texcoord1", "texcoord2", "texcoord3", "diffuse_color", "emissive",
                                       "tangent", "weight", "weight4", "clothing", "joint", "texture_index"};
        for (U32 a = 0; a < 14; ++a) shaders.bindAttribLocation(velocity, a, viewerAttribs[a]);
        const bool linked = compiled && shaders.link(velocity);
        expect((std::string(vertexFile) + " program links").c_str(), linked);
        bool uniforms = linked;
        const char* names[] = {rigged ? "view_proj_cur" : "proj_unjittered", "matrixPalette", "matrixPalettePrev", "depthMap", "depth_params",
                               "motion_y_sign"};
        for (const char* name : names)
            uniforms = linked && shaders.getUniformLocation(velocity, name) >= 0 && uniforms;
        if (rigged) uniforms = linked && shaders.getUniformLocation(velocity, "view_proj_prev") >= 0 && uniforms;
        if (!rigged)
        {
            for (const char* name : {"prev_valid", "prev_clip_from_cur_clip"})
                uniforms = linked && shaders.getUniformLocation(velocity, name) >= 0 && uniforms;
        }
        expect((std::string(vertexFile) + " program exposes its uniforms").c_str(), uniforms);
        shaders.deleteProgramAndObjects(velocity);
    }

    // FSR 2 reactive mask: the viewer's fsr2ReactiveF.glsl, the same way, with the uniforms LLPipeline::generateFsr2Reactive sets.
    {
        std::string vertexBody, fragmentBody;
        const bool found = readViewerShader("postDeferredNoTCV.glsl", vertexBody) && readViewerShader("fsr2ReactiveF.glsl", fragmentBody);
        expect("fsr2ReactiveF.glsl and its vertex shader are readable", found);
        if (found)
        {
            const std::string vertexSource = "#version 400\n#define VERTEX_SHADER 1\n" + vertexBody;
            const std::string fragmentSource = "#version 400\n#define FRAGMENT_SHADER 1\n" + fragmentBody;
            const char* stageSources[] = {vertexSource.c_str(), fragmentSource.c_str()};
            U32 reactive = shaders.createProgram();
            bool compiled = true;
            for (int i = 0; i < 2; ++i)
            {
                U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31);
                std::string log;
                if (!shaders.compileObject(object, 1, &stageSources[i], log))
                {
                    compiled = false;
                    fprintf(stderr, "%s\n", log.c_str());
                }
                shaders.attachObject(reactive, object);
            }
            expect("fsr2ReactiveF.glsl compiles", compiled);
            shaders.bindAttribLocation(reactive, 0, "position");
            const bool reactiveLinked = compiled && shaders.link(reactive);
            expect("FSR 2 reactive program links", reactiveLinked);
            bool uniforms = reactiveLinked;
            for (const char* name : {"normalMap", "reactive_flag", "reactive_value"})
                uniforms = reactiveLinked && shaders.getUniformLocation(reactive, name) >= 0 && uniforms;
            expect("FSR 2 reactive program exposes its uniforms", uniforms);
            shaders.deleteProgramAndObjects(reactive);
        }
    }
    printf("shader geometry/cache: %d checks, %s\n",checks,passed?"PASS":"FAIL");
    return passed;
}
