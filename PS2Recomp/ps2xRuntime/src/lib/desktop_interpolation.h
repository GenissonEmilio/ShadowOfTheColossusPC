#pragma once

// Both textures contain real frames. A low-resolution field estimates local
// motion, then the second pass warps both frames towards their midpoint.
inline constexpr char kFrameMotionFragment[] = R"GLSL(#version 330
in vec2 fragTexCoord;
out vec4 finalColor;
uniform sampler2D texture0;
uniform sampler2D previousFrame;
uniform vec2 sourceSize;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float cost(vec2 uv, vec2 offset)
{
    float error = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            vec2 sampleAt = uv + vec2(x, y) * 4.0 / sourceSize;
            float a = luma(texture(previousFrame, sampleAt).rgb);
            float b = luma(texture(texture0, sampleAt + offset / sourceSize).rgb);
            error += abs(a - b);
        }
    return error / 9.0;
}

void main()
{
    vec2 uv = fragTexCoord;
    float still = cost(uv, vec2(0.0));
    vec2 bestOffset = vec2(0.0);
    float best = still;
    if (still > 0.012)
    {
        for (int y = -3; y <= 3; ++y)
            for (int x = -3; x <= 3; ++x)
            {
                vec2 offset = vec2(x, y) * 4.0;
                float candidate = cost(uv, offset) + 0.0002 * length(offset);
                if (candidate < best)
                {
                    best = candidate;
                    bestOffset = offset;
                }
            }
        vec2 center = bestOffset;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                vec2 offset = center + vec2(x, y) * 2.0;
                float candidate = cost(uv, offset) + 0.0002 * length(offset);
                if (candidate < best)
                {
                    best = candidate;
                    bestOffset = offset;
                }
            }
    }
    float confidence = (best < 0.05 && best < still * 0.75) ? 1.0 : 0.0;
    finalColor = vec4(bestOffset / 32.0 + 0.5, confidence, 1.0);
}
)GLSL";

inline constexpr char kFrameMidpointFragment[] = R"GLSL(#version 330
in vec2 fragTexCoord;
out vec4 finalColor;
uniform sampler2D texture0;
uniform sampler2D previousFrame;
uniform sampler2D motionField;
uniform vec2 sourceSize;

float difference(vec3 a, vec3 b)
{
    vec3 d = abs(a - b);
    return max(d.r, max(d.g, d.b));
}

void main()
{
    vec2 uv = fragTexCoord;
    vec3 real = texture(texture0, uv).rgb;
    vec3 before = texture(previousFrame, uv).rgb;
    vec3 motion = texture(motionField, vec2(uv.x, 1.0 - uv.y)).rgb;
    vec2 halfOffset = (motion.rg - 0.5) * 16.0 / sourceSize;
    vec2 left = uv - halfOffset, right = uv + halfOffset;
    // Keep static pixels and reject unreliable warps, including newly
    // revealed surfaces, cuts and blocks that exceed the search distance.
    if (motion.b > 0.6 && difference(real, before) > 0.008 &&
        all(greaterThanEqual(left, vec2(0.0))) && all(lessThanEqual(left, vec2(1.0))) &&
        all(greaterThanEqual(right, vec2(0.0))) && all(lessThanEqual(right, vec2(1.0))))
    {
        vec3 a = texture(previousFrame, left).rgb;
        vec3 b = texture(texture0, right).rgb;
        if (difference(a, b) < 0.11)
            real = (a + b) * 0.5;
    }
    finalColor = vec4(real, 1.0);
}
)GLSL";
