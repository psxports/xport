#ifndef PSX_H
#define PSX_H

/* Portable PsyQ compatibility surface. The selected xport platform backend
 * supplies the window, input, timer, audio output and file services. */

#include "xport.h"
void gte_write_h(uint16 h);

typedef uint32 u_long;
typedef uint16 u_short;
typedef uint8 u_char;
typedef uint16 ushort;

#define PSX_DRAM_SIZE 0x200000u
#if defined(__cplusplus)
extern "C"
{
#endif
    extern uint32 SCRATCHPAD[256];
    extern uint8 DRAM[PSX_DRAM_SIZE];
#if defined(__cplusplus)
}
#endif

/* Public PsyQ-compatible ABI. Game code includes only this file; it has no
 * dependency on an installed PsyQ SDK or on this project's game headers. */
#define ONE 4096

#define MODE_NTSC 0
#define MODE_PAL 1

#define SPU_OFF 0
#define SPU_ON 1

#define SPU_VOICE_VOLL (0x01u << 0)
#define SPU_VOICE_VOLR (0x01u << 1)
#define SPU_VOICE_VOLMODEL (0x01u << 2)
#define SPU_VOICE_VOLMODER (0x01u << 3)
#define SPU_VOICE_PITCH (0x01u << 4)
#define SPU_VOICE_NOTE (0x01u << 5)
#define SPU_VOICE_SAMPLE_NOTE (0x01u << 6)
#define SPU_VOICE_WDSA (0x01u << 7)
#define SPU_VOICE_ADSR_AMODE (0x01u << 8)
#define SPU_VOICE_ADSR_SMODE (0x01u << 9)
#define SPU_VOICE_ADSR_RMODE (0x01u << 10)
#define SPU_VOICE_ADSR_AR (0x01u << 11)
#define SPU_VOICE_ADSR_DR (0x01u << 12)
#define SPU_VOICE_ADSR_SR (0x01u << 13)
#define SPU_VOICE_ADSR_RR (0x01u << 14)
#define SPU_VOICE_ADSR_SL (0x01u << 15)
#define SPU_VOICE_LSAX (0x01u << 16)
#define SPU_VOICE_ADSR_ADSR1 (0x01u << 17)
#define SPU_VOICE_ADSR_ADSR2 (0x01u << 18)

#define SPU_COMMON_MVOLL (0x01u << 0)
#define SPU_COMMON_MVOLR (0x01u << 1)
#define SPU_COMMON_MVOLMODEL (0x01u << 2)
#define SPU_COMMON_MVOLMODER (0x01u << 3)

#define SPU_ALLCH 0x00ffffffu
#define SPU_KEYCH(voice) (1u << (voice))

#define SS_TICK60 1
#define SS_SERIAL_A 0
#define SS_SERIAL_B 1
#define SS_WAIT_COMPLETED 1

#define CdlModeRept 0x04
#define CdlModeDA 0x01
#define CdlPlay 0x03
#define CdlStop 0x08
#define CdlPause 0x09
#define CdlMute 0x0b
#define CdlDemute 0x0c
#define CdlSetmode 0x0e

#define CdlNoIntr 0x00
#define CdlDataReady 0x01
#define CdlComplete 0x02
#define CdlAcknowledge 0x03
#define CdlDataEnd 0x04
#define CdlDiskError 0x05

typedef void (*CdlCB)(uint8 status, uint8 *result);

typedef struct
{
    uint8 minute;
    uint8 second;
    uint8 sector;
    uint8 track;
} CdlLOC;

typedef struct
{
    uint8 val0;
    uint8 val1;
    uint8 val2;
    uint8 val3;
} CdlATV;

typedef struct
{
    sint16 m[3][3];
    sint32 t[3];
} MATRIX;

typedef struct
{
    sint32 vx;
    sint32 vy;
    sint32 vz;
    sint32 pad;
} VECTOR;

typedef struct
{
    sint16 vx;
    sint16 vy;
    sint16 vz;
    sint16 pad;
} SVECTOR;

typedef struct
{
    uint8 r;
    uint8 g;
    uint8 b;
    uint8 cd;
} CVECTOR;

typedef struct
{
    sint16 left;
    sint16 right;
} SpuVolume;

