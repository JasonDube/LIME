#version 450

const int MAX_BONES = 128;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
    vec4 colorAdjust;  // x=hue, y=saturation, z=brightness, w=unused
} pc;

layout(set = 0, binding = 1) uniform BoneMatrices {
    mat4 bones[MAX_BONES];
} boneData;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;
layout(location = 4) in ivec4 inJoints;   // Bone indices
layout(location = 5) in vec4 inWeights;   // Bone weights

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec4 fragColor;
layout(location = 3) out vec3 fragWorldPos;
layout(location = 4) out vec3 fragColorAdjust;

void main() {
    // Compute skinned position and normal
    mat4 skinMatrix =
        boneData.bones[inJoints.x] * inWeights.x +
        boneData.bones[inJoints.y] * inWeights.y +
        boneData.bones[inJoints.z] * inWeights.z +
        boneData.bones[inJoints.w] * inWeights.w;

    vec4 skinnedPosition = skinMatrix * vec4(inPosition, 1.0);
    vec3 skinnedNormal = mat3(skinMatrix) * inNormal;

    gl_Position = pc.mvp * skinnedPosition;

    // Transform normal to world space
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    fragNormal = normalize(normalMatrix * skinnedNormal);

    fragTexCoord = inTexCoord;
    fragColor = inColor;
    fragWorldPos = vec3(pc.model * skinnedPosition);
    fragColorAdjust = pc.colorAdjust.xyz;
}
