#version 450

layout(set = 0, binding = 0) uniform sampler2D texSampler;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec4 fragColor;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec3 fragColorAdjust;  // x=hue, y=saturation, z=brightness

layout(location = 0) out vec4 outColor;

// RGB to HSV conversion
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// HSV to RGB conversion
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    // Sample texture
    vec4 texColor = texture(texSampler, fragTexCoord);

    // Combine texture with vertex color
    vec3 baseColor = texColor.rgb * fragColor.rgb;

    // Check for hit flash (saturation > 2.5 signals hit flash mode)
    bool hitFlash = fragColorAdjust.y > 2.5;

    vec3 adjustedColor;
    if (hitFlash) {
        // Override to bright red for hit flash
        adjustedColor = vec3(1.0, 0.1, 0.1);
    } else {
        // Apply HSV adjustments
        vec3 hsv = rgb2hsv(baseColor);

        // Hue shift (fragColorAdjust.x is in degrees, convert to 0-1 range)
        hsv.x = fract(hsv.x + fragColorAdjust.x / 360.0);

        // Saturation multiplier
        hsv.y = clamp(hsv.y * fragColorAdjust.y, 0.0, 1.0);

        // Brightness/Value multiplier
        hsv.z = clamp(hsv.z * fragColorAdjust.z, 0.0, 1.0);

        // Convert back to RGB
        adjustedColor = hsv2rgb(hsv);
    }

    // Simple directional lighting
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diffuse = max(dot(normalize(fragNormal), lightDir), 0.0);
    float ambient = 0.3;
    float lighting = ambient + diffuse * 0.7;

    vec3 finalColor = adjustedColor * lighting;

    outColor = vec4(finalColor, texColor.a * fragColor.a);
}
