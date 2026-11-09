// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tilt: each pane is a screen turned in space, seen in perspective, with a
// glowing rim. pPointerMap casts the same ray as the picture, so a click lands
// on the cell drawn under the pointer, not on the cell that the layout has there.

// The turn of the screen: its axes in the columns, its normal in r[2].
mat3 tiltRotation()
{
    float a = radians(p_yaw), b = radians(p_pitch);
    mat3 yaw = mat3(cos(a), 0.0, -sin(a), 0.0, 1.0, 0.0, sin(a), 0.0, cos(a));
    mat3 pitch = mat3(1.0, 0.0, 0.0, 0.0, cos(b), sin(b), 0.0, -sin(b), cos(b));
    return yaw * pitch;
}

// The size of the screen: Size, as a share of the largest turned screen whose
// four corners are seen inside the canvas. A larger one would put its near
// corners past the canvas edge, where they are not drawn and the pointer cannot
// reach them. On a wide pane the largest that fits is smaller, so Size keeps
// its effect there too. It reads only the canvas size and the parameters, so
// every pixel and the pointer map get the same value. A flat screen at Size 1
// fits exactly, and maps onto the canvas unchanged.
float tiltSize(float aspect, float f, mat3 r)
{
    float size = 1.0;
    for (int i = 0; i < 4; ++i) {
        vec2 corner = vec2((i & 1) == 0 ? -1.0 : 1.0, (i & 2) == 0 ? -1.0 : 1.0);
        // The corner at size k is at (0, 0, f) + k * d, and the canvas shows it
        // at f * k * d.xy / (f + k * d.z). Inside the canvas means
        // k * (f * |d.x| - aspect * d.z) <= aspect * f, and the same for y with
        // a half height of 1. That also keeps the corner in front of the eye.
        vec3 d = r[0] * (corner.x * aspect) + r[1] * corner.y;
        float across = f * abs(d.x) - aspect * d.z;
        float down = f * abs(d.y) - d.z;
        if (across > 0.0) size = min(size, aspect * f / across);
        if (down > 0.0) size = min(size, f / down);
    }
    return p_scale * size;
}

// The pane as a plane at the focal distance, turned by yaw and pitch around its
// centre. Returns where the ray through uv meets it, in the pane's own uv, and
// the distance along the ray in t (1 at the centre of a flat pane).
vec2 tiltSource(vec2 uv, out float t)
{
    float aspect = iResolution.x / iResolution.y;
    float f = 1.0 / tan(radians(p_fov) * 0.5);
    vec2 s = (uv * 2.0 - 1.0) * vec2(aspect, 1.0);
    mat3 r = tiltRotation();
    vec3 centre = vec3(0.0, 0.0, f);
    vec3 ray = vec3(s, f);
    // A ray beyond the plane's horizon meets it behind the eye, or never. It
    // takes the limit from this side: far beyond the far edge, where its
    // neighbours are. The map is bilinear between its grid points, so a jump
    // back towards the screen there would put cells in the empty space.
    float dn = max(dot(ray, r[2]), 1e-3);
    t = dot(centre, r[2]) / dn;
    vec3 local = t * ray - centre;
    vec2 p = vec2(dot(local, r[0]) / aspect, dot(local, r[1]));
    return clamp(0.5 + 0.5 * p / tiltSize(aspect, f, r), -2.0, 3.0);
}

// Adds light to a premultiplied colour. Alpha rises to cover the light, so
// the light shows over a translucent window. The colour itself passes as it
// comes, also brighter than its alpha (Bloom's glow before this pack).
vec4 addLight(vec4 color, vec3 light)
{
    float a = max(color.a, clamp(max(light.r, max(light.g, light.b)), 0.0, 1.0));
    return vec4(color.rgb + light, a);
}

vec4 pTerminal(vec2 uv)
{
    float t;
    vec2 source = tiltSource(uv, t);
    vec3 rimColor = p_rimColor.rgb;
    // The distance to the screen's edges in canvas pixels, for each axis:
    // negative outside. The screen covers this pixel by that much, blended
    // over one pixel so that its outline is not jagged.
    vec2 edge = min(source, 1.0 - source) / max(fwidth(source), vec2(1e-6));
    vec2 cover = clamp(edge + 0.5, 0.0, 1.0);
    float coverage = cover.x * cover.y;

    // The space around the screen: the theme background, darker and falling
    // off towards the canvas edges, at the window's opacity, with the glow of
    // the rim. How far outside the screen, in pane heights: 0 on it.
    vec2 outside = max(max(-source, source - 1.0), 0.0) * vec2(iResolution.x / iResolution.y, 1.0);
    float away = length(outside);
    float falloff = 0.35 * (1.2 - 0.5 * length(uv - 0.5));
    vec4 back = vec4(uBackgroundColor.rgb * falloff, 1.0) * uBackgroundColor.a;
    back = addLight(back, rimColor * p_glow * 0.35 * exp(-away * 18.0));
    if (coverage <= 0.0) return back;

    vec4 color = terminalTexel(source);
    // The far side is darker, as a screen turned from the light. A flat screen
    // (t is 1) keeps its colour.
    float shade = clamp(1.0 - 0.35 * (t - 1.0), 0.45, 1.0);
    // A lit rim along the screen's edge, a few canvas pixels wide. It lights
    // the edge against the space around the screen, so it fades where the
    // screen's edge lies on the canvas edge: a flat screen at Size 1 has none.
    float fromEdge = max(min(edge.x, edge.y), 0.0);
    vec2 canvasEdge = min(uv, 1.0 - uv) * iResolution;
    float space = clamp((min(canvasEdge.x, canvasEdge.y) - fromEdge) * 0.5, 0.0, 1.0);
    float rim = exp(-fromEdge * 0.6) * p_glow * space;
    vec4 screen = addLight(vec4(color.rgb * shade, color.a), rimColor * rim);
    return mix(back, screen, coverage);
}

// The pointer map: the same ray as the picture. It reads only uv, the canvas
// size and the parameters, as the contract requires.
vec2 pPointerMap(vec2 uv)
{
    float t;
    return tiltSource(uv, t);
}
