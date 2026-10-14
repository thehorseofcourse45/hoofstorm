# Local glslang autobuild package

This recipe packages the Windows x64 Release and Debug **static** compiler libraries and headers from Vulkan SDK
1.4.357.0 (glslang 16.4.0), with pinned Khronos glslang, SPIRV-Tools and SPIRV-Headers license notices.
No shader compiler DLL or SDK installation is needed by the resulting viewer executable.

Run package_glslang.py --sdk <VulkanSDK/1.4.357.0> --output <archive.tar.gz> with Python. Then install the archive:

    autobuild install --config-file <this-directory/autobuild.xml> --install-dir <package-root> --local <archive.tar.gz> glslang

Pass -DLL_GLSLANG_ROOT=<package-root> to viewer/skeleton/translator CMake configurations. The package has Release and Debug
libraries under lib/release and lib/debug; CMake selects the correct configuration, validates installed file hashes, and
uses the package fingerprint in the SPIR-V cache identity. The Vulkan API SDK/loader dependency is separate.

The isolated autobuild configuration deliberately has no hosted archive URL. A publisher can attach the archive URL and
hash to this installable after publishing it, then add it to the main autobuild.xml. No archive was published and no fake
remote URL or machine-specific file URL was added to the main build manifest. Local autobuild installation is supported
and tested. This is an SDK-repack recipe, not a build-from-source recipe; the pinned SDK is required to recreate the package.

Upstream license sources (SDK tags):
- https://github.com/KhronosGroup/glslang/blob/vulkan-sdk-1.4.357.0/LICENSE.txt
- https://github.com/KhronosGroup/SPIRV-Tools/blob/vulkan-sdk-1.4.357.0/LICENSE
- https://github.com/KhronosGroup/SPIRV-Headers/blob/vulkan-sdk-1.4.357.0/LICENSE
