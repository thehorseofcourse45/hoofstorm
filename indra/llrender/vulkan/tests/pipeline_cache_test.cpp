#include "vkcontext.h"
#include "vkbackends.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "vkframebufferbackend.h"
#include "glconsts.h"
#include <cstring>

int wmain(int argc,wchar_t** argv)
{
    if (argc<2) return 1;
    const bool validation=argc>2 && !wcscmp(argv[2],L"--validate");
    const bool readOnly=argc>2 && !wcscmp(argv[2],L"--readonly");
    VkContext ctx;
    try
    {
        if(!ctx.init(GetModuleHandle(nullptr),nullptr,validation) || (validation && !ctx.validation)) return 1;
        const int bytes=WideCharToMultiByte(CP_UTF8,0,argv[1],-1,nullptr,0,nullptr,nullptr);
        std::string file(bytes,'\0');
        WideCharToMultiByte(CP_UTF8,0,argv[1],-1,file.data(),bytes,nullptr,nullptr);file.pop_back();
        ctx.configurePipelineCache(file,!readOnly);
        {
            VulkanRenderStateBackend state;
            VulkanVertexBufferBackend vb(&ctx,&state);
            VulkanShaderBackend shaders(&ctx);
            VulkanTextureBackend textures(&ctx);
            VulkanFramebufferBackend fb(&ctx,&state);
            const char* source[]={"#version 330\nin vec4 position;void main(){gl_Position=position;}",
                "#version 330\nout vec4 color;void main(){color=vec4(1,0,0,1);}"};
            U32 program=shaders.createProgram();
            for(int i=0;i<2;++i)
            {
                auto object=shaders.createObject(i?0x8B30:0x8B31);std::string log;
                if(!shaders.compileObject(object,1,&source[i],log)) throw std::runtime_error(log);
                shaders.attachObject(program,object);
            }
            shaders.bindAttribLocation(program,0,"position");
            if(!shaders.link(program)) throw std::runtime_error(shaders.getProgramLog(program));
            shaders.use(program);
            BufferHandle vertices;U8* shadow=nullptr;vb.allocate(BufferKind::Vertex,48,vertices,shadow);
            const float triangle[]={-1,-1,0,1,3,-1,0,1,-1,3,0,1};U32 offsets[14]{};
            vb.upload(BufferKind::Vertex,vertices,0,47,triangle);vb.setBuffers(vertices,{},1,offsets);
            state.setViewport(0,0,800,600);state.setCapability(0x0B44,false);
            const float clear[]={0,0,0,1};ctx.beginFrame(clear);
            for(int i=0;i<32;++i)
            {
                state.setDepthTest((i&1)!=0);state.setDepthFunc(0x0200+((i>>1)&7));state.setDepthMask((i&16)!=0);
                vb.drawArrays(0,0,3);
            }
            state.setDepthTest(false);vb.drawArrays(0,0,3);
            uint8_t pixel[4]{};fb.readPixels(400,300,1,1,glc::RGBA,glc::UNSIGNED_BYTE,pixel);
            if(pixel[0]!=255 || pixel[1]!=0 || pixel[2]!=0) throw std::runtime_error("cached pipeline pixel mismatch");
            ctx.endFrame(false);
            vb.free(BufferKind::Vertex,48,vertices,shadow);shaders.deleteProgramAndObjects(program);
        }
        if (argc>2 && !wcscmp(argv[2],L"--toggle"))
        {
            const auto file=ctx.pipelineCacheFile;
            ctx.configurePipelineCache("",false);
            if (ctx.savePipelineCache()) throw std::runtime_error("disabled cache was written");
            ctx.configurePipelineCache(file,true);
            if (!ctx.pipelineCacheLoadedBytes) throw std::runtime_error("re-enabled cache was not loaded");
        }
        const bool saved=ctx.savePipelineCache();
        printf("CACHE loaded_bytes=%zu pipelines=%llu hits=%llu build_ms=%.4f saved=%u pixels=PASS\n",
            ctx.pipelineCacheLoadedBytes,(unsigned long long)ctx.pipelinesCreated,(unsigned long long)ctx.pipelineCacheHits,ctx.pipelineCreationMs,saved);
        ctx.shutdown();
        printf("validation: %u errors, %u warnings\n",ctx.validationErrors,ctx.validationWarnings);
        return ctx.validationErrors || ctx.validationWarnings ? 1 : 0;
    }
    catch(const std::exception& e) {fprintf(stderr,"Cache test: %s\n",e.what());return 1;}
}
