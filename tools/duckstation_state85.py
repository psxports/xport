"""Decode the audited DuckStation serializer version 85 into explicit guest fields"""
import ctypes
from pathlib import Path
import struct


class Reader:
    def __init__(self,data):self.data=data;self.offset=0
    def bytes(self,size):
        if size<0 or self.offset+size>len(self.data):raise ValueError('Truncated device state')
        value=self.data[self.offset:self.offset+size];self.offset+=size;return value
    def values(self,fmt):return list(struct.unpack('<'+fmt,self.bytes(struct.calcsize('<'+fmt))))
    def one(self,fmt):return self.values(fmt)[0]
    def boolean(self):
        value=self.one('B')
        if value not in (0,1):raise ValueError('Invalid serialized boolean')
        return bool(value)
    def vector(self,fmt,maximum):
        size=self.one('I')
        if size>maximum:raise ValueError('Oversized device vector')
        return self.values(str(size)+fmt)
    def marker(self,value):
        size=self.one('I')
        if size!=len(value) or self.bytes(size)!=value.encode('ascii'):raise ValueError('Device marker mismatch')
    def end(self):
        if self.offset!=len(self.data):raise ValueError('Trailing device state')


def cpu(data):
    r=Reader(data);result={}
    for key in ('pending_ticks','downcount','gte_completion_tick','muldiv_completion_tick'):result[key]=r.one('I')
    result['registers']=r.values('34I');result['pc']=r.one('I');result['npc']=r.one('I')
    result['cop0']=r.values('11I')
    for key in ('next_instruction','current_instruction','current_instruction_pc'):result[key]=r.one('I')
    for key in ('current_delay_slot','current_branch_taken','next_delay_slot','branch_taken','exception','bus_error'):result[key]=r.boolean()
    for prefix in ('load_delay','next_load_delay'):
        result[prefix+'_reg']=r.one('B');result[prefix+'_value']=r.one('I')
        if result[prefix+'_reg']>34:raise ValueError('Invalid delayed register')
    result['cache_control']=r.one('I');result['scratchpad']=r.bytes(1024)
    result['gte_registers']=r.values('64I');result['icache_tags']=r.values('256I');result['icache_data']=r.bytes(4096)
    result['using_interpreter']=r.boolean();r.end();return result


def envelope(r):
    value=dict(counter=r.one('I'),increment=r.one('H'),step=r.one('h'),rate=r.one('B'))
    for key in ('decreasing','exponential','phase_invert'):value[key]=r.boolean()
    return value


def sweep(r):
    value=dict(envelope=envelope(r),level=r.one('h'),active=r.boolean());r.bytes(1);return value


def spu(data):
    r=Reader(data);value=spu_reader(r);r.end();return value


def spu_reader(r):
    value=dict(ticks_carry=r.one('i'))
    for key in ('control','status','transfer_control'):value[key]=r.one('H')
    value['transfer_address']=r.one('I')
    for key in ('transfer_address_reg','irq_address','capture_position','master_left','master_right'):value[key]=r.one('H')
    value['master_sweeps']=[sweep(r),sweep(r)]
    value['cd_external_volume']=r.values('4h')
    for key in ('key_on','key_off','end_flags','pitch_modulation','noise_mode','noise_count','noise_level','reverb_on','reverb_base','reverb_current'):value[key]=r.one('I')
    value['reverb_left']=r.one('h');value['reverb_right']=r.one('h');value['reverb_base_reg']=r.one('H')
    value['reverb_registers']=r.values('32H')
    value['reverb_downsample']=r.bytes(1024);value['reverb_upsample']=r.bytes(512);value['reverb_position']=r.one('i')
    value['voices']=[]
    for _ in range(24):
        voice=dict(current_address=r.one('H'),registers=r.values('8H'),counter=r.one('I'),flags=r.one('B'),first_block=r.boolean())
        voice['samples']=r.values('28h');voice['previous_samples']=r.values('3h');voice['history']=r.values('2h')
        voice['last_volume']=r.one('i');voice['volumes']=[sweep(r),sweep(r)];voice['envelope']=envelope(r)
        voice['adsr_phase']=r.one('B');voice['adsr_target']=r.one('h');voice['has_samples']=r.boolean();voice['ignore_loop_address']=r.boolean()
        if voice['adsr_phase']>4:raise ValueError('Invalid ADSR phase')
        value['voices'].append(voice)
    value['transfer_fifo']=r.vector('H',32);value['ram']=r.bytes(0x80000);return value


