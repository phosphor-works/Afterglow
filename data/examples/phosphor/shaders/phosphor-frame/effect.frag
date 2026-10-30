// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "phosphor.glsl"

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 world = phWindow(fragCoord) - iFramebufferSize * vec2(0.60, 0.55);
    float eventAge = min(phAge(0), min(phAge(1), phAge(3)));
    float distance = length(world) / iFramebufferSize.y;
    float front = exp(-phSq((distance - eventAge * 1.1) / 0.022)) * exp(-eventAge * 2.5);
    float behind = max(0.0, eventAge * 1.1 - distance);
    float wake = sin(behind * 145.0) * exp(-behind * 22.0) * exp(-eventAge * 2.0)
               * smoothstep(0.0, 0.025, behind);
    vec2 direction = world / max(length(world), 1.0);
    float scale = iFramebufferSize.y / 1080.0;
    vec2 bend = direction * (front * 14.0 + wake * 5.0) * scale;
    vec2 coord = clamp(fragCoord + bend, vec2(0.5), iResolution.xy - 0.5);
    vec4 c = texture(iChannel0, coord / iResolution.xy);
    // The pulse crosses actual content for an instant, with a narrow colour
    // fringe; it settles back to exact terminal pixels as the front passes.
    vec2 fringe = direction * (front * 2.3 + wake * 0.5) * scale;
    c.r = texture(iChannel0, clamp(coord + fringe, vec2(0.5), iResolution.xy - 0.5) / iResolution.xy).r;
    c.b = texture(iChannel0, clamp(coord - fringe, vec2(0.5), iResolution.xy - 0.5) / iResolution.xy).b;
    vec2 p = vec2(fragCoord.x, iResolution.y - fragCoord.y);
    vec2 r = iResolution.xy;
    float edge = p.y, along = p.x;
    if (r.x - p.x < edge) { edge = r.x - p.x; along = r.x + p.y; }
    if (r.y - p.y < edge) { edge = r.y - p.y; along = r.x + r.y + r.x - p.x; }
    if (p.x < edge) { edge = p.x; along = 2.0 * r.x + r.y + r.y - p.y; }
    float line = exp(-phSq((edge - 1.5) / 0.9));
    float heat = phHeat();
    float focused = iFocus != 0 ? 1.0 : 0.20;
    float perimeter = 2.0 * (r.x + r.y);
    float successAge = phAge(1);
    float focusAge = max(0.0, iTime - iTimeFocus);
    float age = min(successAge, iFocus != 0 ? focusAge : 1e6);
    float tip = age * perimeter / 1.25;
    float delta = along - tip;
    float sweep = exp(-phSq(delta / (iCellSize.y * 3.5))) * exp(-age * 0.8);
    float tail = step(delta, 0.0) * exp(delta / (iCellSize.y * 9.0)) * exp(-age * 1.5);
    float failure = exp(-phAge(0) * 3.5);
    float blocked = iSignal[4].x == 2.0 ? 0.10 + 0.04 * sin(iTime * 3.0) : 0.0;
    vec3 tint = mix(phTint(along / perimeter), phRose, max(failure, blocked));
    float strength = line * (focused * 0.18 + heat * 0.18 + sweep * 0.8 + tail * 0.35 + failure * 0.65 + blocked);
    strength += exp(-edge / 9.0) * (sweep * 0.10 + failure * 0.06);
    fragColor = phIlluminate(c, tint, strength * p_intensity);
}
