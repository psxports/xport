#define XPORT_ENABLE_AUDIT
#include "xport_trace.h"
#include "xport_trace.h"
#include "psx.h"
#include <stdio.h>
#include <string.h>

sint32 xport_memory_readable(const void *address, size_t size)
{
    (void)address;
    (void)size;
    return 0;
}

typedef struct FixtureState
{
    uint32 mmio_value;
    uint32 mmio_reads;
    uint32 mmio_writes;
    uint32 observations;
    uint32 guest_observations;
    uint32 host_observations;
    XportMemoryOperation last_operation;
    XportMemoryRegion last_region;
    uint32 last_offset;
    uint32 audits;
    uint32 last_audit_address;
    const char *last_audit_image;
} FixtureState;

static sint32 mmio_read(void *user, uint32 address, uint32 width, uint32 *value)
{
    FixtureState *state = (FixtureState *)user;
    if (address != 0x1f801100u || width != 4u)
        return 0;
    ++state->mmio_reads;
    *value = state->mmio_value;
    return 1;
}

static sint32 mmio_write(void *user, uint32 address, uint32 width, uint32 value, uint32 byte_mask)
{
    FixtureState *state = (FixtureState *)user;
    if (address != 0x1f801100u || width != 4u || byte_mask != 15u)
        return 0;
    ++state->mmio_writes;
    state->mmio_value = value;
    return 1;
}

static void observe(void *user, XportMemoryOperation operation, XportMemoryRegion region,
                    uint32 offset, size_t width, uint32 before, uint32 after, uint32 byte_mask)
{
    FixtureState *state = (FixtureState *)user;
    (void)width;
    (void)before;
    (void)after;
    (void)byte_mask;
    ++state->observations;
    state->last_operation = operation;
    state->last_region = region;
    state->last_offset = offset;
    if (region == XPORT_MEMORY_DRAM)
        ++state->guest_observations;
    if (region == XPORT_MEMORY_NONE)
        ++state->host_observations;
}

static void audit(void *user, const XportAuditRecord *record)
{
    FixtureState *state = (FixtureState *)user;
    ++state->audits;
    state->last_audit_address = record->address;
    state->last_audit_image = record->image;
}

static int require(sint32 condition, const char *name)
{
    if (condition)
        return 1;
    fprintf(stderr, "Fixture failed: %s\n", name);
    return 0;
}