def gpu(data):
    r=Reader(data);value=dict(status=r.one('I'),draw_mode=r.one('H'),palette=r.one('H'),texture_window=r.one('I'))
    value['window_masks']=r.values('4B');value['texture_flips']=[r.boolean(),r.boolean()]
    value['drawing_area']=r.values('4I');value['drawing_offset']=r.values('2i');duplicate_x=r.one('i')
    if duplicate_x!=value['drawing_offset'][0]:raise ValueError('GPU duplicated offset differs')
    value['pal']=r.boolean();value['texture_disable_mask']=r.boolean()
    value['display_registers']=r.values('3I');value['display_geometry']=r.values('9H')
    value['display_timings']=r.values('10H');value['fractional_ticks']=r.one('i');value['scanline_tick']=r.one('i')
    value['scanline']=r.one('I');value['fractional_dot_ticks']=r.one('i')
    value['hblank']=r.boolean();value['vblank']=r.boolean();value['fields']=r.values('3B')
    value['blitter_state']=r.one('B');value['pending_ticks']=r.one('i');value['command_words']=r.one('I');value['read_latch']=r.one('I')
    value['clut_bits']=r.one('I');value['clut_8bit']=r.boolean();value['clut']=r.values('256H')
    value['transfer']=r.values('6H');value['fifo']=r.vector('Q',256)
    value['blit_buffer']=r.vector('I',1024*1024);value['blit_remaining']=r.one('I');value['render_command']=r.one('I')
    value['polyline_buffer']=r.vector('Q',1024*1024)
    r.marker('GPU-VRAM');value['vram']=r.bytes(1024*512*2)
    r.marker('GPUTextureCache')
    # Retain acceleration cache bytes separately; native rasterization does not use this host cache
    value['texture_cache']=r.bytes(len(data)-r.offset);r.end();return value

def cdrom(reader):
    r = reader
    value = dict(command=r.one('H'), second_response=r.one('H'))
    for key in ('drive_state', 'status', 'secondary_status', 'mode', 'request'):
        value[key] = r.one('B')
    if value['drive_state'] > 14:
        raise ValueError('Invalid CD drive state')
    value['command_event_active'] = r.boolean()
    value['interrupt_enable'], value['interrupt_flags'] = r.values('2B')
    value['last_interrupt_clock'] = r.one('Q')
    value['pending_async_interrupt'] = r.one('B')
    value['setloc_position'] = r.values('3B')
    for key in ('current_lba', 'seek_start_lba', 'seek_end_lba', 'subq_lba'):
        value[key] = r.one('I')
    value['subq_clock'] = r.one('Q')
    value['subq_carry'] = r.one('I')
    for key in ('setloc_pending', 'read_after_seek', 'play_after_seek', 'muted', 'adpcm_muted'):
        value[key] = r.boolean()
    value['xa_filter'] = r.values('2B')
    value['xa_current'] = r.values('2B')
    value['xa_current_set'] = r.boolean()
    value['sector_header'] = r.bytes(4)
    value['sector_subheader'] = r.bytes(4)
    value['sector_header_valid'] = r.boolean()
    value['subq'] = r.bytes(12)
    value['cdda_auto_pause_pending'] = r.boolean()
    for key in ('cdda_report_delay', 'cdda_report_frame', 'play_track', 'async_parameter'):
        value[key] = r.one('B')
    value['fast_forward_rate'] = r.one('b')
    value['audio_matrix'] = r.values('4B')
    value['next_audio_matrix'] = r.values('4B')
    value['xa_last_samples'] = r.values('4i')
    value['xa_ring'] = r.values('64h')
    value['xa_resample_position'], value['xa_sixstep'] = r.values('2B')
    for key in ('parameters', 'response', 'async_response'):
        value[key] = r.vector('B', 16)
    value['prng'] = r.values('2Q')
    value['read_buffer'], value['write_buffer'] = r.values('2I')
    if value['read_buffer'] >= 8 or value['write_buffer'] >= 8:
        raise ValueError('Invalid CD sector-buffer index')
    value['sector_buffers'] = []
    for _ in range(8):
        size, position = r.values('2I')
        if size > 2340 or position > size:
            raise ValueError('Invalid CD sector-buffer bounds')
        value['sector_buffers'].append(dict(size=size, position=position, data=r.bytes(size-position)))
    value['audio_fifo'] = r.vector('I', 88200)
    value['requested_lba'] = r.one('I')
    return value


