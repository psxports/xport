"""Decode the local DuckStation single-stream boundary recorder"""
from trace_layout import PROFILE, PHASE_PCS, PHASE_BYTES, ACTOR_BYTES, INPUT_CALL_BYTES
import argparse
import json
from pathlib import Path
import struct
from trace_phase_gpu import decode_packets
from trace_capture_latency import decode_timing, summarize as summarize_latency

from trace_bundle import validate_bundle
from trace_vblank import Decoder


def decode(source, output):
    output.mkdir(parents=True, exist_ok=False)
    paths = {key: output/filename for key, filename in {
        'actors': 'capture.actors', 'world': 'capture.world',
        'sound': 'sound.json', 'inputs': 'inputs.json'}.items()}
    events, inputs, counts = [], [], {k: 0 for k in range(1, 19)}
    timings = []
    input_calls = []
    phases = []
    effective_settings = []
    metadata, summary = None, None
    pad_events = []
    states = []
    vblanks = Decoder()
    detail_ticks, register_ticks = [], []
    with source.open('rb') as stream, paths['actors'].open('xb') as actors, paths['world'].open('xb') as world, (output/'detail.ram').open('xb') as detail, (output/'registers.bin').open('xb') as registers:
        header = stream.read(12)
        if len(header) != 12:
            raise ValueError('Truncated trace header')
        magic, start, end = struct.unpack('<III', header)
        user = magic == 0x32544646
        if magic not in (0x31544646, 0x32544646) or (not user and end <= start):
            raise ValueError('Invalid trace header')
        complete = False
        footer_tick = None
        while raw := stream.read(12):
            if len(raw) != 12 or complete:
                raise ValueError('Truncated or trailing record')
            kind, tick, length = struct.unpack('<III', raw)
            offset = stream.tell()-12
            if kind not in counts or length > 32*1024*1024 or (not user and not start <= tick <= end):
                raise ValueError('Invalid event envelope')
            payload = stream.read(length)
            if len(payload) != length:
                raise ValueError('Truncated event')
            counts[kind] += 1
            if kind == 1:
                if length != ACTOR_BYTES:
                    raise ValueError('Invalid actor record')
                actors.write(struct.pack('<I', tick)+payload)
            elif kind == 2:
                world.write(struct.pack('<I', tick)+payload)
            elif kind == 3:
                if length != 8:
                    raise ValueError('Invalid input record')
                inputs.append([tick, *struct.unpack('<II', payload)])
            elif kind == 4:
                if length not in (8, 12, 16):
                    raise ValueError('Invalid sound record')
                values = struct.unpack('<'+'I'*(length//4), payload)
                events.append([values[0], tick, *values[1:]])
            elif kind == 6:
                if length != 0x200000+1024 or (detail_ticks and tick != detail_ticks[-1]+1):
                    raise ValueError('Invalid detailed memory interval')
                detail_ticks.append(tick)
                detail.write(struct.pack('<I', tick)+payload)
            elif kind == 7:
                if length != 33*4:
                    raise ValueError('Invalid CPU register record')
                register_ticks.append(tick)
                registers.write(struct.pack('<I', tick)+payload)
            elif kind == 8:
                vblanks.accept(payload, tick, offset)
            elif kind == 9:
                if len(payload) <= 20:
                    raise ValueError('Truncated checkpoint record')
                number, frame, low, high, pc = struct.unpack_from('<5I', payload)
                name = payload[20:].decode('ascii')
                if number != len(states) or not 1 <= len(name) <= 64 or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in name):
                    raise ValueError('Invalid checkpoint identity')
                states.append({'index': number, 'frame': frame, 'game_tick': tick,
                               'emulated_ticks': low | high << 32, 'pc': pc, 'name': name})
            elif kind == 10:
                if len(payload) != 12:
                    raise ValueError('Invalid checkpoint completion')
                number, success, prepare_us = struct.unpack('<3I', payload)
                if number >= len(states) or 'complete' in states[number] or success != 1:
                    raise ValueError('Failed or duplicate checkpoint completion')
                states[number].update(complete=True, prepare_us=prepare_us)
            elif kind == 11:
                if len(payload) != 32:
                    raise ValueError('Invalid controller transfer record')
                frame, low, high, port, device, outgoing, incoming, ack = struct.unpack('<8I', payload)
                clock = low | high << 32
                if port > 1 or outgoing > 255 or incoming > 255 or ack > 1 or (pad_events and clock < pad_events[-1]['emulated_ticks']):
                    raise ValueError('Invalid controller transfer fields')
                pad_events.append({'game_tick': tick, 'frame': frame, 'emulated_ticks': clock,
                                   'port': port, 'device': device, 'outgoing': outgoing, 'incoming': incoming, 'ack': ack})
            elif kind == 12:
                if not user or length != 16 or metadata is not None:
                    raise ValueError('Invalid user trace metadata')
                frequency, low, high, frame = struct.unpack('<4I', payload)
                if not frequency:
                    raise ValueError('Invalid emulated clock frequency')
                metadata = {'ticks_per_second': frequency, 'start_clock': low | high << 32, 'start_frame': frame}
            elif kind == 13:
                if not user or length != 16 or summary is not None:
                    raise ValueError('Invalid user trace summary')
                low, high, frames, peak = struct.unpack('<4I', payload)
                summary = {'end_clock': low | high << 32, 'vblanks': frames, 'peak_queued_bytes': peak}
            elif kind == 14:
                if not user or not payload or b'\x00' in payload:
                    raise ValueError('Invalid effective settings record')
                effective_settings.append(payload.decode('utf-8'))
            elif kind == 15:
                if not user or length != PHASE_BYTES:
                    raise ValueError('Invalid execution phase boundary')
                pc, frame, low, high, stage, callback, pad1, pad2 = struct.unpack_from('<8I', payload)
                if pc not in PHASE_PCS:
                    raise ValueError('Unknown execution boundary PC')
                phases.append({'ordinal': len(phases), 'pc': pc, 'frame': frame,
                               'emulated_ticks': low | high << 32, 'game_tick': tick,
                               'stage': stage, 'callback': callback, 'pad_words': [pad1, pad2],
                               'offset': offset, 'payload_size': length})
            elif kind == 18:
                timing = decode_timing(payload)
                if not user or not vblanks.index or timing['frame'] != vblanks.index[-1]['frame'] or (timings and timing['frame'] != timings[-1]['frame']+1):
                    raise ValueError('Capture timing does not match VBlank')
                timings.append(timing)
            elif kind == 17:
                pc, packets = decode_packets(payload)
                if not user or not phases or phases[-1]['pc'] != pc or phases[-1]['game_tick'] != tick or 'gpu_offset' in phases[-1]:
                    raise ValueError('Unpaired phase GPU record')
                phases[-1].update(gpu_offset=offset, gpu_packets=len(packets))
            elif kind == 16:
                if not user or length != INPUT_CALL_BYTES:
                    raise ValueError('Invalid game input call record')
                controller, pad, frame, low, high, caller = struct.unpack_from('<6I', payload)
                if controller not in (0, 1) or pad != (PROFILE['pad_second'] if controller else PROFILE['pad_first']):
                    raise ValueError('Unsupported game input controller')
                input_calls.append({'ordinal': len(input_calls), 'game_tick': tick, 'controller': controller,
                                    'pad': pad, 'frame': frame, 'emulated_ticks': low | high << 32,
                                    'caller': caller, 'packet_hex': payload[24:].hex()})
            else:
                if (not user and tick != end) or payload != struct.pack('<I', 1):
                    raise ValueError('Capture did not complete cleanly')
                complete = True
                footer_tick = tick
        if not complete:
            raise ValueError('Missing completion footer')
        if counts[17] and counts[17] != counts[15]:
            raise ValueError('Incomplete phase GPU capture')
        if timings and len(timings) != len(vblanks.index):
            raise ValueError('Incomplete VBlank timing capture')
        if detail_ticks != register_ticks:
            raise ValueError('Memory/register boundaries disagree')
        if any(not state.get('complete') for state in states):
            raise ValueError('Uncommitted checkpoint')
    paths['sound'].write_text(json.dumps(events)+'\n')
    paths['inputs'].write_text(json.dumps(inputs)+'\n')
    if user:
        if len(effective_settings) > 1:
            raise ValueError('Effective settings changed during recording')
        if metadata is None or summary is None or not states or summary['vblanks'] != len(vblanks.index) or counts[1] != counts[2]:
            raise ValueError('Incomplete user capture counts')
        if summary['end_clock'] < metadata['start_clock']:
            raise ValueError('Reversed capture clock')
        first_state = states[0]
        if (first_state['frame'] != metadata['start_frame'] or
                first_state['emulated_ticks'] != metadata['start_clock']):
            raise ValueError('Initial checkpoint disagrees with capture origin')
        if vblanks.index and vblanks.index[0]['frame'] != metadata['start_frame']+1:
            raise ValueError('Missing initial VBlank')
        for event in [*vblanks.index, *pad_events, *states, *phases, *input_calls]:
            if not metadata['start_clock'] <= event['emulated_ticks'] <= summary['end_clock']:
                raise ValueError('Event clock outside capture interval')
        if any(a['emulated_ticks'] >= b['emulated_ticks'] for a, b in zip(states, states[1:])):
            raise ValueError('Checkpoint clocks are not increasing')
        validation = {'complete': True, 'boundaries': counts[1], 'comparison_ready': False}
        summary['seconds'] = (summary['end_clock']-metadata['start_clock'])/metadata['ticks_per_second']
    else:
        validation = validate_bundle(paths, start, end)
    (output/'vblank-index.json').write_text(json.dumps(vblanks.index)+'\n')
    (output/'controller-transfers.json').write_text(json.dumps(pad_events)+'\n')
    (output/'phase-boundaries.json').write_text(json.dumps(phases)+'\n')
    (output/'game-input-calls.json').write_text(json.dumps(input_calls)+'\n')
    if timings:
        (output/'capture-timings.json').write_text(json.dumps(timings)+'\n')
        (output/'capture-latency.json').write_text(json.dumps(summarize_latency(timings), indent=2)+'\n')
    if effective_settings:
        (output/'effective-settings.ini').write_text(effective_settings[0], encoding='utf-8')
    return {'start_tick': start, 'end_tick': footer_tick if user else end,
            'duration_limit_seconds': end if user else None, 'records': counts,
            'user_recording': user, 'metadata': metadata, 'summary': summary,
            'effective_settings_recorded': bool(effective_settings),
            'phase_boundaries': len(phases),
            'game_input_calls': len(input_calls),
            'vblanks': len(vblanks.index),
            'vblank_storage': vblanks.statistics,
            'controller_transfers': len(pad_events),
            'states': states,
            'detail_ticks': detail_ticks,
            'paths': {k: str(v.resolve()) for k, v in paths.items()}, 'validation': validation}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    from xport_project import artifact_path
    args.output = artifact_path(args.output)
    result = decode(args.source, args.output)
    (args.output/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
