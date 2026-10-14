# Shared shader compiler and package

The shared llvulkan static library contains the translator, compiler, VulkanShaderBackend and VkProgram. Both viewer
and standalone tests use it. The viewer-built llvulkan_shader_test target exercises that exact archive without starting
the viewer. Release compilation retains the viewer toolchain's strict checks. A standalone Debug test exercises the Debug libs.

The compiler dependency is an isolated autobuild archive made from pinned Vulkan SDK 1.4.357.0 static glslang 16.4.0 and
SPIRV-Tools libraries. third_party/glslang/README.md contains package/install instructions. LL_GLSLANG_ROOT selects the
installed root; Glslang.cmake verifies all payload file hashes and imports Release/Debug configurations. Compiler headers
and binary paths come from that package, with private include/define propagation through llvulkan. The Vulkan API SDK
is still needed for Vulkan headers/import library and skeleton triangle glslc generation. A package-only translator proof
clears VULKAN_SDK and removes SDK PATH entries while building and validating all programs.

The package includes full upstream notices, and viewer_manifest copies them to LICENSES/glslang. It includes no runtime
compiler DLL dependency. SDK Debug static libraries lack compiler PDBs; compiler-internal debug info is unavailable and
LNK4099 is reported rather than suppressed. There is no published archive URL, and the root autobuild.xml was not changed
to a nonportable local URL. A future release publisher can attach a hosted archive/hash to the isolated config and then
register that installable in the main manifest. The recipe repacks an installed SDK; it does not build Khronos sources.

VkProgram has vertex/fragment and optional geometry modules. The backend keeps fixed V/F/G module order, checks geometry
feature availability, and pipeline creation includes the geometry stage only when present. Device feature queries enable
supported geometry, clip/cull-distance and geometry-point-size features. Geometry-uniform visibility uses existing ALL_GRAPHICS
descriptor stage flags. Partial program creation is guarded so failures release modules/layouts/descriptors after the fence.

Cache schema 2 begins with magic, schema and a toolchain identity string. It then stores attribute/reflection/layout data
and two or three module word vectors. The identity includes shader targets, translator/packing revision, glslang semantic
version, SPIRV-Tools version/commit details and the compiler package's payload fingerprint. The viewer shader hash also
includes this salt; GL's default empty salt preserves the old vendor/renderer/version hash inputs and old GL cache behavior.
The shader cache enable check can accept a non-GL binary backend without requiring GL's reported version.

Loading is transactional: size/count/length limits, contiguous sampler bindings, canonical std140 offsets/size, SPIR-V
validation and execution models are checked before driver resource creation. Rejected blobs preserve any linked/bound program.
Successful loads replace it and update the current pointer. The cache limits are 64 MiB per blob, 4096 loose uniforms,
14 attribute names and 32 texture units; the actual uniform block must fit the device maxUniformBufferRange. Sampler arrays
remain unsupported by the runtime and rejected. These tests check malformed local cache files, not adversarially rewritten
valid SPIR-V plus forged reflection; cross-reflecting every descriptor layout remains future hardening/integration work.

GPU checks cover compiled/cached geometry transforms and uniforms, incompatible schema/compiler/translator/package identities,
malformed/truncated payloads, reflection sizes/offsets, unknown format, feature rejection and bound-program replacement.
The corpus now tests all 210 captured programs, including four geometry programs, and all 424 stages. Corpus linking/reflection
is not pixel comparison of every program or verification of all set-2 uniform-block streaming and rendering passes.

The viewer is still guarded to OpenGL. Installing a full Vulkan backend set and replacing GL startup, capability queries,
worker contexts, direct GL/UBO callers and frame/render paths is the next integration work. Keep the guard until that is done.
