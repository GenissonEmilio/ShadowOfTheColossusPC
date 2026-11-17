#pragma once

namespace desktop_video
{
inline constexpr char kAreaFragment[] = R"GLSL(#version 330 core
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform ivec2 sourceSize;
uniform ivec2 targetSize;
out vec4 finalColor;

void main()
{
    vec2 center = fragTexCoord * vec2(textureSize(texture0, 0));
    bool horizontal = sourceSize.x > targetSize.x;
    float centerOnAxis = horizontal ? center.x : center.y;
    float extent = float(horizontal ? sourceSize.x : sourceSize.y);
    float footprint = extent / float(horizontal ? targetSize.x : targetSize.y);
    float first = max(0.0, centerOnAxis - footprint * 0.5);
    float last = min(extent, centerOnAxis + footprint * 0.5);
    int row = horizontal ? int(center.y) : int(center.x);
    row = clamp(row, 0, (horizontal ? sourceSize.y : sourceSize.x) - 1);

    // Integrate coverage over the output pixel along one axis at a time.
    // Positive weights preserve constant colors and cannot create ringing.
    vec4 sum = vec4(0.0);
    for (int i = int(floor(first)); i < int(ceil(last)); ++i)
    {
        float weight = min(last, float(i + 1)) - max(first, float(i));
        ivec2 pixel = horizontal ? ivec2(i, row) : ivec2(row, i);
        sum += texelFetch(texture0, pixel, 0) * weight;
    }
    finalColor = sum / (last - first);
}
)GLSL";

// Contrast and edge search procedure from Timothy Lottes' FXAA white paper.
// https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf
inline constexpr char kFxaaFragment[] = R"GLSL(#version 330 core
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform ivec2 sourceSize;
out vec4 finalColor;

vec4 sourcePixel(ivec2 p)
{
    return texelFetch(texture0, clamp(p, ivec2(0), sourceSize - 1), 0);
}

float luminance(vec3 color)
{
    return dot(color, vec3(0.299, 0.587, 0.114));
}

float luma(ivec2 p)
{
    return luminance(sourcePixel(p).rgb);
}

float edgeEnd(ivec2 pixel, ivec2 tangent, ivec2 normal, float level, float threshold,
              out bool oppositeSide, float center)
{
    const int maxSteps = 8;
    for (int step = 1; step <= maxSteps; ++step)
    {
        ivec2 p = pixel + tangent * step;
        if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, sourceSize))) break;
        float edge = 0.5 * (luma(p) + luma(p + normal)) - level;
        if (abs(edge) >= threshold)
        {
            oppositeSide = (edge < 0.0) != (center < level);
            return float(step);
        }
    }
    oppositeSide = false;
    return float(maxSteps + 1);
}

void main()
{
    ivec2 p = ivec2(fragTexCoord * vec2(textureSize(texture0, 0)));
    vec4 color = sourcePixel(p);
    float center = luminance(color.rgb);
    float left = luma(p + ivec2(-1, 0));
    float right = luma(p + ivec2(1, 0));
    float top = luma(p + ivec2(0, -1));
    float bottom = luma(p + ivec2(0, 1));
    float darkest = min(center, min(min(left, right), min(top, bottom)));
    float brightest = max(center, max(max(left, right), max(top, bottom)));
    float contrast = brightest - darkest;
    if (contrast < max(0.045, brightest * 0.15))
    {
        finalColor = color;
        return;
    }

    float tl = luma(p + ivec2(-1, -1));
    float tr = luma(p + ivec2(1, -1));
    float bl = luma(p + ivec2(-1, 1));
    float br = luma(p + ivec2(1, 1));
    float horizontal = abs(tl + bl - 2.0 * left) + abs(tr + br - 2.0 * right)
                       + 2.0 * abs(top + bottom - 2.0 * center);
    float vertical = abs(tl + tr - 2.0 * top) + abs(bl + br - 2.0 * bottom)
                     + 2.0 * abs(left + right - 2.0 * center);
    bool horizontalEdge = horizontal >= vertical;
    ivec2 normal = horizontalEdge ? ivec2(0, 1) : ivec2(1, 0);
    ivec2 tangent = horizontalEdge ? ivec2(1, 0) : ivec2(0, 1);
    float before = horizontalEdge ? top : left;
    float after = horizontalEdge ? bottom : right;
    bool towardBefore = abs(before - center) >= abs(after - center);
    if (towardBefore) normal = -normal;
    float neighbor = towardBefore ? before : after;
    float level = 0.5 * (center + neighbor);
    float threshold = 0.25 * abs(center - neighbor);

    bool blendBefore, blendAfter;
    float distanceBefore = edgeEnd(p, -tangent, normal, level, threshold, blendBefore, center);
    float distanceAfter = edgeEnd(p, tangent, normal, level, threshold, blendAfter, center);
    bool validEdge = distanceBefore < distanceAfter ? blendBefore : blendAfter;
    float edgeBlend = validEdge ? 0.5 - min(distanceBefore, distanceAfter) / (distanceBefore + distanceAfter) : 0.0;

    float average = (2.0 * (left + right + top + bottom) + tl + tr + bl + br) / 12.0;
    float subpixel = smoothstep(0.0, 1.0, abs(average - center) / contrast);
    // Limit the subpixel component to retain texture detail on high-contrast edges.
    finalColor = mix(color, sourcePixel(p + normal), max(edgeBlend, 0.5 * subpixel * subpixel));
}
)GLSL";
}
