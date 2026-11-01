// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include <terminal_audio.glsl>
#include "phosphor.glsl"

const float PI = 3.141592654;
const float FAR = 1000.0;
// The analyzer supplies smoothed, normalized music energy. A soft knee lifts
// quieter detail without letting loud tracks overwhelm the material lighting.
float audioDrive(float level) { return p_audioStrength * smoothstep(0.015, 0.72, level); }
float bassDrive() { return audioDrive(getBass()); }
float midDrive() { return audioDrive(getMids()); }
float trebleDrive() { return audioDrive(getTreble()); }
// Retained history sets the scene's baseline; music and Luau stay transient.
// Eased rows include only this tab's panes. Exponential saturation bounds
// even very large buffers and preserves a useful response for short sessions.
float historyDrive() {
    return p_historyStrength * (1.0 - exp(-max(uScrollback.z,0.0)/max(p_historyScale,1.0)));
}
float hash21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
mat3 rotation(vec3 a, float t) {
    a = normalize(a);
    float s = sin(t), c = cos(t), k = 1.0 - c;
    return mat3(c+a.x*a.x*k, a.y*a.x*k+a.z*s, a.z*a.x*k-a.y*s,
                a.x*a.y*k-a.z*s, c+a.y*a.y*k, a.z*a.y*k+a.x*s,
                a.x*a.z*k+a.y*s, a.y*a.z*k-a.x*s, c+a.z*a.z*k);
}
vec2 sphere(vec3 ro, vec3 rd, float radius) {
    float b = dot(ro, rd), h = b*b - dot(ro, ro) + radius*radius;
    if (h < 0.0) return vec2(FAR, -FAR);
    h = sqrt(h);
    return vec2(-b-h, -b+h);
}
float roundedMax(float a, float b, float radius) {
    float h = max(radius - abs(a-b), 0.0) / radius;
    return max(a,b) + h*h*radius*0.25;
}
// A piece of a spherical shell, clipped by four planes. Rounded intersections
// form real bevel geometry, including the silhouette, rather than painted edges.
float panelField(vec3 p, vec2 lo, vec2 hi) {
    float d = abs(length(p)-1.058) - 0.042;
    d = roundedMax(d, (lo.x*p.z-p.x)/sqrt(1.0+lo.x*lo.x), 0.012);
    d = roundedMax(d, (p.x-hi.x*p.z)/sqrt(1.0+hi.x*hi.x), 0.012);
    d = roundedMax(d, (lo.y*p.z-p.y)/sqrt(1.0+lo.y*lo.y), 0.012);
    return roundedMax(d, (p.y-hi.y*p.z)/sqrt(1.0+hi.y*hi.y), 0.012);
}
vec3 panelNormal(vec3 p, vec2 lo, vec2 hi) {
    vec2 e = vec2(0.0005, -0.0005);
    return normalize(e.xyy*panelField(p+e.xyy,lo,hi) + e.yyx*panelField(p+e.yyx,lo,hi)
                   + e.yxy*panelField(p+e.yxy,lo,hi) + e.xxx*panelField(p+e.xxx,lo,hi));
}
mat3 faceBasis(int face) {
    if (face == 0) return mat3(vec3(0,0,-1), vec3(0,1,0), vec3(1,0,0));
    if (face == 1) return mat3(vec3(0,0,1), vec3(0,1,0), vec3(-1,0,0));
    if (face == 2) return mat3(vec3(1,0,0), vec3(0,0,-1), vec3(0,1,0));
    if (face == 3) return mat3(vec3(1,0,0), vec3(0,0,1), vec3(0,-1,0));
    if (face == 4) return mat3(1.0);
    return mat3(vec3(-1,0,0), vec3(0,1,0), vec3(0,0,-1));
}
// Broad reflected lighting cards with narrow cores give the metal readable
// curvature. These are evaluated in world space, so reflections slide as it turns.
vec3 environment(vec3 r, float roughness) {
    vec3 c = vec3(0.025,0.04,0.065) * (0.45+0.55*max(r.y,0.0));
    vec3 a = normalize(vec3(-0.8,1.3,1.2));
    vec3 b = normalize(vec3(1.0,0.15,-0.5));
    float key = exp(-pow(abs(dot(r, normalize(vec3(a.z,0,-a.x))))/(0.12+roughness),4.0))
              * pow(max(dot(r,a),0.0), 5.0);
    float rim = pow(max(dot(r,b),0.0), 26.0/(1.0+roughness*10.0));
    float fill = pow(max(dot(r,normalize(vec3(-1,-0.4,0.2))),0.0), 10.0);
    return c + vec3(0.68,0.84,1.0)*key*5.5 + vec3(1.0,0.63,0.32)*rim*3.4
             + vec3(0.12,0.33,0.55)*fill*0.5;
}
float ruptureAt(float failureAge, float successAge) {
    // A quick impulse, a suspended moment, then a damped return. No snapping
    // when recall starts while pieces are still moving outwards.
    float x = (1.0-exp(-failureAge*5.0))*exp(-max(failureAge-4.0,0.0)*0.65);
    if (phAge(1) < phAge(0)) x *= exp(-successAge*successAge*2.0);
    return x;
}

