// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Linear-light shading: the sun glows, and the planet and moon are lit from
// the sun at the origin.
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) flat in int object;

vec4 pTerminal(vec2 uv) {
    vec3 n = normalize(worldNormal);
    if (object == 0) {
        float rim = pow(1.0 - abs(n.z), 2.0);
        return vec4(vec3(1.0, 0.62, 0.22) * (1.4 + rim * 1.2), 1.0);
    }
    vec3 base = object == 2 ? vec3(0.18, 0.42, 0.85) : vec3(0.55, 0.55, 0.58);
    float light = max(dot(n, normalize(-worldPosition)), 0.0);
    return vec4(base * (0.04 + light * 1.1), 1.0);
}
