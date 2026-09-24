"""Encode exact observed decode-time controller values"""
import struct


def decode_pad_schedule(records, start_tick, end_tick):
    """Encode recorded decode-time pad values at observed gameplay ticks"""
    if type(start_tick) is not int or type(end_tick) is not int or start_tick > end_tick:
        raise ValueError('Invalid decode pad interval')
    ranges=[]
    for row in records:
        if (not isinstance(row,list) or len(row)!=3 or
                any(type(value) is not int for value in row) or not 0<=row[1]<=0xffff):
            raise ValueError('Invalid decode-time pad record')
        tick,value=row[:2]
        if not start_tick<=tick<=end_tick or (ranges and tick<ranges[-1][1]):
            raise ValueError('Decode-time pad tick outside ordered interval')
        if ranges and tick==ranges[-1][1]:
            if value!=ranges[-1][2]:raise ValueError('Conflicting decode-time pad values at one tick')
        elif ranges and tick==ranges[-1][1]+1 and ranges[-1][2]==value:
            ranges[-1][1]=tick
        else:ranges.append([tick,tick,value])
    return b''.join(struct.pack('<3I',*row) for row in ranges)
