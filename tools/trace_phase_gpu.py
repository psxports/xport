"""Validate ordered OT payloads captured at an execution phase boundary"""
import struct
from trace_layout import PHASE_PCS


def decode_packets(payload):
    if len(payload) < 8 or len(payload) > 0x200000 or len(payload) % 4:
        raise ValueError('Invalid phase GPU payload size')
    pc = struct.unpack_from('<I', payload)[0]
    if pc not in PHASE_PCS:
        raise ValueError('Invalid phase GPU PC')
    offset, packets = 4, []
    while offset < len(payload):
        words = struct.unpack_from('<I', payload, offset)[0]; offset += 4
        if not words:
            if offset != len(payload):
                raise ValueError('Trailing phase GPU bytes')
            return pc, packets
        if words > 255 or offset+words*4 > len(payload):
            raise ValueError('Invalid phase GPU packet length')
        packets.append(payload[offset:offset+words*4]); offset += words*4
    raise ValueError('Missing phase GPU terminator')
