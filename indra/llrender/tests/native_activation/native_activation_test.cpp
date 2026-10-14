#include "linden_common.h"
#include "llrenderbackend.h"
#include "llgl.h"
#include "llwindow.h"
#include "llwindowcallbacks.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llglslshader.h"
#include "llshadermgr.h"
#include "llshaderbackend.h"
#include "lltexturebackend.h"
#include "llframebufferbackend.h"
#include "llrenderstatebackend.h"
#include "llerrorcontrol.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include "llthread.h"

static int checks = 0;
static bool passed = true;
static void expect(const char* label, bool ok)
{
    ++checks; passed &= ok;
    printf("[%s] native activation: %s\n", ok ? "PASS" : "FAIL", label);
}

int main(int argc, char** argv)
{
    _putenv_s("FS_VK_ASYNC_PIPELINES", "0");   // these checks read the pixels of the first draw; async builds skip it (frames_test covers async)
    const bool fallback = argc > 1 && !strcmp(argv[1], "--fallback");
    const bool useGL = fallback || (argc > 1 && !strcmp(argv[1], "--gl"));
    LLError::initForApplication("", "", true);
    std::string error;
    expect("invalid native handles rejected without activating", !activateVulkanRenderBackend(nullptr,nullptr,true,error) &&
        !isVulkanRenderBackendActive() && !error.empty());
    if (fallback) getShaderBackend(); // a published GL object must prevent a mixed renderer
    // Normal viewer sessions avoid validation overhead; integration checks explicitly keep it enabled.
    gDebugGL = !useGL && !(argc > 1 && !strcmp(argv[1],"--no-validation"));
    LLWindowCallbacks callbacks;
    LLWindow* window = LLWindowManager::createWindow(&callbacks,"Firestorm native renderer test","FirestormNativeRendererTest",
        100,100,800,600,useGL&&!fallback?0:LL_WINDOW_VULKAN);
    expect("viewer native window created",window != nullptr);
    if (!window) return 1;
    window->show(); window->gatherInput();
    expect("active API matches explicit window request",isVulkanRenderBackendActive()!=useGL);
    expect("WGL context matches GL-only path",(wglGetCurrentContext()!=nullptr)==useGL);
    if(useGL)
    {
        expect("late activation rejected without changing the GL context",
            !activateVulkanRenderBackend(GetModuleHandle(nullptr),window->getPlatformWindow(),true,error) &&
            !isVulkanRenderBackendActive() && wglGetCurrentContext()!=nullptr);
        getFramebufferBackend().setClearColor(1,0,0,1);
        getFramebufferBackend().clear(GL_COLOR_BUFFER_BIT);
        U8 pixel[4]{};
        getFramebufferBackend().readPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        expect("GL fallback clear/readback works",pixel[0]==255 && pixel[1]==0 && pixel[2]==0);
        U32 query=0;generateRenderQueries(1,&query);
        beginRenderQuery(GL_ANY_SAMPLES_PASSED,query);endRenderQuery(GL_ANY_SAMPLES_PASSED);
        U32 result=1;getRenderQuery32(query,GL_QUERY_RESULT,&result);
        expect("GL query bridge preserves empty-query results",result==0);deleteRenderQueries(1,&query);
        getFramebufferBackend().setClearColor(0,1,0,1);
        generateRenderQueries(1,&query);beginRenderQuery(GL_TIME_ELAPSED,query);
        getFramebufferBackend().clear(GL_COLOR_BUFFER_BIT);endRenderQuery(GL_TIME_ELAPSED);
        unsigned long long elapsed=0;getRenderQuery64(query,GL_QUERY_RESULT,&elapsed);
        expect("GL query bridge preserves 64-bit timer results",elapsed>0);deleteRenderQueries(1,&query);
        window->swapBuffers();
    }
    else
    {
        expect("all four singleton backends belong to the native set",
            &getRenderStateBackend()==activeVulkanRenderStateBackend() && &getShaderBackend()==activeVulkanShaderBackend() &&
            &getTextureBackend()==activeVulkanTextureBackend() && &getFramebufferBackend()==activeVulkanFramebufferBackend());
        if (const char* cache=getenv("FS_VULKAN_TEST_CACHE")) configureRenderPipelineCache(cache,true);
        expect("activation is idempotent for the owning window",
            activateVulkanRenderBackend(GetModuleHandle(nullptr),window->getPlatformWindow(),true,error));
        LLWindow* second = LLWindowManager::createWindow(&callbacks,"Rejected second graphics window",
            "FirestormSecondGraphicsTest",100,100,320,240);
        expect("another graphics window cannot create a mixed GL session",second==nullptr && isVulkanRenderBackendActive());
        if (second) LLWindowManager::destroyWindow(second);
        expect("native capability limits are initialized before viewer shaders",gGLManager.mInited &&
            gGLManager.mMaxUniformBlockSize>=16384 && gGLManager.mNumTextureImageUnits>=16 &&
            gGLManager.mGLVersionString.find("Vulkan ")==0 && !gGLManager.mHasDebugOutput);
        std::uint32_t blocks[4];
        const float white[4]={1,1,1,1};
        for(unsigned i=0;i<4;++i) { blocks[i]=createRenderUniformBuffer(); uploadRenderUniformBuffer(blocks[i],white,sizeof white); bindRenderUniformBuffer(i,blocks[i]); }
        LLVertexBuffer::initClass(window);
        expect("LLRender initializes with no WGL context",gGL.init(true) && wglGetCurrentContext()==nullptr);
        LLGLSLShader shader;
        shader.mUniform.assign(LLShaderMgr::END_RESERVED_UNIFORMS,-1);
        shader.mAttributeMask=LLVertexBuffer::MAP_VERTEX|LLVertexBuffer::MAP_COLOR|LLVertexBuffer::MAP_TEXCOORD0;
        auto& be=getShaderBackend();
        shader.mProgramObject=be.createProgram();
        const char* sources[]={
            "#version 330\nin vec4 position;\nin vec4 diffuse_color;\nin vec2 texcoord0;\nout vec4 vertex_color;\n"
            "void main(){gl_Position=position;vertex_color=diffuse_color*(texcoord0.x>=0?1.0:0.5);}",
            "#version 330\nin vec4 vertex_color;\nout vec4 color;\n"
            "layout(std140) uniform ReflectionProbes {vec4 probeColor;};\n"
            "layout(std140) uniform GLTFJoints {vec4 jointsColor;};\n"
            "layout(std140) uniform GLTFNodes {vec4 nodesColor;};\n"
            "layout(std140) uniform GLTFMaterials {vec4 materialsColor;};\n"
            "void main(){color=vertex_color*probeColor*jointsColor*nodesColor*materialsColor;}"
        };
        for(int i=0;i<2;++i)
        {
            U32 object=be.createObject(i?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER);
            std::string log;
            expect("shader source stored",be.compileObject(object,1,&sources[i],log));
            be.attachObject(shader.mProgramObject,object);
        }
        expect("native shader links",be.link(shader.mProgramObject));
        if (!be.validate(shader.mProgramObject)) { fprintf(stderr,"%s\n",be.getProgramLog(shader.mProgramObject).c_str());return 1; }
        shader.bind();
        auto draw=[&](float x,bool green){
            gGL.begin(LLRender::TRIANGLES);
            gGL.color4f(green?0.f:1.f,green?1.f:0.f,0,1);
            gGL.texCoord2f(0,0);
            gGL.vertex3f(x-0.2f,-0.3f,0.5f);gGL.vertex3f(x+0.2f,-0.3f,0.5f);gGL.vertex3f(x,0.3f,0.5f);
            gGL.end();gGL.flush();
        };
        const float queryClear[4]={0,0,0,1};
        expect("native query capabilities published",renderQuerySupported(GL_TIME_ELAPSED) && renderQuerySupported(GL_ANY_SAMPLES_PASSED));
        for (int startup=0;startup<3;++startup)
        {
            expect("repeated startup clear/present acquires its own frame",beginRenderBackendFrame(queryClear));
            getFramebufferBackend().clear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            window->swapBuffers();
            expect("startup presentation leaves the frame inactive",!isRenderBackendFrameActive());
        }
        expect("profiling frame acquired",beginRenderBackendFrame(queryClear));
        getRenderStateBackend().setViewport(0,0,800,600);getRenderStateBackend().setDepthTest(false);
        shader.clearStats();shader.placeProfileQuery(true);draw(-0.5f,false);
        expect("real shader runtime profile stays asynchronous",!shader.readProfileQuery(true,false));
        window->swapBuffers();waitRenderBackendIdle();
        expect("real shader receives GPU nanoseconds",shader.readProfileQuery(true,false) && shader.mTimeElapsed>0);
        U32 query=0;generateRenderQueries(1,&query);
        expect("visibility frame acquired",beginRenderBackendFrame(queryClear));
        beginRenderQuery(GL_ANY_SAMPLES_PASSED,query);draw(-0.5f,false);endRenderQuery(GL_ANY_SAMPLES_PASSED);
        window->swapBuffers();U32 result=0;getRenderQuery32(query,GL_QUERY_RESULT,&result);
        expect("native query bridge returns visible samples",result==1);deleteRenderQueries(1,&query);
        expect("full shader profiling frame acquired",beginRenderBackendFrame(queryClear));
        LLGLSLShader::initProfile();shader.clearStats();
        LLGLSLShader::unbind(); // enabling profiling while bound must not end a query that never started
        shader.bind();draw(-0.5f,false);window->swapBuffers();
        boost::json::value profileStats; LLGLSLShader::finishProfile(profileStats);
        LLGLSLShader::unbind();
        expect("real shader profiler receives timing samples and primitives",shader.mTimeElapsed>0 &&
            shader.mSamplesDrawn>0 && shader.mTrianglesDrawn==1 && shader.mBinds==1);
        shader.bind();
        auto frame=[&](){
            const float clear[4]={0,0,0,1};
            if(!beginRenderBackendFrame(clear))return false;
            LLCoordScreen size;window->getSize(&size);
            auto& st=getRenderStateBackend();st.setViewport(0,0,size.mX,size.mY);
            st.setCapability(GL_CULL_FACE,false);st.setCapability(GL_BLEND,false);st.setDepthTest(false);
            const float red[4]={1,0,0,1},green[4]={0,1,0,1};
            uploadRenderUniformBuffer(blocks[0],red,sizeof red); draw(-0.5f,false);
            LLVertexBuffer::drawArraysNoInput(LLRender::TRIANGLES,0,3);
            uploadRenderUniformBuffer(blocks[0],green,sizeof green); draw(0.5f,true);
            U8 left[4]{},right[4]{};
            auto& fb=getFramebufferBackend();
            deferRenderBackendPresentation(true);
            window->swapBuffers(); // viewer display finishes before snapshot callbacks read the same image
            fb.readPixels(size.mX/4,size.mY/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,left);
            fb.readPixels(3*size.mX/4,size.mY/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,right);
            deferRenderBackendPresentation(false);
            endRenderBackendFrame();
            return left[0]==255 && left[1]==0 && right[0]==0 && right[1]==255;
        };
        expect("four uniform bindings preserve pixels through deferred presentation and snapshot readback",frame());
        const auto handle=window->getPlatformWindow();
        expect("resize preserves native window and active backend",window->switchContext(false,LLCoordScreen(640,480),true) &&
            window->getPlatformWindow()==handle && isVulkanRenderBackendActive());
        window->gatherInput();
        expect("drawing and presentation survive resize",frame());
        window->minimize();window->gatherInput();
        const float clear[4]={0,0,0,1};
        expect("minimized native window skips acquisition and defers preview work",!beginRenderBackendFrame(clear) && !isRenderBackendFrameActive());
        window->restore();window->gatherInput();
        expect("restored native window draws",frame());
        uploadRenderUniformBuffer(blocks[0],white,sizeof white);
        deleteRenderUniformBuffer(0); // deleting GL name zero must preserve the native fallback buffer
        // More uploads than the ring can hold must submit safely and rebind every descriptor.
        expect("uniform stress frame acquired",beginRenderBackendFrame(clear));
        for(int i=0;i<300;++i) { uploadRenderUniformBuffer(blocks[0],white,sizeof white); draw(-0.5f,false); }
        U8 stress[4]{}; LLCoordScreen stressSize;window->getSize(&stressSize);
        getFramebufferBackend().readPixels(stressSize.mX/4,stressSize.mY/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,stress);
        expect("named uniform snapshots survive ring overflow",stress[0]==255 && stress[1]==0);
        window->swapBuffers();
        {
            LLPointer<LLVertexBuffer> stream = new LLVertexBuffer(shader.mAttributeMask);
            expect("real viewer streaming buffer allocated",stream->allocateBuffer(3,0));
            const float clear[4]={0,0,0,1};
            expect("streaming frame acquired",beginRenderBackendFrame(clear));
            auto uploadDraw=[&](float center,bool green) {
                LLStrider<LLVector3> vertices;
                LLStrider<LLColor4U> colors;
                LLStrider<LLVector2> uv;
                stream->getVertexStrider(vertices);
                stream->getColorStrider(colors);
                stream->getTexCoord0Strider(uv);
                vertices[0].set(center-0.2f,-0.3f,0.5f);
                vertices[1].set(center+0.2f,-0.3f,0.5f);
                vertices[2].set(center,0.3f,0.5f);
                for(int i=0;i<3;++i){colors[i]=LLColor4U(green?0:255,green?255:0,0,255);uv[i].set(0,0);}
                stream->unmapBuffer();
                stream->setBuffer();
                stream->drawArrays(LLRender::TRIANGLES,0,3);
            };
            uploadDraw(-0.5f,false);
            uploadDraw(0.5f,true);
            LLCoordScreen size;window->getSize(&size);
            U8 left[4]{},right[4]{};
            getFramebufferBackend().readPixels(size.mX/4,size.mY/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,left);
            getFramebufferBackend().readPixels(3*size.mX/4,size.mY/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,right);
            expect("same buffer update preserves previously recorded draw",
                left[0]==255 && left[1]==0 && right[0]==0 && right[1]==255);
            window->swapBuffers();
        }
        {
            LLPointer<LLVertexBuffer> indexed = new LLVertexBuffer(shader.mAttributeMask);
            expect("indexed streaming buffer allocated",indexed->allocateBuffer(6,3));
            LLStrider<LLVector3> vertices; LLStrider<LLColor4U> colors; LLStrider<LLVector2> uv;
            indexed->getVertexStrider(vertices);indexed->getColorStrider(colors);indexed->getTexCoord0Strider(uv);
            for(int side=0;side<2;++side)
            {
                const float center=side?0.5f:-0.5f;
                vertices[side*3].set(center-0.2f,-0.3f,0.5f);
                vertices[side*3+1].set(center+0.2f,-0.3f,0.5f);
                vertices[side*3+2].set(center,0.3f,0.5f);
                for(int i=0;i<3;++i){colors[side*3+i]=LLColor4U(side?0:255,side?255:0,0,255);uv[side*3+i].set(0,0);}
            }
            expect("indexed streaming frame acquired",beginRenderBackendFrame(clear));
            for(int side=0;side<2;++side)
            {
                LLStrider<U16> indices;indexed->getIndexStrider(indices);
                for(int i=0;i<3;++i) indices[i]=side*3+i;
                indexed->unmapBuffer();indexed->setBuffer();indexed->drawRange(LLRender::TRIANGLES,0,5,3,0);
            }
            U8 left[4]{},right[4]{};LLCoordScreen size;window->getSize(&size);
            getFramebufferBackend().readPixels(size.mX/4,size.mY/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,left);
            getFramebufferBackend().readPixels(3*size.mX/4,size.mY/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,right);
            expect("index overwrite preserves the previously recorded triangle",left[0]==255 && left[1]==0 && right[0]==0 && right[1]==255);
            window->swapBuffers();
        }
        expect("UI primitive frame acquired",beginRenderBackendFrame(clear));
        gGL.begin(LLRender::LINE_LOOP); gGL.color4f(1,0,0,1);
        gGL.vertex3f(-0.5f,-0.5f,0.5f); gGL.vertex3f(0.5f,-0.5f,0.5f);
        gGL.vertex3f(0.5f,0.5f,0.5f); gGL.vertex3f(-0.5f,0.5f,0.5f); gGL.end();gGL.flush();
        U8 closure[8]{};LLCoordScreen loopSize;window->getSize(&loopSize);
        getFramebufferBackend().readPixels(loopSize.mX/4-1,loopSize.mY/2,2,1,GL_RGBA,GL_UNSIGNED_BYTE,closure);
        expect("UI line loop includes its closing segment",closure[0]==255 || closure[4]==255);
        gGL.begin(LLRender::POINTS);gGL.vertex3f(0,0,0.5f);gGL.end();gGL.flush();
        window->swapBuffers();
        // Worker TLS renderers must not destroy the main renderer's cached Vulkan buffers.
        on_main_thread();
        const auto cacheBytes = LLVertexBuffer::getBytesAllocated();
        std::thread worker([] { gGL.shutdown(); });
        worker.join();
        expect("worker renderer exit preserves main-thread cached buffers",LLVertexBuffer::getBytesAllocated()==cacheBytes);
        expect("rendering survives worker renderer exit",frame());
        if(argc>1 && !strcmp(argv[1],"--pacing"))
        {
            int frames=0;
            for(;frames<300;++frames){if(!beginRenderBackendFrame(clear)) break;draw(-0.5f,false);window->swapBuffers();}
            expect("pacing diagnostics exercise 300 native frames",frames==300);
        }
        LLGLSLShader::unbind();shader.unload();
        for(auto buffer:blocks) deleteRenderUniformBuffer(buffer);
        gGL.shutdown();
        LLVertexBuffer::cleanupClass();
    }
    LLWindowManager::destroyWindow(window);
    expect("native renderer is torn down before its window",!isVulkanRenderBackendActive());
    std::uint32_t errors,warnings;getRenderBackendValidationCounts(errors,warnings);
    expect("no Vulkan validation errors or warnings",errors==0 && warnings==0);
    printf("native activation: %d checks, %s\n",checks,passed?"PASS":"FAIL");
    return passed?0:1;
}
