#pragma once

// GLSL 330 adaptation of AMD FidelityFX Super Resolution, FP32 EASU and RCAS.
// https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/a21ffb8f6c13233ba336352bdff293894c706575/ffx-fsr/ffx_fsr1.h
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

namespace desktop_video
{
inline constexpr char kEasuFragment[] = R"GLSL(#version 330 core
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform ivec2 sourceSize;
out vec4 finalColor;

vec3 sourcePixel(ivec2 p)
{
    return texelFetch(texture0, clamp(p, ivec2(0), sourceSize - 1), 0).rgb;
}

float luma(vec3 c)
{
    return c.g + 0.5 * (c.r + c.b);
}

void accumulateGradient(inout vec2 direction, inout float strength, float weight,
                        float top, float left, float center, float right, float bottom)
{
    vec2 gradient = vec2(right - left, bottom - top);
    vec2 range = max(abs(vec2(right, bottom) - center), abs(center - vec2(left, top)));
    vec2 magnitude = clamp(abs(gradient) / max(range, vec2(1e-6)), 0.0, 1.0);
    direction += gradient * weight;
    strength += dot(magnitude, magnitude) * weight;
}

void accumulateTap(inout vec3 sum, inout float weights, vec2 off, vec2 direction,
                   vec2 stretch, float lobe, float clipDistance, vec3 color)
{
    vec2 v = vec2(dot(off, direction), dot(off, vec2(-direction.y, direction.x))) * stretch;
    float distanceSquared = min(dot(v, v), clipDistance);
    float base = 0.4 * distanceSquared - 1.0;
    float window = lobe * distanceSquared - 1.0;
    float weight = ((25.0 / 16.0) * base * base - (9.0 / 16.0)) * window * window;
    sum += color * weight;
    weights += weight;
}

void main()
{
    // Interpolated source coordinates include the source crop and orientation.
    vec2 source = fragTexCoord * vec2(textureSize(texture0, 0)) - 0.5;
    ivec2 p = ivec2(floor(source));
    vec2 f = fract(source);

    // EASU's 12 taps, fetched by texel coordinates for OpenGL 3.3.
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = sourcePixel(p + ivec2( 0, -1));
    vec3 c = sourcePixel(p + ivec2( 1, -1));
    vec3 e = sourcePixel(p + ivec2(-1,  0));
    vec3 center = sourcePixel(p);
    vec3 g = sourcePixel(p + ivec2( 1,  0));
    vec3 h = sourcePixel(p + ivec2( 2,  0));
    vec3 i = sourcePixel(p + ivec2(-1,  1));
    vec3 j = sourcePixel(p + ivec2( 0,  1));
    vec3 k = sourcePixel(p + ivec2( 1,  1));
    vec3 l = sourcePixel(p + ivec2( 2,  1));
    vec3 n = sourcePixel(p + ivec2( 0,  2));
    vec3 o = sourcePixel(p + ivec2( 1,  2));

    vec2 direction = vec2(0.0);
    float strength = 0.0;
    accumulateGradient(direction, strength, (1.0 - f.x) * (1.0 - f.y),
                       luma(b), luma(e), luma(center), luma(g), luma(j));
    accumulateGradient(direction, strength, f.x * (1.0 - f.y),
                       luma(c), luma(center), luma(g), luma(h), luma(k));
    accumulateGradient(direction, strength, (1.0 - f.x) * f.y,
                       luma(center), luma(i), luma(j), luma(k), luma(n));
    accumulateGradient(direction, strength, f.x * f.y,
                       luma(g), luma(j), luma(k), luma(l), luma(o));
    float directionSquared = dot(direction, direction);
    direction = directionSquared < (1.0 / 32768.0)
        ? vec2(1.0, 0.0) : direction * inversesqrt(directionSquared);
    strength *= 0.5;
    strength *= strength;
    float diagonal = 1.0 / max(abs(direction.x), abs(direction.y));
    vec2 stretch = vec2(1.0 + (diagonal - 1.0) * strength, 1.0 - 0.5 * strength);
    float lobe = mix(0.5, 0.25 - 0.04, strength);
    float clipDistance = 1.0 / lobe;

    vec3 sum = vec3(0.0);
    float weights = 0.0;
    accumulateTap(sum, weights, vec2( 0, -1) - f, direction, stretch, lobe, clipDistance, b);
    accumulateTap(sum, weights, vec2( 1, -1) - f, direction, stretch, lobe, clipDistance, c);
    accumulateTap(sum, weights, vec2(-1,  0) - f, direction, stretch, lobe, clipDistance, e);
    accumulateTap(sum, weights, vec2( 0,  0) - f, direction, stretch, lobe, clipDistance, center);
    accumulateTap(sum, weights, vec2( 1,  0) - f, direction, stretch, lobe, clipDistance, g);
    accumulateTap(sum, weights, vec2( 2,  0) - f, direction, stretch, lobe, clipDistance, h);
    accumulateTap(sum, weights, vec2(-1,  1) - f, direction, stretch, lobe, clipDistance, i);
    accumulateTap(sum, weights, vec2( 0,  1) - f, direction, stretch, lobe, clipDistance, j);
    accumulateTap(sum, weights, vec2( 1,  1) - f, direction, stretch, lobe, clipDistance, k);
    accumulateTap(sum, weights, vec2( 2,  1) - f, direction, stretch, lobe, clipDistance, l);
    accumulateTap(sum, weights, vec2( 0,  2) - f, direction, stretch, lobe, clipDistance, n);
    accumulateTap(sum, weights, vec2( 1,  2) - f, direction, stretch, lobe, clipDistance, o);

    // Clamp to the local four-pixel range to limit ringing.
    vec3 minimum = min(min(center, g), min(j, k));
    vec3 maximum = max(max(center, g), max(j, k));
    finalColor = vec4(clamp(sum / weights, minimum, maximum), 1.0);
}
)GLSL";

inline constexpr char kRcasFragment[] = R"GLSL(#version 330 core
in vec2 fragTexCoord;
uniform sampler2D texture0;
out vec4 finalColor;

vec3 sourcePixel(ivec2 p, ivec2 size)
{
    return texelFetch(texture0, clamp(p, ivec2(0), size - 1), 0).rgb;
}

void main()
{
    ivec2 size = textureSize(texture0, 0);
    ivec2 p = ivec2(fragTexCoord * vec2(size));
    vec3 b = sourcePixel(p + ivec2( 0, -1), size);
    vec3 d = sourcePixel(p + ivec2(-1,  0), size);
    vec3 e = sourcePixel(p, size);
    vec3 f = sourcePixel(p + ivec2( 1,  0), size);
    vec3 h = sourcePixel(p + ivec2( 0,  1), size);

    vec3 minimum = min(min(b, d), min(f, h));
    vec3 maximum = max(max(b, d), max(f, h));
    vec3 hitMin = min(minimum, e) / max(4.0 * maximum, vec3(1e-6));
    vec3 hitMax = (1.0 - max(maximum, e)) / min(4.0 * minimum - 4.0, vec3(-1e-6));
    vec3 limits = max(-hitMin, hitMax);
    // Use one stop below maximum sharpening.
    float lobe = 0.5 * clamp(max(max(limits.r, limits.g), limits.b), -(0.25 - 1.0 / 16.0), 0.0);
    finalColor = vec4((e + lobe * (b + d + f + h)) / (1.0 + 4.0 * lobe), 1.0);
}
)GLSL";
}