int main(void)
{
    FixtureState state;
    uint8 (*read8)(uint32) = r_u8;
    uint16 (*read16)(uint32) = r_u16;
    uint32 (*read32)(uint32) = r_u32;
    sint32 (*write8)(uint32, uint32) = w_u8;
    sint32 (*write16)(uint32, uint32) = w_u16;
    sint32 (*write32)(uint32, uint32) = w_u32;
    uint8 bios[512 * 1024];
    uint8 host[16];
    uint32 observations_before;
    memset(&state, 0, sizeof(state));
    memset(DRAM, 0, sizeof(DRAM));
    memset(SCRATCHPAD, 0, sizeof(SCRATCHPAD));
    memset(bios, 0x5a, sizeof(bios));
    memset(host, 0, sizeof(host));
    xport_memory_init(DRAM, sizeof(DRAM), (uint8 *)SCRATCHPAD, sizeof(SCRATCHPAD), bios, sizeof(bios));
    xport_memory_bind_mmio(0x1f801100u, 0x30u, mmio_read, mmio_write, &state);
    xport_memory_bind_observer(observe, &state);

    if (!require(write32(0x80000001u, 0x89abcdefu), "debugger write symbol")) return 1;
    if (!require(read32(0xa0000001u) == 0x89abcdefu, "debugger read symbol")) return 1;
    if (!require(write16(0x1f800001u, 0x7654u) && read16(0x9f800001u) == 0x7654u,
                 "scratchpad debugger symbols")) return 1;
    if (!require(write8(0x1f800003u, 0x1a5u) && read8(0xbf800003u) == 0xa5u,
                 "scratchpad width truncation")) return 1;
    if (!require(r_s8(0x80000001u) == -17, "signed byte")) return 1;
    if (!require(r_s16(0x80000001u) == -12817, "signed halfword")) return 1;
    if (!require(w_u8(PSX_DRAM_SIZE - 1u, 0x1ffu), "edge write")) return 1;
    if (!require(r_u8(0x801fffffu) == 0xffu, "width truncation")) return 1;
    if (!require(!w_u16(PSX_DRAM_SIZE - 1u, 1u), "edge rejection")) return 1;
    if (!require(xport_update_u8(PSX_DRAM_SIZE - 1u, XPORT_MEMORY_UPDATE_ADD, 2u) &&
                 r_u8(PSX_DRAM_SIZE - 1u) == 1u, "update truncation")) return 1;
    if (!require(w_u16(20u, 0xff00u) &&
                 xport_update_u16(20u, XPORT_MEMORY_UPDATE_OR, 0x55u) &&
                 r_u16(20u) == 0xff55u, "update bitwise")) return 1;
    if (!require(w_u32(24u, 12u) &&
                 xport_update_u32(24u, XPORT_MEMORY_UPDATE_MULTIPLY, 3u) &&
                 xport_update_u32(24u, XPORT_MEMORY_UPDATE_DIVIDE, 4u) &&
                 r_u32(24u) == 9u, "update arithmetic")) return 1;

    state.mmio_value = 0x12345678u;
    if (!require(r_u32(0x1f801100u) == 0x12345678u && state.mmio_reads == 1u,
                 "MMIO read exactly once")) return 1;
    if (!require(w_u32(0x1f801100u, 0xaabbccddu) && state.mmio_writes == 1u,
                 "MMIO write exactly once")) return 1;
    if (!require(xport_update_u32(0x1f801100u, XPORT_MEMORY_UPDATE_XOR, 0xffu) &&
                 state.mmio_reads == 2u && state.mmio_writes == 2u && state.mmio_value == 0xaabbcc22u,
                 "MMIO update exactly once")) return 1;

    xport_memory_clear_error();
    if (!require(!w_u8(0xbfc00000u, 0u) &&
                 xport_memory_error() == XPORT_MEMORY_ERROR_READONLY, "BIOS readonly")) return 1;
    if (!require(r_u8(0xbfc00000u) == 0x5au, "BIOS read")) return 1;

    DRAM[100] = 1u;
    DRAM[101] = 2u;
    DRAM[102] = 3u;
    DRAM[103] = 4u;
    if (!require(xport_guest_copy(xport_guest_ref(102u), xport_guest_ref(100u), 4u),
                 "guest overlap copy")) return 1;
    if (!require(DRAM[102] == 1u && DRAM[103] == 2u && DRAM[104] == 3u && DRAM[105] == 4u,
                 "memmove semantics")) return 1;
    if (!require(state.last_operation == XPORT_MEMORY_COPY && state.last_region == XPORT_MEMORY_DRAM &&
                 state.last_offset == 102u, "copy destination observation")) return 1;
    host[1] = 0x34u;
    host[2] = 0x12u;
    if (!require(*(const uint16 *)(host + 1) == 0x1234u, "unaligned host load")) return 1;
    observations_before = state.observations;
    *(uint16 *)(host + 3) = 0x5678u;
    if (!require(state.observations == observations_before, "host buffer is not guest memory")) return 1;
    if (!require(w_u32(200, 0x01020304u), "registered pointer store")) return 1;
    if (!require(state.guest_observations > 0u, "registered guest pointer observation")) return 1;
    observations_before = state.observations;
    if (!require(xport_store_u8(DRAM + 204, 0x5au) && xport_load_u8(DRAM + 204) == 0x5au,
                 "registered byte pointer access")) return 1;
    {
        POLY_F3 *primitive = (POLY_F3 *)(DRAM + 208);
        setPolyF3(primitive);
        setRGB0(primitive, 1, 2, 3);
        setXY3(primitive, 4, 5, 6, 7, 8, 9);
        if (!require(getlen(primitive) == 4u && getcode(primitive) == 0x20u &&
                     state.observations > observations_before, "PsyQ packet observer")) return 1;
    }

    DRAM[300] = 0x11u;
    DRAM[301] = 0x22u;
    DRAM[302] = 0x33u;
    DRAM[303] = 0x44u;
    if (!require(w_masked_u32(300u, 0xaabbccddu, 5u), "masked store")) return 1;
    if (!require(DRAM[300] == 0xddu && DRAM[301] == 0x22u && DRAM[302] == 0xbbu && DRAM[303] == 0x44u,
                 "masked store bytes")) return 1;

    xport_audit_bind(audit, &state);
    XPORT_FUNCTION_MARKER("GAME.EXE", 0x80010000u);
    FUNCTION_MARKER(0x80010004u, "GAME.EXE");
    if (!require(state.audits == 2u && state.last_audit_address == 0x80010004u &&
                 strcmp(state.last_audit_image, "GAME.EXE") == 0, "audit aliases")) return 1;
    puts("access_api_fixture: passed");
    return 0;
}
