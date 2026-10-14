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

add_executable(llvulkan_frames_test EXCLUDE_FROM_ALL tests/frames_test.cpp)
target_include_directories(llvulkan_frames_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../../llcommon")
target_link_libraries(llvulkan_frames_test PRIVATE llvulkan)