vec4 traceObject(vec3 ro, vec3 rd, float time) {
    if (sphere(ro,rd,4.1).y <= 0.0) return vec4(0,0,0,FAR);
    float t = time*p_speed;
    // Signal ages at this sample's moment, which the shutter can put slightly
    // in the past.
    float lag = timeSeconds()-time;
    float failureAge = max(0.0,phAge(0)-lag), successAge = max(0.0,phAge(1)-lag);
    float heat = p_commandHeat;
    float bass = bassDrive(), mids = midDrive(), treble = trebleDrive();
    float history = historyDrive();
    float rupture = ruptureAt(failureAge,successAge);
    float impact = exp(-failureAge*5.0), release = exp(-successAge*3.0);
    vec3 energy = mix(vec3(0.10,0.7,1.0),vec3(0.48,0.22,1.0),0.15+0.1*sin(t));
    energy = mix(energy,vec3(0.40,0.28,1.0),history*0.24);
    energy = mix(energy,vec3(1.0,0.16,0.07),rupture*0.72);
    energy = mix(energy,vec3(0.35,1.0,0.78),release*0.5);
    mat3 object = rotation(vec3(0,1,0),t*0.16+0.35)*rotation(vec3(0,0,1),0.24)
                * rotation(vec3(1,0,0),0.16*sin(t*0.17));
    vec3 o = transpose(object)*ro, d = transpose(object)*rd;
    float hitDistance = FAR, seed = 0.0, edge = 0.0, radius = 0.0;
    vec3 normal = vec3(0), point = vec3(0);
    vec2 panelUv = vec2(0);
    // 96 curved panels. A conservative bounding sphere rejects almost every
    // panel before its rounded surface is marched. The outer sphere is exact.
    if (sphere(o,d,1.6+bass*0.22+history*0.06+rupture*1.35).y > 0.0) {
        for (int face = 0; face < 6; ++face) {
            mat3 basis = faceBasis(face);
            vec3 fo = transpose(basis)*o, fd = transpose(basis)*d;
            for (int tile = 0; tile < 16; ++tile) {
                vec2 cell = vec2(tile%4,tile/4);
                vec2 lo = cell*0.5-1.0+0.014, hi = lo+0.472;
                float id = hash21(vec2(tile,face)+5.0);
                if (id > 0.88) continue; // Open service ports reveal the plasma at rest.
                vec3 centre = normalize(vec3((lo+hi)*0.5,1.0))*1.058;
                vec3 moved = centre*(1.0+heat*0.12+bass*0.20+history*0.055+rupture*(0.5+id*0.8));
                vec2 bound = sphere(fo-moved,fd,0.44);
                if (bound.y < 0.0 || bound.x > hitDistance) continue;
                mat3 tumble = rotation(vec3(id-0.5,0.7,0.35),rupture*(id-0.4)*2.2);
                vec3 localO = transpose(tumble)*(fo-moved)+centre;
                vec3 localD = transpose(tumble)*fd;
                vec2 shell = sphere(localO,localD,1.1);
                float travel = max(0.0,max(bound.x,shell.x));
                float limit = min(hitDistance,min(bound.y,shell.y));
                for (int stepIndex = 0; stepIndex < 28; ++stepIndex) {
                    if (travel > limit) break;
                    vec3 p = localO+localD*travel;
                    float distance = panelField(p,lo,hi);
                    if (distance < 0.00035) {
                        hitDistance = travel;
                        point = ro+rd*travel;
                        normal = normalize(object*basis*tumble*panelNormal(p,lo,hi));
                        radius = length(p);
                        vec2 q = p.xy/max(p.z,0.001);
                        panelUv = (q-lo)/(hi-lo);
                        edge = min(min(q.x-lo.x,hi.x-q.x),min(q.y-lo.y,hi.y-q.y))*p.z;
                        seed = id;
                        break;
                    }
                    travel += max(distance*0.86,0.0002);
                }
            }
        }
    }

    float surfaceDepth = hitDistance;
    vec3 color = vec3(0.0);
    if (hitDistance < FAR) {
        vec3 n = normal;
        float facing = clamp(dot(n,-rd),0.0,1.0);
        float fresnel = pow(1.0-facing,5.0);
        vec3 reflectance = mix(vec3(0.23,0.30,0.37),vec3(0.55,0.62,0.66),seed*0.45);
        float roughness = 0.065+seed*0.035;
        color = environment(reflect(rd,n),roughness)*mix(reflectance,vec3(0.95),fresnel);
        color += vec3(0.006,0.011,0.018)*(0.4+max(n.y,0.0));
        // Recessed perimeter, sparse etched registration lines and micro-grain.
        float recess = smoothstep(0.004,0.023,edge);
        color *= mix(0.45,1.0,recess);
        float engraving = exp(-abs(edge-0.032)*1300.0)*0.45;
        float ticks = step(0.82,fract(panelUv.x*14.0))*step(panelUv.y,0.14)*step(0.06,panelUv.y);
        color *= 1.0-engraving-ticks*0.12;
        float grain = sin(panelUv.y*180.0)*sin(panelUv.x*71.0)*0.012;
        color *= 1.0+grain;
        float seam = exp(-max(edge,0.0)*420.0)*(0.035+heat*0.14+bass*0.20+history*0.09);
        float underside = 1.0-smoothstep(1.025,1.06,radius);
        float indicator = exp(-length((panelUv-vec2(0.18,0.22))*vec2(1,3))*180.0);
        color += energy*(seam+underside*(0.055+heat*0.18)*(0.12+0.88*exp(-max(edge,0.0)*100.0))+indicator*(3.0+history*4.0+audioDrive(audioBarSmooth(seed))*12.0));
        // Light scattered from the reactor onto inward-facing fragments.
        color += energy*max(dot(n,normalize(-point)),0.0)*(0.07+heat*0.14+bass*0.28)/(0.5+dot(point,point));
    }

    // A rotating participating volume: nested, twisted plasma filaments inside
    // a glassy spherical boundary. Integrate only in front of opaque fragments.
    vec2 volume = sphere(ro,rd,0.91);
    float enter = max(volume.x,0.0), leave = min(volume.y,hitDistance);
    if (leave > enter) {
        surfaceDepth = min(surfaceDepth,enter);
        float stepLength = (leave-enter)/48.0;
        float transmission = 1.0;
        vec3 volumeLight = vec3(0);
        mat3 core = rotation(vec3(0.3,1,0.2),-t*0.36);
        for (int i = 0; i < 48; ++i) {
            vec3 p = core*(ro+rd*(enter+(float(i)+0.5)*stepLength));
            float r = length(p);
            vec3 q = p*(9.0+history*1.8);
            q.x += sin(p.y*5.0+t)*(0.8+mids*0.65);
            float gyroid = dot(sin(q+vec3(t*0.7,0,t*0.3)),cos(q.yzx));
            float filament = exp(-abs(gyroid)*16.0);
            float envelope = (1.0-smoothstep(0.75,0.91,r))*smoothstep(0.15,0.5,r);
            float density = filament*envelope;
            float hot = exp(-r*r*14.0);
            vec3 plasma = mix(energy,vec3(0.52,0.2,1.0),smoothstep(-0.4,0.5,p.y)*0.45);
            volumeLight += transmission*(plasma*density*(4.0+heat*10.0+bass*18.0+history*5.0) + vec3(0.7,0.9,1.0)*hot*(2.0+bass*4.0))*stepLength;
            transmission *= exp(-(density*1.8+hot*0.5)*stepLength);
        }
        color = color*transmission+volumeLight;
        vec3 n = normalize(ro+rd*enter);
        float fresnel = pow(1.0-max(dot(n,-rd),0.0),3.0);
        color += environment(reflect(rd,n),0.04)*fresnel*0.16 + energy*fresnel*0.18;
    }

    // Three thin, occluded orbital traces with machined breaks and circulating
    // energy. Pixel-footprint filtering keeps subpixel curves stable at any DPR.
    for (int ring = 0; ring < 3; ++ring) {
        float index = float(ring);
        mat3 plane = rotation(vec3(0,0,1),0.20+index*0.55)*rotation(vec3(1,0,0),0.30+index*0.44);
        vec3 ringO = transpose(plane)*ro, ringD = transpose(plane)*rd;
        if (abs(ringD.y) < 0.001) continue;
        float distance = -ringO.y/ringD.y;
        if (distance <= 0.0 || distance >= hitDistance) continue;
        vec3 p = ringO+ringD*distance;
        float radius = 1.47+index*0.15+heat*0.035+bass*0.065;
        float radial = abs(length(p.xz)-radius);
        float footprint = max(0.002,distance/uWindowSize.y/max(abs(ringD.y),0.12));
        float line = exp(-phSq(radial/footprint))*0.003/footprint;
        float angle = atan(p.z,p.x);
        float arc = smoothstep(-0.3,0.2,sin(angle*(2.0+index)+t*0.15+index));
        float runner = pow(max(0.0,cos(angle-t*(0.8+index*0.1)+index*2.0)),110.0);
        vec3 tint = ring == 1 ? vec3(0.55,0.28,0.95) : energy;
        // Angular spectrum detail travels with the ring; broad energy still
        // reads as a pulse, rather than turning the scene into a bar chart.
        float spectral = audioDrive(audioBarSmooth(fract(angle/(2.0*PI)+index/3.0)));
        color += tint*line*((0.20+history*0.18)*arc+spectral*1.6+runner*(2.0+heat*3.0+treble*5.0));
    }

    // Curved magnetic filaments run through the surrounding space. Segment
    // distance is filtered by the ray footprint, with real opaque occlusion.
    if (sphere(ro,rd,1.8+rupture*1.2).y > 0.0) {
        for (int strand = 0; strand < 10; ++strand) {
            float index = float(strand), phase = index*2.39996+t*0.28;
            mat3 strandRotation = rotation(vec3(0,0,1),sin(index*3.0)*0.4);
            vec3 previous = vec3(0);
            for (int segment = 0; segment <= 32; ++segment) {
                float u = float(segment)/32.0;
                float angle = phase+u*(2.2+rupture*0.8)+mids*0.16*sin(u*PI*3.0+index);
                float r = 0.85+u*(0.65+rupture*1.1);
                vec3 p = vec3(cos(angle)*r,sin(angle*1.3+index)*0.5+sin(u*PI)*0.5,sin(angle)*r);
                p = strandRotation*p;
                if (segment > 0) {
                    vec3 v = p-previous, w = ro-previous;
                    float b = dot(rd,v), c = dot(v,v), e = dot(v,w);
                    float alongSegment = clamp((e-b*dot(rd,w))/max(c-b*b,0.00001),0.0,1.0);
                    vec3 nearest = previous+v*alongSegment;
                    float alongRay = dot(nearest-ro,rd);
                    if (alongRay > 0.0 && alongRay < hitDistance) {
                        float distance = length(ro+rd*alongRay-nearest);
                        float width = max(0.002,alongRay/uWindowSize.y*0.6);
                        float line = exp(-phSq(distance/width))*0.002/width;
                        float wave = pow(max(0.0,sin(u*7.0-t*2.0+index)),8.0);
                        vec3 tint = mix(energy,vec3(0.4,0.18,1.0),0.35+0.25*sin(index));
                        color += tint*line*(0.035+heat*0.18+rupture*0.4+mids*0.50+history*0.20)*(0.15+wave*2.0);
                    }
                }
                previous = p;
            }
        }
    }

    // Depth-distributed motes: a sharp point in the focal plane, a soft disc in
    // front and behind it. Their projection and occlusion share the 3D camera.
    for (int i = 0; i < 36; ++i) {
        float id = float(i), h = hash21(vec2(id,91.3));
        float angle = id*2.39996+t*(0.05+h*0.07);
        float radius = 1.5+h*1.5+rupture*0.35;
        vec3 p = vec3(cos(angle)*radius,(hash21(vec2(id,17))-0.5)*3.1,sin(angle)*radius);
        float along = dot(p-ro,rd);
        if (along < 0.0 || along > hitDistance) continue;
        float distance = length(ro+rd*along-p);
        float coc = 0.003+abs(along-6.2)*0.008;
        float mote = exp(-phSq(distance/coc))*0.00005/(coc*coc);
        color += mix(energy,vec3(1,0.67,0.38),h)*mote*(0.25+heat*0.4+history*0.45+audioDrive(audioBarSmooth(0.65+h*0.35))*3.5);
    }
    float closest = min(max(-dot(ro,rd),0.0),hitDistance);
    float radial = length(ro+rd*closest);
    color += energy*exp(-radial*radial*1.6)*(0.012+heat*0.024+impact*0.13+release*0.04+bass*0.065);
    return vec4(max(color,vec3(0.0)),surfaceDepth);
}

