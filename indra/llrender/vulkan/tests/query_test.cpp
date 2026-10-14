#include "vkquerybackend.h"
#include "vkbackends.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include <cstring>

int main(int argc,char** argv)
{
    const bool validation=argc>1 && !strcmp(argv[1],"--validate");
    VkContext ctx; unsigned checks=0;
    auto check=[&](bool condition,const char* label)
    { if(!condition) throw std::runtime_error(label); ++checks; printf("PASS %s\n",label); };
    try
    {
        if(!ctx.init(GetModuleHandle(nullptr),nullptr,validation) || (validation && !ctx.validation)) return 1;
        {
            VulkanRenderStateBackend state;
            VulkanVertexBufferBackend vb(&ctx,&state);
            VulkanShaderBackend shaders(&ctx);
            VulkanTextureBackend textures(&ctx);
            VulkanFramebufferBackend fb(&ctx,&state);
            VulkanQueryBackend queries(&ctx);
            constexpr U32 TIME=0x88BF,SAMPLES=0x8914,ANY=0x8C2F,PRIMITIVES=0x8C87;
            check(queries.supported(TIME) && queries.supported(SAMPLES) && queries.supported(PRIMITIVES),"GPU query capabilities");
            check(VulkanQueryBackend::elapsed(250,4,8,2.5)==25,"timestamp wrap and nanosecond conversion");
            U32 program=shaders.createProgram();
            const char* source[]={
                "#version 330\nin vec4 position;\nuniform float z;\nvoid main(){gl_Position=vec4(position.xy,z,1);}",
                "#version 330\nout vec4 color;void main(){color=vec4(1,0,0,1);}"};
            for(int stage=0;stage<2;++stage)
            {
                auto object=shaders.createObject(stage?0x8B30:0x8B31);std::string log;
                if(!shaders.compileObject(object,1,&source[stage],log)) throw std::runtime_error(log);
                shaders.attachObject(program,object);
            }
            shaders.bindAttribLocation(program,0,"position");
            if(!shaders.link(program)) throw std::runtime_error(shaders.getProgramLog(program));
            BufferHandle vertices; U8* shadow=nullptr; vb.allocate(BufferKind::Vertex,48,vertices,shadow);
            const float triangle[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1}; U32 offsets[14]{};
            vb.upload(BufferKind::Vertex,vertices,0,47,triangle);vb.setBuffers(vertices,{},1,offsets);
            shaders.use(program);auto z=shaders.getUniformLocation(program,"z");
            state.setViewport(0,0,800,600);state.setCapability(0x0B44,false);state.setDepthTest(true);state.setDepthFunc(0x0201);
            const float clear[]={0,0,0,1};
            U32 ids[8];queries.generate(8,ids);
            const auto pools=queries.poolCount();U32 unused[1024];queries.generate(1024,unused);
            check(queries.poolCount()==pools,"logical query names allocate no GPU pools");queries.destroy(1024,unused);
            ctx.beginFrame(clear);
            shaders.uniform1f(program,z,.2f);vb.drawArrays(0,0,3);state.setDepthMask(false);
            queries.begin(ANY,ids[0]);shaders.uniform1f(program,z,.8f);vb.drawArrays(0,0,3);queries.end(ANY);
            queries.begin(ANY,ids[1]);shaders.uniform1f(program,z,0);vb.drawArrays(0,0,3);queries.end(ANY);
            const auto submits=ctx.blockingSubmissions;
            check(!queries.available(ids[0]) && !queries.available(ids[1]),"unsubmitted results are unavailable");
            check(ctx.blockingSubmissions==submits,"availability polling never submits or waits");
            check(!ctx.stallCounts[size_t(VkContext::Stall::Query)],"availability polling adds no blocking query metrics");
            ctx.endFrame(false);
            check(queries.result(ids[0])==0,"occluded bounding geometry returns zero");
            check(ctx.stallCounts[size_t(VkContext::Stall::Query)]>0,"explicit query reads are attributed to blocking query metrics");
            check(queries.result(ids[1])==1,"visible bounding geometry returns true");
            // Reusing a logical name cannot expose the previous generation's availability or result.
            ctx.beginFrame(clear);state.setDepthTest(false);
            check(ctx.slotCount>1 || ctx.lastGpuMs>0,"asynchronous whole-frame GPU timing"); // with several slots the result is one frame older
            queries.begin(ANY,ids[0]);vb.drawArrays(0,0,3);queries.end(ANY);
            check(fb.isRendering(),"ordinary visibility queries preserve rendering scope");
            check(!queries.available(ids[0]),"reissued query rejects stale availability");
            U32 color=0;textures.generate(1,&color);textures.activateUnit(0);textures.bind(0x0DE1,color);
            textures.image2D(0x0DE1,0,0x8058,800,600,0x1908,0x1401,nullptr);
            U32 offscreen=fb.createFramebuffer();fb.bind(offscreen);fb.attach(offscreen,0x8CE0,0x0DE1,color,0);fb.bind(0);
            queries.begin(TIME,ids[2]);queries.begin(SAMPLES,ids[3]);queries.begin(PRIMITIVES,ids[4]);
            vb.drawArrays(0,0,3);fb.bind(offscreen);vb.drawArrays(0,0,3);fb.bind(0);
            queries.end(PRIMITIVES);queries.end(SAMPLES);queries.end(TIME);
            ctx.endFrame(false);
            check(queries.result(ids[0])==1,"reissued query reads its new result");
            check(queries.result(ids[3])==800u*600u*2u,"precise samples accumulate across rendering scopes");
            check(queries.result(ids[4])==2,"hardware primitive statistics accumulate across target switches");
            const auto ns=queries.result(ids[2]);check(ns>0 && ns<10000000000ull,"GPU timer produces bounded nanoseconds");
            printf("GPU_TIMER_NS=%llu\n",(unsigned long long)ns);
            // Simultaneous sample-count and boolean scopes share physical occlusion segments.
            ctx.beginFrame(clear);queries.begin(SAMPLES,ids[3]);vb.drawArrays(0,0,3);
            queries.begin(ANY,ids[1]);vb.drawArrays(0,0,3);queries.end(ANY);
            vb.drawArrays(0,0,3);queries.end(SAMPLES);ctx.endFrame(false);
            check(queries.result(ids[3])==800u*600u*3u && queries.result(ids[1])==1,"overlapping logical occlusion scopes");
            U32 geometryProgram=shaders.createProgram();
            const char* geometrySource="#version 330\nlayout(triangles) in;\nlayout(triangle_strip,max_vertices=6) out;\n"
                "void main(){for(int j=0;j<2;++j){for(int i=0;i<3;++i){gl_Position=gl_in[i].gl_Position;EmitVertex();}EndPrimitive();}}";
            for(int stage=0;stage<3;++stage)
            {
                U32 object=shaders.createObject(stage==0?0x8B31:stage==1?0x8B30:0x8DD9);std::string log;
                const char* text=stage==2?geometrySource:source[stage];
                if(!shaders.compileObject(object,1,&text,log)) throw std::runtime_error(log);
                shaders.attachObject(geometryProgram,object);
            }
            shaders.bindAttribLocation(geometryProgram,0,"position");
            if(!shaders.link(geometryProgram)) throw std::runtime_error(shaders.getProgramLog(geometryProgram));
            ctx.beginFrame(clear);queries.begin(PRIMITIVES,ids[4]);
            shaders.use(program);vb.drawArrays(0,0,3);
            shaders.use(geometryProgram);shaders.uniform1f(geometryProgram,shaders.getUniformLocation(geometryProgram,"z"),0);
            vb.drawArrays(0,0,3);queries.end(PRIMITIVES);ctx.endFrame(false);
            check(queries.result(ids[4])==3,"mixed shader statistics use geometry output primitives");
            shaders.use(program);shaders.deleteProgramAndObjects(geometryProgram);
            // Explicit readback flushes once; an active timer survives the partial submission.
            ctx.beginFrame(clear);queries.begin(TIME,ids[2]);queries.begin(ANY,ids[1]);vb.drawArrays(0,0,3);queries.end(ANY);
            const auto beforeRead=ctx.blockingSubmissions;
            check(queries.result(ids[1])==1 && ctx.blockingSubmissions==beforeRead+1,"explicit current-frame result submits before waiting");
            vb.drawArrays(0,0,3);queries.end(TIME);ctx.endFrame(false);
            check(queries.result(ids[2])>0,"timer survives a synchronous partial submission");
            // Queries discarded before their submission are retained safely, then pages recycle after the fence.
            for(int frame=0;frame<8;++frame)
            {
                ctx.beginFrame(clear);
                for(int i=0;i<600;++i)
                {
                    U32 id;queries.generate(1,&id);queries.begin(ANY,id);vb.drawArrays(0,0,3);queries.end(ANY);queries.destroy(1,&id);
                }
                ctx.endFrame(false);
            }
            check(queries.poolCount()<=8,"query pages remain bounded after in-flight deletion and reuse");
            ctx.beginFrame(clear);queries.begin(ANY,ids[5]);queries.end(ANY);ctx.endFrame(false);
            check(queries.result(ids[5])==0,"empty visibility scope returns zero");
            ctx.beginFrame(clear);queries.begin(TIME,ids[2]);vb.drawArrays(0,0,3);ctx.endFrame(false);
            queries.end(TIME);
            check(ctx.validationErrors==0,"timer finalization after presentation records no invalid commands");
            check(queries.result(ids[2])>0,"timer result remains valid after presentation");
            ctx.beginFrame(clear);queries.begin(TIME,ids[2]);vb.drawArrays(0,0,3);ctx.endFrame(false);
            ctx.beginFrame(clear);vb.drawArrays(0,0,3);ctx.endFrame(false);queries.end(TIME);
            check(queries.result(ids[2])>0 && ctx.validationErrors==0,"timer segments survive multiple frames and finalize while inactive");
            // Water bounds beyond the far plane must survive when depth clamp is enabled.
            ctx.beginFrame(clear);state.setCapability(0x864F,true);state.setDepthTest(true);state.setDepthFunc(0x0203);
            queries.begin(ANY,ids[6]);shaders.uniform1f(program,z,2);vb.drawArrays(0,0,3);queries.end(ANY);ctx.endFrame(false);
            check(ctx.depthClamp && queries.result(ids[6])==1,"far-plane water bounds use native depth clamp");
            vb.free(BufferKind::Vertex,48,vertices,shadow);queries.destroy(8,ids);fb.destroyFramebuffer(offscreen);textures.destroy(1,&color);
            ctx.hostQueryReset=false;
            check(!queries.supported(ANY) && !queries.supported(TIME),"unsupported host-reset devices disable native query callers");
            ctx.hostQueryReset=true;
            shaders.deleteProgramAndObjects(program);
        }
        ctx.shutdown();
        check(ctx.validationErrors==0 && ctx.validationWarnings==0,"zero Vulkan validation messages");
        printf("QUERY_CHECKS=%u validation=%u errors=%u warnings=%u\n",checks,validation,ctx.validationErrors,ctx.validationWarnings);
        return 0;
    }
    catch(const std::exception& error){fprintf(stderr,"Query test: %s\n",error.what());return 1;}
}
