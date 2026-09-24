#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "psx_spu.h"

static inline sint16 audio_sample_clamp(sint32 value)
{
    if (value < -32768)
        return -32768;
    if (value > 32767)
        return 32767;
    return (sint16)value;
}

static inline sint16 audio_sample_mix(sint16 destination, sint32 source)
{
    return audio_sample_clamp((sint32)destination + source);
}

static const sint16 spu_gaussian[512] = {
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    0,     0,     0,     0,     0,     0,     0,     1,     1,     1,     1,     2,     2,     2,     3,     3,     3,     4,     4,     5,     5,     6,     7,     7,     8,     9,     9,     10,    11,    12,    13,    14,    15,    16,    17,    18,    19,    21,    22,    24,    25,    27,    28,    30,    32,    33,    35,    37,    39,    41,    44,    46,    48,    51,    53,    56,    58,    61,    64,    67,    70,    73,    77,    80,    84,    87,    91,    95,    99,    103,   107,   111,   116,   120,   125,   130,   135,   140,   145,   150,   156,   161,   167,   173,   179,   186,   192,   199,   205,   212,   219,   227,   234,   242,   250,   257,   266,   274,   283,   291,   300,   309,   319,   328,   338,   348,   358,   369,   379,   390,   401,   412,
    424,   436,   448,   460,   473,   485,   498,   512,   525,   539,   553,   567,   582,   597,   612,   627,   643,   659,   675,   692,   708,   726,   743,   761,   779,   797,   816,   835,   854,   874,   894,   914,   935,   956,   977,   999,   1020,  1043,  1066,  1089,  1112,  1136,  1160,  1184,  1209,  1234,  1260,  1286,  1312,  1339,  1366,  1394,  1422,  1450,  1479,  1508,  1537,  1567,  1598,  1628,  1660,  1691,  1723,  1756,  1789,  1822,  1856,  1890,  1924,  1959,  1995,  2031,  2067,  2104,  2141,  2179,  2217,  2256,  2295,  2334,  2374,  2415,  2456,  2497,  2539,  2582,  2624,  2668,  2712,  2756,  2801,  2846,  2892,  2938,  2985,  3032,  3079,  3128,  3176,  3225,  3275,  3325,  3376,  3427,  3479,  3531,  3584,  3637,  3691,  3745,  3799,  3855,  3910,  3967,  4023,  4081,  4138,  4197,  4255,  4315,  4374,  4435,  4495,  4557,  4619,  4681,  4744,  4807,
    4871,  4935,  5000,  5065,  5131,  5197,  5264,  5332,  5399,  5468,  5536,  5606,  5676,  5746,  5817,  5888,  5959,  6032,  6104,  6177,  6251,  6325,  6400,  6475,  6550,  6626,  6702,  6779,  6856,  6934,  7012,  7091,  7170,  7249,  7329,  7409,  7490,  7571,  7653,  7735,  7817,  7900,  7983,  8066,  8150,  8234,  8319,  8404,  8489,  8575,  8661,  8748,  8834,  8922,  9009,  9097,  9185,  9273,  9362,  9451,  9541,  9630,  9720,  9811,  9901,  9992,  10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288, 11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809, 12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343, 14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308, 17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665, 18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888, 19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944, 21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801, 21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435, 22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826, 22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};

/* Types. */
typedef struct SPU_VOICE_REGISTERS
{
    sint16 volume_left;
    sint16 volume_right;
    uint16 pitch;
    uint16 start_address;
    uint16 adsr1;
    uint16 adsr2;
    uint16 repeat_address;
} SPU_VOICE_REGISTERS;

typedef struct SPU_VOICE
{
    SPU_VOICE_REGISTERS registers;
    uint32 current_address;
    uint32 repeat_address;
    uint32 phase;
    uint64 sample_position;
    sint16 history1;
    sint16 history2;
    sint16 decoded[31];
    uint8 decoded_index;
    uint8 decoded_valid;
    uint8 block_flags;
    uint8 first_block;
    uint16 envelope;
    SPU_ADSR_PHASE adsr_phase;
    uint32 envelope_counter;
    uint16 envelope_increment;
    sint16 envelope_step;
    uint8 envelope_rate;
    uint8 envelope_decreasing;
    uint8 envelope_exponential;
} SPU_VOICE;

typedef struct SPU_STATE
{
    uint8 ram[SPU_RAM_SIZE];
    SPU_VOICE voices[SPU_VOICE_COUNT];
    uint32 end_flags;
    sint16 master_left;
    sint16 master_right;
    uint8 initialized;
} SPU_STATE;

enum
{
    VAB_BANK_COUNT = 16,
    VAB_PROGRAM_COUNT = 128,
    VAB_TONES_PER_PROGRAM = 16,
    SPU_MIX_CHUNK_FRAMES = 256
};

typedef struct VAB_HEADER_RECORD
{
    uint32 magic;
    uint8 reserved_04[0x0e];
    uint16 program_count;
    uint16 tone_count;
    uint16 sample_count;
    uint8 master_volume;
    uint8 master_pan;
    uint8 reserved_1a[6];
} VAB_HEADER_RECORD;

typedef struct VAB_PROGRAM_RECORD
{
    uint8 tone_count;
    uint8 volume;
    uint8 priority;
    uint8 mode;
    uint8 pan;
    uint8 reserved_05[0x0b];
} VAB_PROGRAM_RECORD;

typedef struct VAB_TONE_RECORD
{
    uint8 priority;
    uint8 mode;
    uint8 volume;
    uint8 pan;
    uint8 center;
    uint8 shift;
    uint8 note_min;
    uint8 note_max;
    uint8 reserved_08[4];
    uint8 pitch_bend_min;
    uint8 pitch_bend_max;
    uint8 reserved_0e[2];
    uint16 adsr1;
    uint16 adsr2;
    sint16 program;
    sint16 sample;
    uint8 reserved_18[8];
} VAB_TONE_RECORD;

typedef struct VAB_PROGRAM
{
    uint8 tone_count;
    uint8 volume;
    uint8 priority;
    uint8 mode;
    uint8 pan;
} VAB_PROGRAM;

typedef struct VAB_TONE
{
    uint8 priority;
    uint8 mode;
    uint8 volume;
    uint8 pan;
    uint8 center;
    uint8 shift;
    uint8 note_min;
    uint8 note_max;
    uint8 pitch_bend_min;
    uint8 pitch_bend_max;
    uint16 adsr1;
    uint16 adsr2;
    sint16 program;
    sint16 sample;
} VAB_TONE;

typedef struct VAB_BANK
{
    VAB_PROGRAM programs[VAB_PROGRAM_COUNT];
    VAB_TONE tones[VAB_PROGRAM_COUNT][VAB_TONES_PER_PROGRAM];
    uint32 sample_addresses[256];
    uint32 body_address;
    uint32 body_size;
    uint8 master_volume;
    uint8 master_pan;
    uint8 program_count;
    uint8 valid;
    uint8 transferred;
} VAB_BANK;

typedef struct LIBSND_VOICE
{
    sint16 bank;
    sint16 program;
    sint16 tone;
    uint16 age;
    uint8 priority;
    uint8 keyed;
} LIBSND_VOICE;

