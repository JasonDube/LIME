#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec3 gizmoPosition;
} pc;

layout(location = 0) out vec3 fragColor;

void main() {
    // Transform gizmo vertices relative to gizmo position
    vec3 worldPos = inPosition + pc.gizmoPosition;
    gl_Position = pc.mvp * vec4(worldPos, 1.0);
    fragColor = inColor;
}
