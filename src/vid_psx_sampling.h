#ifndef VID_PSX_SAMPLING_H
#define VID_PSX_SAMPLING_H

// The shader supplies vidPsxWord for its exact VRAM transport
#define VID_PSX_SAMPLING_GLSL \
    "\n#ifndef VID_PSX_ADDRESS\n#define VID_PSX_ADDRESS vidPsxAddress\n#endif\n" \
    "uint vidPsxWord(ivec2 address);\n" \
    "int vidPsxAxis(int p,int size,bool repeatAxis){\n" \
    "if(!repeatAxis)return clamp(p,0,size-1);\n" \
    "int r=int(uint(p<0?-(p+1):p)%uint(size));return p<0?size-1-r:r;}\n" \
    "ivec2 vidPsxAddress(ivec2 pixel,ivec4 region,uint repeatAxes){\n" \
    "return region.xy+ivec2(vidPsxAxis(pixel.x,region.z,(repeatAxes&1u)!=0u),\n" \
    "vidPsxAxis(pixel.y,region.w,(repeatAxes&2u)!=0u));}\n" \
    "uint vidPsxDecode(uint tpage,uint clut,ivec2 pixel){\n" \
    "uvec2 uv=uvec2(pixel)&uvec2(255u);uint depth=(tpage>>7)&3u;\n" \
    "uint shift=depth==0u?2u:(depth==1u?1u:0u);\n" \
    "uint word=vidPsxWord(ivec2(int((tpage&15u)*64u+(uv.x>>shift)),int(((tpage>>4)&1u)*256u+uv.y))&ivec2(1023,511));\n" \
    "if(depth<2u){uint index=depth==0u?(word>>((uv.x&3u)*4u))&15u:(word>>((uv.x&1u)*8u))&255u;\n" \
    "word=vidPsxWord(ivec2(int((clut&63u)*16u+index),int(clut>>6))&ivec2(1023,511));}return word;}\n" \
    "vec4 vidPsxColor(uint word,uint stpClass,bool zeroOpaque){\n" \
    "bool stp=(word&32768u)!=0u;\n" \
    "if((word==0u&&!zeroOpaque)||(stpClass==1u&&stp)||(stpClass==2u&&!stp))return vec4(0.0);\n" \
    "uvec3 c=uvec3(word,word>>5,word>>10)&uvec3(31u);return vec4(vec3((c<<3)|(c>>2))/255.0,1.0);}\n" \
    "vec4 vidPsxTap(ivec2 pixel,uint tpage,uint clut,ivec4 region,uint repeatAxes,uint stpClass,bool zeroOpaque){\n" \
    "return vidPsxColor(vidPsxDecode(tpage,clut,VID_PSX_ADDRESS(pixel,region,repeatAxes)),stpClass,zeroOpaque);}\n" \
    "vec4 vidPsxFilter(vec2 uv,uint tpage,uint clut,ivec4 region,uint repeatAxes,uint stpClass,bool zeroOpaque){\n" \
    "vec2 p=uv-0.5;ivec2 base=ivec2(floor(p));vec2 f=fract(p);\n" \
    "vec4 a=vidPsxTap(base,tpage,clut,region,repeatAxes,stpClass,zeroOpaque);\n" \
    "vec4 b=vidPsxTap(base+ivec2(1,0),tpage,clut,region,repeatAxes,stpClass,zeroOpaque);\n" \
    "vec4 c=vidPsxTap(base+ivec2(0,1),tpage,clut,region,repeatAxes,stpClass,zeroOpaque);\n" \
    "vec4 d=vidPsxTap(base+ivec2(1,1),tpage,clut,region,repeatAxes,stpClass,zeroOpaque);\n" \
    "vec4 color=mix(mix(a,b,f.x),mix(c,d,f.x),f.y);\n" \
    "return color.a>0.0?vec4(color.rgb/color.a,1.0):vec4(0.0);}\n" \
    "vec4 vidPsxSample(vec2 uv,uint tpage,uint clut,ivec4 region,uint repeatAxes,uint stpClass,bool filtered,bool zeroOpaque,out uint nearestWord){\n" \
    "nearestWord=vidPsxDecode(tpage,clut,VID_PSX_ADDRESS(ivec2(floor(uv)),region,repeatAxes));\n" \
    "vec4 color=vidPsxColor(nearestWord,stpClass,zeroOpaque);\n" \
    "return color.a==0.0||!filtered?color:vidPsxFilter(uv,tpage,clut,region,repeatAxes,stpClass,zeroOpaque); }\n"

#endif
