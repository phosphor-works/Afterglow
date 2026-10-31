// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#define AFTERGLOW_HDR 1
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) flat in int material;
void mainImage(out vec4 color,in vec2 pixel) {
    vec3 n=normalize(worldNormal), v=normalize(vec3(0,0,4.4)-worldPosition);
    vec3 l=normalize(vec3(-2,3,4)-worldPosition);
    vec3 tint=mix(vec3(0.06,0.6,1.0),vec3(0.75,0.08,0.65),float(material)/3.0);
    float diffuse=max(dot(n,l),0.0);
    float specular=pow(max(dot(n,normalize(l+v)),0.0),96.0);
    float rim=pow(1.0-max(dot(n,v),0.0),3.0);
    vec3 reflected=reflect(-v,n);
    float strip=pow(max(0.0,1.0-abs(reflected.y-0.35)),40.0);
    vec3 light=tint*(0.10+0.45*diffuse+1.2*rim)+vec3(2.5)*specular+vec3(0.65,0.8,1.0)*strip;
    if(material==0) light=tint*(1.2+getOverall()*p_audio*2.0)+vec3(0.4)*rim;
    color=vec4(light,1);
}
