#include "linden_common.h"
#include "llgpubenchmark.h"
#include "llrenderbackend.h"
#include "llgl.h"
#include "llwindow.h"
#include "llwindowcallbacks.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llglslshader.h"
#include "llshadermgr.h"
#include "llshaderbackend.h"
#include "llframebufferbackend.h"
#include "llerrorcontrol.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cmath>

int main(int argc,char** argv)
{
    const bool useGL=argc>1 && !strcmp(argv[1],"--gl");
    gDebugGL=!(argc>1 && !strcmp(argv[1],"--no-validation"));
    LLError::initForApplication("","",true);
    unsigned checks=0;bool passed=true;
    auto check=[&](const char* label,bool ok){++checks;passed &= ok;printf("[%s] calibration: %s\n",ok?"PASS":"FAIL",label);};
    LLWindowCallbacks callbacks;
    LLWindow* window=LLWindowManager::createWindow(&callbacks,"Firestorm calibration test","FirestormCalibrationTest",100,100,800,600,useGL?0:LL_WINDOW_VULKAN);
    if(!window) return 1;
    window->show();window->gatherInput();
    check("requested API active",isVulkanRenderBackendActive()!=useGL);
    LLVertexBuffer::initClass(window);check("render initialized",gGL.init(true));
    LLGLSLShader shader;shader.mName="Benchmark Shader";
    shader.mUniform.assign(LLShaderMgr::END_RESERVED_UNIFORMS,-1);shader.mAttributeMask=LLVertexBuffer::MAP_VERTEX;
    auto& be=getShaderBackend();shader.mProgramObject=be.createProgram();
    const char* files[]={"benchmarkV.glsl","benchmarkF.glsl"};
    for(int stage=0;stage<2;++stage)
    {
        std::ifstream in(std::string("C:/phoenix-firestorm/indra/newview/app_settings/shaders/class1/interface/")+files[stage]);
        std::string text="#version 330\n"+std::string(std::istreambuf_iterator<char>(in),{});const char* source=text.c_str();
        auto object=be.createObject(stage?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER);std::string log;
        check("real benchmark stage compiles",be.compileObject(object,1,&source,log));be.attachObject(shader.mProgramObject,object);
    }
    be.bindAttribLocation(shader.mProgramObject,0,"position");check("real benchmark program links",be.link(shader.mProgramObject));
    shader.bind();
    be.uniform1i(shader.mProgramObject,be.getUniformLocation(shader.mProgramObject,"diffuseMap"),0);
    shader.unbind();
    LLGLSLShader::sCanProfile=false;
    check("disabled profiling uses hardware fallback",measureGpuBandwidth(shader)<0 && (useGL || !isRenderBackendFrameActive()));
    LLGLSLShader::sCanProfile=true;
    LLGLSLShader::sProfileEnabled=true;
    check("full profiling is not interrupted",measureGpuBandwidth(shader)<0 && LLGLSLShader::sProfileEnabled);
    LLGLSLShader::sProfileEnabled=false;
    const auto started=std::chrono::steady_clock::now();
    const float gbps=measureGpuBandwidth(shader);
    check("startup calibration measures finite positive bandwidth",std::isfinite(gbps) && gbps>0);
    check("startup calibration completes its owned frame and profiling",(useGL || !isRenderBackendFrameActive()) && !LLGLSLShader::sProfileEnabled);
    check("expected workload and GPU elapsed time recorded",shader.mSamplesDrawn==U64(1024)*1024*32*64 && shader.mTimeElapsed>0);
    printf("CALIBRATION backend=%s gbps=%.4f wall_ms=%.3f samples=%llu gpu_ns=%llu\n",useGL?"gl":"vulkan",gbps,
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count(),
        (unsigned long long)shader.mSamplesDrawn,(unsigned long long)shader.mTimeElapsed);
    const float clear[4]={0,0,0,1};
    if(!useGL)
    {
        check("existing frame acquired",beginRenderBackendFrame(clear));
        check("in-session calibration borrows the frame",measureGpuBandwidth(shader)>0 && isRenderBackendFrameActive());
        window->swapBuffers();
        ShowWindow((HWND)window->getPlatformWindow(),SW_MINIMIZE);window->gatherInput();
        check("minimized calibration defers",measureGpuBandwidth(shader)<0 && !isRenderBackendFrameActive());
        ShowWindow((HWND)window->getPlatformWindow(),SW_RESTORE);window->gatherInput();
        check("frame acquisition recovers after calibration",beginRenderBackendFrame(clear));
    }
    getFramebufferBackend().setClearColor(0,1,0,1);getFramebufferBackend().clear(GL_COLOR_BUFFER_BIT);
    U8 pixel[4]{};getFramebufferBackend().readPixels(400,300,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    check("normal rendering works after calibration cleanup",pixel[0]==0 && pixel[1]==255 && pixel[2]==0);
    window->swapBuffers();
    LLGLSLShader invalid;
    invalid.mUniform.assign(LLShaderMgr::END_RESERVED_UNIFORMS,-1);invalid.mAttributeMask=LLVertexBuffer::MAP_VERTEX;
    invalid.mProgramObject=be.createProgram();
    const char* broken[]={"#version 330\nin vec4 position;out vec2 tc0;void main(){gl_Position=position;tc0=position.xy*.5+.5;}",
        "#version 330\nin vec2 tc0;\nout vec4 frag_color;\nuniform sampler2D diffuseMap;\nvoid main(){frag_color=vec4(1.0)-texture(diffuseMap,tc0);}"};
    for(int stage=0;stage<2;++stage)
    {
        auto object=be.createObject(stage?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER);std::string log;
        check("fault fixture compiles",be.compileObject(object,1,&broken[stage],log));be.attachObject(invalid.mProgramObject,object);
    }
    be.bindAttribLocation(invalid.mProgramObject,0,"position");check("fault fixture links",be.link(invalid.mProgramObject));
    check("incorrect benchmark pixels reject calibration",measureGpuBandwidth(invalid)<0 && !LLGLSLShader::sProfileEnabled);
    check("failed calibration releases its owned frame",useGL || !isRenderBackendFrameActive());
    invalid.unload();
    LLGLSLShader::unbind();shader.unload();gGL.shutdown();LLVertexBuffer::cleanupClass();LLWindowManager::destroyWindow(window);
    std::uint32_t errors,warnings;getRenderBackendValidationCounts(errors,warnings);
    check("no Vulkan validation errors or warnings",!errors && !warnings);
    printf("calibration: %u checks, %s\n",checks,passed?"PASS":"FAIL");return passed?0:1;
}
