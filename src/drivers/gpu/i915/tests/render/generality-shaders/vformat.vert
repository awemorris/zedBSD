/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generality test's vertex format step, vertex stage:
 * attributes of the 8-, 16- and 10-bit formats libGLESv2 hands the
 * executor (normalized, scaled, integer, half float), the same at every
 * corner, turned back into the integers they were made of and passed on
 * as twelve words of a flat output array.  A missing component checks the
 * fetcher's default (0, and 1 or 1.0 for w).
 */
#version 450

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 unorm8;		/* R8G8B8A8_UNORM */
layout(location = 2) in vec2 sscaled16;		/* R16G16_SSCALED */
layout(location = 3) in ivec2 sint16;		/* R16G16_SINT */
layout(location = 4) in uvec4 uint8;		/* R8G8B8_UINT: w is the integer 1 */
layout(location = 5) in vec4 half4;		/* R16G16B16A16_SFLOAT */
layout(location = 6) in vec4 packed10;		/* A2B10G10R10_UNORM_PACK32 */
layout(location = 7) in vec4 snorm16;		/* R16_SNORM: y 0, z 0, w 1.0 */
layout(location = 8) in vec2 sscaled8;		/* R8G8_SSCALED */
layout(location = 9) in uvec4 uint16;		/* R16G16B16_UINT: w is the integer 1 */

layout(location = 0) flat out uvec4 words[3];

void main()
{
    uvec4 bytes;

    bytes = uvec4(round(unorm8 * 255.0));
    words[0].x = bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
    words[0].y = (uint(int(sscaled16.x)) & 65535u) | (uint(int(sscaled16.y)) << 16);
    words[0].z = (uint(sint16.x) & 65535u) | (uint(sint16.y) << 16);
    words[0].w = uint8.x | (uint8.y << 8) | (uint8.z << 16) | (uint8.w << 24);
    words[1].x = packHalf2x16(half4.xy);
    words[1].y = packHalf2x16(half4.zw);
    words[1].z = uint(round(packed10.x * 1023.0)) | (uint(round(packed10.y * 1023.0)) << 10) |
                 (uint(round(packed10.z * 1023.0)) << 20) | (uint(round(packed10.w * 3.0)) << 30);
    words[1].w = (uint(int(round(snorm16.x * 32767.0))) & 65535u) | (uint(snorm16.y == 0.0) << 16) |
                 (uint(snorm16.z == 0.0) << 17) | (uint(snorm16.w == 1.0) << 18);
    words[2].x = (uint(int(sscaled8.x)) & 65535u) | (uint(int(sscaled8.y)) << 16);
    words[2].y = uint16.x | (uint16.y << 16);
    words[2].z = uint16.z | (uint16.w << 16);
    words[2].w = 0x600df00du;
    gl_Position = position;
}
