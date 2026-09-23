"""Narrow, documented exclusions for GP0 payload comparisons; never mutate captures."""

from xport_project import load_project

IGNORE_FT4_PADDING = load_project()[1].get('trace_layout', {}).get('gpu', {}).get('ignore_ft4_padding', False)
IGNORE_RAW_SPRITE_RGB = load_project()[1].get('trace_layout', {}).get('gpu', {}).get('ignore_raw_sprite_rgb', False)


def canonical_packet(packet, raw_sprite_rgb=False):
    if IGNORE_FT4_PADDING and len(packet) == 36 and (packet[3] & 0xfc) == 0x2c:
        p = bytearray(packet)
        p[26:28] = b'\0\0'
        p[34:36] = b'\0\0'
        return bytes(p)
    if IGNORE_FT4_PADDING and len(packet) == 48 and (packet[3] & 0xfc) == 0x3c:
        p = bytearray(packet)
        p[15] = p[27] = p[39] = 0
        p[34:36] = b'\0\0'
        p[46:48] = b'\0\0'
        return bytes(p)
    # GP0(65h), variable-size raw textured rectangle: RGB does not modulate texels.
    # Explicitly opt in; other packet sizes/opcodes retain their RGB bytes.
    # The calling project must retain proof for each opted-in exclusion
    if raw_sprite_rgb and len(packet) == 16 and packet[3] == 0x65:
        return b'\0\0\0' + packet[3:]
    return packet
