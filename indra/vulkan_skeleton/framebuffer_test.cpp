// GPU regression checks for framebuffer binding, layouts, clears, reads, copies and sampling.
#include "vkframebufferbackend.h"
#include "vkbackends.h"
#include "vkshaderbackend.h"
#include "vktexturebackend.h"
#include "glconsts.h"
#include <cmath>
#include <cstring>

bool test_framebuffers(VkContext& ctx, VulkanRenderStateBackend& st, VulkanVertexBufferBackend& vb,
                       VulkanShaderBackend& shaders, VulkanTextureBackend& tex, VulkanFramebufferBackend& fb)
{
    constexpr U32 C0 = 0x8CE0, D = 0x8D00, COLOR = 0x4000, DEPTH = 0x100;
    bool passed = true;
    U32 checks = 0;
    auto expect = [&](const char* name, bool ok) {
        printf("  [%s] framebuffer: %s\n", ok ? "PASS" : "FAIL", name);
        passed &= ok;
        ++checks;
    };
    auto program = [&](const char* fragment) {
        const char* vertex = "#version 330\nin vec4 position;\nin vec2 texcoord0;\nout vec2 uv;\nvoid main(){gl_Position=position;uv=texcoord0;}";
        U32 id = shaders.createProgram();
        for (int i = 0; i < 2; ++i)
        {
            U32 object = shaders.createObject(i ? 0x8B30 : 0x8B31);
            const char* src = i ? fragment : vertex;
            std::string log;
            if (!shaders.compileObject(object, 1, &src, log)) { fprintf(stderr, "%s\n", log.c_str()); abort(); }
            shaders.attachObject(id, object);
        }
        shaders.bindAttribLocation(id, 0, "position");
        shaders.bindAttribLocation(id, 2, "texcoord0");
        if (!shaders.link(id)) { fprintf(stderr, "%s\n", shaders.getProgramLog(id).c_str()); abort(); }
        return id;
    };
    U32 mrt = program("#version 330\nin vec2 uv;\nlayout(location=0) out vec4 out0;\nlayout(location=1) out vec4 out1;\n"
                       "void main(){out0=uv.y<0.5?vec4(1,0,0,1):vec4(0,0,1,1);out1=vec4(0,1,0,1);}");
    U32 sample = program("#version 330\nin vec2 uv;\nuniform sampler2D diffuseMap;\nout vec4 out0;\n"
                          "void main(){out0=texture(diffuseMap,uv);}");
    shaders.uniform1i(sample, shaders.getUniformLocation(sample, "diffuseMap"), 0);
    U32 shadow = program("#version 330\nin vec2 uv;\nuniform sampler2DShadow shadowMap;\nout vec4 out0;\n"
                          "void main(){float c=texture(shadowMap,vec3(uv,uv.x<0.5?0.25:0.75));out0=vec4(c,c,c,1);}");
    shaders.uniform1i(shadow, shaders.getUniformLocation(shadow, "shadowMap"), 0);

    U32 images[5];
    tex.generate(5, images);
    for (int i = 0; i < 5; ++i)
    {
        U32 target = i == 4 ? glc::TEXTURE_CUBE_MAP_ARRAY : glc::TEXTURE_2D;
        tex.bind(target, images[i]);
        tex.setFilter(target, glc::NEAREST, glc::NEAREST);
        tex.setWrap(target, glc::CLAMP_TO_EDGE, glc::CLAMP_TO_EDGE, -1);
        if (i == 4) tex.image3D(target, 0, glc::RGBA8, 64, 64, 6, glc::RGBA, glc::UNSIGNED_BYTE, nullptr);
        else tex.image2D(target, 0, i == 2 ? glc::DEPTH_COMPONENT24 : glc::RGBA8, 64, 64,
                          i == 2 ? glc::DEPTH_COMPONENT : glc::RGBA, glc::UNSIGNED_BYTE, nullptr);
    }
    U32 a = fb.createFramebuffer(), b = fb.createFramebuffer(), empty = fb.createFramebuffer();
    fb.bind(empty);
    expect("missing attachment status", fb.checkStatus() == 0x8CD7);
    fb.bind(a);
    fb.attach(a, C0, glc::TEXTURE_2D, images[0], 0);
    fb.attach(a, C0 + 1, glc::TEXTURE_2D, images[1], 0);
    fb.attach(a, D, glc::TEXTURE_2D, images[2], 0);
    fb.setDrawBuffers(2);
    expect("MRT with shared-size depth is complete", fb.checkStatus() == 0x8CD5);
    fb.bind(b);
    fb.attach(b, C0, glc::TEXTURE_2D, images[3], 0);
    fb.bind(0);

    BufferHandle vertices;
    U8* cpu;
    vb.allocate(BufferKind::Vertex, 96, vertices, cpu);
    const float positions[4][4] = {{-1,-1,0,1},{1,-1,0,1},{1,1,0,1},{-1,1,0,1}};
    const float uv[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    memcpy(cpu, positions, 64);
    memcpy(cpu + 64, uv, 32);
    vb.upload(BufferKind::Vertex, vertices, 0, 95, cpu);
    U32 offsets[14]{};
    offsets[2] = 64;
    auto geometry = [&] { vb.setBuffers(vertices, {}, (1u << 0) | (1u << 2), offsets); };
    auto pixel = [&](int x, int y, int r, int g, int blue) {
        U8 data[4]{};
        fb.readPixels(x, y, 1, 1, glc::RGBA, glc::UNSIGNED_BYTE, data);
        return std::abs((int) data[0] - r) <= 2 && std::abs((int) data[1] - g) <= 2 && std::abs((int) data[2] - blue) <= 2;
    };
    printf("scene: framebuffers\n");
    float frameClear[4] = {0.1f, 0.2f, 0.3f, 1};
    if (!ctx.beginFrame(frameClear)) return false;
    st.setCapability(0x0B44, true); // GL_CULL_FACE
    st.setCullFace(0x0405);
    st.setCapability(0x0BE2, false); // blend
    st.setDepthTest(true);
    st.setDepthFunc(glc::NEVER + 1);
    st.setDepthMask(true);
    st.setColorMask(true, true, true, true);
    st.setCapability(0x0C11, false); // scissor
    st.setViewport(0, 0, 64, 64);
    fb.bind(a);
    fb.setClearColor(0,0,0,1);
    fb.clear(COLOR | DEPTH);
    shaders.use(mrt);
    geometry();
    vb.drawArrays(0, 0, 3);
    expect("triangle rendered into offscreen color", pixel(48, 16, 255, 0, 0));
    expect("outside offscreen triangle remains clear", pixel(8, 48, 0, 0, 0));
    st.setDepthFunc(glc::NEVER + 3); // LEQUAL, second triangle includes shared edges
    geometry();
    vb.drawArrays(2, 0, 4); // triangle fan, completes the quad
    expect("offscreen GL bottom row is red", pixel(32, 16, 255, 0, 0));
    expect("offscreen GL top row is blue", pixel(32, 48, 0, 0, 255));
    fb.setReadBuffer(C0 + 1);
    expect("second draw attachment receives green output", pixel(32, 32, 0, 255, 0));
    fb.setReadBuffer(C0);
    float depth = 0;
    fb.readPixels(32, 32, 1, 1, glc::DEPTH_COMPONENT, glc::FLOAT, &depth);
    expect("depth attachment contains transformed GL depth", std::abs(depth - 0.5f) < 0.001f);
    U32 integerDepth = 0;
    fb.readPixels(32,32,1,1,glc::DEPTH_COMPONENT,glc::UNSIGNED_INT,&integerDepth);
    expect("snapshot integer depth conversion", integerDepth >= 2147483500u && integerDepth <= 2147483800u);

    st.setCapability(0x0C11, true);
    st.setScissor(8,8,8,8);
    fb.setClearColor(1,1,0,1);
    fb.clear(COLOR);
    expect("mid-pass scissor clear changes inside", pixel(10,10,255,255,0));
    expect("mid-pass scissor clear preserves outside", pixel(20,10,255,0,0));
    st.setScissor(-4,-4,8,8);
    fb.setClearColor(1,0,1,1);
    fb.clear(COLOR);
    expect("negative scissor clips to attachment bounds", pixel(1,1,255,0,255));
    st.setScissor(100,100,8,8);
    fb.clear(COLOR);
    expect("empty scissor is a no-op", pixel(32,16,255,0,0));
    st.setCapability(0x0C11, false);

    fb.bind(b);
    fb.setClearColor(0,1,1,1);
    fb.clear(COLOR);
    expect("nested framebuffer has independent color", pixel(32,32,0,255,255));
    fb.bind(a);
    expect("restored framebuffer retains color", pixel(32,16,255,0,0));
    tex.bind(glc::TEXTURE_2D, images[3]);
    tex.copyFromFramebuffer2D(glc::TEXTURE_2D, 0, 0, 0, 0, 0, 64, 64);
    fb.bindRead(b);
    expect("copyFromFramebuffer2D bottom orientation", pixel(32,16,255,0,0));
    expect("copyFromFramebuffer2D top orientation", pixel(32,48,0,0,255));
    expect("read binding leaves draw framebuffer unchanged", fb.getBound() == a);
    fb.bindRead(a);
    tex.bind(glc::TEXTURE_CUBE_MAP_ARRAY, images[4]);
    tex.copyFromFramebuffer3D(glc::TEXTURE_CUBE_MAP_ARRAY, 0, 0, 0, 3, 0, 0, 64, 64);
    tex.bind(glc::TEXTURE_2D, images[3]);
    tex.copyImageFromFramebuffer2D(glc::TEXTURE_2D, 0, glc::RGBA8, 0, 0, 64, 64);
    fb.bindRead(b);
    expect("copyImageFromFramebuffer2D defines copied contents", pixel(32,48,0,0,255));

    fb.bind(0);
    st.setViewport(0,0,ctx.extent.width,ctx.extent.height);
    st.setDepthTest(false);
    tex.bind(glc::TEXTURE_2D, images[0]);
    shaders.use(sample);
    geometry();
    vb.drawArrays(2,0,4);
    expect("offscreen sampled in next pass: bottom stays red", pixel(400,150,255,0,0));
    expect("offscreen sampled in next pass: top stays blue", pixel(400,450,0,0,255));
    tex.bind(glc::TEXTURE_2D, images[3]);
    tex.copyFromFramebuffer2D(glc::TEXTURE_2D, 0, 0, 0, 300, 400, 64, 64);
    fb.bindRead(b);
    expect("default framebuffer copy converts BGRA to RGBA", pixel(32,32,0,0,255));
    fb.bindRead(0);
    tex.bind(glc::TEXTURE_2D, images[2]);
    tex.setCompare(glc::TEXTURE_2D, true, glc::NEVER + 3);
    shaders.use(shadow);
    geometry();
    vb.drawArrays(2,0,4);
    expect("shadow sampler: reference below stored depth passes", pixel(200,300,255,255,255));
    expect("shadow sampler: reference above stored depth fails", pixel(600,300,0,0,0));
    ctx.endFrame(false);
    VK_CHECK(vkWaitForFences(ctx.device,1,&ctx.slot().fence,VK_TRUE,UINT64_MAX));

    std::vector<U8> array(6 * 64 * 64 * 4);
    tex.bind(glc::TEXTURE_CUBE_MAP_ARRAY, images[4]);
    tex.readImage(glc::TEXTURE_CUBE_MAP_ARRAY,0,glc::RGBA,glc::UNSIGNED_BYTE,array.data());
    size_t copied = ((size_t) 3 * 64 * 64 + 16 * 64 + 32) * 4;
    expect("copyFromFramebuffer3D selects requested array layer", array[copied] == 255 && array[copied+2] == 0);
    expect("copyFromFramebuffer3D preserves other layers", array[0] == 0 && array[2] == 0);
    const U32 formats[] = {glc::RGBA8,glc::RGBA16F,glc::RGB16F,glc::RG16F,glc::R16F,glc::R8,glc::R11F_G11F_B10F};
    U32 formatImages[7];
    tex.generate(7,formatImages);
    for (int i=0;i<7;++i)
    {
        tex.bind(glc::TEXTURE_2D,formatImages[i]);
        tex.image2D(glc::TEXTURE_2D,0,formats[i],64,64,glc::RGBA,glc::UNSIGNED_BYTE,nullptr);
    }
    if (!ctx.beginFrame(frameClear)) return false;
    fb.setClearColor(0,1,0,1);
    fb.clear(COLOR);
    expect("first explicit window clear uses caller color",pixel(400,300,0,255,0));
    float windowDepth=0;
    fb.readPixels(400,300,1,1,glc::DEPTH_COMPONENT,glc::FLOAT,&windowDepth);
    expect("window depth readback keeps initial clear",windowDepth==1.0f);
    for (int i=0;i<7;++i)
    {
        fb.bind(b);
        fb.attach(b,C0,glc::TEXTURE_2D,formatImages[i],0);
        fb.setClearColor(0.25f,0.5f,0.75f,1);
        fb.clear(COLOR);
        float rgba[4]{};
        fb.readPixels(32,32,1,1,glc::RGBA,glc::FLOAT,rgba);
        float green=(i==4||i==5)?0.0f:0.5f;
        float blue=(i==3||i==4||i==5)?0.0f:0.75f;
        expect(("clear/read framebuffer format " + std::to_string(formats[i])).c_str(),
               std::abs(rgba[0]-0.25f)<0.005f && std::abs(rgba[1]-green)<0.005f && std::abs(rgba[2]-blue)<0.005f);
    }
    fb.bind(a);
    fb.attach(a,C0,glc::TEXTURE_2D,0,0);
    fb.attach(a,C0+1,glc::TEXTURE_2D,0,0);
    fb.setDrawBufferNone();
    fb.setReadBuffer(0);
    expect("depth-only framebuffer is complete",fb.checkStatus()==0x8CD5);
    fb.setClearDepth(0.25);
    fb.clear(DEPTH);
    fb.readPixels(32,32,1,1,glc::DEPTH_COMPONENT,glc::FLOAT,&depth);
    expect("depth-only clear/read",depth==0.25f);
    fb.bind(b);
    fb.attach(b,D,glc::TEXTURE_2D,images[2],0);
    fb.setClearDepth(0.75);
    fb.clear(DEPTH);
    fb.bind(a);
    fb.readPixels(32,32,1,1,glc::DEPTH_COMPONENT,glc::FLOAT,&depth);
    expect("shared depth attachment retains writes across bindings",depth==0.75f);
    fb.bind(0);
    ctx.endFrame(false);
    VK_CHECK(vkWaitForFences(ctx.device,1,&ctx.slot().fence,VK_TRUE,UINT64_MAX));
    if (!ctx.beginFrame(frameClear)) return false;
    fb.bind(a);
    fb.clear(DEPTH);
    fb.readPixels(32,32,1,1,glc::DEPTH_COMPONENT,glc::FLOAT,&depth);
    expect("clear-depth state persists across frames",depth==0.75f);
    fb.bind(0);
    ctx.endFrame(false);
    VK_CHECK(vkWaitForFences(ctx.device,1,&ctx.slot().fence,VK_TRUE,UINT64_MAX));
    fb.destroyFramebuffer(a); fb.destroyFramebuffer(b); fb.destroyFramebuffer(empty);
    tex.destroy(7,formatImages);
    tex.destroy(5,images);
    vb.free(BufferKind::Vertex,96,vertices,cpu);
    shaders.use(0);
    shaders.deleteProgramAndObjects(mrt); shaders.deleteProgramAndObjects(sample); shaders.deleteProgramAndObjects(shadow);
    printf("framebuffer checks: %u, result %s\n",checks,passed?"PASS":"FAIL");
    return passed;
}