typedef struct
{
    uint32 voice;
    uint32 mask;
    SpuVolume volume;
    SpuVolume volmode;
    SpuVolume volumex;
    uint16 pitch;
    uint16 note;
    uint16 sample_note;
    sint16 envx;
    uint32 addr;
    uint32 loop_addr;
    sint32 a_mode;
    sint32 s_mode;
    sint32 r_mode;
    uint16 ar;
    uint16 dr;
    uint16 sr;
    uint16 rr;
    uint16 sl;
    uint16 adsr1;
    uint16 adsr2;
} SpuVoiceAttr;

typedef struct
{
    SpuVolume volume;
    sint32 reverb;
    sint32 mix;
} SpuExtAttr;

typedef struct
{
    uint32 mask;
    SpuVolume mvol;
    SpuVolume mvolmode;
    SpuVolume mvolx;
    SpuExtAttr cd;
    SpuExtAttr ext;
} SpuCommonAttr;

typedef struct
{
    sint16 vx;
    sint16 vy;
} DVECTOR;

struct PSX_RECT
{
    sint16 x;
    sint16 y;
    sint16 w;
    sint16 h;
};

typedef struct
{
    SVECTOR v;
    uint8 uv[2];
    uint16 pad;
    CVECTOR c;
    DVECTOR sxy;
    uint32 sz;
} RVECTOR;

typedef struct
{
    RVECTOR r01;
    RVECTOR r12;
    RVECTOR r20;
    RVECTOR *r0;
    RVECTOR *r1;
    RVECTOR *r2;
    uint32 *rtn;
} CRVECTOR3;

typedef struct
{
    RVECTOR r01;
    RVECTOR r02;
    RVECTOR r31;
    RVECTOR r32;
    RVECTOR rc;
    RVECTOR *r0;
    RVECTOR *r1;
    RVECTOR *r2;
    RVECTOR *r3;
    uint32 *rtn;
} CRVECTOR4;

typedef struct
{
    uint32 tag;
    uint32 code[15];
} DR_ENV;

typedef struct
{
    PSX_RECT clip;
    sint16 ofs[2];
    PSX_RECT tw;
    uint16 tpage;
    uint8 dtd;
    uint8 dfe;
    uint8 isbg;
    uint8 r0;
    uint8 g0;
    uint8 b0;
    DR_ENV dr_env;
} DRAWENV;

typedef struct
{
    PSX_RECT disp;
    PSX_RECT screen;
    uint8 isinter;
    uint8 isrgb24;
    uint8 pad0;
    uint8 pad1;
} DISPENV;

typedef struct
{
    uint32 addr : 24;
    uint32 len : 8;
    uint8 r0;
    uint8 g0;
    uint8 b0;
    uint8 code;
} P_TAG;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    sint16 x1, y1;
    sint16 x2, y2;
} POLY_F3;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    sint16 x1, y1;
    sint16 x2, y2;
    sint16 x3, y3;
} POLY_F4;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    uint8 u0, v0;
    uint16 clut;
    sint16 x1, y1;
    uint8 u1, v1;
    uint16 tpage;
    sint16 x2, y2;
    uint8 u2, v2;
    uint16 pad1;
} POLY_FT3;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    uint8 u0, v0;
    uint16 clut;
    sint16 x1, y1;
    uint8 u1, v1;
    uint16 tpage;
    sint16 x2, y2;
    uint8 u2, v2;
    uint16 pad1;
    sint16 x3, y3;
    uint8 u3, v3;
    uint16 pad2;
} POLY_FT4;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    uint8 r1, g1, b1, pad1;
    sint16 x1, y1;
    uint8 r2, g2, b2, pad2;
    sint16 x2, y2;
} POLY_G3;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    uint8 r1, g1, b1, pad1;
    sint16 x1, y1;
    uint8 r2, g2, b2, pad2;
    sint16 x2, y2;
    uint8 r3, g3, b3, pad3;
    sint16 x3, y3;
} POLY_G4;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    uint8 r1, g1, b1, pad1;
    sint16 x1, y1;
} LINE_G2;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0, x1, y1;
} LINE_F2;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    uint8 u0, v0;
    uint16 clut;
    uint8 r1, g1, b1, p1;
    sint16 x1, y1;
    uint8 u1, v1;
    uint16 tpage;
    uint8 r2, g2, b2, p2;
    sint16 x2, y2;
    uint8 u2, v2;
    uint16 pad2;
} POLY_GT3;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    uint8 u0, v0;
    uint16 clut;
    uint8 r1, g1, b1, p1;
    sint16 x1, y1;
    uint8 u1, v1;
    uint16 tpage;
    uint8 r2, g2, b2, p2;
    sint16 x2, y2;
    uint8 u2, v2;
    uint16 pad2;
    uint8 r3, g3, b3, p3;
    sint16 x3, y3;
    uint8 u3, v3;
    uint16 pad3;
} POLY_GT4;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    uint8 u0, v0;
    uint16 clut;
    sint16 w, h;
} SPRT;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    uint8 u0, v0;
    uint16 clut;
} SPRT_16;

