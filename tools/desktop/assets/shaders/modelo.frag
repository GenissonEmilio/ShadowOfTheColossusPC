#version 330 core
// Fragment shader template. Apply effects to texture0 at fragTexCoord.
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 sourceSize; // original picture size, in pixels
uniform vec2 outputSize; // displayed picture size, in physical pixels
uniform float time;     // elapsed seconds
out vec4 finalColor;
void main()
{
    finalColor = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
}
