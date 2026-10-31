// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
void mainImage(out vec4 color,in vec2 pixel) {
    vec2 uv=pixel/iResolution.xy;
    vec4 scene=texture(iBuffer0,uv);
    vec3 glow=vec3(0);
    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
        vec3 sampleColor=texture(iBuffer0,uv+vec2(x,y)*3.0/iResolution.xy).rgb;
        glow+=max(sampleColor-vec3(0.7),vec3(0))/25.0;
    }
    vec3 radiance=(scene.rgb+glow*0.35)*p_exposure;
    vec3 mapped=linearToSrgb(vec3(1)-exp(-radiance));
    vec4 background=texture(iChannel0,uv);
    color=vec4(mapped+background.rgb*(1.0-scene.a),max(scene.a,background.a));
    color.rgb=min(color.rgb,vec3(color.a));
}
