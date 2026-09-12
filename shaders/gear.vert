#version 450
// gear.vert - transforms one gear's vertices and forwards lighting inputs.
//
// Bindings:
//   set 0, binding 0 : scene uniform buffer (view-projection, light, camera)
//   push constant    : per-gear model matrix, checker tint, checker phase

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 viewProj;
    vec4 lightDir;    // xyz = unit vector pointing from the surface toward the key light
    vec4 cameraPos;   // xyz = eye position in world space
    vec4 params;      // x = ambient, y = specular strength, z = time, w = unused
} scene;

layout(push_constant) uniform PushConstants {
    mat4 model;       // rotation about Z + translation into the gear plane
    vec4 color;       // rgb = checker tint, a = unused
    vec4 uvParams;    // xy = checker repeats, zw = checker phase offset
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vTexCoord;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out vec3 vTint;

void main() {
    vec4 world = pc.model * vec4(inPosition, 1.0);

    vWorldPos = world.xyz;
    // The model matrix only contains a rotation and a translation, so the upper
    // left 3x3 block transforms normals correctly (no inverse-transpose needed).
    vNormal   = mat3(pc.model) * inNormal;
    vTexCoord = inTexCoord * pc.uvParams.xy + pc.uvParams.zw;
    vTint     = pc.color.rgb;

    gl_Position = scene.viewProj * world;
}