float noise3(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f*f*(3.0-2.0*f);
    vec4 h = vec4(0,1,157,158)+dot(i,vec3(1,157,113));
    vec4 a = fract(sin(h)*43758.5453), b = fract(sin(h+113.0)*43758.5453);
    vec4 n = mix(a,b,f.z);
    return mix(mix(n.x,n.y,f.x),mix(n.z,n.w,f.x),f.y);
}
float fbm(vec3 p) {
    return noise3(p)*0.57+noise3(p*2.03+7.1)*0.28+noise3(p*4.07-3.4)*0.15;
}
// An illuminated haze behind the subject: slow sheets of turbulence, narrow
// caustic strands and shafts through suspended dust. It spans the full window.
vec3 atmosphere(vec3 rd, float t, float heat) {
    vec2 p = rd.xy/max(-rd.z,0.15);
    vec3 q = vec3(p*3.0,t*0.035);
    float warp = fbm(q+vec3(0,t*0.05,0));
    float clouds = fbm(q*2.3+warp*2.0);
    float diagonal = p.y-p.x*0.32+0.12;
    float curtain = exp(-abs(diagonal+(warp-0.5)*0.3)*6.0);
    float wisps = pow(max(0.0,clouds-0.22),2.0)*curtain;
    float thread = exp(-abs(diagonal+(clouds-0.5)*0.18)*105.0);
    vec3 tint = mix(vec3(0.035,0.23,0.34),vec3(0.24,0.065,0.38),smoothstep(-0.25,0.65,p.x));
    vec3 color = tint*(0.008+wisps*1.4)*(0.5+heat*0.5);
    color += tint*thread*(0.04+heat*0.14)*clouds;
    // Diverging light shafts from the rear of the chamber, modulated by haze.
    float shafts = pow(max(0.0,sin(atan(p.x+0.1,p.y-0.6)*27.0+t*0.08)),12.0);
    color += vec3(0.04,0.16,0.21)*shafts*curtain*(0.03+heat*0.1)*clouds;
    color += vec3(0.012,0.021,0.042)*exp(-length(p-vec2(0.35,0.1))*2.0);
    return color;
}
vec3 traceScene(vec2 pixel, float shutter) {
    // The full clock, so the scene does not jump when iTime wraps.
    float time = max(0.0,timeSeconds()-shutter), t = time*p_speed;
    float heat = p_commandHeat;
    vec2 uv = (pixel-uWindowSize*vec2(0.60,0.55))/uWindowSize.y;
    // The pointer anywhere in the window steers the one camera all panes share.
    vec2 pointer = uWindowMouse.x < 0.0 ? vec2(0.0)
        : clamp(vec2(uWindowMouse.x,uWindowSize.y-uWindowMouse.y)/uWindowSize-0.5,-0.5,0.5);
    mat3 camera = rotation(vec3(0,1,0),sin(t*0.12)*0.16+pointer.x*p_parallax)
                * rotation(vec3(1,0,0),-0.12-pointer.y*p_parallax*0.24);
    vec3 ro = camera*vec3(0,0.45,6.2), rd = camera*normalize(vec3(uv*3.2,-3.2));
    vec4 subject = traceObject(ro,rd,time);
    vec3 haze = atmosphere(rd,t,heat+midDrive()*0.7+historyDrive()*0.8);
    vec3 color = subject.rgb+haze*(subject.a < FAR ? 0.12 : 1.0);
    float ground = abs(rd.y) > 0.0001 ? (-1.48-ro.y)/rd.y : FAR;
    if (ground > 0.0 && ground < subject.a) {
        vec3 p = ro+rd*ground;
        float radial = length(p.xz);
        float rough = noise3(vec3(p.xz*8.0,0.0));
        vec3 n = normalize(vec3(sin(p.x*17.0+p.z*9.0),500.0,cos(p.z*13.0-p.x*8.0)));
        vec3 reflectedRay = reflect(rd,n);
        vec4 reflected = traceObject(p+n*0.002,reflectedRay,time);
        float fresnel = 0.16+pow(1.0-max(dot(n,-rd),0.0),5.0)*0.45;
        vec3 floorColor = vec3(0.001,0.002,0.004)+reflected.rgb*fresnel;
        floorColor += atmosphere(reflectedRay,t,heat+historyDrive()*0.8)*0.28;
        // Concentric machined grooves pick up the core's moving caustic light.
        float width = max(0.003,ground/uWindowSize.y/max(abs(rd.y),0.08));
        float rings = abs(fract(radial*2.4+0.5)-0.5)/2.4;
        float etched = exp(-phSq(rings/width))*0.0025/width;
        float ripple = sin(radial*16.0-t*1.6+fbm(vec3(p.xz*1.4,t*0.07))*4.0);
        vec3 cyan = vec3(0.035,0.55,0.8), violet = vec3(0.35,0.07,0.65);
        vec3 tint = mix(cyan,violet,smoothstep(-1.0,2.0,p.x));
        floorColor += tint*(etched*0.015+pow(max(0.0,ripple),18.0)*(0.008+heat*0.018+bassDrive()*0.045+historyDrive()*0.025))
                    *exp(-radial*0.33)*(0.65+rough*0.35);
        float age = min(phAge(0),min(phAge(1),phAge(3)));
        float shock = exp(-phSq((radial-age*4.0)/0.07))*exp(-age*0.8);
        floorColor += mix(cyan,vec3(1.0,0.18,0.05),exp(-phAge(0)))*shock*0.8;
        floorColor += tint*exp(-radial*radial*0.4)*(0.008+heat*0.025+historyDrive()*0.025);
        float fog = 1.0-exp(-ground*0.085);
        color = mix(floorColor,haze,fog);
    }
    return color;
}

