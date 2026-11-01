// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "phosphor.glsl"

vec3 highlight(vec2 p) {
    vec4 c = terminalTexel(terminalUv(p));
    if (phLight()) c.rgb = max(c.rgb-pow(uBackgroundColor.rgb,vec3(2.2))*c.a,vec3(0));
    float brightness = max(c.r,max(c.g,c.b));
    // Soft threshold leaves the dark environment dark, with enough radiance
    // above white for the reflected softboxes and reactor to bloom naturally.
    return c.rgb*max(brightness-0.55,0.0)/max(brightness,0.001);
}
vec3 filmic(vec3 x) {
    return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0.0,1.0);
}
vec4 pTerminal(vec2 uv) {
    vec2 fragCoord = terminalPixel(uv);
    vec4 c = terminalTexel(uv);
    vec3 bloom = vec3(0), streak = vec3(0);
    float scale = uWindowSize.y/1080.0;
    // Deterministic low-discrepancy disc, shared across panes. Three lens
    // scattering scales retain a tight core and a soft, much wider halo.
    for (int i = 0; i < 24; ++i) {
        float angle = float(i)*2.39996323;
        float radius = sqrt((float(i)+0.5)/24.0);
        vec2 offset = vec2(cos(angle),sin(angle))*radius*scale;
        bloom += highlight(fragCoord+offset*5.0)*0.5;
        bloom += highlight(fragCoord+offset*19.0)*0.32;
        bloom += highlight(fragCoord+offset*54.0)*0.18;
    }
    for (int i = 1; i <= 12; ++i) {
        float distance = float(i*i)*0.85*scale;
        float weight = exp(-float(i)*0.23);
        streak += (highlight(fragCoord+vec2(distance,0))+highlight(fragCoord-vec2(distance,0)))*weight;
    }
    vec3 radiance = c.rgb+bloom*(p_bloom/24.0)+streak*(p_streak/8.0);
    vec2 centred = (phWindow(fragCoord)/uWindowSize-0.5)*vec2(1.0,0.7);
    radiance *= 1.0-0.20*dot(centred,centred);
    // The transfer works on straight colour: the light takes the frame's alpha.
    vec3 rgb = c.a > 0.0 ? pow(filmic(radiance/c.a),vec3(1.0/2.2))*c.a : vec3(0);
    if (phLight()) {
        vec3 background = pow(uBackgroundColor.rgb,vec3(2.2))*c.a;
        vec3 scene = pow(filmic(max(c.rgb-background,vec3(0))+bloom*(p_bloom/24.0)
                             +streak*(p_streak/8.0)),vec3(1.0/2.2));
        float strength = max(scene.r,max(scene.g,scene.b));
        vec3 paper = pow(max(min(c.rgb,background),vec3(0)),vec3(1.0/2.2));
        rgb = mix(paper,vec3(0.10,0.16,0.24)+scene*0.16,strength*0.85);
    }
    // A fixed sub-LSB dither prevents banding without animated grain on text.
    vec2 pixel = phWindow(fragCoord);
    float noise = fract(52.9829189*fract(dot(pixel,vec2(0.06711056,0.00583715))))-0.5;
    return vec4(clamp(rgb+noise/255.0,vec3(0),vec3(c.a)),c.a);
}