typedef SPRT_16 SPRT_8;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
    sint16 w, h;
} TILE;

typedef struct
{
    uint32 tag;
    uint8 r0, g0, b0, code;
    sint16 x0, y0;
} TILE_16;

typedef TILE_16 TILE_8;
typedef TILE_16 TILE_1;

typedef struct
{
    uint32 tag;
    uint32 code[2];
} DR_MODE;

typedef DR_MODE DR_TWIN;
typedef DR_MODE DR_AREA;
typedef DR_MODE DR_OFFSET;

typedef struct
{
    uint32 tag;
    uint32 code[5];
} DR_MOVE;

typedef struct
{
    uint32 tag;
    uint32 code[3];
    uint32 p[13];
} DR_LOAD;

typedef struct
{
    uint32 tag;
    uint32 code[1];
} DR_TPAGE;

typedef DR_MODE DR_STP;

#define setVector(vector, _x, _y, _z) ((vector)->vx = (_x), (vector)->vy = (_y), (vector)->vz = (_z))
#define setRECT(rect, _x, _y, width, height) ((rect)->x = (_x), (rect)->y = (_y), (rect)->w = (width), (rect)->h = (height))
#define setRGB0(packet, red, green, blue) ((packet)->r0 = (red), (packet)->g0 = (green), (packet)->b0 = (blue))
#define setRGB1(packet, red, green, blue) ((packet)->r1 = (red), (packet)->g1 = (green), (packet)->b1 = (blue))
#define setRGB2(packet, red, green, blue) ((packet)->r2 = (red), (packet)->g2 = (green), (packet)->b2 = (blue))
#define setRGB3(packet, red, green, blue) ((packet)->r3 = (red), (packet)->g3 = (green), (packet)->b3 = (blue))
#define setXY0(packet, _x, _y) ((packet)->x0 = (_x), (packet)->y0 = (_y))
#define setXY2(packet, _x0, _y0, _x1, _y1) ((packet)->x0 = (_x0), (packet)->y0 = (_y0), (packet)->x1 = (_x1), (packet)->y1 = (_y1))
#define setXY3(packet, _x0, _y0, _x1, _y1, _x2, _y2) (setXY2(packet, _x0, _y0, _x1, _y1), (packet)->x2 = (_x2), (packet)->y2 = (_y2))
#define setXY4(packet, _x0, _y0, _x1, _y1, _x2, _y2, _x3, _y3) (setXY3(packet, _x0, _y0, _x1, _y1, _x2, _y2), (packet)->x3 = (_x3), (packet)->y3 = (_y3))
#define setWH(packet, width, height) ((packet)->w = (width), (packet)->h = (height))
#define setUV0(packet, _u, _v) ((packet)->u0 = (_u), (packet)->v0 = (_v))
#define setUV3(packet, _u0, _v0, _u1, _v1, _u2, _v2) ((packet)->u0 = (_u0), (packet)->v0 = (_v0), (packet)->u1 = (_u1), (packet)->v1 = (_v1), (packet)->u2 = (_u2), (packet)->v2 = (_v2))
#define setUV4(packet, _u0, _v0, _u1, _v1, _u2, _v2, _u3, _v3) (setUV3(packet, _u0, _v0, _u1, _v1, _u2, _v2), (packet)->u3 = (_u3), (packet)->v3 = (_v3))
#define setlen(packet, length) (((P_TAG *)(packet))->len = (uint8)(length))
#define setaddr(packet, address) (((P_TAG *)(packet))->addr = (uint32)(intptr)(address))
#define setcode(packet, command) (((P_TAG *)(packet))->code = (uint8)(command))
#define getlen(packet) ((uint8)((P_TAG *)(packet))->len)
#define getaddr(packet) ((uint32)((P_TAG *)(packet))->addr)
#define getcode(packet) ((uint8)((P_TAG *)(packet))->code)
#define nextPrim(packet) ((void *)(intptr)(getaddr(packet) | 0x80000000UL))
#define isendprim(packet) (getaddr(packet) == 0xffffffUL)
#define addPrim(ot, packet) (setaddr(packet, getaddr(ot)), setaddr(ot, packet))
#define addPrims(ot, first, last) (setaddr(last, getaddr(ot)), setaddr(ot, first))
#define termPrim(packet) setaddr(packet, 0xffffffffUL)
#define setSemiTrans(packet, enabled) ((enabled) ? setcode(packet, getcode(packet) | 2) : setcode(packet, getcode(packet) & ~2))
#define setShadeTex(packet, raw_texture) ((raw_texture) ? setcode(packet, getcode(packet) | 1) : setcode(packet, getcode(packet) & ~1))
#define getTPage(depth, abr, x, y) ((((depth) & 3) << 7) | (((abr) & 3) << 5) | (((y) & 0x100) >> 4) | (((x) & 0x3ff) >> 6) | (((y) & 0x200) << 2))
#define getClut(x, y) (((y) << 6) | (((x) >> 4) & 0x3f))
#define setTPage(packet, depth, abr, x, y) ((packet)->tpage = (uint16)getTPage(depth, abr, x, y))
#define setClut(packet, x, y) ((packet)->clut = (uint16)getClut(x, y))
#define psx_draw_mode(dfe, dtd, tpage) (0xe1000000UL | ((dtd) ? 0x0200UL : 0) | ((dfe) ? 0x0400UL : 0) | ((tpage) & 0x9ff))
#define psx_texture_window(texture_window) ((texture_window) ? (0xe2000000UL | (((texture_window)->y & 0xff) >> 3 << 15) | (((texture_window)->x & 0xff) >> 3 << 10) | ((~((texture_window)->h - 1) & 0xff) >> 3 << 5) | ((~((texture_window)->w - 1) & 0xff) >> 3)) : 0)
#define setDrawMode(packet, dfe, dtd, tpage, texture_window) (setlen(packet, 2), ((uint32 *)(packet))[1] = psx_draw_mode(dfe, dtd, tpage), ((uint32 *)(packet))[2] = psx_texture_window((PSX_RECT *)(texture_window)))
#define setTexWindow(packet, texture_window) (setlen(packet, 2), ((uint32 *)(packet))[1] = psx_texture_window((PSX_RECT *)(texture_window)), ((uint32 *)(packet))[2] = 0)
#define setPolyF3(packet) (setlen(packet, 4), setcode(packet, 0x20))
#define setPolyFT3(packet) (setlen(packet, 7), setcode(packet, 0x24))
#define setPolyG3(packet) (setlen(packet, 6), setcode(packet, 0x30))
#define setPolyGT3(packet) (setlen(packet, 9), setcode(packet, 0x34))
#define setPolyF4(packet) (setlen(packet, 5), setcode(packet, 0x28))
#define setPolyFT4(packet) (setlen(packet, 9), setcode(packet, 0x2c))
#define setPolyG4(packet) (setlen(packet, 8), setcode(packet, 0x38))
#define setPolyGT4(packet) (setlen(packet, 12), setcode(packet, 0x3c))
#define setSprt8(packet) (setlen(packet, 3), setcode(packet, 0x74))
#define setSprt16(packet) (setlen(packet, 3), setcode(packet, 0x7c))
#define setSprt(packet) (setlen(packet, 4), setcode(packet, 0x64))
#define setTile1(packet) (setlen(packet, 2), setcode(packet, 0x68))
#define setTile8(packet) (setlen(packet, 2), setcode(packet, 0x70))
#define setTile16(packet) (setlen(packet, 2), setcode(packet, 0x78))
#define setTile(packet) (setlen(packet, 3), setcode(packet, 0x60))

