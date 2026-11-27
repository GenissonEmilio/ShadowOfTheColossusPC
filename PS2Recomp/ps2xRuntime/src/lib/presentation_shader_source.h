#pragma once

#include <string>
#include <string_view>

namespace desktop_video
{
    // Share the presentation algorithms with GLES; only the language header
    // differs. Pair ES 300 fragments with this vertex program, rather than
    // raylib's GLES 100 default vertex shader.
    inline constexpr char kGlesPresentationVertex[] = R"GLSL(#version 300 es
precision highp float;
precision highp int;
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)GLSL";

    inline std::string glesPresentationFragment(std::string_view source)
    {
        constexpr std::string_view desktop = "#version 330";
        if (!source.starts_with(desktop))
            return std::string(source);
        const size_t newline = source.find('\n');
        return "#version 300 es\nprecision highp float;\nprecision highp int;\n" +
               std::string(newline == std::string_view::npos ? std::string_view{} : source.substr(newline + 1));
    }
}