def mdec(r):
    value = dict(status=r.one('I'), dma_in=r.boolean(), dma_out=r.boolean())
    value['input_fifo'] = r.vector('H', 512)
    value['output_fifo'] = r.vector('I', 192)
    value['state'] = r.one('B')
    if value['state'] > 5:
        raise ValueError('Invalid MDEC state')
    value['remaining_halfwords'] = r.one('I')
    value['iq_uv'], value['iq_y'] = r.bytes(64), r.bytes(64)
    value['scale_table'], value['blocks'] = r.values('64h'), r.values('384h')
    value['current_block'], value['current_coefficient'] = r.values('2I')
    value['q_scale'] = r.one('H')
    value['block_rgb'] = r.values('256I')
    value['copy_out_pending'] = r.boolean()
    return value


def events(r):
    value = dict(global_clock=r.one('Q'), event_clock=r.one('Q'), active=[])
    count = r.one('I')
    if count > (len(r.data) - r.offset) // 28:
        raise ValueError('Oversized timing event inventory')
    names = set()
    for _ in range(count):
        size = r.one('I')
        if not 0 < size <= 128:
            raise ValueError('Invalid timing event name length')
        name = r.bytes(size).decode('ascii')
        if name in names:
            raise ValueError('Duplicate timing event')
        names.add(name)
        value['active'].append(dict(name=name, next_clock=r.one('Q'), last_clock=r.one('Q'),
                                    period=r.one('i'), interval=r.one('i')))
    return value


def timers(reader):
    value = dict(counters=[])
    for _ in range(3):
        counter = dict(zip(('mode', 'counter', 'target'), reader.values('3I')))
        for key in ('gate', 'external_clock', 'external_counting', 'counting', 'irq_done'):
            counter[key] = reader.boolean()
        value['counters'].append(counter)
    value['sysclk_ticks_carry'] = reader.one('i')
    value['sysclk_div8_carry'] = reader.one('I')
    return value


def pad(reader):
    r = reader
    value = dict(ports=[], multitaps=[])
    for _ in range(8):
        kind = r.one('I')
        controller = dict(type=kind)
        if kind not in (0, 1, 2):
            raise ValueError(f'Unsupported checkpoint controller type: {kind}')
        if kind:
            r.marker('Controller')
        if kind == 1:
            controller.update(buttons=r.one('H'), transfer_state=r.one('B'))
            if controller['transfer_state'] > 4:
                raise ValueError('Invalid digital controller transfer state')
        elif kind == 2:
            controller.update(command=r.one('B'), command_step=r.one('B'), response_length=r.one('B'))
            if controller['command'] > 9 or controller['response_length'] > 8:
                raise ValueError('Invalid analog controller command')
            controller['rx'] = r.bytes(8)
            controller['tx'] = r.bytes(8)
            for key in ('analog_mode', 'analog_locked', 'dualshock_enabled', 'configuration_mode'):
                controller[key] = r.boolean()
            controller['rumble_config'] = r.bytes(6)
            controller['status'] = r.one('B')
            controller['axes'] = r.values('4B')
            controller['buttons'] = r.one('H')
            controller['motors'] = r.values('2B')
        card = None
        if r.boolean():
            r.marker('MemoryCard')
            card = dict(state=r.one('B'), flag=r.one('B'), address=r.one('H'),
                        sector_offset=r.one('B'), checksum=r.one('B'), last_byte=r.one('B'),
                        data=r.bytes(128 * 1024), changed=r.boolean())
        value['ports'].append(dict(controller=controller, memory_card=card))
    for _ in range(2):
        tap = dict(state=r.one('B'), selected_slot=r.one('B'), transfer_step=r.one('I'))
        if tap['state'] > 4:
            raise ValueError('Invalid multitap transfer state')
        for key in ('invalid_command', 'all_controllers', 'controller_done'):
            tap[key] = r.boolean()
        tap['buffer'] = r.bytes(32)
        value['multitaps'].append(tap)
    value['state'] = r.one('I')
    if value['state'] > 2:
        raise ValueError('Invalid pad transfer state')
    value['control'] = r.one('H')
    value['status'] = r.one('I')
    value['mode'], value['baud'] = r.values('2H')
    value['receive'], value['transmit'] = r.values('2B')
    value['receive_full'], value['transmit_full'] = r.boolean(), r.boolean()
    return value

