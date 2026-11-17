#ifndef VID_SHADOW_H
#define VID_SHADOW_H

// prepend to a fragment shader; samplers use linear LEQUAL comparison
// uv/depth are normalized, texel is reciprocal width/height, layer is not filtered
#define VID_SHADOW_SAMPLING_GLSL \
    "void vidShadowTent3(vec2 uv,vec2 texel,out vec2 p0,out vec2 p1,out vec2 w0,out vec2 w1){" \
    "vec2 t=uv/texel-0.5,b=floor(t),f=fract(t);" \
    "w0=(3.0-2.0*f)*0.25;w1=(1.0+2.0*f)*0.25;" \
    "p0=(b-0.5+(2.0-f)/(3.0-2.0*f))*texel;" \
    "p1=(b+1.5+f/(1.0+2.0*f))*texel;}" \
    "vec2 vidShadowGradient(vec3 dx,vec3 dy){" \
    "float scale=max(max(abs(dx.x),abs(dx.y)),max(abs(dy.x),abs(dy.y)));" \
    "dx/=max(scale,1e-20);dy/=max(scale,1e-20);float d=dx.x*dy.y-dx.y*dy.x;" \
    "return abs(d)>0.000001?vec2(dy.y*dx.z-dx.y*dy.z,dx.x*dy.z-dy.x*dx.z)/d:vec2(0.0);}" \
    "float vidShadowTent3Array(highp sampler2DArrayShadow image,vec3 coord,float layer,vec2 texel,vec2 gradient){" \
    "vec2 p0,p1,w0,w1;vidShadowTent3(coord.xy,texel,p0,p1,w0,w1);" \
    "vec2 q0=vec2(p1.x,p0.y),q1=vec2(p0.x,p1.y);" \
    "return texture(image,vec4(p0,layer,coord.z+dot(gradient,p0-coord.xy)))*w0.x*w0.y+" \
    "texture(image,vec4(q0,layer,coord.z+dot(gradient,q0-coord.xy)))*w1.x*w0.y+" \
    "texture(image,vec4(q1,layer,coord.z+dot(gradient,q1-coord.xy)))*w0.x*w1.y+" \
    "texture(image,vec4(p1,layer,coord.z+dot(gradient,p1-coord.xy)))*w1.x*w1.y;}\n"

#endif
