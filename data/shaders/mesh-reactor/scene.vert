// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "scene.glsl"
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 worldNormal;
layout(location=2) flat out int material;
layout(location=3) out vec2 surfaceUV;
layout(location=4) flat out float seed;
vec4 pMesh() {
    const ivec2 corners[6]=ivec2[6](ivec2(0,0),ivec2(1,0),ivec2(0,1),ivec2(0,1),ivec2(1,0),ivec2(1,1));
    int cell=gl_VertexIndex/6;
    ivec2 corner=corners[gl_VertexIndex%6];
    vec2 uv=vec2(ivec2(cell%192,cell/192)+corner)/vec2(192,64);
    float u=uv.x*TAU, v=uv.y*TAU, t=iTime*p_speed;
    material=gl_InstanceIndex;
    surfaceUV=uv;
    seed=float(material)*0.17;
    vec3 point,normal;
    mat3 model=ry(t*0.15+0.35)*rz(0.22)*rx(0.12*sin(t*0.3));
    if(material==1) {
        int panelIndex=cell/128, tile=panelIndex%16, face=panelIndex/16;
        vec2 puv=vec2(ivec2(cell%16,(cell%128)/16)+corner)/vec2(16,8);
        seed=hash31(vec3(tile,face,5));
        surfaceUV=puv;
        point=panel(puv,tile);
        normal=normalize(cross(panel(puv+vec2(0.001,0),tile)-panel(puv-vec2(0.001,0),tile),
                               panel(puv+vec2(0,0.001),tile)-panel(puv-vec2(0,0.001),tile)));
        vec3 centre=panel(vec2(0.5),tile);
        mat3 tumble=rx(rupture()*(seed-0.5)*2.0)*ry(rupture()*seed);
        point=tumble*(point-centre)+centre*(1.0+0.12*p_commandHeat+0.12*drive()+0.06*history()+rupture()*(0.5+seed));
        normal=tumble*normal;
        model=model*faceBasis(face);
        // Open service ports expose the core.
        if(seed>0.89) point=centre*0.0;
    } else if(material>=2 && material<=4 || material==11) {
        float radius=1.3+float(material-2)*0.28;
        float tube=material==2 ? 0.035 : 0.018;
        if(material==11) { radius=2.25; tube=0.06; model=mat3(1); }
        else model=model*rx(float(material)*0.74+t*0.1)*rz(float(material)*0.4);
        normal=vec3(cos(u)*cos(v),sin(v),sin(u)*cos(v));
        point=vec3(cos(u)*radius,0,sin(u)*radius)+tube*normal;
        if(material==11) point.y-=1.65;
    } else if(material==12) {
        point=vec3((uv.x-0.5)*80.0,-1.73,(uv.y-0.5)*80.0);
        normal=vec3(0,1,0); model=mat3(1);
    } else {
        float phi=uv.y*TAU*0.5;
        normal=vec3(sin(phi)*cos(u),cos(phi),sin(phi)*sin(u));
        point=normal*(0.84+0.025*drive());
        if(material>=5) {
            float phase=float(material)*2.39996+t*(0.18+float(material)*0.02);
            vec3 orbit=vec3(cos(phase)*2.05,sin(phase*0.7)*0.65,sin(phase)*2.05);
            point=normal*(0.035+0.012*seed)+orbit;
        }
    }
    worldPosition=model*point;
    worldNormal=model*normal;
    vec3 view=transpose(cameraBasis())*(worldPosition-camera());
    float aspect=uWindowSize.x/max(uWindowSize.y,1.0);
    const float nearPlane=0.1,farPlane=40.0,focal=2.65;
    vec4 clipPosition=vec4(view.x*focal/aspect,-view.y*focal,
        farPlane/(nearPlane-farPlane)*view.z+farPlane*nearPlane/(nearPlane-farPlane),-view.z);
    // Crop a single window camera to this pane's buffer viewport.
    clipPosition.xy=(clipPosition.xy*uWindowSize
        +(uWindowSize-2.0*uTerminalRect.xy-uTerminalRect.zw)*clipPosition.w)/uTerminalRect.zw;
    return clipPosition;
}
