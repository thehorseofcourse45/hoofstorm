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
            U32 program=shaders.createProgram();const char* sources[]={"#version 330\nin vec4 position;void main(){gl_Position=position;}","#version 330\nuniform sampler2D diffuseMap;\nuniform float marker;\nout vec4 color;void main(){color=texture(diffuseMap,vec2(.5))+vec4(marker,0,0,0);}"};
            for(int i=0;i<2;++i){U32 object=shaders.createObject(i?0x8B30:0x8B31);std::string log;check(shaders.compileObject(object,1,&sources[i],log),"shader accepted");shaders.attachObject(program,object);}
            shaders.bindAttribLocation(program,0,"position");check(shaders.link(program),"upload workload shader links");shaders.use(program);shaders.uniform1i(program,shaders.getUniformLocation(program,"diffuseMap"),0);
            state.setDepthTest(false);state.setCapability(0x0B44,false);state.setViewport(0,0,800,600);
            U32 texture;textures.generate(1,&texture);textures.activateUnit(0);textures.bind(glc::TEXTURE_2D,texture);textures.image2D(glc::TEXTURE_2D,0,glc::RGBA8,128,128,glc::RGBA,glc::UNSIGNED_BYTE,nullptr);textures.setFilter(glc::TEXTURE_2D,glc::NEAREST,glc::NEAREST);
            std::vector<U8> red(512),green(512);for(int i=0;i<128;++i){red[i*4]=255;red[i*4+3]=255;green[i*4+1]=255;green[i*4+3]=255;}
            auto upload=[&](bool g){textures.subImage2D(glc::TEXTURE_2D,0,0,64,128,1,glc::RGBA,glc::UNSIGNED_BYTE,(g?green:red).data());};
            std::vector<U32> registry;
            if(argc>1 && !strcmp(argv[1],"--registry")){registry.resize(10000);textures.generate((U32)registry.size(),registry.data());}
            const float clear[4]={0,0,0,1};std::array<double,5> times{};auto allocated=ctx.textureUploadAllocations,ring=ctx.textureRingUploads,waits=ctx.blockingSubmissions;
            for(auto& ms:times){auto start=std::chrono::steady_clock::now();for(int f=0;f<6;++f){check(ctx.beginFrame(clear),"upload frame acquired");for(int i=0;i<512;++i)upload(i%2);ctx.endFrame(false);VK_CHECK(vkWaitForFences(ctx.device,1,&ctx.slot().fence,VK_TRUE,UINT64_MAX));}ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
            std::sort(times.begin(),times.end());printf("UPLOAD_METRICS median_ms=%.6f allocations=%llu ring_uploads=%llu calls=15360 trials=5\n",times[2],ctx.textureUploadAllocations-allocated,ctx.textureRingUploads-ring);
            if(!baseline)check(ctx.textureUploadAllocations==allocated && ctx.textureRingUploads-ring==15360,"ordinary recorded texture updates allocate no staging buffers");
            check(ctx.blockingSubmissions==waits,"bounded ordinary upload workload adds no partial submits");
            BufferHandle vertices;U8* shadow=nullptr;vb.allocate(BufferKind::Vertex,96,vertices,shadow);const float positions[]={-.7f,-.3f,.5f,1,-.3f,-.3f,.5f,1,-.5f,.3f,.5f,1,.3f,-.3f,.5f,1,.7f,-.3f,.5f,1,.5f,.3f,.5f,1};U32 offsets[14]{};vb.upload(BufferKind::Vertex,vertices,0,95,positions);vb.setBuffers(vertices,{},1,offsets);
            const auto capacity=ctx.ringSize;ctx.ringSize=4096;check(ctx.beginFrame(clear),"overflow frame acquired");auto before=ctx.blockingSubmissions;
            upload(false);vb.drawArrays(0,0,3);for(int i=0;i<40;++i)upload(true);vb.drawArrays(0,3,3);
            U8 left[4]{},right[4]{};fb.readPixels(200,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,left);fb.readPixels(600,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,right);
            check(left[0]==255 && left[1]==0 && right[0]==0 && right[1]==255,"ring reuse preserves early and late texture draws plus uniform snapshots");
            if(!baseline)check(ctx.blockingSubmissions>before && ctx.stallCounts[size_t(VkContext::Stall::Upload)]>0,"ring capacity fences reuse with upload stall attribution");ctx.endFrame(false);
            ctx.ringSize=256;check(ctx.beginFrame(clear),"oversized fallback frame acquired");before=ctx.textureUploadAllocations;upload(false);vb.drawArrays(0,0,3);fb.readPixels(200,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,left);check(left[0]==255 && ctx.textureUploadAllocations==before+1,"uploads larger than the ring retain transient staging fallback");ctx.endFrame(false);ctx.ringSize=capacity;
            VK_CHECK(vkDeviceWaitIdle(ctx.device));before=ctx.textureUploadAllocations;auto ringBefore=ctx.textureRingUploads;auto blockingBefore=ctx.blockingSubmissions;auto batchedBefore=ctx.batchedUploads;upload(true);
            check(ctx.blockingSubmissions==blockingBefore && ctx.batchedUploads>batchedBefore && ctx.textureRingUploads==ringBefore && ctx.batch.cmd,"inactive-frame uploads join the batch without waiting for the GPU");
            std::vector<U8> read(128*128*4);textures.readImage(glc::TEXTURE_2D,0,glc::RGBA,glc::UNSIGNED_BYTE,read.data());
            check(read[64*128*4+1]==255 && !ctx.batch.cmd,"readback submits the batch first and sees the uploaded pixels");
            // batch ordering across a frame boundary: upload between frames, draw in the next frame
            upload(false);upload(true);upload(false);check(ctx.beginFrame(clear),"batch frame acquired");vb.drawArrays(0,0,3);fb.readPixels(200,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,left);ctx.endFrame(false);
            check(left[0]==255 && left[1]==0,"batched uploads execute before the next frame draws");
            for(int i=0;i<3;++i){upload(i&1);}{U32 transient;textures.generate(1,&transient);textures.bind(glc::TEXTURE_2D,transient);textures.image2D(glc::TEXTURE_2D,0,glc::RGBA8,64,64,glc::RGBA,glc::UNSIGNED_BYTE,nullptr);textures.destroy(1,&transient);textures.bind(glc::TEXTURE_2D,texture);}
            check(ctx.beginFrame(clear),"frame after destroy of a texture created in the batch");ctx.endFrame(false);VK_CHECK(vkDeviceWaitIdle(ctx.device));ctx.collectReleasedResources();
            textures.destroy((U32)registry.size(),registry.data());
            U32 attachments[2];textures.generate(2,attachments);
            textures.bind(glc::TEXTURE_2D,attachments[0]);textures.image2D(glc::TEXTURE_2D,0,glc::RGBA8,8,8,glc::RGBA,glc::UNSIGNED_BYTE,nullptr);
            textures.bind(glc::TEXTURE_2D,attachments[1]);textures.image2D(glc::TEXTURE_2D,0,0x8CAC,8,8,glc::DEPTH_COMPONENT,glc::FLOAT,nullptr);
            auto target=fb.createFramebuffer();fb.bind(target);fb.attach(target,0x8CE0,glc::TEXTURE_2D,attachments[0],0);fb.attach(target,0x8D00,glc::TEXTURE_2D,attachments[1],0);fb.bind(0);
            check(ctx.beginFrame(clear),"locality frame acquired");fb.bind(target);state.setDepthMask(true);fb.setClearColor(1,0,0,1);fb.setClearDepth(.25);fb.clear(0x4100);
            textures.bind(glc::TEXTURE_2D,texture);upload(true);textures.generateMipmaps(glc::TEXTURE_2D);textures.readImage(glc::TEXTURE_2D,0,glc::RGBA,glc::UNSIGNED_BYTE,read.data());
            fb.readPixels(4,4,1,1,glc::RGBA,glc::UNSIGNED_BYTE,left);float depth=0;fb.readPixels(4,4,1,1,glc::DEPTH_COMPONENT,glc::FLOAT,&depth);
            check(left[0]==255 && left[1]==0 && depth==.25f,"other texture upload, mip generation and readback preserve framebuffer attachments");
            fb.bind(0);ctx.endFrame(false);fb.destroyFramebuffer(target);textures.destroy(2,attachments);
            vb.free(BufferKind::Vertex,96,vertices,shadow);textures.destroy(1,&texture);shaders.use(0);shaders.deleteProgramAndObjects(program);
        }
        ctx.shutdown();check(!ctx.validationErrors && !ctx.validationWarnings,"zero synchronization validation messages including teardown");printf("UPLOAD_CHECKS=%u PASS\n",checks);return 0;
    }catch(const std::exception& e){fprintf(stderr,"upload ring test: %s\n",e.what());return 1;}
}
