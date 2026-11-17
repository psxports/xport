#ifndef VID_BRDF_H
#define VID_BRDF_H

// normalized N/V/L share one space; returned specular includes incident NdotL
// material is roughness/F0/coat weight, coatProfile is coat roughness/F0
#define VID_BRDF_GLSL \
    "float vidBrdfFresnel(float f0,float cosine){return f0+(1.0-f0)*pow(1.0-clamp(cosine,0.0,1.0),5.0);}" \
    "float vidBrdfGgx(float roughness,float f0,float nl,float nv,float nh,float vh){" \
    "float a=max(0.04,roughness*roughness),a2=a*a,d=nh*nh*(a2-1.0)+1.0;" \
    "float distribution=a2/(3.14159265*d*d),k=(roughness+1.0)*(roughness+1.0)*0.125;" \
    "float geometry=nv/(nv*(1.0-k)+k)*nl/(nl*(1.0-k)+k);" \
    "return distribution*geometry*vidBrdfFresnel(f0,vh)/(4.0*max(nv,0.0001));}" \
    "float vidBrdfSpecular(vec3 profile,vec2 coatProfile,vec3 n,vec3 v,vec3 l){" \
    "if(profile.y<=0.0&&profile.z<=0.0)return 0.0;float nl=max(dot(n,l),0.0),nv=max(dot(n,v),0.0001);" \
    "vec3 h=(l+v)*inversesqrt(max(dot(l+v,l+v),0.000001));float nh=max(dot(n,h),0.0),vh=max(dot(v,h),0.0);" \
    "float coat=profile.z;return (profile.y>0.0?vidBrdfGgx(profile.x,profile.y,nl,nv,nh,vh):0.0)*(1.0-coat*vidBrdfFresnel(coatProfile.y,nv))+" \
    "coat*vidBrdfGgx(coatProfile.x,coatProfile.y,nl,nv,nh,vh);}" \
    "float vidBrdfTransmission(float coat,float nv,float coatF0,float attenuation){return 1.0-attenuation*coat*vidBrdfFresnel(coatF0,nv);}"

#endif