vec4 pTerminal(vec2 uv) {
    vec4 base = terminalTexel(uv);
    vec2 pixel = phWindow(terminalPixel(uv));
    // Four rotated-grid samples. A short shutter softens fast fracture motion
    // without smearing terminal text or accumulating stale scene history.
    int samples = p_quality > 1.5 ? 4 : (p_quality > 0.5 ? 2 : 1);
    const vec2 offsets[4] = vec2[4](vec2(-0.375,-0.125),vec2(0.125,-0.375),vec2(0.375,0.125),vec2(-0.125,0.375));
    vec3 light = vec3(0);
    for (int i = 0; i < samples; ++i) {
        float shutter = float(i)/float(samples)*min(max(iTimeDelta,0.0),1.0/30.0)*p_shutter;
        int sampleIndex = samples == 2 ? i*2 : i;
        light += traceScene(pixel+(samples == 1 ? vec2(0) : offsets[sampleIndex]),shutter);
    }
    light *= p_intensity*(1.0+historyDrive()*0.18)/float(samples);
    float empty = 1.0-smoothstep(0.01,0.08,length(base.rgb-uBackgroundColor.rgb*base.a));
    // Store scene-linear radiance in RGBA16F. The lens pass performs bloom
    // before tone mapping; glyphs are composited afterward and remain crisp.
    // The transfer works on straight colour: undo and redo the premultiply.
    vec3 linearBase = base.a > 0.0 ? pow(max(base.rgb/base.a,vec3(0)),vec3(2.2))*base.a : vec3(0);
    return vec4(linearBase+light*base.a*empty,base.a);
}
