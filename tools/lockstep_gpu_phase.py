"""Preserve pending GPU command and DMA phase without executing either"""

import struct

HEADER = struct.Struct('<4I32sQ6Ii3I2Q2iI')
MAGIC = 0x31435047


def encode_gpu_phase(state, record_id, frame, pc):
    record = bytes.fromhex(record_id)
    if len(record) != 32 or not 0 <= frame <= 0xFFFFFFFF or not 0 <= pc <= 0xFFFFFFFF:
        raise ValueError('Invalid GPU phase identity')
    if state['overclock']['active']:
        raise ValueError('GPU phase does not support CPU overclock')
    gpu = state['gpu']
    if any(gpu[key] for key in ('blitter_state', 'blit_remaining', 'blit_buffer', 'polyline_buffer')):
        raise ValueError('GPU phase requires a partial-command continuation')
    fifo = gpu['fifo']
    if len(fifo) > 256 or any(not 0 <= word <= 0xFFFFFFFFFFFFFFFF for word in fifo):
        raise ValueError('Invalid GPU FIFO')
    clock = state['events']['global_clock']
    event = next((row for row in state['events']['active']
                  if row['name'] == 'GPU Command Tick'), None)
    if event is None:
        if gpu['pending_ticks'] > 0:
            raise ValueError('Pending GPU work has no timing event')
        event = {'next_clock': 0, 'last_clock': 0, 'period': 0, 'interval': 0}
        active = 0
    else:
        if not 0 <= event['last_clock'] <= clock < event['next_clock']:
            raise ValueError('Invalid pending GPU event phase')
        active = 1
    dma = state['dma']
    channel = dma['channels'][2]
    values = (MAGIC, 1, frame, pc, record, clock,
              channel['base'], channel['block'], channel['control'], int(channel['request']),
              dma['priority'], dma['interrupt'], gpu['pending_ticks'], gpu['status'],
              gpu['command_words'], active, event['next_clock'], event['last_clock'],
              event['period'], event['interval'], len(fifo))
    return HEADER.pack(*values) + struct.pack(f'<{len(fifo)}Q', *fifo)


def decode_gpu_phase(data, record_id, frame, pc):
    if len(data) < HEADER.size:
        raise ValueError('Truncated GPU phase header')
    row = HEADER.unpack_from(data)
    if row[:4] != (MAGIC, 1, frame, pc) or row[4] != bytes.fromhex(record_id):
        raise ValueError('GPU phase identity differs')
    count = row[-1]
    if count > 256 or len(data) != HEADER.size + 8 * count:
        raise ValueError('Invalid GPU phase payload size')
    if row[9] not in (0, 1) or row[15] not in (0, 1):
        raise ValueError('Invalid GPU phase flags')
    if row[15] and not row[17] <= row[5] < row[16]:
        raise ValueError('Invalid GPU phase event clocks')
    if not row[15] and (row[12] > 0 or any(row[16:20])):
        raise ValueError('Inactive GPU phase contains pending work')
    return {'clock': row[5],
            'dma': dict(zip(('base', 'block', 'control', 'request', 'priority', 'interrupt'), row[6:12])),
            'gpu': {'pending_ticks': row[12], 'status': row[13], 'command_words': row[14],
                    'fifo': list(struct.unpack_from(f'<{count}Q', data, HEADER.size))},
            'event': dict(zip(('active', 'next_clock', 'last_clock', 'period', 'interval'), row[15:20]))}
