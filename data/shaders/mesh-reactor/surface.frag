// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "scene.glsl"
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) flat in int material;
layout(location=3) in vec2 surfaceUV;
layout(location=4) flat in float seed;

vec3 environment(vec3 r,float roughness) {
    vec3 blue=vec3(0.12,0.45,0.9),pink=vec3(0.7,0.10,0.42);
    float card=pow(max(dot(r,normalize(vec3(-1,1.5,2))),0.0),mix(160.0,12.0,roughness));
    float rim=pow(max(dot(r,normalize(vec3(1,0.5,-1))),0.0),mix(100.0,8.0,roughness));
    float strip=exp(-abs(r.y-0.45)*mix(110.0,14.0,roughness))*smoothstep(-0.4,0.5,r.z);
    return vec3(0.018,0.025,0.045)+blue*card*5.0+pink*rim*4.0+vec3(1.6,1.9,2.3)*strip;
}
vec4 pTerminal(vec2 uv) {
    vec3 n=normalize(worldNormal),view=normalize(camera()-worldPosition);
    vec3 energy=energyColor();
    float t=iTime*p_speed;
    float fresnel=pow(1.0-max(dot(n,view),0.0),5.0);
    vec3 light;
    if(material==0) {
        vec3 p=worldPosition*4.0;
        float a=noise3(p+vec3(0,t*0.4,0));
        float b=noise3(p*2.7+vec3(a*2.0,-t*0.3,a));
        float veins=pow(1.0-abs(b*2.0-1.0),14.0);
        light=energy*(0.18+veins*(2.4+drive()*2.0)+a*0.25)+vec3(0.55,0.8,1)*pow(veins,4.0)*2.0;
    } else if(material==12) {
        vec2 p=worldPosition.xz;
        float radius=length(p);
        vec2 g=abs(fract(p*0.8+0.5)-0.5)/max(fwidth(p*0.8),vec2(0.001));
        float grid=1.0-smoothstep(0.4,1.5,min(g.x,g.y));
        float ring=exp(-abs(radius-2.25)*28.0);
        float pool=exp(-radius*radius*0.35);
        float shadow=smoothstep(0.25,1.7,radius);
        light=vec3(0.014,0.023,0.04)+energy*(pool*0.16*shadow+grid*0.018+ring*0.20);
        light+=environment(reflect(-view,n),0.5)*0.07;
        float age=signalAge(3);
        float shock=exp(-pow((radius-age*3.0)/0.10,2.0))*exp(-age);
        if(signalValue(3)!=0.0) light+=energy*shock*0.9;
        light*=exp(-radius*0.13);
    } else if(material>=5 && material<=10) {
        light=energy*1.1+vec3(0.5)*fresnel;
    } else {
        float edge=min(min(surfaceUV.x,1.0-surfaceUV.x),min(surfaceUV.y,1.0-surfaceUV.y));
        float seam=material==1 ? 1.0-smoothstep(0.018,0.045,edge) : 0.0;
        float grooves=material==1 ? smoothstep(0.96,0.995,cos(surfaceUV.y*TAU*12.0)) : 0.0;
        grooves*=1.0-smoothstep(0.2,0.7,fwidth(surfaceUV.y*12.0));
        float roughness=0.20+seed*0.16;
        light=environment(reflect(-view,n),roughness)*(0.3+0.7*fresnel);
        float key=max(dot(n,normalize(vec3(-2,3,4)-worldPosition)),0.0);
        light+=vec3(0.10,0.16,0.23)*key*(1.0-grooves*0.35);
        light+=energy*(seam*0.40+pow(1.0-max(dot(n,view),0.0),3.0)*0.24);
        if(material!=1) {
            float band=pow(max(cos(surfaceUV.x*TAU*3.0-iTime*0.35),0.0),28.0);
            light+=energy*(0.035+band*0.9)*(0.7+drive()*0.6);
        } else {
            // Recessed panel number strips and one status lamp per tile.
            float label=step(0.69,surfaceUV.y)*step(surfaceUV.y,0.74)
                       *step(0.2,surfaceUV.x)*step(surfaceUV.x,0.6)
                       *step(0.45,fract(surfaceUV.x*45.0+seed));
            float lamp=1.0-smoothstep(0.014,0.025,length(surfaceUV-vec2(0.77,0.73)));
            light+=vec3(0.3,0.6,0.8)*label*0.18+energy*lamp*2.0;
        }
    }
    float distanceToCamera=length(camera()-worldPosition);
    float fog=1.0-exp(-distanceToCamera*0.025);
    light=mix(light,vec3(0.018,0.025,0.06),fog);
    float alpha=material==12 ? exp(-pow(length(worldPosition.xz)/10.0,2.0)) : 1.0;
    return vec4(light*alpha,alpha);
}
