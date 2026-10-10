#ifndef XPORT_PSX_MDEC_H
#define XPORT_PSX_MDEC_H

#include "xport.h"

typedef struct MdecRleBlock
{
    sint16 coefficients[64];
    uint32 position;
    uint16 quantization_scale;
} MdecRleBlock;

extern const sint16 mdec_standard_scale[64];

/* Coefficients use four fractional bits and transposed diagonal placement */
void mdec_rle_begin(MdecRleBlock *block);

/* Return one for a completed block, zero for more input, or minus one for invalid state */
sint32 mdec_rle_decode(MdecRleBlock *block, const uint16 *input, uint32 count, uint32 *consumed, const uint8 quantization[64]);

/* Scale uses the original hardware matrix order; output is row-major signed pixels */
sint32 mdec_idct(sint16 coefficients[64], const sint16 scale[64]);
sint32 mdec_color(uint32 pixels[256], const sint16 blocks[384], uint32 signed_output);

#if defined(XPORT_MDEC_TESTING)
void mdec_idct_test_1d(const sint16 values[8], sint16 optimized[8], sint16 reference[8]);
void mdec_idct_test_reference(sint16 coefficients[64], const sint16 scale[64]);
#endif

typedef struct MdecState
{
    uint32 status, dma_in, dma_out;
    uint16 input[512];
    uint32 input_head, input_count;
    uint32 output[192];
    uint32 output_head, output_count;
    uint32 phase, remaining_halfwords;
    uint8 quantization_y[64], quantization_uv[64];
    sint16 scale[64], blocks[384];
    uint32 current_block, current_coefficient, quantization_scale;
    uint32 pixels[256], copy_ticks;
} MdecState;

void mdec_initialize(MdecState *state);
void mdec_control(MdecState *state, uint32 control);
uint32 mdec_write_data(MdecState *state, uint32 value);
/* DMA input reports accepted halfwords, including a possible partial final word */
uint32 mdec_dma_write(MdecState *state, const uint32 *words, uint32 count);
uint32 mdec_dma_read(MdecState *state, uint32 *words, uint32 count);
/* Zero requests a CPU stall; one supplies data or the empty-port value */
sint32 mdec_read_data(MdecState *state, uint32 *value, uint32 *stall_ticks);
void mdec_advance(MdecState *state, uint32 ticks);
sint32 mdec_state_valid(const MdecState *state);
/* Portable little-endian state; a rejected load leaves the live decoder untouched */
sint32 mdec_state_io(FILE *file, MdecState *state, sint32 load);

#endif
