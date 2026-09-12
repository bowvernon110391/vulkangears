#version 450
// gear.frag - checkered, tinted, lit shading for the gears.
//
// The checker comes from a mipmapped texture that is tinted per gear, so all
// three gears share one texture and one pipeline.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vTexCoord;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in vec3 vTint;

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 viewProj;
    vec4 lightDir;
    vec4 cameraPos;
    vec4 params;
} scene;

layout(set = 0, binding = 1) uniform sampler2D checkerTex;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 color;
    vec4 uvParams;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(scene.lightDir.xyz);
    vec3 V = normalize(scene.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    float diffuse  = max(dot(N, L), 0.0);
    float specular = pow(max(dot(N, H), 0.0), 64.0);

    // Texture is stored in sRGB and sampled to linear; vTint shifts the hue.
    vec3 checker = texture(checkerTex, vTexCoord).rgb;
    vec3 albedo  = checker * vTint * 1.6;

    vec3 color = albedo * scene.params.x                 // ambient
               + albedo * diffuse * 0.95                 // key light
               + vec3(specular * scene.params.y);        // highlight

    // A little rim light separates the gears from the background.
    float rim = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 3.0);
    color += vTint * rim * 0.30;

    outColor = vec4(color, 1.0);
}
