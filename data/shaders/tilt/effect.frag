// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tilt: each pane is a screen turned in space, seen in perspective, with a
// glowing rim. pPointerMap casts the same ray as the picture, so a click lands
// on the cell drawn under the pointer, not on the cell that the layout has there.

// The pane as a plane at the focal distance, turned by yaw and pitch around its
// centre. Returns where the ray through uv meets it, in the pane's own uv, and
// the distance along the ray in t (1 at the centre of a flat pane).
vec2 tiltSource(vec2 uv, out float t)
{
    float aspect = iResolution.x / iResolution.y;
    float f = 1.0 / tan(radians(p_fov) * 0.5);
    vec2 s = (uv * 2.0 - 1.0) * vec2(aspect, 1.0);
    float a = radians(p_yaw), b = radians(p_pitch);
    mat3 yaw = mat3(cos(a), 0.0, -sin(a), 0.0, 1.0, 0.0, sin(a), 0.0, cos(a));
    mat3 pitch = mat3(1.0, 0.0, 0.0, 0.0, cos(b), sin(b), 0.0, -sin(b), cos(b));
    mat3 r = yaw * pitch;
    vec3 centre = vec3(0.0, 0.0, f);
    vec3 ray = vec3(s, f);
    // A ray beyond the plane's horizon meets it behind the eye, or never. It
    // takes the limit from this side: far beyond the far edge, where its
    // neighbours are. The map is linear between its grid points, so a jump
    // back towards the screen there would put cells in the empty space.
    float dn = max(dot(ray, r[2]), 1e-3);
    t = dot(centre, r[2]) / dn;
    vec3 local = t * ray - centre;
    vec2 p = vec2(dot(local, r[0]) / aspect, dot(local, r[1]));
    return clamp(0.5 + 0.5 * p / p_scale, -2.0, 3.0);
}

vec4 pTerminal(vec2 uv)
{
    float t;
    vec2 source = tiltSource(uv, t);
    vec3 rimColor = p_rimColor.rgb;
    // How far outside the screen, in pane heights: 0 on it.
    vec2 outside = max(max(-source, source - 1.0), 0.0) * vec2(iResolution.x / iResolution.y, 1.0);
    float away = length(outside);
    if (away > 0.0) {
        vec3 back = vec3(0.02, 0.025, 0.04) * (1.2 - 0.5 * length(uv - 0.5));
        back += rimColor * p_glow * 0.35 * exp(-away * 18.0);
        return vec4(back, 1.0);
    }
    vec4 color = terminalTexel(source);
    // The far side is darker, as a screen turned from the light.
    float shade = clamp(1.25 - 0.35 * t, 0.45, 1.0);
    // A lit rim along the screen's edge, a few pixels wide on the canvas.
    vec2 px = min(source, 1.0 - source) * iResolution / max(t, 0.1);
    float rim = exp(-min(px.x, px.y) * 0.6) * p_glow;
    return vec4(color.rgb * shade + rimColor * rim * color.a, color.a);
}

// The pointer map: the same ray as the picture. It reads only uv, the canvas
// size and the parameters, as the contract requires.
vec2 pPointerMap(vec2 uv)
{
    float t;
    return tiltSource(uv, t);
}
