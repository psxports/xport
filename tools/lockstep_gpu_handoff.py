"""Export GPU device state at a stopped, seeded handoff without admitting a match"""
import hashlib
import mmap
import struct
from pathlib import Path

from duckstation_state85 import initial_state
from lockstep_gpu_phase import encode_gpu_phase
from lockstep_contract import mapping_name


def handoff_sample_counter(adapter):
    offset = (10 + 3 + 32 * 3 + 511 * 3) * 4
    with mmap.mmap(-1, offset + 12, tagname=mapping_name(adapter.run_id, "deferred"),
                   access=mmap.ACCESS_READ) as view:
        before, low, high = struct.unpack_from("<3I", view, offset)
        after = struct.unpack_from("<I", view, offset)[0]
        if not before or before != after or before & 1:
            raise RuntimeError("Stopped handoff sample counter is unavailable")
        return low | high << 32


def export_clock_handoff(adapter, output_path):
    timers = adapter.timer_state()
    beam = adapter.beam_state()
    crtc = adapter.crtc_state()
    words = [row[key] for key in ("counter", "target", "mode") for row in timers["counters"]]
    words.append(timers["sysclk_div8_carry"])
    irq = beam["irq"]
    payload = struct.pack("<Q13I10I3I3B", timers["cpu_clock"], *crtc, *words,
                          irq["status"], irq["mask"], irq["lines"],
                          *(int(row["irq_done"]) for row in timers["counters"]))
    Path(output_path).write_bytes(payload)
    return output_path


def capture_handoff_state(adapter, raw_path, decoded_path, frame):
    raw_path = Path(raw_path)
    size = adapter.capture_checkpoint(frame - 1)
    if adapter.export_checkpoint(frame - 1) != size:
        raise RuntimeError('GPU handoff export size changed')
    payload = raw_path.read_bytes()
    if len(payload) != size:
        raise RuntimeError('GPU handoff payload size differs')
    state_bytes, wait_bytes = struct.unpack_from('<2I', payload)
    if 8 + state_bytes + wait_bytes != len(payload):
        raise RuntimeError('Incomplete cooperative checkpoint')
    machine = payload[8:8 + state_bytes]
    header = bytearray(216)
    struct.pack_into('<II', header, 0, 0x43435544, 85)
    struct.pack_into('<4I', header, 200, 0, state_bytes, state_bytes, 216)
    decoded_path = Path(decoded_path).with_suffix('.decoder.sav')
    decoded_path.write_bytes(header + machine)
    try:
        state = initial_state(decoded_path)
    finally:
        decoded_path.unlink()
    return state, payload


def export_cd_handoff(state, output_path):
    compact_disc = state['cdrom']
    drive = next((row for row in state['events']['active']
                  if row['name'] == 'CDROM Drive Event'), None)
    cd_state = (int(compact_disc['drive_state'] == 7 and drive is not None),
                drive['next_clock'] if drive else 0, drive['interval'] if drive else 0,
                compact_disc['requested_lba'], compact_disc['mode'], compact_disc['secondary_status'])
    fifo = compact_disc['sector_buffers'][compact_disc['read_buffer']]['data']
    Path(output_path).write_bytes(struct.pack('<6QI', *cd_state, len(fifo)) + fifo)


def export_spu_handoff(state, output_path, sample_counter=0):
    sound = state['spu']
    words = [sound[key] for key in ('control', 'status', 'transfer_address', 'irq_address',
             'end_flags', 'key_on', 'key_off', 'pitch_modulation', 'reverb_on',
             'reverb_base', 'reverb_current', 'master_left', 'master_right')]
    words.extend(value & 0xffffffff for value in sound['cd_external_volume'])
    words.extend([sound['reverb_left'] & 0xffffffff, sound['reverb_right'] & 0xffffffff])
    words.extend(sound['reverb_registers'])
    for voice in sound['voices']:
        envelope = voice['envelope']
        words.extend(voice['registers'])
        words.extend([voice['current_address'], voice['counter'], voice['flags'],
                      int(voice['first_block']), int(voice['has_samples']),
                      int(voice['ignore_loop_address']), voice['adsr_phase']])
        words.extend([envelope['counter'], envelope['increment'], envelope['step'] & 0xffffffff,
                      envelope['rate'], int(envelope['decreasing']), int(envelope['exponential'])])
        words.extend(value & 0xffffffff for value in voice['history'])
        words.extend(value & 0xffffffff for value in voice['previous_samples'] + voice['samples'])
    Path(output_path).write_bytes(struct.pack('<' + str(len(words)) + 'IQ', *words, sample_counter))


def export_gpu_handoff(adapter, raw_path, output_path, record_id, frame, pc):
    output_path = Path(output_path)
    state, payload = capture_handoff_state(adapter, raw_path, output_path, frame)
    export_spu_handoff(state, output_path.with_suffix('.spu.bin'), handoff_sample_counter(adapter))
    gpu = state['gpu']
    if state['cpu']['pc'] != pc:
        raise RuntimeError('GPU handoff CPU boundary changed')
    if (gpu['blitter_state'] or gpu['command_words'] or gpu['blit_remaining'] or
            gpu['blit_buffer'] or gpu['polyline_buffer']):
        raise RuntimeError('GPU handoff contains an unsupported partial rendering command')
    fifo = [word & 0xFFFFFFFF for word in gpu['fifo']]
    if any(word >> 24 not in range(0xE1, 0xE7) for word in fifo):
        raise RuntimeError('GPU handoff FIFO requires a rendering-command continuation')
    dma = state['dma']['channels'][2]
    if dma['control'] & 0x01000000 and dma['control'] & 0x00000603 != 0x00000401:
        raise RuntimeError('GPU handoff DMA continuation is not a linked list')
    raster = [gpu['draw_mode'] & 0x1FF, gpu['draw_mode'] & 0x600,
              gpu['texture_window'], *gpu['drawing_area'],
              *(value & 0xFFFFFFFF for value in gpu['drawing_offset'])]
    data = (struct.pack('<6I', 0x31485047, 1, frame, pc, dma['base'], dma['control']) +
            bytes.fromhex(record_id) + struct.pack('<9I', *raster) +
            struct.pack('<I', len(fifo)) + struct.pack(f'<{len(fifo)}I', *fifo) + gpu['vram'])
    export_cd_handoff(state, output_path.with_suffix('.cd.bin'))
    phase_data = encode_gpu_phase(state, record_id, frame, pc)
    output_path.write_bytes(data)
    output_path.with_suffix('.phase.bin').write_bytes(phase_data)
    return {'contract': 'xport-gpu-handoff-v1', 'record_id': record_id,
            'frame': frame, 'pc': f'0x{pc:08X}', 'device_only': True,
            'accepted_match': False, 'payload_sha256': hashlib.sha256(payload).hexdigest(),
            'gpu_file_sha256': hashlib.sha256(data).hexdigest(),
            'gpu_phase_sha256': hashlib.sha256(phase_data).hexdigest(),
            'fifo_words': len(fifo), 'dma': dma}
