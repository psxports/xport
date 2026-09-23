"""Decode the audited DuckStation serializer version 85 into explicit guest fields"""
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
    r=Reader(data);value=dict(ticks_carry=r.one('i'))
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
    value['transfer_fifo']=r.vector('H',32);value['ram']=r.bytes(0x80000);r.end();return value


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