typedef struct IMA_ADPCM_STREAM
{
    const uint8 *file_data;
    uint32 file_size;
    const uint8 *audio_data;
    uint32 audio_size;
    uint32 total_frames;
    uint32 decoded_frames;
    uint32 block_offset;
    uint32 sample_in_block;
    uint16 block_align;
    uint16 samples_per_block;
    uint16 channels;
    uint32 sample_rate;
    sint32 predictor[2];
    sint32 step_index[2];
    sint32 block_loaded;
} IMA_ADPCM_STREAM;

typedef struct CD_AUDIO_STATE
{
    uint8 *data;
    IMA_ADPCM_STREAM stream;
    CdlATV mix;
    sint16 volume_left;
    sint16 volume_right;
    sint32 track;
    uint8 active;
    uint8 paused;
    uint8 muted;
} CD_AUDIO_STATE;

typedef struct LIBSND_STATE
{
    VAB_BANK banks[VAB_BANK_COUNT];
    LIBSND_VOICE voices[SPU_VOICE_COUNT];
    CD_AUDIO_STATE cd;
    CdlCB ready_callback;
    CdlCB sync_callback;
    uint32 next_sound_ram_address;
    uint32 tick_accumulator;
    uint32 tick_count;
    sint32 tick_mode;
    sint32 ticks_running;
    sint32 vab_transfer_complete;
    uint8 cd_mode;
    uint8 initialized;
} LIBSND_STATE;

/* Variables. */
static SPU_STATE spu;

static LIBSND_STATE libsnd;

static const uint16 libsnd_pitch_table[192] = {
    0x1000, 0x100e, 0x101d, 0x102c, 0x103b, 0x104a, 0x1059, 0x1068, 0x1078, 0x1087, 0x1096, 0x10a5, 0x10b5, 0x10c4, 0x10d4, 0x10e3, 0x10f3, 0x1103, 0x1113, 0x1122, 0x1132, 0x1142, 0x1152, 0x1162, 0x1172, 0x1182, 0x1193, 0x11a3, 0x11b3, 0x11c4, 0x11d4, 0x11e5, 0x11f5, 0x1206, 0x1216, 0x1227, 0x1238, 0x1249, 0x125a, 0x126b, 0x127c, 0x128d, 0x129e, 0x12af, 0x12c1, 0x12d2, 0x12e3, 0x12f5, 0x1306, 0x1318, 0x132a, 0x133c, 0x134d, 0x135f, 0x1371, 0x1383, 0x1395, 0x13a7, 0x13ba, 0x13cc, 0x13de, 0x13f1, 0x1403, 0x1416, 0x1428, 0x143b, 0x144e, 0x1460, 0x1473, 0x1486, 0x1499, 0x14ac, 0x14bf, 0x14d3, 0x14e6, 0x14f9, 0x150d, 0x1520, 0x1534, 0x1547, 0x155b, 0x156f, 0x1583, 0x1597, 0x15ab, 0x15bf, 0x15d3, 0x15e7, 0x15fb, 0x1610, 0x1624, 0x1638, 0x164d, 0x1662, 0x1676, 0x168b,
    0x16a0, 0x16b5, 0x16ca, 0x16df, 0x16f4, 0x170a, 0x171f, 0x1734, 0x174a, 0x175f, 0x1775, 0x178b, 0x17a1, 0x17b6, 0x17cc, 0x17e2, 0x17f9, 0x180f, 0x1825, 0x183b, 0x1852, 0x1868, 0x187f, 0x1896, 0x18ac, 0x18c3, 0x18da, 0x18f1, 0x1908, 0x191f, 0x1937, 0x194e, 0x1965, 0x197d, 0x1995, 0x19ac, 0x19c4, 0x19dc, 0x19f4, 0x1a0c, 0x1a24, 0x1a3c, 0x1a55, 0x1a6d, 0x1a85, 0x1a9e, 0x1ab7, 0x1acf, 0x1ae8, 0x1b01, 0x1b1a, 0x1b33, 0x1b4c, 0x1b66, 0x1b7f, 0x1b98, 0x1bb2, 0x1bcc, 0x1be5, 0x1bff, 0x1c19, 0x1c33, 0x1c4d, 0x1c67, 0x1c82, 0x1c9c, 0x1cb7, 0x1cd1, 0x1cec, 0x1d07, 0x1d22, 0x1d3d, 0x1d58, 0x1d73, 0x1d8e, 0x1da9, 0x1dc5, 0x1de0, 0x1dfc, 0x1e18, 0x1e34, 0x1e50, 0x1e6c, 0x1e88, 0x1ea4, 0x1ec1, 0x1edd, 0x1efa, 0x1f16, 0x1f33, 0x1f50, 0x1f6d, 0x1f8a, 0x1fa7, 0x1fc5, 0x1fe2,
};

/* Functions. */
static void spu_key_off(uint32 voice_mask);
static void spu_key_on(uint32 voice_mask);
static void spu_init(void);
static void spu_shutdown(void);
static void libsnd_reset(void);
static void cd_audio_stop_locked(void);

void SsInit(void)
{
    xport_audio_lock();
    if (libsnd.initialized)
    {
        xport_audio_unlock();
        return;
    }
    spu_init();
    libsnd_reset();
    libsnd.initialized = 1;
    xport_audio_unlock();
    xport_audio_init();
}

void SsEnd(void)
{
    xport_audio_lock();
    spu_key_off(SPU_ALLCH);
    cd_audio_stop_locked();
    xport_audio_unlock();
}

void SsQuit(void)
{
    xport_audio_shutdown();
    xport_audio_lock();
    cd_audio_stop_locked();
    memset(&libsnd, 0, sizeof(libsnd));
    spu_shutdown();
    xport_audio_unlock();
}

void SsSetMVol(sint16 left, sint16 right)
{
    SpuCommonAttr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mask = SPU_COMMON_MVOLL | SPU_COMMON_MVOLR;
    attr.mvol.left = (sint16)(left * 129);
    attr.mvol.right = (sint16)(right * 129);
    SpuSetCommonAttr(&attr);
}

void SsSetSerialVol(sint8 serial, sint16 left, sint16 right)
{
    if (serial != SS_SERIAL_A)
        return;
    if (left < 0)
        left = 0;
    if (left > 127)
        left = 127;
    if (right < 0)
        right = 0;
    if (right > 127)
        right = 127;
    xport_audio_lock();
    libsnd.cd.volume_left = left;
    libsnd.cd.volume_right = right;
    xport_audio_unlock();
}

void SsSetTickMode(sint32 mode)
{
    xport_audio_lock();
    libsnd.tick_mode = mode;
    libsnd.tick_accumulator = 0;
    xport_audio_unlock();
}

void SsStart(void)
{
    xport_audio_lock();
    libsnd.ticks_running = libsnd.tick_mode == SS_TICK60;
    xport_audio_unlock();
}

static void spu_init(void)
{
    memset(&spu, 0, sizeof(spu));
    spu.master_left = 0x3fff;
    spu.master_right = 0x3fff;
    spu.initialized = 1;
}

static void spu_shutdown(void)
{
    memset(&spu, 0, sizeof(spu));
}

sint32 spu_upload(uint32 byte_address, const void *source, uint32 byte_count)
{
    if (!spu.initialized || source == 0 || byte_address >= SPU_RAM_SIZE || byte_count > SPU_RAM_SIZE - byte_address)
        return 0;
    memcpy(spu.ram + byte_address, source, byte_count);
    return 1;
}

