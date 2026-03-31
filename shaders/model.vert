#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
    vec4 colorAdjust;  // x=hue, y=saturation, z=brightness, w=unused
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;

layout(location = 0) flat out vec3 fragNormal;  // flat shading - no interpolation
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec4 fragColor;
layout(location = 3) out vec3 fragWorldPos;
layout(location = 4) out vec4 fragColorAdjust;  // x=hue, y=saturation, z=brightness, w=alpha

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);

    // Transform normal to world space
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    fragNormal = normalize(normalMatrix * inNormal);

    fragTexCoord = inTexCoord;
    fragColor = inColor;
    fragWorldPos = vec3(pc.model * vec4(inPosition, 1.0));
    fragColorAdjust = pc.colorAdjust;
}