/* PsyQ declares these primitive initializers as macros. Keep the familiar
 * uppercase spellings used by reconstructed game code without adding wrapper
 * functions or another packet-initialization semantic. */
#define SetDrawMode(packet, dfe, dtd, tpage, texture_window) setDrawMode(packet, dfe, dtd, tpage, texture_window)
#define SetPolyF3(packet) setPolyF3(packet)
#define SetPolyF4(packet) setPolyF4(packet)
#define SetPolyFT3(packet) setPolyFT3(packet)
#define SetPolyFT4(packet) setPolyFT4(packet)
#define SetPolyG3(packet) setPolyG3(packet)
#define SetPolyG4(packet) setPolyG4(packet)
#define SetPolyGT3(packet) setPolyGT3(packet)
#define SetPolyGT4(packet) setPolyGT4(packet)
#define SetSemiTrans(packet, enabled) setSemiTrans(packet, enabled)
#define SetShadeTex(packet, raw_texture) setShadeTex(packet, raw_texture)
#define SetSprt(packet) setSprt(packet)
#define SetSprt16(packet) setSprt16(packet)
#define SetSprt8(packet) setSprt8(packet)
#define SetTexWindow(packet, texture_window) setTexWindow(packet, texture_window)
#define SetTile(packet) setTile(packet)
#define SetTile1(packet) setTile1(packet)
#define SetTile16(packet) setTile16(packet)
#define SetTile8(packet) setTile8(packet)