sint32 spu_download(uint32 byte_address, void *destination, uint32 byte_count)
{
    if (!spu.initialized || destination == 0 || byte_address >= SPU_RAM_SIZE || byte_count > SPU_RAM_SIZE - byte_address)
        return 0;
    memcpy(destination, spu.ram + byte_address, byte_count);
    return 1;
}

uint64 spu_voice_sample_position(sint32 voice)
{
    uint64 position;
    if (voice < 0 || voice >= SPU_VOICE_COUNT)
        return ~(uint64)0;
    xport_audio_lock();
    position = spu.voices[voice].sample_position >> 12;
    xport_audio_unlock();
    return position;
}

sint32 spu_decode_adpcm_block(const uint8 block[16], sint16 *history1, sint16 *history2, sint16 output[28])
{
    static const sint8 filter_pos[16] = {0, 60, 115, 98, 122, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const sint8 filter_neg[16] = {0, 0, -52, -55, -60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    sint32 filter;
    sint32 shift;
    sint32 previous1;
    sint32 previous2;
    sint32 index;

    if (block == 0 || history1 == 0 || history2 == 0 || output == 0)
        return 0;
    filter = block[0] >> 4;
    shift = block[0] & 15;
    if (shift > 12)
        shift = 9;
    previous1 = *history1;
    previous2 = *history2;

    for (index = 0; index < 28; ++index)
    {
        uint8 packed = block[2 + index / 2];
        sint32 nibble = (index & 1) ? (packed >> 4) : (packed & 15);
        sint32 sample;
        if (nibble & 8)
            nibble -= 16;
        sample = (nibble << 12) >> shift;
        sample += (previous1 * filter_pos[filter]) >> 6;
        sample += (previous2 * filter_neg[filter]) >> 6;
        sample = audio_sample_clamp(sample);
        previous2 = previous1;
        previous1 = sample;
        output[index] = (sint16)sample;
    }

    *history1 = (sint16)previous1;
    *history2 = (sint16)previous2;
    return 28;
}

static void spu_set_voice_registers(sint32 voice, const SPU_VOICE_REGISTERS *registers)
{
    if (!spu.initialized || registers == 0 || voice < 0 || voice >= SPU_VOICE_COUNT)
        return;
    spu.voices[voice].registers = *registers;
    spu.voices[voice].repeat_address = (uint32)registers->repeat_address * 8u;
}

static void spu_set_voice_volume(sint32 voice, sint16 left, sint16 right)
{
    if (!spu.initialized || voice < 0 || voice >= SPU_VOICE_COUNT)
        return;
    spu.voices[voice].registers.volume_left = left;
    spu.voices[voice].registers.volume_right = right;
}

static void spu_set_voice_pitch(sint32 voice, uint16 pitch)
{
    if (!spu.initialized || voice < 0 || voice >= SPU_VOICE_COUNT)
        return;
    spu.voices[voice].registers.pitch = pitch;
}

static sint32 spu_get_voice_registers(sint32 voice, SPU_VOICE_REGISTERS *registers)
{
    if (!spu.initialized || !registers || voice < 0 || voice >= SPU_VOICE_COUNT)
        return 0;
    *registers = spu.voices[voice].registers;
    return 1;
}

static uint16 read_u16_le(const uint8 *data)
{
    return (uint16)(data[0] | ((uint16)data[1] << 8));
}

static uint32 read_u32_le(const uint8 *data)
{
    return (uint32)data[0] | ((uint32)data[1] << 8) | ((uint32)data[2] << 16) | ((uint32)data[3] << 24);
}

static void libsnd_reset(void)
{
    sint32 voice;
    memset(&libsnd, 0, sizeof(libsnd));
    libsnd.next_sound_ram_address = 0x1010;
    libsnd.vab_transfer_complete = 1;
    libsnd.cd.track = -1;
    libsnd.cd.volume_left = 127;
    libsnd.cd.volume_right = 127;
    libsnd.cd.mix.val0 = 128;
    libsnd.cd.mix.val3 = 128;
    for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
        libsnd.voices[voice].age = SPU_VOICE_COUNT;
}

static sint32 vab_is_valid(sint16 bank)
{
    return bank >= 0 && bank < VAB_BANK_COUNT && libsnd.banks[bank].valid && libsnd.banks[bank].transferred;
}

static const VAB_PROGRAM *vab_get_program(sint16 bank, sint16 program)
{
    if (!vab_is_valid(bank) || program < 0 || program >= VAB_PROGRAM_COUNT || libsnd.banks[bank].programs[program].tone_count == 0)
        return 0;
    return &libsnd.banks[bank].programs[program];
}

static const VAB_TONE *vab_get_tone(sint16 bank, sint16 program, sint16 tone)
{
    const VAB_PROGRAM *program_entry = vab_get_program(bank, program);
    if (program_entry == 0 || tone < 0 || tone >= program_entry->tone_count)
        return 0;
    return &libsnd.banks[bank].tones[program][tone];
}

static sint16 vab_open_head(const uint8 *header, sint16 requested_bank)
{
    const VAB_HEADER_RECORD *header_record = (const VAB_HEADER_RECORD *)header;
    VAB_BANK parsed;
    uint16 program_count;
    uint16 tone_count;
    uint16 sample_count;
    uint32 tone_group = 0;
    uint32 program;
    uint32 size_table_offset;
    uint32 sample_offset = 0;
    sint16 bank;

    if (header == 0 || header_record->magic != 0x56414270u)
        return -1;
    program_count = header_record->program_count;
    tone_count = header_record->tone_count;
    sample_count = header_record->sample_count;
    if (program_count > VAB_PROGRAM_COUNT || tone_count > 2048 || sample_count > 255)
        return -1;
    if (requested_bank < 0)
    {
        for (bank = 0; bank < VAB_BANK_COUNT; ++bank)
            if (!libsnd.banks[bank].valid)
                break;
        if (bank == VAB_BANK_COUNT)
            return -1;
    }
    else
    {
        if (requested_bank >= VAB_BANK_COUNT || libsnd.banks[requested_bank].valid)
            return -1;
        bank = requested_bank;
    }

    memset(&parsed, 0, sizeof(parsed));
    parsed.master_volume = header_record->master_volume;
    parsed.master_pan = header_record->master_pan;
    parsed.program_count = (uint8)program_count;
    for (program = 0; program < VAB_PROGRAM_COUNT; ++program)
    {
        const VAB_PROGRAM_RECORD *source = (const VAB_PROGRAM_RECORD *)(header_record + 1) + program;
        VAB_PROGRAM *destination = parsed.programs + program;
        uint32 tone;
        if (source->tone_count == 0)
            continue;
        if (source->tone_count > VAB_TONES_PER_PROGRAM || tone_group >= program_count)
            return -1;
        destination->tone_count = source->tone_count;
        destination->volume = source->volume;
        destination->priority = source->priority;
        destination->mode = source->mode;
        destination->pan = source->pan;
        for (tone = 0; tone < source->tone_count; ++tone)
        {
            const VAB_TONE_RECORD *tone_source = (const VAB_TONE_RECORD *)((const VAB_PROGRAM_RECORD *)(header_record + 1) + VAB_PROGRAM_COUNT) + tone_group * VAB_TONES_PER_PROGRAM + tone;
            VAB_TONE *tone_destination = &parsed.tones[program][tone];
            tone_destination->priority = tone_source->priority;
            tone_destination->mode = tone_source->mode;
            tone_destination->volume = tone_source->volume;
            tone_destination->pan = tone_source->pan;
            tone_destination->center = tone_source->center;
            tone_destination->shift = tone_source->shift;
            tone_destination->note_min = tone_source->note_min;
            tone_destination->note_max = tone_source->note_max;
            tone_destination->pitch_bend_min = tone_source->pitch_bend_min;
            tone_destination->pitch_bend_max = tone_source->pitch_bend_max;
            tone_destination->adsr1 = tone_source->adsr1;
            tone_destination->adsr2 = tone_source->adsr2;
            tone_destination->program = tone_source->program;
            tone_destination->sample = tone_source->sample;
            if (tone_destination->sample < 1 || tone_destination->sample > (sint16)sample_count)
                return -1;
        }
        ++tone_group;
    }
    if (tone_group != program_count)
        return -1;

    size_table_offset = 32 + 128 * 16 + (uint32)program_count * 16 * 32;
    for (program = 0; program < 256; ++program)
    {
        uint32 size = (uint32)read_u16_le(header + size_table_offset + program * 2) * 8u;
        if (program >= 1 && program <= sample_count)
            parsed.sample_addresses[program] = libsnd.next_sound_ram_address + sample_offset;
        sample_offset += size;
    }
    parsed.body_size = sample_offset;
    if (parsed.body_size == 0 || libsnd.next_sound_ram_address > SPU_RAM_SIZE || parsed.body_size > SPU_RAM_SIZE - libsnd.next_sound_ram_address)
        return -1;
    parsed.body_address = libsnd.next_sound_ram_address;
    parsed.valid = 1;
    libsnd.banks[bank] = parsed;
    libsnd.next_sound_ram_address = (libsnd.next_sound_ram_address + parsed.body_size + 7u) & ~7u;
    return bank;
}

sint16 SsVabOpenHead(uint8 *header, sint16 requested_bank)
{
    sint16 bank;
    xport_audio_lock();
    bank = libsnd.initialized ? vab_open_head(header, requested_bank) : -1;
    if (bank >= 0)
        libsnd.vab_transfer_complete = 0;
    xport_audio_unlock();
    return bank;
}

sint16 SsVabTransBody(uint8 *body, sint16 bank)
{
    VAB_BANK *entry;
    sint16 result = -1;
    xport_audio_lock();
    if (libsnd.initialized && bank >= 0 && bank < VAB_BANK_COUNT && body != 0)
    {
        entry = libsnd.banks + bank;
        if (entry->valid && spu_upload(entry->body_address, body, entry->body_size))
        {
            entry->transferred = 1;
            result = bank;
        }
    }
    libsnd.vab_transfer_complete = result >= 0;
    xport_audio_unlock();
    return result;
}

sint16 SsVabTransCompleted(sint16 mode)
{
    sint16 complete;
    xport_audio_lock();
    complete = (sint16)libsnd.vab_transfer_complete;
    xport_audio_unlock();
    return complete;
}

static uint16 note_to_pitch(sint16 note, uint16 fine, const VAB_TONE *tone)
{
    sint32 fine_index = ((sint32)fine + tone->shift) / 8;
    sint32 carry = 0;
    sint32 semitone;
    sint32 octave;
    sint32 remainder;
    uint32 pitch;
    if (fine_index >= 16)
    {
        carry = 1;
        fine_index -= 16;
    }
    semitone = carry + note + 60 - tone->center;
    octave = semitone / 12;
    if (semitone < 0 && semitone % 12)
        --octave;
    remainder = semitone - octave * 12;
    pitch = libsnd_pitch_table[remainder * 16 + fine_index];
    if (octave > 5)
        pitch <<= octave - 5;
    else if (octave < 5)
        pitch >>= 5 - octave;
    return (uint16)pitch;
}

static sint16 compose_volume(sint16 primary, sint16 secondary, uint8 bank_volume, const VAB_PROGRAM *program, const VAB_TONE *tone, sint32 left)
{
    uint32 maximum;
    uint32 pan;
    uint32 volume;
    uint32 channel;
    if (primary < 0)
        primary = 0;
    if (secondary < 0)
        secondary = 0;
    if (primary == secondary)
    {
        maximum = (uint32)primary;
        pan = 64;
    }
    else if (secondary >= primary)
    {
        maximum = (uint32)secondary;
        pan = secondary ? 127u - ((uint32)primary << 6) / (uint32)secondary : 64;
    }
    else
    {
        maximum = (uint32)primary;
        pan = primary ? ((uint32)secondary << 6) / (uint32)primary : 64;
    }
    volume = maximum * 0x3fffu * bank_volume / 16129u;
    volume = volume * program->volume * tone->volume / 0x3f01u;
    channel = volume;
    if (tone->pan >= 64)
    {
        if (left)
            channel = channel * (127 - tone->pan) / 63;
    }
    else if (!left)
        channel = channel * tone->pan / 63;
    if (program->pan >= 64)
    {
        if (left)
            channel = channel * (127 - program->pan) / 63;
    }
    else if (!left)
        channel = channel * program->pan / 63;
    if (pan >= 64)
    {
        if (left)
            channel = channel * (127 - pan) / 63;
    }
    else if (!left)
        channel = channel * pan / 63;
    return (sint16)(channel * channel / 0x3fff);
}

static sint16 start_voice(sint16 voice, sint16 bank, sint16 program, sint16 tone_index, sint16 note, sint16 fine, sint16 left, sint16 right)
{
    const VAB_PROGRAM *program_entry = vab_get_program(bank, program);
    const VAB_TONE *tone = vab_get_tone(bank, program, tone_index);
    SPU_VOICE_REGISTERS registers;
    uint32 sample_address;
    if (voice < 0 || voice >= SPU_VOICE_COUNT || program_entry == 0 || tone == 0)
        return -1;
    sample_address = libsnd.banks[bank].sample_addresses[tone->sample];
    if (sample_address == 0)
        return -1;
    memset(&registers, 0, sizeof(registers));
    registers.volume_left = compose_volume(left, right, libsnd.banks[bank].master_volume, program_entry, tone, 1);
    registers.volume_right = compose_volume(left, right, libsnd.banks[bank].master_volume, program_entry, tone, 0);
    registers.pitch = note_to_pitch(note, (uint16)fine, tone);
    registers.start_address = (uint16)(sample_address >> 3);
    registers.repeat_address = registers.start_address;
    registers.adsr1 = tone->adsr1;
    registers.adsr2 = tone->adsr2;
    spu_set_voice_registers(voice, &registers);
    spu_key_on(1u << voice);
    libsnd.voices[voice].bank = bank;
    libsnd.voices[voice].program = program;
    libsnd.voices[voice].tone = tone_index;
    libsnd.voices[voice].priority = tone->priority;
    libsnd.voices[voice].age = 0;
    libsnd.voices[voice].keyed = 1;
    return voice;
}

sint16 SsUtKeyOn(sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right)
{
    const VAB_TONE *requested_tone;
    sint16 voice;
    sint16 selected = -1;
    sint16 best_priority;
    uint16 best_envelope = 0xffffu;
    sint16 best_age = 0;
    xport_audio_lock();
    requested_tone = vab_get_tone(bank, program, tone);
    if (requested_tone == 0)
    {
        xport_audio_unlock();
        return -1;
    }
    best_priority = requested_tone->priority;
    for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
    {
        sint16 priority = libsnd.voices[voice].priority;
        if (spu.voices[voice].adsr_phase == SPU_ADSR_OFF && spu.voices[voice].envelope == 0)
        {
            selected = voice;
            break;
        }
        if (priority < best_priority)
        {
            best_priority = priority;
            best_envelope = spu.voices[voice].envelope;
            best_age = (sint16)libsnd.voices[voice].age;
            selected = voice;
        }
        else if (priority == best_priority && (spu.voices[voice].envelope < best_envelope || (spu.voices[voice].envelope == best_envelope && (sint16)libsnd.voices[voice].age > best_age)))
        {
            best_envelope = spu.voices[voice].envelope;
            best_age = (sint16)libsnd.voices[voice].age;
            selected = voice;
        }
    }
    if (selected >= 0)
    {
        for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
            ++libsnd.voices[voice].age;
        selected = start_voice(selected, bank, program, tone, note, fine, left, right);
    }
    xport_audio_unlock();
    return selected;
}

sint16 SsUtKeyOnV(sint16 voice, sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right)
{
    sint16 result;
    xport_audio_lock();
    result = start_voice(voice, bank, program, tone, note, fine, left, right);
    xport_audio_unlock();
    return result;
}

void SpuInit(void)
{
    xport_audio_lock();
    spu_init();
    xport_audio_unlock();
}

void SpuInitHot(void)
{
    SpuInit();
}

void SpuQuit(void)
{
    xport_audio_lock();
    spu_shutdown();
    xport_audio_unlock();
}

void SpuSetVoiceAttr(SpuVoiceAttr *attr)
{
    sint32 voice;
    if (!attr)
        return;
    xport_audio_lock();
    for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
    {
        SPU_VOICE_REGISTERS registers;
        if (!(attr->voice & SPU_KEYCH(voice)) || !spu_get_voice_registers(voice, &registers))
            continue;
        if (attr->mask & SPU_VOICE_VOLL)
            registers.volume_left = attr->volume.left;
        if (attr->mask & SPU_VOICE_VOLR)
            registers.volume_right = attr->volume.right;
        if (attr->mask & SPU_VOICE_PITCH)
            registers.pitch = attr->pitch;
        if (attr->mask & SPU_VOICE_WDSA)
            registers.start_address = (uint16)(attr->addr >> 3);
        if (attr->mask & SPU_VOICE_LSAX)
            registers.repeat_address = (uint16)(attr->loop_addr >> 3);
        if (attr->mask & SPU_VOICE_ADSR_ADSR1)
            registers.adsr1 = attr->adsr1;
        if (attr->mask & SPU_VOICE_ADSR_ADSR2)
            registers.adsr2 = attr->adsr2;
        spu_set_voice_registers(voice, &registers);
    }
    xport_audio_unlock();
}

void SpuGetVoiceAttr(SpuVoiceAttr *attr)
{
    SpuVoiceAttr result;
    SPU_VOICE_REGISTERS registers;
    uint32 requested;
    sint32 voice;
    if (!attr)
        return;
    requested = attr->voice;
    for (voice = 0; voice < SPU_VOICE_COUNT && !(requested & SPU_KEYCH(voice)); ++voice)
        ;
    if (voice == SPU_VOICE_COUNT)
        return;
    xport_audio_lock();
    if (!spu_get_voice_registers(voice, &registers))
    {
        xport_audio_unlock();
        return;
    }
    memset(&result, 0, sizeof(result));
    result.voice = SPU_KEYCH(voice);
    result.volume.left = registers.volume_left;
    result.volume.right = registers.volume_right;
    result.volumex = result.volume;
    result.pitch = registers.pitch;
    result.envx = (sint16)spu.voices[voice].envelope;
    result.addr = (uint32)registers.start_address * 8u;
    result.loop_addr = (uint32)registers.repeat_address * 8u;
    result.adsr1 = registers.adsr1;
    result.adsr2 = registers.adsr2;
    *attr = result;
    xport_audio_unlock();
}

void SpuSetKey(sint32 on_off, uint32 voice_bit)
{
    xport_audio_lock();
    if (on_off == SPU_ON)
        spu_key_on(voice_bit & SPU_ALLCH);
    else
        spu_key_off(voice_bit & SPU_ALLCH);
    xport_audio_unlock();
}

sint32 SpuGetKeyStatus(uint32 voice_bit)
{
    sint32 voice;
    sint32 status = 0;
    xport_audio_lock();
    for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
        if ((voice_bit & SPU_KEYCH(voice)) && spu.voices[voice].adsr_phase != SPU_ADSR_OFF)
        {
            status = 1;
            break;
        }
    xport_audio_unlock();
    return status;
}

void SpuSetCommonAttr(SpuCommonAttr *attr)
{
    if (!attr)
        return;
    xport_audio_lock();
    if (attr->mask & SPU_COMMON_MVOLL)
        spu.master_left = attr->mvol.left;
    if (attr->mask & SPU_COMMON_MVOLR)
        spu.master_right = attr->mvol.right;
    xport_audio_unlock();
}

void SpuGetCommonAttr(SpuCommonAttr *attr)
{
    if (!attr)
        return;
    xport_audio_lock();
    memset(attr, 0, sizeof(*attr));
    attr->mvol.left = spu.master_left;
    attr->mvol.right = spu.master_right;
    attr->mvolx = attr->mvol;
    xport_audio_unlock();
}

void SpuSetVoiceVolume(sint32 voice, sint16 left, sint16 right)
{
    xport_audio_lock();
    spu_set_voice_volume(voice, left, right);
    xport_audio_unlock();
}

void SpuSetVoicePitch(sint32 voice, uint16 pitch)
{
    xport_audio_lock();
    spu_set_voice_pitch(voice, pitch);
    xport_audio_unlock();
}

void SpuSetVoiceStartAddr(sint32 voice, uint32 start_address)
{
    xport_audio_lock();
    if (spu.initialized && voice >= 0 && voice < SPU_VOICE_COUNT)
        spu.voices[voice].registers.start_address = (uint16)(start_address >> 3);
    xport_audio_unlock();
}

void SpuSetVoiceLoopStartAddr(sint32 voice, uint32 loop_start_address)
{
    xport_audio_lock();
    if (spu.initialized && voice >= 0 && voice < SPU_VOICE_COUNT)
    {
        spu.voices[voice].registers.repeat_address = (uint16)(loop_start_address >> 3);
        spu.voices[voice].repeat_address = loop_start_address;
    }
    xport_audio_unlock();
}

static void envelope_reset(SPU_VOICE *voice, uint8 rate, uint8 rate_mask, sint32 decreasing, sint32 exponential)
{
    sint32 base_step = 7 - (rate & 3);
    voice->envelope_rate = rate;
    voice->envelope_decreasing = (uint8)decreasing;
    voice->envelope_exponential = (uint8)exponential;
    voice->envelope_counter = 0;
    voice->envelope_increment = 0x8000;
    voice->envelope_step = (sint16)(decreasing ? ~base_step : base_step);
    if (rate < 44)
    {
        voice->envelope_step = (sint16)(voice->envelope_step << (11 - (rate >> 2)));
    }
    else if (rate >= 48)
    {
        voice->envelope_increment = (uint16)(voice->envelope_increment >> ((rate >> 2) - 11));
        if ((rate & rate_mask) == rate_mask)
            voice->envelope_increment = 0;
        else if (voice->envelope_increment == 0)
            voice->envelope_increment = 1;
    }
}

static void update_adsr(SPU_VOICE *voice)
{
    uint16 adsr1 = voice->registers.adsr1;
    uint16 adsr2 = voice->registers.adsr2;
    switch (voice->adsr_phase)
    {
        case SPU_ADSR_ATTACK:
            envelope_reset(voice, (uint8)((adsr1 >> 8) & 0x7f), 0x7f, 0, (adsr1 & 0x8000) != 0);
            break;
        case SPU_ADSR_DECAY:
            envelope_reset(voice, (uint8)(((adsr1 >> 4) & 0x0f) << 2), 0x7c, 1, 1);
            break;
        case SPU_ADSR_SUSTAIN:
            envelope_reset(voice, (uint8)((adsr2 >> 6) & 0x7f), 0x7f, (adsr2 & 0x4000) != 0, (adsr2 & 0x8000) != 0);
            break;
        case SPU_ADSR_RELEASE:
            envelope_reset(voice, (uint8)((adsr2 & 0x1f) << 2), 0x7c, 1, (adsr2 & 0x20) != 0);
            break;
        default:
            voice->envelope_counter = 0;
            voice->envelope_increment = 0;
            voice->envelope_step = 0;
            break;
    }
}

static void tick_adsr(SPU_VOICE *voice)
{
    sint32 step;
    sint32 level;
    uint32 increment;
    sint32 target;
    if (voice->adsr_phase == SPU_ADSR_OFF || voice->envelope_increment == 0)
        return;
    step = voice->envelope_step;
    level = voice->envelope;
    increment = voice->envelope_increment;
    if (voice->envelope_exponential)
    {
        if (voice->envelope_decreasing)
        {
            step = (step * level) >> 15;
        }
        else if (level >= 0x6000)
        {
            if (voice->envelope_rate < 40)
                step >>= 2;
            else if (voice->envelope_rate >= 44)
                increment >>= 2;
            else
            {
                step >>= 1;
                increment >>= 1;
            }
        }
    }
    voice->envelope_counter += increment;
    if (!(voice->envelope_counter & 0x8000))
        return;
    voice->envelope_counter = 0;
    level += step;
    if (level < 0)
        level = 0;
    if (level > 32767)
        level = 32767;
    voice->envelope = (uint16)level;

    if (voice->adsr_phase == SPU_ADSR_ATTACK)
        target = 32767;
    else if (voice->adsr_phase == SPU_ADSR_DECAY)
    {
        target = ((voice->registers.adsr1 & 15) + 1) * 0x800;
        if (target > 32767)
            target = 32767;
    }
    else if (voice->adsr_phase == SPU_ADSR_RELEASE)
        target = 0;
    else
        return;

    if ((!voice->envelope_decreasing && level >= target) || (voice->envelope_decreasing && level <= target))
    {
        if (voice->adsr_phase == SPU_ADSR_ATTACK)
            voice->adsr_phase = SPU_ADSR_DECAY;
        else if (voice->adsr_phase == SPU_ADSR_DECAY)
            voice->adsr_phase = SPU_ADSR_SUSTAIN;
        else
        {
            voice->adsr_phase = SPU_ADSR_OFF;
            voice->envelope = 0;
        }
        update_adsr(voice);
    }
}

static void decode_voice_block(SPU_VOICE *voice)
{
    uint8 block[16];
    uint32 index;
    for (index = 0; index < 16; ++index)
        block[index] = spu.ram[(voice->current_address + index) & (SPU_RAM_SIZE - 1)];
    voice->decoded[0] = voice->decoded[28];
    voice->decoded[1] = voice->decoded[29];
    voice->decoded[2] = voice->decoded[30];
    spu_decode_adpcm_block(block, &voice->history1, &voice->history2, voice->decoded + 3);
    voice->block_flags = block[1];
    if (block[1] & 4)
        voice->repeat_address = voice->current_address;
    voice->decoded_valid = 1;
}

static sint32 interpolate_voice(const SPU_VOICE *voice)
{
    uint32 fraction = (voice->phase >> 4) & 0xff;
    uint32 sample = 3 + (voice->phase >> 12);
    sint32 result = spu_gaussian[0x0ff - fraction] * voice->decoded[sample - 3];
    result += spu_gaussian[0x1ff - fraction] * voice->decoded[sample - 2];
    result += spu_gaussian[0x100 + fraction] * voice->decoded[sample - 1];
    result += spu_gaussian[0x000 + fraction] * voice->decoded[sample];
    return result >> 15;
}

static sint32 fixed_volume(uint16 bits)
{
    if (bits & 0x8000)
        return 0; /* Volume sweeps are outside AA's proven use. */
    return (sint16)(bits << 1);
}

static sint32 apply_volume(sint32 sample, sint32 volume)
{
    return (sample * volume) >> 15;
}

static sint32 master_volume_8(uint16 bits)
{
    sint32 volume = fixed_volume(bits);
    if (volume <= 0)
        return 0;
    return (volume + 64) >> 7;
}

static void spu_key_on(uint32 voice_mask)
{
    sint32 index;
    if (!spu.initialized)
        return;
    for (index = 0; index < SPU_VOICE_COUNT; ++index)
    {
        SPU_VOICE *voice;
        if (!(voice_mask & (1u << index)))
            continue;
        voice = spu.voices + index;
        voice->current_address = (uint32)(voice->registers.start_address & 0xfffeu) * 8u;
        voice->repeat_address = (uint32)voice->registers.repeat_address * 8u;
        voice->phase = 0;
        voice->sample_position = 0;
        voice->history1 = 0;
        voice->history2 = 0;
        memset(voice->decoded, 0, sizeof(voice->decoded));
        voice->decoded_index = 0;
        voice->decoded_valid = 0;
        voice->block_flags = 0;
        voice->first_block = 1;
        voice->envelope = 0;
        voice->adsr_phase = SPU_ADSR_ATTACK;
        update_adsr(voice);
        spu.end_flags &= ~(1u << index);
    }
}

static void spu_key_off(uint32 voice_mask)
{
    sint32 index;
    if (!spu.initialized)
        return;
    for (index = 0; index < SPU_VOICE_COUNT; ++index)
    {
        SPU_VOICE *voice;
        if (!(voice_mask & (1u << index)))
            continue;
        voice = spu.voices + index;
        if (voice->adsr_phase != SPU_ADSR_OFF)
            voice->adsr_phase = SPU_ADSR_RELEASE;
    }
}

uint32 spu_end_flags(void)
{
    return spu.end_flags;
}

SPU_ADSR_PHASE spu_voice_phase(sint32 voice)
{
    if (voice < 0 || voice >= SPU_VOICE_COUNT)
        return SPU_ADSR_OFF;
    return spu.voices[voice].adsr_phase;
}

uint16 spu_voice_envelope(sint32 voice)
{
    if (voice < 0 || voice >= SPU_VOICE_COUNT)
        return 0;
    return spu.voices[voice].envelope;
}

static const sint32 ima_index_table[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static const sint32 ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static sint16 ima_decode_nibble(IMA_ADPCM_STREAM *stream, sint32 channel, uint8 nibble)
{
    sint32 step = ima_step_table[stream->step_index[channel]];
    sint32 difference = step >> 3;
    if (nibble & 1)
        difference += step >> 2;
    if (nibble & 2)
        difference += step >> 1;
    if (nibble & 4)
        difference += step;
    if (nibble & 8)
        stream->predictor[channel] -= difference;
    else
        stream->predictor[channel] += difference;
    stream->predictor[channel] = audio_sample_clamp(stream->predictor[channel]);
    stream->step_index[channel] += ima_index_table[nibble & 7];
    if (stream->step_index[channel] < 0)
        stream->step_index[channel] = 0;
    if (stream->step_index[channel] > 88)
        stream->step_index[channel] = 88;
    return (sint16)stream->predictor[channel];
}

static sint32 ima_load_block(IMA_ADPCM_STREAM *stream)
{
    const uint8 *block;
    uint32 header_size = stream->channels * 4u;
    sint32 channel;
    if (stream->block_offset >= stream->audio_size || stream->audio_size - stream->block_offset < header_size)
        return 0;
    block = stream->audio_data + stream->block_offset;
    for (channel = 0; channel < stream->channels; ++channel)
    {
        stream->predictor[channel] = (sint16)read_u16_le(block + channel * 4);
        stream->step_index[channel] = block[channel * 4 + 2];
        if (stream->step_index[channel] > 88)
            return 0;
    }
    stream->sample_in_block = 0;
    stream->block_loaded = 1;
    return 1;
}

static sint32 ima_open(IMA_ADPCM_STREAM *stream, const uint8 *data, uint32 size)
{
    uint32 offset = 12;
    uint32 fact_frames = 0;
    sint32 have_format = 0;
    memset(stream, 0, sizeof(*stream));
    if (data == 0 || size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
        return 0;
    while (offset + 8 <= size)
    {
        const uint8 *chunk = data + offset;
        uint32 chunk_size = read_u32_le(chunk + 4);
        uint32 payload = offset + 8;
        if (chunk_size > size - payload)
            return 0;
        if (memcmp(chunk, "fmt ", 4) == 0)
        {
            if (chunk_size < 20 || read_u16_le(data + payload) != 0x11)
                return 0;
            stream->channels = read_u16_le(data + payload + 2);
            stream->sample_rate = read_u32_le(data + payload + 4);
            stream->block_align = read_u16_le(data + payload + 12);
            if (read_u16_le(data + payload + 14) != 4 || read_u16_le(data + payload + 16) < 2)
                return 0;
            stream->samples_per_block = read_u16_le(data + payload + 18);
            have_format = 1;
        }
        else if (memcmp(chunk, "fact", 4) == 0 && chunk_size >= 4)
            fact_frames = read_u32_le(data + payload);
        else if (memcmp(chunk, "data", 4) == 0)
        {
            stream->audio_data = data + payload;
            stream->audio_size = chunk_size;
        }
        offset = payload + chunk_size + (chunk_size & 1u);
    }
    if (!have_format || stream->audio_data == 0 || stream->channels == 0 || stream->channels > 2 || stream->sample_rate != SPU_SAMPLE_RATE || stream->block_align < stream->channels * 4u || stream->samples_per_block == 0)
        return 0;
    stream->file_data = data;
    stream->file_size = size;
    stream->total_frames = fact_frames;
    if (stream->total_frames == 0)
        stream->total_frames = (stream->audio_size / stream->block_align) * stream->samples_per_block;
    return 1;
}

static uint32 ima_decode(IMA_ADPCM_STREAM *stream, sint16 *stereo, uint32 frames)
{
    uint32 written = 0;
    while (written < frames && stream->decoded_frames < stream->total_frames)
    {
        const uint8 *block;
        uint32 encoded_sample;
        uint32 group;
        uint32 within;
        sint16 sample[2];
        sint32 channel;
        if (!stream->block_loaded && !ima_load_block(stream))
            break;
        block = stream->audio_data + stream->block_offset;
        if (stream->sample_in_block == 0)
        {
            sample[0] = (sint16)stream->predictor[0];
            sample[1] = stream->channels == 2 ? (sint16)stream->predictor[1] : sample[0];
        }
        else
        {
            encoded_sample = stream->sample_in_block - 1;
            group = encoded_sample / 8u;
            within = encoded_sample & 7u;
            for (channel = 0; channel < stream->channels; ++channel)
            {
                uint32 byte_offset = stream->channels * 4u + group * stream->channels * 4u + channel * 4u + within / 2u;
                uint8 byte;
                uint8 nibble;
                if (byte_offset >= stream->block_align || stream->block_offset + byte_offset >= stream->audio_size)
                    return written;
                byte = block[byte_offset];
                nibble = (within & 1u) ? byte >> 4 : byte & 15;
                sample[channel] = ima_decode_nibble(stream, channel, nibble);
            }
            if (stream->channels == 1)
                sample[1] = sample[0];
        }
        stereo[written * 2] = sample[0];
        stereo[written * 2 + 1] = sample[1];
        ++written;
        ++stream->decoded_frames;
        ++stream->sample_in_block;
        if (stream->sample_in_block >= stream->samples_per_block)
        {
            uint32 remaining = stream->audio_size - stream->block_offset;
            stream->block_offset += remaining < stream->block_align ? remaining : stream->block_align;
            stream->block_loaded = 0;
        }
    }
    return written;
}

static uint8 *cd_track_load(sint32 track, uint32 *size)
{
    char path[64];
    FILE *file;
    long length;
    uint8 *data;
    snprintf(path, sizeof(path), "MUSIC/%d.WAV", track);
    file = fopen(path, "rb");
    if (file == 0 || fseek(file, 0, SEEK_END) != 0)
    {
        if (file != 0)
            fclose(file);
        return 0;
    }
    length = ftell(file);
    if (length <= 0 || (uint64)length > 0xffffffffu || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return 0;
    }
    data = (uint8 *)malloc((size_t)length);
    if (data == 0 || fread(data, 1, (size_t)length, file) != (size_t)length)
    {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *size = (uint32)length;
    return data;
}

static void cd_audio_stop_locked(void)
{
    free(libsnd.cd.data);
    libsnd.cd.data = 0;
    memset(&libsnd.cd.stream, 0, sizeof(libsnd.cd.stream));
    libsnd.cd.track = -1;
    libsnd.cd.active = 0;
    libsnd.cd.paused = 0;
}

sint32 CdPlay(sint32 mode, sint32 *track, sint32 offset)
{
    IMA_ADPCM_STREAM stream;
    uint8 *data;
    uint32 size;
    sint32 selected;
    CdlCB callback;
    if (track == 0 || mode < 0 || offset < 0)
        return 0;
    selected = *track;
    data = cd_track_load(selected, &size);
    if (data == 0 || !ima_open(&stream, data, size))
    {
        free(data);
        return 0;
    }
    xport_audio_lock();
    cd_audio_stop_locked();
    libsnd.cd.data = data;
    libsnd.cd.stream = stream;
    libsnd.cd.track = selected;
    libsnd.cd.active = 1;
    callback = libsnd.sync_callback;
    xport_audio_unlock();
    if (callback != 0)
        callback(CdlComplete, 0);
    return 1;
}

sint32 CdControl(uint8 command, uint8 *parameter, uint8 *result)
{
    CdlCB callback;
    uint8 status = CdlComplete;
    xport_audio_lock();
    switch (command)
    {
        case CdlPlay:
            if (libsnd.cd.data != 0)
            {
                libsnd.cd.active = 1;
                libsnd.cd.paused = 0;
            }
            break;
        case CdlPause:
            libsnd.cd.paused = 1;
            break;
        case CdlStop:
            cd_audio_stop_locked();
            break;
        case CdlMute:
            libsnd.cd.muted = 1;
            break;
        case CdlDemute:
            libsnd.cd.muted = 0;
            break;
        case CdlSetmode:
            libsnd.cd_mode = parameter != 0 ? parameter[0] : 0;
            break;
        default:
            status = CdlDiskError;
            break;
    }
    callback = libsnd.sync_callback;
    xport_audio_unlock();
    if (result != 0)
        result[0] = status;
    if (callback != 0)
        callback(status, result);
    return status != CdlDiskError;
}

sint32 CdControlB(uint8 command, uint8 *parameter, uint8 *result)
{
    return CdControl(command, parameter, result);
}

sint32 CdControlF(uint8 command, uint8 *parameter)
{
    return CdControl(command, parameter, 0);
}

sint32 CdMix(CdlATV *volume)
{
    if (volume == 0)
        return 0;
    xport_audio_lock();
    libsnd.cd.mix = *volume;
    xport_audio_unlock();
    return 1;
}

CdlCB CdReadyCallback(CdlCB callback)
{
    CdlCB previous;
    xport_audio_lock();
    previous = libsnd.ready_callback;
    libsnd.ready_callback = callback;
    xport_audio_unlock();
    return previous;
}

CdlCB CdSyncCallback(CdlCB callback)
{
    CdlCB previous;
    xport_audio_lock();
    previous = libsnd.sync_callback;
    libsnd.sync_callback = callback;
    xport_audio_unlock();
    return previous;
}

void spu_mix(sint32 *interleaved_stereo, uint32 frame_count)
{
    uint32 frame;
    if (interleaved_stereo == 0)
        return;
    for (frame = 0; frame < frame_count; ++frame)
    {
        sint32 mix_left = 0;
        sint32 mix_right = 0;
        sint32 index;
        for (index = 0; index < SPU_VOICE_COUNT; ++index)
        {
            SPU_VOICE *voice = spu.voices + index;
            sint32 sample;
            sint32 amplitude;
            uint32 step;
            if (voice->adsr_phase == SPU_ADSR_OFF)
                continue;
            if (!voice->decoded_valid)
                decode_voice_block(voice);
            sample = interpolate_voice(voice);
            amplitude = apply_volume(sample, voice->envelope);
            mix_left += apply_volume(amplitude, fixed_volume((uint16)voice->registers.volume_left));
            mix_right += apply_volume(amplitude, fixed_volume((uint16)voice->registers.volume_right));
            tick_adsr(voice);
            step = voice->registers.pitch;
            if (step > 0x3fff)
                step = 0x3fff;
            voice->phase += step;
            voice->sample_position += step;
            if ((voice->phase >> 12) >= 28)
            {
                voice->phase -= 28u << 12;
                voice->decoded_valid = 0;
                voice->first_block = 0;
                voice->current_address = (voice->current_address + 16) & (SPU_RAM_SIZE - 1);
                if (voice->block_flags & 1)
                {
                    spu.end_flags |= 1u << index;
                    voice->current_address = voice->repeat_address & (SPU_RAM_SIZE - 1);
                    if (!(voice->block_flags & 2))
                    {
                        voice->adsr_phase = SPU_ADSR_OFF;
                        voice->envelope = 0;
                    }
                }
            }
        }
        mix_left = (mix_left * master_volume_8((uint16)spu.master_left)) >> 8;
        mix_right = (mix_right * master_volume_8((uint16)spu.master_right)) >> 8;
        interleaved_stereo[frame * 2] += mix_left;
        interleaved_stereo[frame * 2 + 1] += mix_right;
    }
}

void spu_render(sint16 *interleaved_stereo, uint32 frame_count)
{
    uint32 offset = 0;
    CdlCB end_callback = 0;
    if (interleaved_stereo == 0 || frame_count == 0)
        return;
    memset(interleaved_stereo, 0, (size_t)frame_count * 2u * sizeof(*interleaved_stereo));
    xport_audio_lock();
    if (!spu.initialized)
    {
        xport_audio_unlock();
        return;
    }
    while (offset < frame_count)
    {
        sint32 mixed[SPU_MIX_CHUNK_FRAMES * 2];
        sint16 decoded[SPU_MIX_CHUNK_FRAMES * 2];
        uint32 count = frame_count - offset;
        uint32 sample;
        if (count > SPU_MIX_CHUNK_FRAMES)
            count = SPU_MIX_CHUNK_FRAMES;
        memset(mixed, 0, count * 2u * sizeof(*mixed));
        spu_mix(mixed, count);
        for (sample = 0; sample < count * 2; ++sample)
            interleaved_stereo[offset * 2 + sample] = audio_sample_clamp(mixed[sample]);
        if (libsnd.cd.active && !libsnd.cd.paused && !libsnd.cd.muted)
        {
            uint32 produced = ima_decode(&libsnd.cd.stream, decoded, count);
            for (sample = 0; sample < produced; ++sample)
            {
                sint32 input_left = decoded[sample * 2];
                sint32 input_right = decoded[sample * 2 + 1];
                sint32 routed_left = (input_left * libsnd.cd.mix.val0 + input_right * libsnd.cd.mix.val2) >> 7;
                sint32 routed_right = (input_left * libsnd.cd.mix.val1 + input_right * libsnd.cd.mix.val3) >> 7;
                sint32 cd_left = routed_left * libsnd.cd.volume_left / 127;
                sint32 cd_right = routed_right * libsnd.cd.volume_right / 127;
                interleaved_stereo[(offset + sample) * 2] = audio_sample_mix(interleaved_stereo[(offset + sample) * 2], cd_left);
                interleaved_stereo[(offset + sample) * 2 + 1] = audio_sample_mix(interleaved_stereo[(offset + sample) * 2 + 1], cd_right);
            }
            if (produced < count)
            {
                libsnd.cd.active = 0;
                end_callback = libsnd.ready_callback;
            }
        }
        offset += count;
    }
    if (libsnd.ticks_running)
    {
        libsnd.tick_accumulator += frame_count;
        while (libsnd.tick_accumulator >= SPU_SAMPLE_RATE / 60u)
        {
            libsnd.tick_accumulator -= SPU_SAMPLE_RATE / 60u;
            ++libsnd.tick_count;
        }
    }
    xport_audio_unlock();
    if (end_callback != 0)
        end_callback(CdlDataEnd, 0);
}

static int spu_state_block(FILE *file, void *data, size_t size, int load)
{
    uint32 stored = (uint32)size;
    if (load)
        return fread(&stored, 4, 1, file) == 1 && stored == size && fread(data, 1, size, file) == size;
    return fwrite(&stored, 4, 1, file) == 1 && fwrite(data, 1, size, file) == size;
}

int spu_state_io(FILE *f, int load)
{
    return spu_state_block(f, &spu, sizeof(spu), load);
}
