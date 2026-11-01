// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include <terminal_multipass.glsl>
#include "scene.glsl"
vec3 chamber(vec2 uv) {
    // Centred, aspect-corrected, y up.
    vec2 p=(uv-0.5)*vec2(uWindowSize.x/uWindowSize.y,-1.0);
    float t=sceneTime();
    vec3 cyan=vec3(0.015,0.12,0.20),violet=vec3(0.11,0.015,0.20);
    float n=noise3(vec3(p*3.0,t*0.025));
    float cloud=noise3(vec3(p*6.0+vec2(n),t*0.04));
    vec3 c=mix(cyan,violet,smoothstep(-0.7,0.7,p.x))*(0.07+cloud*0.25);
    c+=energyColor()*exp(-dot(p,p)*4.5)*0.085;
    // Three particle planes with different drift and depth.
    for(int i=0;i<3;++i) {
        float layer=float(i);
        vec2 q=p*(35.0+layer*19.0)+vec2(t*(0.08+layer*0.03),t*0.035);
        vec2 id=floor(q),f=fract(q)-0.5;
        float h=hash31(vec3(id,layer));
        float dotGlow=exp(-dot(f,f)*(180.0+layer*50.0))*step(0.975,h);
        c+=mix(vec3(0.05,0.25,0.5),vec3(0.6,0.15,0.5),h)*dotGlow*(0.25+layer*0.14);
    }
    float shafts=pow(max(0.0,cos(atan(p.y,p.x+0.001)*9.0+t*0.03)),18.0);
    c+=energyColor()*shafts*exp(-length(p)*5.0)*0.055;
    return c;
}
vec4 pTerminal(vec2 uv) {
    // The background layer keeps its cell backgrounds, selections, images and
    // the window's opacity. The scene shows where the window background does.
    vec4 base=terminalTexel(uv);
    float empty=1.0-smoothstep(0.01,0.08,length(base.rgb-uBackgroundColor.rgb*base.a));
    vec2 windowUV=(uTerminalRect.xy+uv*uTerminalRect.zw)/uWindowSize;
    vec4 scene=texture(iChannel0,uv);
    // Soften isolated subpixel edges on every pixel: two-sample MSAA leaves
    // some, and a device without it falls back to one sample.
    vec2 texel=1.0/vec2(textureSize(iChannel0,0));
    vec4 n=texture(iChannel0,uv+vec2(0,texel.y));
    vec4 s=texture(iChannel0,uv-vec2(0,texel.y));
    vec4 e=texture(iChannel0,uv+vec2(texel.x,0));
    vec4 w=texture(iChannel0,uv-vec2(texel.x,0));
    float contrast=length(n.rgb-s.rgb)+length(e.rgb-w.rgb);
    scene=mix(scene,(n+s+e+w)*0.25,0.3*smoothstep(0.2,1.2,contrast));
    vec3 glow=vec3(0);
    // A fixed golden-angle kernel has no repeated eight-point ghost images.
    for(int tap=0;tap<32;++tap) {
        float r=sqrt((float(tap)+0.5)/32.0);
        float angle=float(tap)*2.39996323;
        vec2 offset=vec2(cos(angle),sin(angle))*r*20.0/max(iResolution.y,1.0);
        offset.x*=iResolution.y/iResolution.x;
        glow+=max(texture(iChannel0,uv+offset).rgb-vec3(0.85),vec3(0))*exp(-r*r*3.0)*0.07;
    }
    vec3 radiance=scene.rgb+chamber(windowUV)*(1.0-scene.a);
    if(p_linearOutput<0.5) radiance+=glow;
    float vignette=1.0-0.36*smoothstep(0.18,0.8,length(windowUV-0.5));
    radiance*=p_exposure*vignette;
    if(p_linearOutput>0.5) return vec4(mix(toLinearPremultiplied(base).rgb,radiance*base.a,empty),base.a);
    // ACES fitted display curve; preserve bright cyan and warm metal edges.
    vec3 mapped=clamp((radiance*(2.51*radiance+0.03))/(radiance*(2.43*radiance+0.59)+0.14),0.0,1.0);
    return vec4(mix(base.rgb,linearToSrgb(mapped)*base.a,empty),base.a);
}
