#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 selectionColor;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = pc.selectionColor;
}
