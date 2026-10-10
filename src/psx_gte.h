#ifndef XPORT_PSX_GTE_H
#define XPORT_PSX_GTE_H

#include "psx.h"

/* Native PsyQ inline helpers share the runtime COP2 register bank */
static inline void gte_lwc2(uint32 index, uint32 value)
{
    xport_gte_write_data(index, value);
}

static inline void gte_swc2(uint32 index, void *output)
{
    xport_store_le32(output, xport_gte_read_data(index));
}

static inline void gte_ctc2(uint32 value, uint32 index)
{
    xport_gte_write_control(index, value);
}

static inline void gte_SetRotMatrix(const void *matrix)
{
    const uint8 *bytes = (const uint8 *)matrix;
    uint32 index;
    for (index = 0; index < 4; ++index)
        xport_gte_write_control(index, xport_load_le32(bytes + 4u * index));
    xport_gte_write_control(4u, xport_load_le16(bytes + 16u));
}

static inline void gte_SetTransVector(const void *vector)
{
    const uint8 *bytes = (const uint8 *)vector;
    uint32 index;
    for (index = 0; index < 3; ++index)
        xport_gte_write_control(5u + index, xport_load_le32(bytes + 4u * index));
}

static inline void gte_SetTransMatrix(const void *matrix)
{
    gte_SetTransVector((const uint8 *)matrix + 20u);
}

static inline void gte_SetGeomScreen(sint32 distance)
{
    SetGeomScreen(distance);
}

static inline void gte_SetGeomOffset(sint32 x, sint32 y)
{
    SetGeomOffset(x, y);
}

static inline void gte_ldtr(sint32 x, sint32 y, sint32 z)
{
    xport_gte_write_control(5u, (uint32)x);
    xport_gte_write_control(6u, (uint32)y);
    xport_gte_write_control(7u, (uint32)z);
}

static inline void xport_gte_load_vertex(uint32 index, const void *vector)
{
    const uint8 *bytes = (const uint8 *)vector;
    xport_gte_write_data(2u * index, xport_load_le32(bytes));
    xport_gte_write_data(2u * index + 1u, xport_load_le16(bytes + 4u));
}

static inline void gte_ldv0(const void *vector)
{
    xport_gte_load_vertex(0u, vector);
}

static inline void gte_ldv1(const void *vector)
{
    xport_gte_load_vertex(1u, vector);
}

static inline void gte_ldv2(const void *vector)
{
    xport_gte_load_vertex(2u, vector);
}

static inline void gte_ldv3(const void *a, const void *b, const void *c)
{
    gte_ldv0(a);
    gte_ldv1(b);
    gte_ldv2(c);
}

static inline void gte_ldsv(const void *vector)
{
    const uint8 *bytes = (const uint8 *)vector;
    uint32 index;
    for (index = 0; index < 3; ++index)
        xport_gte_write_data(9u + index, xport_load_le16(bytes + 2u * index));
}

static inline void gte_stsv(void *vector)
{
    uint8 *bytes = (uint8 *)vector;
    uint32 index;
    for (index = 0; index < 3; ++index)
        xport_store_le16(bytes + 2u * index, (uint16)xport_gte_read_data(9u + index));
}

static inline void gte_ldclmv(const void *matrix_column)
{
    const uint8 *bytes = (const uint8 *)matrix_column;
    uint32 index;
    for (index = 0; index < 3; ++index)
        xport_gte_write_data(9u + index, xport_load_le16(bytes + 6u * index));
}

static inline void gte_stclmv(void *matrix_column)
{
    uint8 *bytes = (uint8 *)matrix_column;
    uint32 index;
    for (index = 0; index < 3; ++index)
        xport_store_le16(bytes + 6u * index, (uint16)xport_gte_read_data(9u + index));
}

static inline void gte_SetDQA(sint32 coefficient)
{
    xport_gte_write_control(27u, (uint32)coefficient);
}

static inline void gte_SetDQB(sint32 offset)
{
    xport_gte_write_control(28u, (uint32)offset);
}

static inline void gte_stdp(void *output)
{
    gte_swc2(8u, output);
}

static inline void gte_lddp(sint32 value)
{
    xport_gte_write_data(8u, (uint32)value);
}

static inline void gte_ldsxy3(uint32 a, uint32 b, uint32 c)
{
    gte_lwc2(12u, a);
    gte_lwc2(13u, b);
    gte_lwc2(14u, c);
}

static inline void gte_stsxy(void *output)
{
    gte_swc2(14u, output);
}

static inline void gte_stsxy3(void *a, void *b, void *c)
{
    gte_swc2(12u, a);
    gte_swc2(13u, b);
    gte_swc2(14u, c);
}

static inline void gte_stsz(void *output)
{
    gte_swc2(19u, output);
}

static inline void gte_stotz(void *output)
{
    gte_swc2(7u, output);
}

static inline void gte_stflg(void *output)
{
    PsxGteSnapshot state;
    psx_gte_snapshot(&state);
    xport_store_le32(output, (uint32)state.flag);
}

static inline void gte_stlvnl(void *output)
{
    gte_swc2(25u, output);
    gte_swc2(26u, (uint8 *)output + 4u);
    gte_swc2(27u, (uint8 *)output + 8u);
}

static inline void gte_rtps(void)
{
    xport_gte_execute(0x480001u);
}

static inline void gte_rtpt(void)
{
    xport_gte_execute(0x480030u);
}

static inline void gte_rt(void)
{
    xport_gte_execute(0x480012u);
}

static inline void gte_rtir(void)
{
    xport_gte_execute(0x49E012u);
}

static inline void gte_nclip(void)
{
    xport_gte_execute(0x1400006u);
}

static inline void gte_avsz3(void)
{
    xport_gte_execute(0x158002Du);
}

static inline void gte_avsz4(void)
{
    xport_gte_execute(0x168002Eu);
}

static inline void gte_dpcs(void)
{
    xport_gte_execute(0x780010u);
}

static inline void gte_dpct(void)
{
    xport_gte_execute(0xF8002Au);
}

static inline void gte_mvmva(uint32 sf, uint32 matrix, uint32 vector, uint32 translation, uint32 lm)
{
    xport_gte_execute(0x400012u | ((sf & 1u) << 19u) | ((matrix & 3u) << 17u) | ((vector & 3u) << 15u) | ((translation & 3u) << 13u) | ((lm & 1u) << 10u));
}

#define gte_rtps_b() gte_rtps()
#define gte_nclip_b() gte_nclip()
#define gte_avsz4_b() gte_avsz4()

static inline void gte_ldVXY0(const void *p)
{
    gte_lwc2(0u, xport_load_le32(p));
}

static inline void gte_ldVZ0(const void *p)
{
    gte_lwc2(1u, xport_load_le16(p));
}

static inline void gte_ldVXY1(const void *p)
{
    gte_lwc2(2u, xport_load_le32(p));
}

static inline void gte_ldVZ1(const void *p)
{
    gte_lwc2(3u, xport_load_le16(p));
}

static inline void gte_ldVXY2(const void *p)
{
    gte_lwc2(4u, xport_load_le32(p));
}

static inline void gte_ldVZ2(const void *p)
{
    gte_lwc2(5u, xport_load_le16(p));
}

#endif
