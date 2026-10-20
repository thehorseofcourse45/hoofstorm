# Shared Vulkan runtime, compiler and shader backend for viewer and GPU tests.
find_package(Vulkan REQUIRED)
add_library(llvulkan STATIC
    vkpipelinecache.cpp vkcontext.cpp vkbackends.cpp vkprogram.cpp vktexturebackend.cpp vkframebufferbackend.cpp vkquerybackend.cpp
    translator/llshadertranslator.cpp translator/llshadercompiler.cpp vkshaderbackend.cpp)
target_compile_features(llvulkan PUBLIC cxx_std_17)
target_include_directories(llvulkan PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/translator")
target_include_directories(llvulkan PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan PUBLIC Vulkan::Vulkan)
target_compile_definitions(llvulkan PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(llvulkan PRIVATE /EHsc)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/third_party/glslang/Glslang.cmake")
target_link_libraries(llvulkan PRIVATE ll::glslang)
# Stage notices independently of compiler binaries for viewer_manifest packaging.
file(COPY "${LL_GLSLANG_ROOT}/LICENSES/glslang" DESTINATION "${CMAKE_BINARY_DIR}/shader-compiler-licenses")

# AMD FidelityFX SDK 1.1.4 (FSR 3.1 upscaler + Vulkan backend), vendored under ffx/; see ffx/README.
# Headers are public as <FidelityFX/...>. AMD's offline shader compiler is replaced by ffx_blobgen, which compiles the GLSL
# to SPIR-V with the glslang above and writes the blob table the backend asks for.
set(FFX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ffx")
add_executable(ffx_blobgen ffx/tools/ffx_blobgen.cpp)
target_compile_features(ffx_blobgen PRIVATE cxx_std_17)
target_link_libraries(ffx_blobgen PRIVATE ll::glslang)
file(GLOB_RECURSE FFX_SHADER_INPUTS CONFIGURE_DEPENDS "${FFX_DIR}/src/backends/vk/shaders/fsr3upscaler/*.glsl" "${FFX_DIR}/include/FidelityFX/gpu/*.h")
set(FFX_BLOBS "${CMAKE_CURRENT_BINARY_DIR}/ffx_fsr3upscaler_blobs.cpp")
add_custom_command(OUTPUT "${FFX_BLOBS}"
    COMMAND ffx_blobgen "${FFX_DIR}" "${FFX_BLOBS}"
    DEPENDS ffx_blobgen ${FFX_SHADER_INPUTS}
    COMMENT "FidelityFX: compiling FSR 3.1 upscaler shaders to SPIR-V")
set(FFX_SOURCES
    ffx/src/backends/vk/ffx_vk.cpp
    ffx/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
    ffx/src/shared/ffx_assert.cpp ffx/src/shared/ffx_breadcrumbs_list.cpp ffx/src/shared/ffx_message.cpp ffx/src/shared/ffx_object_management.cpp)
target_sources(llvulkan PRIVATE ${FFX_SOURCES} "${FFX_BLOBS}" vkfsr2.cpp)
target_include_directories(llvulkan PUBLIC "${FFX_DIR}/include")
target_include_directories(llvulkan PRIVATE
    "${FFX_DIR}/src/shared" "${FFX_DIR}/src/backends/shared" "${FFX_DIR}/src/components" "${FFX_DIR}/src/backends/vk")
set_source_files_properties(${FFX_SOURCES} "${FFX_BLOBS}" PROPERTIES COMPILE_DEFINITIONS NOMINMAX)

add_executable(llvulkan_fsr_shader_test EXCLUDE_FROM_ALL tests/fsr_shader_test.cpp)
target_link_libraries(llvulkan_fsr_shader_test PRIVATE llvulkan)

# This target uses the exact llvulkan archive built for the viewer; it never launches or logs into the viewer.
add_executable(llvulkan_shader_test EXCLUDE_FROM_ALL tests/shader_runtime_test.cpp tests/shader_test.cpp)
target_include_directories(llvulkan_shader_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_shader_test PRIVATE llvulkan)

# Repeatable headless frame workloads and pixel guards; never starts the viewer.
add_executable(llvulkan_frame_pacing_test EXCLUDE_FROM_ALL tests/frame_pacing_test.cpp)
target_include_directories(llvulkan_frame_pacing_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_frame_pacing_test PRIVATE llvulkan)

# Native query lifetime, visibility, statistics and timestamps on the real GPU.
add_executable(llvulkan_query_test EXCLUDE_FROM_ALL tests/query_test.cpp)
target_include_directories(llvulkan_query_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_query_test PRIVATE llvulkan)

# Real driver cache round-trip, compatibility rejection and cold/warm pipeline timings.
add_executable(llvulkan_pipeline_cache_test EXCLUDE_FROM_ALL tests/pipeline_cache_test.cpp)
target_include_directories(llvulkan_pipeline_cache_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_pipeline_cache_test PRIVATE llvulkan)

add_executable(llvulkan_rgb10_test EXCLUDE_FROM_ALL tests/rgb10_test.cpp)
target_include_directories(llvulkan_rgb10_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_rgb10_test PRIVATE llvulkan)

add_executable(llvulkan_buffer_pool_test EXCLUDE_FROM_ALL tests/buffer_pool_test.cpp)
target_include_directories(llvulkan_buffer_pool_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_buffer_pool_test PRIVATE llvulkan)

add_executable(llvulkan_upload_ring_test EXCLUDE_FROM_ALL tests/upload_ring_test.cpp)
target_include_directories(llvulkan_upload_ring_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_upload_ring_test PRIVATE llvulkan)

add_executable(llvulkan_fsr_pass_test EXCLUDE_FROM_ALL tests/fsr_pass_test.cpp)
target_include_directories(llvulkan_fsr_pass_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_fsr_pass_test PRIVATE llvulkan)

add_executable(llvulkan_frames_test EXCLUDE_FROM_ALL tests/frames_test.cpp)
target_include_directories(llvulkan_frames_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_frames_test PRIVATE llvulkan)

# FidelityFX FSR 3.1 upscaler context create/destroy on a validated headless device.
add_executable(llvulkan_fsr2_context_test EXCLUDE_FROM_ALL tests/fsr2_context_test.cpp)
target_include_directories(llvulkan_fsr2_context_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_fsr2_context_test PRIVATE llvulkan)

# VulkanFsrUpscaler: synthetic jittered dispatches with output readback on a validated headless device.
add_executable(llvulkan_fsr2_test EXCLUDE_FROM_ALL tests/fsr2_test.cpp)
target_include_directories(llvulkan_fsr2_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_fsr2_test PRIVATE llvulkan)
