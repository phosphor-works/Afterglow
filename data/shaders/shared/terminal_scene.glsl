// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Object hierarchies for TERMINAL mesh passes (metadata `geometryPasses`
// entries with `objects`). Afterglow computes each object's world transform
// every frame, from its parent's transform and its own translate, rotate,
// scale and spin, and draws each object with a mesh once. In the vertex stage,
// gl_InstanceIndex is the object's index in metadata order.
//
// The matrices map model coordinates to the scene's world coordinates. The
// pack still applies its own camera and projection. Pass objectIndex() to the
// fragment stage in a flat varying when the fragment stage needs it.

#ifndef AFTERGLOW_TERMINAL_SCENE_GLSL
#define AFTERGLOW_TERMINAL_SCENE_GLSL

layout(std140, binding = 18) uniform TerminalScene {
    mat4 uObjectMatrix[64];       // offset 0     world transform of each object
    mat4 uObjectNormalMatrix[64]; // offset 4096  inverse transpose, for normals
};                                // total 8192 bytes

#ifdef AFTERGLOW_TERMINAL_VERTEX_STAGE
int objectIndex() {
    return gl_InstanceIndex;
}

// A model-space position in world space.
vec3 objectPosition(vec3 position) {
    return (uObjectMatrix[clamp(gl_InstanceIndex, 0, 63)] * vec4(position, 1.0)).xyz;
}

// A model-space normal in world space.
vec3 objectNormal(vec3 normal) {
    return normalize(mat3(uObjectNormalMatrix[clamp(gl_InstanceIndex, 0, 63)]) * normal);
}
#endif

#endif // AFTERGLOW_TERMINAL_SCENE_GLSL
