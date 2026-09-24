#ifndef PSX_PAD_H
#define PSX_PAD_H

#include <stdio.h>
#include "xport.h"

#if defined(__cplusplus)
extern "C"
{
#endif

    void pad_publish(sint32 controller, sint32 connected, uint16 buttons);
    int pad_state_io(FILE *file, int load);

#if defined(__cplusplus)
}
#endif

#endif
