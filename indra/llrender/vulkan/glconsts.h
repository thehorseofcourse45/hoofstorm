// OpenGL constants the Vulkan texture backend needs. The viewer's backend interfaces use GL values as their vocabulary, and the Windows
// <GL/gl.h> stops at GL 1.1, so the later ones are listed here (values from the Khronos registry).
#pragma once

#include <cstdint>

namespace glc
{
// targets
constexpr uint32_t TEXTURE_2D = 0x0DE1, TEXTURE_3D = 0x806F, TEXTURE_RECTANGLE = 0x84F5, TEXTURE_CUBE_MAP = 0x8513,
                   TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515, TEXTURE_CUBE_MAP_NEGATIVE_Z = 0x851A, TEXTURE_CUBE_MAP_ARRAY = 0x9009;
// wrap
constexpr uint32_t REPEAT = 0x2901, CLAMP_TO_EDGE = 0x812F, MIRRORED_REPEAT = 0x8370, CLAMP_TO_BORDER = 0x812D;
// filters
constexpr uint32_t NEAREST = 0x2600, LINEAR = 0x2601, NEAREST_MIPMAP_NEAREST = 0x2700, LINEAR_MIPMAP_NEAREST = 0x2701,
                   NEAREST_MIPMAP_LINEAR = 0x2702, LINEAR_MIPMAP_LINEAR = 0x2703;
// comparison functions
constexpr uint32_t NEVER = 0x0200;      // NEVER .. ALWAYS = 0x200 .. 0x207, same order as VkCompareOp
// pixel formats
constexpr uint32_t RED = 0x1903, GREEN = 0x1904, BLUE = 0x1905, ALPHA = 0x1906, RGB = 0x1907, RGBA = 0x1908, LUMINANCE = 0x1909,
                   LUMINANCE_ALPHA = 0x190A, RG = 0x8227, BGR = 0x80E0, BGRA = 0x80E1, DEPTH_COMPONENT = 0x1902;
constexpr uint32_t ZERO = 0, ONE = 1;
// pixel types
constexpr uint32_t BYTE = 0x1400, UNSIGNED_BYTE = 0x1401, SHORT = 0x1402, UNSIGNED_SHORT = 0x1403, INT = 0x1404, UNSIGNED_INT = 0x1405,
                   FLOAT = 0x1406, HALF_FLOAT = 0x140B;
// sized internal formats
constexpr uint32_t R8 = 0x8229, RG8 = 0x822B, RGB8 = 0x8051, RGBA8 = 0x8058, RGB10_A2 = 0x8059, RGBA16 = 0x805B, SRGB = 0x8C40, SRGB8 = 0x8C41, SRGB_ALPHA = 0x8C42,
                   SRGB8_ALPHA8 = 0x8C43, R16F = 0x822D, RG16F = 0x822F, RGB16F = 0x881B, RGBA16F = 0x881A, R32F = 0x822E, RG32F = 0x8230,
                   RGB32F = 0x8815, RGBA32F = 0x8814, R11F_G11F_B10F = 0x8C3A, DEPTH_COMPONENT16 = 0x81A5, DEPTH_COMPONENT24 = 0x81A6,
                   DEPTH_COMPONENT32F = 0x8CAC, ALPHA8 = 0x803C, LUMINANCE8 = 0x8040, LUMINANCE8_ALPHA8 = 0x8045;
// driver-side compression requests (the viewer's RenderCompressTextures): stored uncompressed under Vulkan
constexpr uint32_t COMPRESSED_ALPHA = 0x84E9, COMPRESSED_LUMINANCE = 0x84EA, COMPRESSED_LUMINANCE_ALPHA = 0x84EB, COMPRESSED_RGB = 0x84ED,
                   COMPRESSED_RGBA = 0x84EE, COMPRESSED_RED = 0x8225, COMPRESSED_RG = 0x8226, COMPRESSED_SRGB = 0x8C48,
                   COMPRESSED_SRGB_ALPHA = 0x8C49;
// block compressed formats (S3TC)
constexpr uint32_t COMPRESSED_RGB_S3TC_DXT1 = 0x83F0, COMPRESSED_RGBA_S3TC_DXT1 = 0x83F1, COMPRESSED_RGBA_S3TC_DXT3 = 0x83F2,
                   COMPRESSED_RGBA_S3TC_DXT5 = 0x83F3;
// level queries
constexpr uint32_t TEXTURE_WIDTH = 0x1000, TEXTURE_HEIGHT = 0x1001, TEXTURE_COMPRESSED = 0x86A1, TEXTURE_COMPRESSED_IMAGE_SIZE = 0x86A0;
} // namespace glc
