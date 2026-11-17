#ifndef VID_REFLECTION_H
#define VID_REFLECTION_H

// shader helpers use camera-space surface position/normal and world-space cube axes
#define VID_CUBE_REFLECTION_GLSL \
    "vec3 vidCubeRay(vec3 position,vec3 normal,vec3 row0,vec3 row1,vec3 row2){" \
    "vec3 r=reflect(normalize(position),normalize(normal));" \
    "return vec3(dot(row0,r),dot(row1,r),dot(row2,r));}" \
    "float vidCubeFresnel(float nDotV,float base){" \
    "return base+(1.0-base)*pow(1.0-clamp(abs(nDotV),0.0,1.0),5.0);}" \
    "vec3 vidCubeSurface(samplerCube image,vec3 ray,float roughness,float maxLod," \
    "float coat,float nDotV,float coatF0){" \
    "float r=clamp(roughness,0.0,1.0);vec3 color=textureLod(image,ray,maxLod*r*r).rgb;" \
    "if(coat>0.0)color=mix(color,textureLod(image,ray,0.0).rgb," \
    "clamp(coat,0.0,1.0)*vidCubeFresnel(nDotV,coatF0));return color;}\n"

// local planar depth reconstruction; invalid/sky samples keep the direction-only ray
#define VID_CUBE_PARALLAX_GLSL \
    "bool vidCubeDepthPoint(highp samplerCube image,vec3 ray,vec2 clip,out vec3 point){" \
    "float depth=textureLod(image,ray,0.0).r;if(depth>=1.0||clip.x<=0.0||clip.y<=clip.x)return false;" \
    "float major=max(max(abs(ray.x),abs(ray.y)),abs(ray.z));" \
    "float distance=clip.x*clip.y/(clip.y-depth*(clip.y-clip.x));point=ray*(distance/max(major,0.000001));return true;}" \
    "vec3 vidCubeParallaxRay(highp samplerCube image,vec3 ray,vec3 offset,vec2 clip,float texel,vec4 limits){" \
    "vec3 ar=abs(ray);float major=max(max(ar.x,ar.y),ar.z),second=max(min(ar.x,ar.y),min(max(ar.x,ar.y),ar.z));" \
    "if(major-second<major*texel*2.0)return ray;vec3 q0,q1,q2;if(!vidCubeDepthPoint(image,ray,clip,q0))return ray;" \
    "vec3 axis=abs(ray.y)<0.9?vec3(0.0,1.0,0.0):vec3(1.0,0.0,0.0);" \
    "vec3 tangent=normalize(cross(ray,axis)),bitangent=cross(ray,tangent);" \
    "if(!vidCubeDepthPoint(image,normalize(ray+tangent*texel),clip,q1)||!vidCubeDepthPoint(image,normalize(ray+bitangent*texel),clip,q2))return ray;" \
    "float scale=length(q0);if(length(offset)>scale*limits.x)return ray;q0/=scale;q1/=scale;q2/=scale;offset/=scale;" \
    "vec3 a=q1-q0,b=q2-q0;if(max(length(a),length(b))>limits.y)return ray;" \
    "vec3 normal=cross(a,b);float n2=dot(normal,normal),denom=dot(normal,ray);" \
    "if(n2<1e-16||abs(denom)<sqrt(n2)*limits.w)return ray;" \
    "float distance=dot(normal,q0-offset)/denom;if(distance<=0.0)return ray;" \
    "vec3 hit=offset+ray*distance;float h2=dot(hit,hit);if(h2<=1e-16)return ray;vec3 corrected=hit*inversesqrt(h2),observed;if(!vidCubeDepthPoint(image,corrected,clip,observed))return ray;return length(observed/scale-hit)<=length(hit)*limits.z?corrected:ray;}\n"

#endif
