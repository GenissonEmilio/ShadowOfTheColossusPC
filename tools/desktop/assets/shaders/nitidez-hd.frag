#version 330 core
// Restrained unsharp mask for the upscaled original picture; no new textures.
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec2 sourceSize;
out vec4 finalColor;
void main()
{
    vec2 stepSize = 1.0 / sourceSize;
    vec4 center = texture(texture0, fragTexCoord);
    vec3 left = texture(texture0, clamp(fragTexCoord - vec2(stepSize.x, 0), 0.0, 1.0)).rgb;
    vec3 right = texture(texture0, clamp(fragTexCoord + vec2(stepSize.x, 0), 0.0, 1.0)).rgb;
    vec3 up = texture(texture0, clamp(fragTexCoord - vec2(0, stepSize.y), 0.0, 1.0)).rgb;
    vec3 down = texture(texture0, clamp(fragTexCoord + vec2(0, stepSize.y), 0.0, 1.0)).rgb;
    vec3 detail = center.rgb - (left + right + up + down) * 0.25;
    finalColor = vec4(clamp(center.rgb + detail * 0.35, 0.0, 1.0), center.a);
}
