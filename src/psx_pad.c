#include <string.h>
#include "psx.h"
#include "psx_pad.h"

static uint8 *pad_packets[2];
static uint8 pad_actuators[2][6];
static uint8 pad_alignment[2][6];
static sint32 pad_active;
static sint32 pad_main_mode[2];
static uint32 pad_buttons;

static sint32 pad_index(sint32 port)
{
    return (port >> 4) & 1;
}

void pad_publish(sint32 controller, sint32 connected, uint16 buttons)
{
    uint8 *packet;
    if (controller < 0 || controller >= 2)
        return;
    packet = pad_packets[controller];
    if (packet == 0)
        return;
    memset(packet, 0xff, 8);
    if (!connected)
        return;
    packet[0] = 0;
    packet[1] = 0x41;
    packet[2] = (uint8)buttons;
    packet[3] = (uint8)(buttons >> 8);
}

void PadInit(sint32 mode)
{
    if (mode == 0)
    {
        pad_buttons = 0;
        xport_input_init();
    }
}

uint32 PadRead(sint32 controller)
{
    pad_buttons = xport_input_read(controller);
    return pad_buttons;
}

void PadInitDirect(uint8 *pad1, uint8 *pad2)
{
    pad_packets[0] = pad1;
    pad_packets[1] = pad2;
    pad_publish(0, 0, 0xffff);
    pad_publish(1, 0, 0xffff);
}

void PadStartCom(void)
{
    pad_active = 1;
}

void PadStopCom(void)
{
    pad_active = 0;
}

sint32 PadGetState(sint32 port)
{
    return pad_active && (port == 0 || port == 0x10) ? 6 : 0;
}

sint32 PadInfoMode(sint32 port, sint32 term, sint32 offset)
{
    (void)term;
    (void)offset;
    return pad_main_mode[pad_index(port)];
}

void PadSetAct(sint32 port, const void *actuator, sint32 length)
{
    sint32 index = pad_index(port);
    if (actuator == 0 || length <= 0)
        return;
    if (length > (sint32)sizeof(pad_actuators[index]))
        length = (sint32)sizeof(pad_actuators[index]);
    memcpy(pad_actuators[index], actuator, (size_t)length);
}

sint32 PadSetActAlign(sint32 port, const void *alignment)
{
    sint32 index = pad_index(port);
    const uint8 *bytes = (const uint8 *)alignment;
    if (alignment == 0)
        return 0;
    memset(pad_alignment[index], 0, sizeof(pad_alignment[index]));
    if (bytes[0] != 0)
        memcpy(pad_alignment[index], alignment, sizeof(pad_alignment[index]));
    return 1;
}

void PadSetMainMode(sint32 port, sint32 mode, sint32 lock)
{
    (void)lock;
    pad_main_mode[pad_index(port)] = mode;
}

int pad_state_io(FILE *file, int load)
{
    uint32 size = (uint32)sizeof(pad_buttons);
    if (load)
        return fread(&size, 4, 1, file) == 1 && size == sizeof(pad_buttons) && fread(&pad_buttons, 1, sizeof(pad_buttons), file) == sizeof(pad_buttons);
    return fwrite(&size, 4, 1, file) == 1 && fwrite(&pad_buttons, 1, sizeof(pad_buttons), file) == sizeof(pad_buttons);
}
