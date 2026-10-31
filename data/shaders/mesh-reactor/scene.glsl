// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
const float TAU=6.28318530718;
float hash31(vec3 p) {
    p=fract(p*0.1031); p+=dot(p,p.yzx+33.33);
    return fract((p.x+p.y)*p.z);
}
float noise3(vec3 p) {
    vec3 q=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(mix(hash31(q),hash31(q+vec3(1,0,0)),f.x),
                   mix(hash31(q+vec3(0,1,0)),hash31(q+vec3(1,1,0)),f.x),f.y),
               mix(mix(hash31(q+vec3(0,0,1)),hash31(q+vec3(1,0,1)),f.x),
                   mix(hash31(q+vec3(0,1,1)),hash31(q+vec3(1,1,1)),f.x),f.y),f.z);
}
float history() { return 1.0-exp(-max(iScrollback.z,0.0)/4000.0); }
float drive() { return p_audio*smoothstep(0.01,0.8,getBass()); }
float rupture() {
    float age=max(0.0,iTime-iSignal[0].y);
    float r=(1.0-exp(-age*5.0))*exp(-max(age-3.0,0.0)*0.8);
    if(iSignal[0].x==0.0) r=0.0;
    if(iSignal[1].y>iSignal[0].y) r*=exp(-pow(max(0.0,iTime-iSignal[1].y)*2.0,2.0));
    return r;
}
vec3 energyColor() {
    return mix(mix(vec3(0.025,0.65,1.0),vec3(0.45,0.12,1.0),history()*0.45),
               vec3(1.0,0.12,0.025),rupture()*0.85);
}
mat3 rx(float a) { float c=cos(a),s=sin(a); return mat3(1,0,0,0,c,s,0,-s,c); }
mat3 ry(float a) { float c=cos(a),s=sin(a); return mat3(c,0,-s,0,1,0,s,0,c); }
mat3 rz(float a) { float c=cos(a),s=sin(a); return mat3(c,s,0,-s,c,0,0,0,1); }
vec3 camera() { return vec3(0,1.35,6.5+rupture()*1.2); }
mat3 cameraBasis() {
    vec3 z=normalize(camera()), x=normalize(cross(vec3(0,1,0),z));
    return mat3(x,cross(z,x),z);
}
mat3 faceBasis(int face) {
    if(face==0) return ry(TAU*0.25);
    if(face==1) return ry(-TAU*0.25);
    if(face==2) return rx(-TAU*0.25);
    if(face==3) return rx(TAU*0.25);
    if(face==4) return mat3(1);
    return ry(TAU*0.5);
}
vec3 panel(vec2 uv, int tile) {
    vec2 cell=vec2(tile%4,tile/4);
    vec2 p=(cell+mix(vec2(0.065),vec2(0.935),uv))*0.5-1.0;
    float edge=min(min(uv.x,1.0-uv.x),min(uv.y,1.0-uv.y));
    float bevel=smoothstep(0.0,0.12,edge);
    return normalize(vec3(p,1))*(1.045+0.035*bevel);
}