#if defined(__cplusplus)
extern "C"
{
#endif

    void AddPrim(void *ot, void *prim);
    void AddPrims(void *ot, void *first, void *last);
    VECTOR *ApplyMatrix(MATRIX *matrix, SVECTOR *input, VECTOR *output);
    VECTOR *ApplyMatrixLV(MATRIX *matrix, VECTOR *input, VECTOR *output);
    VECTOR *ApplyRotMatrix(SVECTOR *input, VECTOR *output);
    SVECTOR *ApplyMatrixSV(MATRIX *matrix, SVECTOR *input, SVECTOR *output);
    sint32 AverageZ3(sint32 z0, sint32 z1, sint32 z2);
    sint32 AverageZ4(sint32 z0, sint32 z1, sint32 z2, sint32 z3);
    sint32 ClearImage(PSX_RECT *rectangle, uint8 red, uint8 green, uint8 blue);
    uint32 *ClearOTag(uint32 *ot, sint32 count);
    uint32 *ClearOTagR(uint32 *ot, sint32 count);
    sint32 ccos(sint32 a);
    sint32 csin(sint32 a);
    sint32 DrawSync(sint32 mode);
    void DrawOTag(uint32 *ot);
    void FlushCache(void);
    uint16 GetClut(sint32 x, sint32 y);
    sint32 GetRCnt(sint32 counter);
    uint16 GetTPage(sint32 depth, sint32 abr, sint32 x, sint32 y);
    void InitGeom(void);
    sint32 LoadImagePSX(PSX_RECT *rectangle, uint32 *pixels);
    uint16 LoadClut(uint32 *pixels, sint32 x, sint32 y);
    uint16 LoadTPage(uint32 *pixels, sint32 depth, sint32 abr, sint32 x, sint32 y, sint32 width, sint32 height);
    sint32 MoveImage(PSX_RECT *rectangle, sint32 x, sint32 y);
    sint32 NormalClip(sint32 point0, sint32 point1, sint32 point2);
    uint32 PadRead(sint32 controller);
    void PadInit(sint32 mode);
    sint32 PadGetState(sint32 port);
    sint32 PadInfoMode(sint32 port, sint32 term, sint32 offset);
    void PadInitDirect(uint8 *pad1, uint8 *pad2);
    void PadSetAct(sint32 port, const void *actuator, sint32 length);
    sint32 PadSetActAlign(sint32 port, const void *alignment);
    void PadSetMainMode(sint32 port, sint32 mode, sint32 lock);
    void PadStartCom(void);
    void PadStopCom(void);
    void PopMatrix(void);
    void PushMatrix(void);
    void ReadGeomOffset(sint32 *x, sint32 *y);
    sint32 ReadGeomScreen(void);
    void ReadRotMatrix(MATRIX *matrix);
    sint32 ResetGraph(sint32 mode);
    sint32 ResetCallback(void);
    MATRIX *RotMatrix(SVECTOR *rotation, MATRIX *matrix);
    MATRIX *RotMatrixZ(sint32 rotation, MATRIX *matrix);
    MATRIX *RotMatrixYXZ(SVECTOR *rotation, MATRIX *matrix);
    void RotTrans(SVECTOR *input, VECTOR *output, sint32 *flags);
    sint32 RotTransPers(SVECTOR *input, sint32 *screen_xy, sint32 *projection, sint32 *flags);
    void RotTransSV(SVECTOR *input, SVECTOR *output, sint32 *flags);
    MATRIX *ScaleMatrix(MATRIX *matrix, VECTOR *scale);
    void SetDispMask(sint32 enabled);
    DISPENV *SetDefDispEnv(DISPENV *environment, sint32 x, sint32 y, sint32 width, sint32 height);
    DRAWENV *SetDefDrawEnv(DRAWENV *environment, sint32 x, sint32 y, sint32 width, sint32 height);
    void SetDrawArea(void *packet, PSX_RECT *rectangle);
    void SetDrawEnv(void *packet, DRAWENV *environment);
    void SetDrawStp(void *packet, sint32 enabled);
    void SetFarColor(sint32 red, sint32 green, sint32 blue);
    void SetFogNear(sint32 distance, sint32 projection);
    void SetGeomOffset(sint32 x, sint32 y);
    void SetGeomScreen(sint32 distance);
    sint32 SetGraphDebug(sint32 level);
    void SetRotMatrix(MATRIX *matrix);
    void SetTransMatrix(MATRIX *matrix);
    sint32 StoreImage(PSX_RECT *rectangle, uint32 *pixels);
    DISPENV *PutDispEnv(DISPENV *environment);
    DRAWENV *PutDrawEnv(DRAWENV *environment);
    void SsEnd(void);
    void SsInit(void);
    void SsQuit(void);
    void SsSetMVol(sint16 left, sint16 right);
    void SsSetSerialVol(sint8 serial, sint16 left, sint16 right);
    void SsSetTickMode(sint32 mode);
    void SsStart(void);
    sint16 SsUtKeyOn(sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right);
    sint16 SsUtKeyOnV(sint16 voice, sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right);
    sint16 SsVabOpenHead(uint8 *header, sint16 requested_bank);
    sint16 SsVabTransBody(uint8 *body, sint16 bank);
    sint16 SsVabTransCompleted(sint16 mode);
    sint32 CdControl(uint8 command, uint8 *parameter, uint8 *result);
    sint32 CdControlB(uint8 command, uint8 *parameter, uint8 *result);
    sint32 CdControlF(uint8 command, uint8 *parameter);
    sint32 CdMix(CdlATV *volume);
    sint32 CdPlay(sint32 mode, sint32 *track, sint32 offset);
    CdlCB CdReadyCallback(CdlCB callback);
    CdlCB CdSyncCallback(CdlCB callback);
    void SpuGetCommonAttr(SpuCommonAttr *attr);
    void SpuGetVoiceAttr(SpuVoiceAttr *attr);
    sint32 SpuGetKeyStatus(uint32 voice_bit);
    void SpuInit(void);
    void SpuInitHot(void);
    void SpuQuit(void);
    void SpuSetCommonAttr(SpuCommonAttr *attr);
    void SpuSetKey(sint32 on_off, uint32 voice_bit);
    void SpuSetVoiceAttr(SpuVoiceAttr *attr);
    void SpuSetVoiceLoopStartAddr(sint32 voice, uint32 loop_start_address);
    void SpuSetVoicePitch(sint32 voice, uint16 pitch);
    void SpuSetVoiceStartAddr(sint32 voice, uint32 start_address);
    void SpuSetVoiceVolume(sint32 voice, sint16 left, sint16 right);
    MATRIX *TransMatrix(MATRIX *matrix, VECTOR *translation);
    sint32 VSync(sint32 mode);
    void *VSyncCallback(void *callback);
    sint32 VectorNormal(VECTOR *input, VECTOR *output);
    sint32 VectorNormalS(VECTOR *input, SVECTOR *output);
    void SetVideoMode(sint32 mode);
    sint32 gte_project(const SVECTOR *input, sint32 *screen_xy, sint32 *flags);
    void gte_transform(const SVECTOR *input, VECTOR *output, sint32 *flags);

#if defined(__cplusplus)
}
#endif

#endif
