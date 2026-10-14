#include "vkcontext.h"
#include "vkbackends.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include "vkquerybackend.h"
#include "glconsts.h"
#include <algorithm>
#include <chrono>
#include <cstring>

int main(int argc, char** argv)
{
    const bool uploads=argc>1 && !strcmp(argv[1],"textures");
    const bool validation=argc>2 && !strcmp(argv[2],"--validate");
    VkContext ctx;
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
            const char* source[]={
                "#version 330\nin vec4 position;\nvoid main(){gl_Position=position;}",
                "#version 330\nuniform sampler2D diffuseMap;\nuniform vec4 tint;\nout vec4 color;\nvoid main(){color=texture(diffuseMap,vec2(.5))*tint;}"};
            U32 program=shaders.createProgram();
            for(int stage=0;stage<2;++stage)
            {
                const auto object=shaders.createObject(stage?0x8B30:0x8B31);std::string log;
                if(!shaders.compileObject(object,1,&source[stage],log)) return 1;
                shaders.attachObject(program,object);
            }
            shaders.bindAttribLocation(program,0,"position");
            if(!shaders.link(program)) {fprintf(stderr,"%s\n",shaders.getProgramLog(program).c_str());return 1;}
            shaders.use(program);
            const auto tint=shaders.getUniformLocation(program,"tint");
            shaders.uniform1i(program,shaders.getUniformLocation(program,"diffuseMap"),0);
            U32 texture=0;textures.generate(1,&texture);textures.activateUnit(0);textures.bind(glc::TEXTURE_2D,texture);
            std::vector<uint8_t> white(128*128*4,255),red=white,green=white;
            for(size_t i=0;i<red.size();i+=4) {red[i+1]=red[i+2]=0;green[i]=green[i+2]=0;}
            textures.image2D(glc::TEXTURE_2D,0,glc::RGBA8,128,128,glc::RGBA,glc::UNSIGNED_BYTE,white.data());
            textures.setFilter(glc::TEXTURE_2D,glc::NEAREST,glc::NEAREST);
            BufferHandle vertices;U8* shadow=nullptr;vb.allocate(BufferKind::Vertex,96,vertices,shadow);
            const float positions[]={-.7f,-.3f,.5f,1,-.3f,-.3f,.5f,1,-.5f,.3f,.5f,1,
                                      .3f,-.3f,.5f,1, .7f,-.3f,.5f,1, .5f,.3f,.5f,1};
            U32 offsets[14]{};vb.upload(BufferKind::Vertex,vertices,0,95,positions);vb.setBuffers(vertices,{},1,offsets);
            state.setViewport(0,0,800,600);state.setCapability(0x0B44,false);state.setDepthTest(false);
            const float clear[4]={0,0,0,1};
            std::vector<double> times;
            const int operations=uploads?8:256;
            uint64_t waits=0;
            for(int frame=0;frame<29;++frame)
            {
                const auto start=std::chrono::steady_clock::now();
                if(!ctx.beginFrame(clear)) return 1;
                const auto before=ctx.blockingSubmissions;
                for(int i=0;i<operations;++i)
                {
                    const bool right=(i%2)!=0;
                    if(uploads)
                    {
                        textures.subImage2D(glc::TEXTURE_2D,0,0,0,128,128,glc::RGBA,glc::UNSIGNED_BYTE,(right?green:red).data());
                        shaders.uniform4f(program,tint,1,1,1,1);
                    }
                    else
                    {
                        vb.upload(BufferKind::Vertex,vertices,0,95,positions);
                        shaders.uniform4f(program,tint,right?0.f:1.f,right?1.f:0.f,0,1);
                    }
                    vb.drawArrays(0,right?3:0,3);
                }
                const auto frameWaits=ctx.blockingSubmissions-before;
                ctx.endFrame(false);
                VK_CHECK(vkWaitForFences(ctx.device,1,&ctx.slot().fence,VK_TRUE,UINT64_MAX));
                const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                if(frame>=5) {times.push_back(ms);waits+=frameWaits;}
            }
            // One final frame checks both early and late draws across the entire upload batch.
            ctx.beginFrame(clear);
            for(int i=0;i<operations;++i)
            {
                const bool right=i%2;
                if(uploads) {textures.subImage2D(glc::TEXTURE_2D,0,0,0,128,128,glc::RGBA,glc::UNSIGNED_BYTE,(right?green:red).data());shaders.uniform4f(program,tint,1,1,1,1);}
                else {vb.upload(BufferKind::Vertex,vertices,0,95,positions);shaders.uniform4f(program,tint,right?0.f:1.f,right?1.f:0.f,0,1);}
                vb.drawArrays(0,right?3:0,3);
            }
            uint8_t left[4]{},right[4]{};
            fb.readPixels(200,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,left);
            fb.readPixels(600,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,right);
            if(left[0]!=255 || left[1]!=0 || right[0]!=0 || right[1]!=255) throw std::runtime_error("Frame workload pixel regression");
            ctx.endFrame(false);
            if(waits != 0) throw std::runtime_error("Unbounded workload reintroduced synchronous partial submissions");
            std::sort(times.begin(),times.end());
            printf("PERF workload=%s validation=%u frames=%zu operations=%d median_ms=%.4f p95_ms=%.4f blocking_submits=%llu\n",
                uploads?"textures":"stream",validation,times.size(),operations,times[times.size()/2],times[(times.size()*95)/100],(unsigned long long)waits);
            // Partial writes retain untouched vertices when a physical buffer is replaced.
            textures.subImage2D(glc::TEXTURE_2D,0,0,0,128,128,glc::RGBA,glc::UNSIGNED_BYTE,white.data());
            ctx.beginFrame(clear);
            vb.upload(BufferKind::Vertex,vertices,0,95,positions);
            shaders.uniform4f(program,tint,1,0,0,1);vb.drawArrays(0,0,3);
            vb.upload(BufferKind::Vertex,vertices,0,47,positions);
            shaders.uniform4f(program,tint,0,1,0,1);vb.drawArrays(0,3,3);
            fb.readPixels(200,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,left);
            fb.readPixels(600,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,right);
            if(left[0]!=255 || right[1]!=255) throw std::runtime_error("Partial upload lost untouched vertices");
            ctx.endFrame(false);
            // Large repeated mutations exercise the bounded-memory fallback, not an unbounded allocation pool.
            BufferHandle large;U8* largeShadow=nullptr;const U32 largeSize=8u<<20;
            vb.allocate(BufferKind::Vertex,largeSize,large,largeShadow);
            ctx.beginFrame(clear);const auto capBefore=ctx.blockingSubmissions;
            vb.setBuffers(large,{},1,offsets);
            for(int i=0;i<24;++i){vb.upload(BufferKind::Vertex,large,0,95,positions);vb.drawArrays(0,3,3);}
            if(ctx.blockingSubmissions==capBefore) throw std::runtime_error("Streaming memory cap did not fence reuse");
            fb.readPixels(600,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,right);
            if(right[1]!=255) throw std::runtime_error("Streaming memory cap lost pixels");
            if (!ctx.stallCounts[size_t(VkContext::Stall::BufferReuse)] || !ctx.stallCounts[size_t(VkContext::Stall::Readback)] ||
                ctx.stallMs[size_t(VkContext::Stall::BufferReuse)]<=0 || ctx.stallMs[size_t(VkContext::Stall::Readback)]<=0)
                throw std::runtime_error("Blocking operations lost reason-specific timing attribution");
            printf("stall attribution: buffer-reuse and readback PASS\n");
            ctx.endFrame(false);vb.free(BufferKind::Vertex,largeSize,large,largeShadow);
            printf("partial uploads and bounded streaming: PASS\n");
            vb.free(BufferKind::Vertex,96,vertices,shadow);textures.destroy(1,&texture);shaders.deleteProgramAndObjects(program);
        }
        ctx.shutdown();
        printf("validation: %u errors, %u warnings; pixels PASS\n",ctx.validationErrors,ctx.validationWarnings);
        return ctx.validationErrors==0 && ctx.validationWarnings==0 ? 0:1;
    }
    catch(const std::exception& error) {fprintf(stderr,"Frame pacing test: %s\n",error.what());return 1;}
}

