#version 330 core
// Restrained unsharp mask for the upscaled original picture; no new textures.
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec2 sourceSize;
uniform int fsrSharpened;
out vec4 finalColor;
void main()
{
    vec4 center = texture(texture0, fragTexCoord);
    if (fsrSharpened != 0)
    {
        finalColor = center;
        return;
    }
    vec2 stepSize = 1.0 / sourceSize;
    vec3 left = texture(texture0, clamp(fragTexCoord - vec2(stepSize.x, 0), 0.0, 1.0)).rgb;
    vec3 right = texture(texture0, clamp(fragTexCoord + vec2(stepSize.x, 0), 0.0, 1.0)).rgb;
    vec3 up = texture(texture0, clamp(fragTexCoord - vec2(0, stepSize.y), 0.0, 1.0)).rgb;
    vec3 down = texture(texture0, clamp(fragTexCoord + vec2(0, stepSize.y), 0.0, 1.0)).rgb;
    vec3 detail = center.rgb - (left + right + up + down) * 0.25;
    vec3 low = min(center.rgb, min(min(left, right), min(up, down)));
    vec3 high = max(center.rgb, max(max(left, right), max(up, down)));
    finalColor = vec4(clamp(center.rgb + detail * 0.25, low, high), center.a);
}
