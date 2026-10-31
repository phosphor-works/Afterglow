// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Places each object with its scene matrix, then views the scene from above
// the orbital plane. The camera spans the canvas, so each pane shows the
// whole orrery.
#include <terminal_scene.glsl>

layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) flat out int object;

vec4 pMesh() {
    worldPosition = objectPosition(meshPosition);
    worldNormal = objectNormal(meshNormal);
    object = objectIndex();
    const vec3 eye = vec3(0.0, 3.2, 9.5);
    vec3 forward = normalize(-eye);
    vec3 right = normalize(cross(forward, vec3(0.0, 1.0, 0.0)));
    vec3 up = cross(right, forward);
    vec3 relative = worldPosition - eye;
    vec3 view = vec3(dot(relative, right), dot(relative, up), dot(relative, forward));
    const float nearPlane = 0.1, farPlane = 40.0, focal = 2.4;
    float aspect = iResolution.x / max(iResolution.y, 1.0);
    // Vulkan clip space: y down, depth 0 near to 1 far.
    return vec4(view.x * focal / aspect, -view.y * focal,
                farPlane / (farPlane - nearPlane) * (view.z - nearPlane), view.z);
}
