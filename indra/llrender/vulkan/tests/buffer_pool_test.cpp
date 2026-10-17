#include "vkcontext.h"
#include "vkbackends.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include "glconsts.h"
#include <chrono>
#include <array>
#include <algorithm>
#include <cstring>

int main(int argc,char** argv)
{
    bool baseline=argc>1 && !strcmp(argv[1],"--baseline");VkContext ctx;unsigned checks=0;
    auto check=[&](bool ok,const char* label){if(!ok)throw std::runtime_error(label);++checks;printf("PASS %s\n",label);};
    try{
        check(ctx.init(GetModuleHandle(nullptr),nullptr,true) && ctx.validation,"validated GPU initialized");
        {
            VulkanRenderStateBackend state;VulkanVertexBufferBackend vb(&ctx,&state);VulkanShaderBackend shaders(&ctx);VulkanTextureBackend textures(&ctx);VulkanFramebufferBackend fb(&ctx,&state);
            U32 program=shaders.createProgram();const char* sources[]={"#version 330\nin vec4 position;void main(){gl_Position=position;}","#version 330\nuniform vec4 tint;\nout vec4 color;void main(){color=tint;}"};
            for(int i=0;i<2;++i){U32 object=shaders.createObject(i?0x8B30:0x8B31);std::string log;check(shaders.compileObject(object,1,&sources[i],log),"shader accepted");shaders.attachObject(program,object);}
            shaders.bindAttribLocation(program,0,"position");check(shaders.link(program),"pool workload shader links");shaders.use(program);state.setDepthTest(false);state.setCapability(0x0B44,false);state.setViewport(0,0,800,600);
            const float clear[4]={0,0,0,1},green[4]={0,1,0,1},red[4]={1,0,0,1};U32 offsets[14]{};
            std::array<double,5> times{};const auto allocations=ctx.bufferAllocations,reuses=ctx.bufferPoolHits;
            for(auto& ms:times){auto start=std::chrono::steady_clock::now();for(int frame=0;frame<12;++frame){check(ctx.beginFrame(clear),"churn frame acquired");for(int i=0;i<64;++i){
                BufferHandle handle;U8* shadow=nullptr;U32 size=2048u+4096u*(i%8);vb.allocate(BufferKind::Vertex,size,handle,shadow);
                const float x=i%2? .5f:-.5f;const float triangle[]={x-.2f,-.3f,0,1,x+.2f,-.3f,0,1,x,.3f,0,1};
                vb.upload(BufferKind::Vertex,handle,0,47,triangle);vb.setBuffers(handle,{},1,offsets);shaders.uniform4fv(program,shaders.getUniformLocation(program,"tint"),1,i%2?green:red);vb.drawArrays(0,0,3);vb.free(BufferKind::Vertex,size,handle,shadow);
            }ctx.endFrame(false);}ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
            check(ctx.beginFrame(clear),"retired resources collected after GPU fence");
            BufferHandle handle;U8* shadow=nullptr;vb.allocate(BufferKind::Vertex,4096,handle,shadow);float triangle[]={-.7f,-.3f,0,1,-.3f,-.3f,0,1,-.5f,.3f,0,1};
            vb.upload(BufferKind::Vertex,handle,0,47,triangle);vb.setBuffers(handle,{},1,offsets);shaders.uniform4fv(program,shaders.getUniformLocation(program,"tint"),1,red);vb.drawArrays(0,0,3);vb.free(BufferKind::Vertex,4096,handle,shadow);
            BufferHandle next;shadow=nullptr;vb.allocate(BufferKind::Vertex,4096,next,shadow);for(int i=0;i<3;++i)triangle[i*4]+=1.f;
            vb.upload(BufferKind::Vertex,next,0,47,triangle);vb.setBuffers(next,{},1,offsets);shaders.uniform4fv(program,shaders.getUniformLocation(program,"tint"),1,green);vb.drawArrays(0,0,3);vb.free(BufferKind::Vertex,4096,next,shadow);
            U8 left[4]{},right[4]{};fb.readPixels(200,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,left);fb.readPixels(600,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,right);
            check(left[0]==255 && left[1]==0 && right[0]==0 && right[1]==255,"free then allocate cannot overwrite an in-flight draw");ctx.endFrame(false);
            std::sort(times.begin(),times.end());printf("POOL_METRICS median_ms=%.6f allocations=%llu reuses=%llu frames=60 buffers=3840 trials=5\n",times[2],ctx.bufferAllocations-allocations,ctx.bufferPoolHits-reuses);
            if(!baseline)check(ctx.bufferAllocations-allocations<=128 && ctx.bufferPoolHits-reuses>3700,"geometry churn reuses completed GPU allocations");
            check(ctx.beginFrame(clear),"pool budget test frame acquired");ctx.endFrame(false);VK_CHECK(vkDeviceWaitIdle(ctx.device));
            std::vector<BufferHandle> handles(70);std::vector<U8*> shadows(70,nullptr);for(int i=0;i<70;++i)vb.allocate(i%2?BufferKind::Index:BufferKind::Vertex,1<<20,handles[i],shadows[i]);
            for(int i=0;i<70;++i)vb.free(i%2?BufferKind::Index:BufferKind::Vertex,1<<20,handles[i],shadows[i]);
            ctx.collectReleasedResources();check(vb.getVramBytesUsed()<=(256u<<20),"completed buffer cache remains bounded to 256 MiB");
            {const auto memory=ctx.bufferMemoryAllocations;std::vector<BufferHandle> live(1000);std::vector<U8*> liveShadows(1000,nullptr);
            for(int i=0;i<1000;++i)vb.allocate(i%2?BufferKind::Index:BufferKind::Vertex,1024u+977u*(i%40),live[i],liveShadows[i]);
            printf("POOL_MEMORY_ALLOCATIONS live_buffers=1000 allocations=%llu\n",ctx.bufferMemoryAllocations-memory);
            check(ctx.bufferMemoryAllocations-memory<=16,"1000 live small buffers share a few memory allocations");
            for(int i=0;i<1000;++i)vb.free(i%2?BufferKind::Index:BufferKind::Vertex,1024u+977u*(i%40),live[i],liveShadows[i]);ctx.collectReleasedResources();}
            shaders.use(0);shaders.deleteProgramAndObjects(program);
        }
        ctx.shutdown();check(!ctx.validationErrors && !ctx.validationWarnings,"zero validation messages including pool teardown");printf("POOL_CHECKS=%u PASS\n",checks);return 0;
    }catch(const std::exception& e){fprintf(stderr,"buffer pool test: %s\n",e.what());return 1;}
}