def initial_state(path):
    data = Path(path).read_bytes()
    if len(data) < 216 or struct.unpack_from('<I', data)[0] != 0x43435544:
        raise ValueError('Unsupported DuckStation checkpoint header')
    version = struct.unpack_from('<I', data, 4)[0]
    if version < 81 or version > 85:
        raise ValueError('Unsupported DuckStation checkpoint version')
    compression, compressed, size, offset = struct.unpack_from('<4I', data, 200)
    if size > 32 * 1024 * 1024 or offset < 216 or offset + compressed != len(data):
        raise ValueError('Invalid checkpoint payload bounds')
    payload = data[offset:]
    if compression == 2:
        from duckstation_runtime import executable
        library = ctypes.CDLL(str(executable().parent / 'zstd.dll'))
        library.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
        library.ZSTD_decompress.restype = ctypes.c_size_t
        destination = ctypes.create_string_buffer(size)
        result = library.ZSTD_decompress(destination, size, payload, len(payload))
        if result != size:
            raise ValueError('Checkpoint decompression failed')
        payload = destination.raw
    elif compression != 0 or len(payload) != size:
        raise ValueError('Unsupported checkpoint compression')
    reader = Reader(payload)
    reader.marker('System')
    region = reader.one('B')
    taints, frame, internal_frame = reader.values('3I')
    bios_hash = reader.bytes(16).hex()
    reader.marker('CPU')
    processor = cpu(reader.bytes(6637))
    reader.marker('Bus')
    ram_size = reader.one('I')
    if ram_size != 0x200000:
        raise ValueError('Checkpoint RAM size unsupported')
    memory_timings = reader.values('15I')
    ram = reader.bytes(ram_size)
    memory_control = reader.values('10I')
    tty_size = reader.one('I')
    if tty_size > 4096:
        raise ValueError('Oversized checkpoint TTY buffer')
    reader.bytes(tty_size)
    reader.boolean()
    reader.marker('DMA')
    dma = dict(halt_ticks=reader.one('I'), channels=[])
    for _ in range(7):
        base, block, control = reader.values('3I')
        dma['channels'].append(dict(base=base, block=block, control=control, request=reader.boolean()))
    dma['priority'], dma['interrupt'] = reader.values('2I')
    reader.marker('InterruptController')
    interrupts = reader.values('3I')
    reader.marker('GPU')
    graphics = gpu(payload[reader.offset:])
    # The GPU parser exposes the tail after its typed GPUTextureCache marker
    tail = Reader(graphics['texture_cache'])
    writes = tail.one('I')
    if writes > len(tail.data) // 44:
        raise ValueError('Oversized GPU texture-cache inventory')
    for _ in range(writes):
        tail.bytes(40)
        palettes = tail.one('I')
        tail.bytes(palettes * 544)
    reader.offset = len(payload) - len(tail.data) + tail.offset
    graphics['texture_cache'] = tail.data[:tail.offset]
    reader.marker('CDROM')
    compact_disc = cdrom(reader)
    reader.marker('Pad')
    controllers = pad(reader)
    reader.marker('Timers')
    counters = timers(reader)
    reader.marker('SPU')
    sound = spu_reader(reader)
    reader.marker('MDEC')
    decoder = mdec(reader)
    reader.marker('SIO')
    serial = dict(control=reader.one('H'), status=reader.one('I'), mode=reader.one('H'), baud=reader.one('H'))
    reader.marker('PIO')
    pio_type, pio_size = reader.one('B'), reader.one('I')
    if pio_type != 0 or pio_size != 4:
        raise ValueError('Unsupported checkpoint PIO device')
    reader.marker('Events')
    schedule = events(reader)
    reader.marker('Overclock')
    overclock = dict(active=reader.boolean(), numerator=reader.one('I'), denominator=reader.one('I'))
    reader.marker('Cheevos')
    achievements = reader.bytes(reader.one('I'))
    reader.end()
    return dict(region=region, taints=taints, frame=frame, internal_frame=internal_frame,
                bios_hash=bios_hash, cpu=processor, ram=ram, memory_timings=memory_timings,
                memory_control=memory_control, dma=dma, interrupts=interrupts, gpu=graphics,
                cdrom=compact_disc, pad=controllers, timers=counters, spu=sound, mdec=decoder,
                sio=serial, pio_type=pio_type, events=schedule, overclock=overclock,
                achievements=achievements, payload_size=reader.offset,
                scope='Typed full checkpoint extraction; native state conversion and continuation remain required')
