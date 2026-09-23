"""Validate absolute-tick controller schedules and decoded input evidence"""
import struct

BUTTONS = ('select', 'l3', 'r3', 'start', 'up', 'right', 'down', 'left',
           'l2', 'r2', 'l1', 'r1', 'triangle', 'circle', 'cross', 'square')
MASKS = {name: 1 << i for i, name in enumerate(BUTTONS)}


def segments(schedule):
    if not isinstance(schedule, list):
        raise ValueError('Schedule must be a list')
    result = []
    previous = -1
    for row in schedule:
        if not isinstance(row, dict) or set(row) != {'start', 'end', 'buttons'}:
            raise ValueError('Invalid schedule segment')
        start, end, buttons = row['start'], row['end'], row['buttons']
        if type(start) is not int or type(end) is not int or not previous < start <= end < 2**32:
            raise ValueError('Unsorted, overlapping or invalid schedule intervals')
        if not isinstance(buttons, list) or any(not isinstance(b, str) or b not in MASKS for b in buttons):
            raise ValueError('Unknown controller button')
        if len(set(buttons)) != len(buttons):
            raise ValueError('Duplicate controller button')
        result.append((start, end, sum(MASKS[b] for b in buttons)))
        previous = end
    return result


def pad_bytes(schedule):
    return b''.join(struct.pack('<III', *row) for row in segments(schedule))


def check_inputs(inputs, schedule, start, end):
    rows = segments(schedule)
    if type(start) is not int or type(end) is not int or not 0 <= start < end:
        raise ValueError('Invalid input verification range')
    if not isinstance(inputs, list) or len(inputs) != end-start:
        raise ValueError('Incomplete decoded input evidence')
    index = 0
    for tick, actual in enumerate(inputs, start):
        while index < len(rows) and rows[index][1] < tick:
            index += 1
        mask = rows[index][2] if index < len(rows) and rows[index][0] <= tick else 0
        if actual != [tick, mask, mask]:
            raise ValueError(f'Schedule mismatch at tick {tick}: expected mask {mask}, observed {actual}')
    return {'schedule_verified': True, 'input_records': end-start}
