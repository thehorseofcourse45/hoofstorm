#include "vkcontext.h"
#include "vkbackends.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include "glconsts.h"
#include <cmath>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <array>

int main(int argc, char** argv)
{
    VkContext ctx;unsigned checks=0;ctx.tolerateFeedback=getenv("FS_VK_TOLERATE_FEEDBACK")!=nullptr;
    auto check=[&](bool ok,const char* label){if(!ok) throw std::runtime_error(label);++checks;printf("PASS %s\n",label);};
    try
    {
        if(!ctx.init(GetModuleHandle(nullptr),nullptr,true) || !ctx.validation) return 1;
        {
            VulkanRenderStateBackend state;VulkanVertexBufferBackend vb(&ctx,&state);
            VulkanShaderBackend shaders(&ctx);VulkanTextureBackend textures(&ctx);VulkanFramebufferBackend fb(&ctx,&state);
            U32 names[2];textures.generate(2,names);textures.activateUnit(0);textures.bind(glc::TEXTURE_2D,names[0]);
            const float input[]={.125f,.501f,.875f,2.f/3.f};float actual[4]{};
            textures.image2D(glc::TEXTURE_2D,0,0x8059u,1,1,glc::RGBA,glc::FLOAT,input);
            textures.readImage(glc::TEXTURE_2D,0,glc::RGBA,glc::FLOAT,actual);
            for(int i=0;i<4;++i)check(std::abs(actual[i]-input[i])<=(i<3?.0005f:.00001f),"float upload preserves ten-bit color and two-bit alpha");
            const uint8_t bytes[]={255,0,128,85};textures.subImage2D(glc::TEXTURE_2D,0,0,0,1,1,glc::RGBA,glc::UNSIGNED_BYTE,bytes);
            uint8_t returned[4]{};textures.readImage(glc::TEXTURE_2D,0,glc::RGBA,glc::UNSIGNED_BYTE,returned);
            check(!memcmp(bytes,returned,4),"byte upload and readback preserve channels");
            const float outside[]={-1,2,.25f,2};textures.subImage2D(glc::TEXTURE_2D,0,0,0,1,1,glc::RGBA,glc::FLOAT,outside);
            textures.readImage(glc::TEXTURE_2D,0,glc::RGBA,glc::FLOAT,actual);
            check(actual[0]==0 && actual[1]==1 && actual[3]==1,"normalized upload clamps component bounds");
            textures.bind(glc::TEXTURE_2D,names[1]);textures.image2D(glc::TEXTURE_2D,0,0x8059u,8,8,glc::RGBA,glc::UNSIGNED_BYTE,nullptr);
            auto target=fb.createFramebuffer();fb.bind(target);fb.attach(target,0x8CE0,glc::TEXTURE_2D,names[1],0);
            check(fb.checkStatus()==0x8CD5,"non-HDR normal render target is complete");
            fb.bind(0);const float clear[]={0,0,0,1};ctx.beginFrame(clear);fb.bind(target);fb.setClearColor(.125f,.501f,.875f,2.f/3.f);fb.clear(0x4000);
            fb.readPixels(4,4,1,1,glc::RGBA,glc::FLOAT,actual);
            check(std::abs(actual[0]-.125f)<.0005f && std::abs(actual[1]-.501f)<.0005f && std::abs(actual[2]-.875f)<.0005f && std::abs(actual[3]-2.f/3.f)<.00001f,"GPU clear/readback retains packed normal-buffer precision");
            U32 program=shaders.createProgram();const char* source[]={"#version 330\nin vec4 position;void main(){gl_Position=position;}",
                "#version 330\nuniform sampler2D diffuseMap;\nout vec4 color;void main(){color=texture(diffuseMap,vec2(.5));}"};
            for(int i=0;i<2;++i){auto object=shaders.createObject(i?0x8B30:0x8B31);std::string log;check(shaders.compileObject(object,1,&source[i],log),"sample stage accepted");shaders.attachObject(program,object);}
            shaders.bindAttribLocation(program,0,"position");check(shaders.link(program),"normal buffer sampler links");shaders.use(program);
            shaders.uniform1i(program,shaders.getUniformLocation(program,"diffuseMap"),0);textures.bind(glc::TEXTURE_2D,names[1]);textures.setFilter(glc::TEXTURE_2D,glc::NEAREST,glc::NEAREST);
            BufferHandle vertices;U8* shadow=nullptr;vb.allocate(BufferKind::Vertex,48,vertices,shadow);const float triangle[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};U32 offsets[14]{};
            vb.upload(BufferKind::Vertex,vertices,0,47,triangle);vb.setBuffers(vertices,{},1,offsets);state.setDepthTest(false);state.setCapability(0x0B44,false);
            fb.bind(0);state.setViewport(0,0,800,600);vb.drawArrays(0,0,3);fb.readPixels(400,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,returned);
            check(returned[0]==32 && returned[1]==128 && returned[2]==223 && returned[3]==170,"shader samples normalized RGB10_A2 channels");ctx.endFrame(false);
            textures.bind(glc::TEXTURE_2D,names[0]);textures.image2D(glc::TEXTURE_2D,0,0x805Bu,1,1,glc::RGBA,glc::FLOAT,input);
            textures.readImage(glc::TEXTURE_2D,0,glc::RGBA,glc::FLOAT,actual);
            for(int i=0;i<4;++i)check(std::abs(actual[i]-input[i])<.000008f,"HDR normal buffer upload retains sixteen-bit normalized precision");
            textures.bind(glc::TEXTURE_2D,names[1]);textures.image2D(glc::TEXTURE_2D,0,0x805Bu,8,8,glc::RGBA,glc::UNSIGNED_BYTE,nullptr);
            ctx.beginFrame(clear);fb.bind(target);fb.setClearColor(.125f,.501f,.875f,2.f/3.f);fb.clear(0x4000);
            fb.readPixels(4,4,1,1,glc::RGBA,glc::FLOAT,actual);
            check(std::abs(actual[0]-.125f)<.000008f && std::abs(actual[1]-.501f)<.000008f && std::abs(actual[2]-.875f)<.000008f && std::abs(actual[3]-2.f/3.f)<.000008f,"HDR normal buffer GPU clear/readback retains sixteen-bit precision");
            fb.bind(0);vb.drawArrays(0,0,3);fb.readPixels(400,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,returned);
            check(returned[0]==32 && returned[1]==128 && returned[2]==223 && returned[3]==170,"shader samples HDR normalized normal-buffer channels");ctx.endFrame(false);
            U32 sparse=shaders.createProgram();
            const char* fragment=argc>1 && !strcmp(argv[1],"--active-feedback") ?
                "#version 330\nuniform sampler2D inactiveTarget;\nuniform sampler2D sourceMap;\nout vec4 color;void main(){color=texture(sourceMap,vec2(.5))+texture(inactiveTarget,vec2(.5))*.001;}" :
                "#version 330\nuniform sampler2D inactiveTarget;\nuniform sampler2D sourceMap;\nout vec4 color;vec4 unusedHelper(){return texture(inactiveTarget,vec2(.5));}void main(){color=texture(sourceMap,vec2(.5));}";
            for(int i=0;i<2;++i){auto object=shaders.createObject(i?0x8B30:0x8B31);const char* text=i?fragment:source[0];std::string log;check(shaders.compileObject(object,1,&text,log),"sparse sampler stage accepted");shaders.attachObject(sparse,object);}
            shaders.bindAttribLocation(sparse,0,"position");check(shaders.link(sparse),"sparse sampler program links");
            auto render=[&](U32 p){
                shaders.use(p);shaders.uniform1i(p,shaders.getUniformLocation(p,"sourceMap"),0);
                shaders.uniform1i(p,shaders.getUniformLocation(p,"inactiveTarget"),1);
                textures.activateUnit(0);textures.bind(glc::TEXTURE_2D,names[0]);
                textures.activateUnit(1);textures.bind(glc::TEXTURE_2D,names[1]);
                ctx.beginFrame(clear);fb.bind(target);state.setViewport(0,0,8,8);vb.drawArrays(0,0,3);
                fb.readPixels(4,4,1,1,glc::RGBA,glc::UNSIGNED_BYTE,returned);
                check(returned[0]==32 && returned[1]==128 && returned[2]==223 && returned[3]==170,"unused attachment sampler cannot cause feedback or change pixels");
                fb.bind(0);ctx.endFrame(false);
            };
            render(sparse);
            check(shaders.getUniformLocation(sparse,"inactiveTarget")==-1 && shaders.getUniformLocation(sparse,"sourceMap")>=0,"reflection exposes only the active sampler");
            std::vector<U8> binary;U32 format=0;check(shaders.getProgramBinary(sparse,binary,format),"sparse sampler cache serializes");
            U32 cached=shaders.createProgram();check(shaders.loadProgramBinary(cached,format,binary.data(),(S32)binary.size()),"sparse sampler cache reloads");render(cached);
            shaders.use(0);shaders.deleteProgramAndObjects(sparse);shaders.deleteProgramAndObjects(cached);
            U32 depthTex;textures.generate(1,&depthTex);textures.activateUnit(0);textures.bind(glc::TEXTURE_2D,depthTex);
            textures.image2D(glc::TEXTURE_2D,0,0x8CAC,8,8,glc::DEPTH_COMPONENT,glc::FLOAT,nullptr);
            fb.bind(target);fb.attach(target,0x8D00,glc::TEXTURE_2D,depthTex,0);fb.bind(0);
            U32 depthProgram=shaders.createProgram();const char* depthSource="#version 330\nuniform sampler2D depthMap;\nout vec4 color;void main(){color=vec4(texture(depthMap,vec2(.5)).r);}";
            for(int i=0;i<2;++i){auto object=shaders.createObject(i?0x8B30:0x8B31);const char* text=i?depthSource:source[0];std::string log;check(shaders.compileObject(object,1,&text,log),"depth sampling stage accepted");shaders.attachObject(depthProgram,object);}
            shaders.bindAttribLocation(depthProgram,0,"position");check(shaders.link(depthProgram),"depth sampling program links");
            shaders.use(depthProgram);shaders.uniform1i(depthProgram,shaders.getUniformLocation(depthProgram,"depthMap"),0);
            ctx.beginFrame(clear);fb.bind(target);state.setDepthTest(true);state.setDepthMask(true);state.setDepthFunc(0x0207);
            fb.setClearDepth(.25);fb.clear(0x0100);state.setDepthMask(argc>1 && !strcmp(argv[1],"--active-depth-feedback"));
            vb.drawArrays(0,0,3);vb.drawArrays(0,0,3);fb.readPixels(4,4,1,1,glc::RGBA,glc::UNSIGNED_BYTE,returned);
            check(returned[0]==64 && returned[1]==64,"shared read-only depth sampling preserves pixels across repeated draws");
            float depth=0;fb.readPixels(4,4,1,1,glc::DEPTH_COMPONENT,glc::FLOAT,&depth);check(depth==.25f,"read-only depth test preserves the attachment");
            fb.bind(0);state.setViewport(0,0,800,600);vb.drawArrays(0,0,3);fb.readPixels(400,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,returned);
            check(returned[0]==64,"same depth descriptor samples outside its attachment");
            fb.bind(target);state.setViewport(0,0,8,8);vb.drawArrays(0,0,3);fb.readPixels(4,4,1,1,glc::RGBA,glc::UNSIGNED_BYTE,returned);
            check(returned[0]==64,"cached descriptor follows shared-depth layout changes in the same frame");
            // A non-sampling draw must be able to resume depth writes on the same framebuffer.
            shaders.use(program);textures.bind(glc::TEXTURE_2D,names[0]);state.setDepthMask(true);vb.drawArrays(0,0,3);
            fb.readPixels(4,4,1,1,glc::DEPTH_COMPONENT,glc::FLOAT,&depth);check(std::abs(depth-.5f)<.0001f,"ordinary draw resumes depth writes after sampling");
            shaders.use(depthProgram);textures.bind(glc::TEXTURE_2D,depthTex);state.setDepthMask(false);vb.drawArrays(0,0,3);
            state.setDepthMask(true);fb.setClearDepth(.75);fb.clear(0x0100);state.setDepthMask(false);vb.drawArrays(0,0,3);
            fb.readPixels(4,4,1,1,glc::RGBA,glc::UNSIGNED_BYTE,returned);check(returned[0]==191,"depth clear resumes writable layout then samples updated depth");
            fb.bind(0);ctx.endFrame(false);shaders.use(0);shaders.deleteProgramAndObjects(depthProgram);
            textures.destroy(1,&depthTex);
            // Descriptor work should scale with bound samplers, not the number of textures in a region.
            ctx.beginFrame(clear);textures.activateUnit(0);textures.bind(glc::TEXTURE_2D,names[0]);
            auto measure=[&](const char* type){
                VkDescriptorImageInfo info{};for(int i=0;i<100;++i) textures.unitDescriptor(0,type,info);
                std::array<double,5> times{};
                for(auto& ms:times){auto start=std::chrono::steady_clock::now();for(int i=0;i<10000;++i) textures.unitDescriptor(0,type,info);ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
                std::sort(times.begin(),times.end());return times[2];
            };
            const double small=measure("sampler2D"),smallFallback=measure("samplerCube");
            std::vector<U32> registry(10000);textures.generate((U32)registry.size(),registry.data());
            textures.bind(glc::TEXTURE_2D,registry.back());const U8 white[4]={255,255,255,255};textures.image2D(glc::TEXTURE_2D,0,glc::RGBA8,1,1,glc::RGBA,glc::UNSIGNED_BYTE,white);
            const double large=measure("sampler2D"),largeFallback=measure("samplerCube");
            printf("LOOKUP_MS small=%.6f large=%.6f fallback_small=%.6f fallback_large=%.6f calls=10000 registry=10000 trials=5\n",small,large,smallFallback,largeFallback);
            VkDescriptorImageInfo descriptor{};textures.unitDescriptor(0,"sampler2D",descriptor);check(descriptor.imageView!=VK_NULL_HANDLE && descriptor.imageLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"large registry preserves sampled image descriptor");
            textures.unitDescriptor(0,"samplerCube",descriptor);check(descriptor.imageView!=VK_NULL_HANDLE && descriptor.imageLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"large registry preserves mismatched-type fallback");
            check(large < small*5 && largeFallback < smallFallback*5,"descriptor lookup cost does not grow linearly with the texture registry");
            textures.setFilter(glc::TEXTURE_2D,glc::NEAREST,glc::NEAREST);textures.unitDescriptor(0,"sampler2D",descriptor);auto nearest=descriptor.sampler;
            textures.setFilter(glc::TEXTURE_2D,glc::LINEAR,glc::LINEAR);textures.unitDescriptor(0,"sampler2D",descriptor);check(descriptor.sampler!=nearest,"sampler cache distinguishes filter changes");
            textures.setFilter(glc::TEXTURE_2D,glc::NEAREST,glc::NEAREST);textures.unitDescriptor(0,"sampler2D",descriptor);check(descriptor.sampler==nearest,"sampler cache reuses restored state");

            ctx.endFrame(false);textures.destroy((U32)registry.size(),registry.data());
            shaders.deleteProgramAndObjects(program);vb.free(BufferKind::Vertex,48,vertices,shadow);fb.destroyFramebuffer(target);textures.destroy(2,names);
        }
        ctx.shutdown();check(!ctx.validationErrors && !ctx.validationWarnings,"zero Vulkan validation messages");printf("RGB10_CHECKS=%u PASS\n",checks);return 0;
    }
    catch(const std::exception& e){fprintf(stderr,"RGB10 test: %s\n",e.what());return 1;}
}
