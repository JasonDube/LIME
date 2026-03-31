#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 wireColor;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    gl_PointSize = 10.0;  // Size for vertex points
}
