# -*- cmake -*-

project(llrender)

include(00-Common)
include(OpenGL)
include(FreeType)
include(LLCommon)
include(LLImage)
include(LLWindow)

set(llrender_SOURCE_FILES
    llcubemap.cpp
    llcubemaparray.cpp
    llfontbitmapcache.cpp
    llfontfreetype.cpp
    llfontfreetypesvg.cpp
    llfontgl.cpp
    llfontvertexbuffer.cpp
    llfontregistry.cpp
    llgpubenchmark.cpp
    llgl.cpp
    llglslshader.cpp
    llgltexture.cpp
    llimagegl.cpp
    llpostprocess.cpp
    llrender.cpp
    llrenderbackend.cpp
    llrenderbackend_vulkan.cpp
    llrenderbackendselection.cpp
    llrenderstatebackend_gl.cpp
    llshaderbackend_gl.cpp
    lltexturebackend_gl.cpp
    llframebufferbackend_gl.cpp
    llrender2dutils.cpp
    llrendernavprim.cpp
    llrendersphere.cpp
    llrendertarget.cpp
    llshadermgr.cpp
    lltexture.cpp
    lltexturemanagerbridge.cpp
    lluiimage.cpp
    llvertexbuffer.cpp
    llvertexbuffer_gl.cpp
    llglcommonfunc.cpp
    )
    
set(llrender_HEADER_FILES
    CMakeLists.txt

    llcubemap.h
    llcubemaparray.h
    llfontgl.h
    llfontvertexbuffer.h
    llfontfreetype.h
    llfontfreetypesvg.h
    llfontbitmapcache.h
    llfontregistry.h
    llgpubenchmark.h
    llgl.h
    llglheaders.h
    llglslshader.h
    llglstates.h
    llgltexture.h
    llgltypes.h
    llimagegl.h
    llpostprocess.h
    llrender.h
    llrenderbackendselection.h
    llrenderbackend.h
    llrenderstatebackend.h
    llshaderbackend.h
    lltexturebackend.h
    llframebufferbackend.h
    llrender2dutils.h
    llrendernavprim.h
    llrendersphere.h
    llshadermgr.h
    lltexture.h
    lltexturemanagerbridge.h
    lluiimage.h
    lluiimage.inl
    llvertexbuffer.h
    llvertexbuffertypes.h
    llvertexbufferbackend.h
    llglcommonfunc.h
    )

list(APPEND llrender_SOURCE_FILES ${llrender_HEADER_FILES})

if (BUILD_HEADLESS)
  add_library (llrenderheadless
    ${llrender_SOURCE_FILES}
    )

  set_property(TARGET llrenderheadless
    PROPERTY COMPILE_DEFINITIONS LL_MESA=1 LL_MESA_HEADLESS=1
      )

  target_link_libraries(llrenderheadless
          llcommon
          llimage
          llmath
          llrender
          llxml
          llfilesystem
          )
endif (BUILD_HEADLESS)

add_library (llrender ${llrender_SOURCE_FILES})
target_include_directories( llrender  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})

set_property(TARGET llrender PROPERTY POSITION_INDEPENDENT_CODE ON)

# Libraries on which this library depends, needed for Linux builds
# Sort by high-level to low-level
target_link_libraries(llrender
        llcommon
        llimage
        llmath
        llfilesystem
        llxml
        llwindow
        ll::freetype
        OpenGL::GL
        OpenGL::GLU
        )


# Building the runtime does not make the incomplete Vulkan viewer path eligible for selection.
option(LL_VULKAN_RUNTIME "Build the experimental Windows Vulkan runtime" OFF)
if(LL_VULKAN_RUNTIME)
    if(NOT WINDOWS)
        message(FATAL_ERROR "The current Vulkan window runtime requires Windows")
    endif()
    add_subdirectory(vulkan)
    target_compile_definitions(llrender PRIVATE LL_VULKAN_RUNTIME_AVAILABLE=1)
    target_link_libraries(llrender llvulkan)
endif()

if(WINDOWS AND LL_VULKAN_RUNTIME)
    add_executable(viewer_vulkan_activation_test EXCLUDE_FROM_ALL tests/native_activation/native_activation_test.cpp)
    target_link_libraries(viewer_vulkan_activation_test llrender llwindow)
    add_executable(viewer_gpu_calibration_test EXCLUDE_FROM_ALL tests/native_activation/calibration_test.cpp)
    target_link_libraries(viewer_gpu_calibration_test llrender llwindow)
endif()
