// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#define AFTERGLOW_AUDIO 1
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 worldNormal;
layout(location=2) flat out int material;

mat3 rotateX(float a) {
    float c=cos(a), s=sin(a);
    return mat3(1,0,0, 0,c,s, 0,-s,c);
}
mat3 rotateY(float a) {
    float c=cos(a), s=sin(a);
    return mat3(c,0,-s, 0,1,0, s,0,c);
}
void mainMesh(out vec4 clipPosition) {
    const ivec2 corners[6]=ivec2[6](ivec2(0,0),ivec2(1,0),ivec2(0,1),ivec2(0,1),ivec2(1,0),ivec2(1,1));
    int cell=gl_VertexIndex/6;
    vec2 uv=vec2(ivec2(cell%96,cell/96)+corners[gl_VertexIndex%6])/vec2(96,32);
    float u=uv.x*6.2831853, v=uv.y*6.2831853;
    float t=iTime*p_speed;
    material=gl_InstanceIndex;
    float energy=p_audio*getBass()+min(iScrollback.z/20000.0,0.3);
    vec3 point, normal;
    if(material==0) {
        float phi=uv.y*3.14159265;
        normal=vec3(sin(phi)*cos(u),cos(phi),sin(phi)*sin(u));
        point=normal*(0.52+0.06*energy);
    } else {
        float radius=0.78+float(material)*0.25;
        float tube=0.085+0.015*energy;
        normal=vec3(cos(u)*cos(v),sin(v),sin(u)*cos(v));
        point=vec3(cos(u)*radius,0,sin(u)*radius)+tube*normal;
    }
    // Each child inherits the assembly rotation, then applies its own local rotation.
    mat3 assembly=rotateY(t*0.22)*rotateX(0.3);
    mat3 local=rotateX(float(material)*0.72+t*(0.2+float(material)*0.09))*rotateY(t*0.3);
    mat3 model=assembly*local;
    worldPosition=model*point;
    worldNormal=model*normal;
    vec3 view=worldPosition-vec3(0,0,4.4);
    float aspect=iResolution.x/max(iResolution.y,1.0);
    // Vulkan perspective: Y down, depth 0..1, camera looks along negative Z.
    const float nearPlane=0.1, farPlane=40.0, focal=2.5;
    clipPosition=vec4(view.x*focal/aspect,-view.y*focal,
        farPlane/(nearPlane-farPlane)*view.z+farPlane*nearPlane/(nearPlane-farPlane),-view.z);
}
