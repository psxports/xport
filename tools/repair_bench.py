"""Declarative, isolated MIPS versus native C differential bench"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import time

from mips_audit_machine import Machine
from repair_contract import cases, expr, identifier, identity, load, native_sources, number, program, sha
from trace_worker import write_receipt
from xport_project import artifact_path
from pipeline_metrics import measured

TYPES = {'uint32', 'sint32', 'uint16', 'sint16', 'uint8', 'sint8', 'uint32 *', 'sint32 *', 'void *'}


class ObservedMachine(Machine):
    def __init__(self, words, memory, limit):
        super().__init__(words)
        self.mem = bytearray(memory); self.events = []; self.limit = limit; self.strict = True

    def event(self, value):
        if len(self.events) >= self.limit:
            raise ValueError('Oracle event limit exceeded')
        self.events.append(value)

    def offset(self, address, width):
        address &= 0xffffffff
        if width not in (1,2,4):raise ValueError('Invalid access width')
        physical = address & 0x1fffffff
        if address >> 29 not in (0, 4, 5) or physical + width > len(self.mem):
            raise ValueError('Audit access outside declared RAM')
        if physical % width:
            raise ValueError('Unaligned audit access')
        return physical

    def get(self, address, width=4, signed=False):
        offset = self.offset(address, width)
        value = int.from_bytes(self.mem[offset:offset+width], 'little', signed=signed) & 0xffffffff
        self.event(dict(kind='read', pc=getattr(self, 'pc', None), address=offset, width=width, value=value))
        return value

    def put(self, address, value, width=4):
        offset = self.offset(address, width)
        value &= (1 << (width*8))-1
        self.mem[offset:offset+width] = value.to_bytes(width, 'little')
        self.event(dict(kind='write', pc=getattr(self, 'pc', None), address=offset, width=width, value=value))


def evaluate(root, contract, case):
    words = program(root, contract)
    size = number(contract.get('ram_size', 2097152))
    if not 64 <= size <= 2097152:
        raise ValueError('RAM size must be 64..2097152')
    memory = bytearray([number(contract.get('fill', 0)) & 255])*size
    from repair_contract import pinned
    for seed in contract.get('memory_images', []):
        _, data = pinned(root, seed); offset = number(seed.get('offset', 0))
        if offset < 0 or offset+len(data) > size:
            raise ValueError('RAM image exceeds declared memory')
        memory[offset:offset+len(data)] = data
    machine = ObservedMachine(words, memory, number(contract.get('max_events', 20000)))
    for item in contract.get('memory', []):
        width = number(item['width'])
        if width not in (1, 2, 4): raise ValueError('Invalid memory width')
        machine.put(expr(item['address'], case), expr(item['value'], case), width)
    machine.events.clear()
    for register, value in contract.get('registers', {}).items():
        register = number(register)
        if not 0 <= register < 32: raise ValueError('Invalid register')
        machine.r[register] = expr(value, case)
    machine.r[0] = 0
    original_memory = bytes(machine.mem); original_registers = machine.r.copy()
    calls = []; hooks = {}
    for spec in contract.get('hooks', []):
        address = number(spec['address']); arity = number(spec['arity'])
        if not 0 <= arity <= 8 or address in hooks: raise ValueError('Invalid hook arity/address')
        def call(m, spec=spec, address=address, arity=arity):
            args = list(m.r[4:4+min(arity, 4)])
            args.extend(m.get(m.r[29]+16+4*i) for i in range(max(0, arity-4)))
            calls.append([address, arity, *args, *([0]*(8-arity))])
            bindings = dict(case, **{'arg'+str(i):v for i,v in enumerate(args)})
            for write in spec.get('writes', []):
                m.put(expr(write['address'], bindings), expr(write['value'], bindings), number(write['width']))
            m.r[2] = expr(spec.get('return', 0), bindings)
            m.event(dict(kind='call', pc=address, arguments=args, result=m.r[2]))
        hooks[address] = call
    result = machine.run(number(contract['original']['entry']), {number(x) for x in contract['original']['stops']}, hooks,
                         observer=lambda event: machine.event(dict(kind='instruction', **event)),
                         max_steps=number(contract.get('max_steps', 10000)))
    return dict(result=result, memory=bytes(machine.mem), calls=calls, events=machine.events,
                initial_memory=original_memory, initial_registers=original_registers)


def cexpr(value, bindings):
    if isinstance(value, str) and value in bindings: return bindings[value]
    if not isinstance(value, dict): return 'UINT32_C(%d)' % (number(value)&0xffffffff)
    if set(value) != {'op', 'args'} or len(value['args']) != 2: raise ValueError('Invalid expression')
    operators = {'add': '+', 'sub': '-', 'and': '&', 'or': '|', 'xor': '^', 'shl': '<<', 'shr': '>>'}
    op = value['op']; symbol = operators.get(op)
    if symbol is None: raise ValueError('Unsupported expression')
    a, b = [cexpr(v, bindings) for v in value['args']]
    if op in ('shl', 'shr'): b = '('+b+' & 31u)'
    return '((uint32)('+a+' '+symbol+' '+b+'))'


def harness(contract, source, keys):
    size = number(contract.get('ram_size', 2097152))
    cap = number(contract.get('max_native_events', 20000))
    if not 64 <= size <= 2097152 or not 1 <= cap <= 1000000: raise ValueError('Invalid harness bounds')
    prefix = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t uint32; typedef int32_t sint32;
typedef uint16_t uint16; typedef int16_t sint16;
typedef uint8_t uint8; typedef int8_t sint8;
#define GDB_CALL
#define FF_FUNCTION_MARKER(a,b)
static uint8 ram[RAM_SIZE]; static uint32 cfg[CFG_SIZE];
static uint32 calls[EVENT_CAP][10], access_log[EVENT_CAP][5], ncalls, naccess;
static uint32 offset(uint32 a,uint32 n){uint32 p=a&0x1fffffffu;
 if(!((a>>29)==0||(a>>29)==4||(a>>29)==5)||p>RAM_SIZE||n>RAM_SIZE-p||p%n)exit(20);return p;}
static void access_event(uint32 kind,uint32 line,uint32 a,uint32 n,uint32 v){
 uint32 *e;if(naccess>=EVENT_CAP)exit(21);e=access_log[naccess++];e[0]=kind;e[1]=line;e[2]=a;e[3]=n;e[4]=v;}
static uint32 read_mem(uint32 a,uint32 n,uint32 line){uint32 v=0,p=offset(a,n);memcpy(&v,ram+p,n);access_event(0,line,p,n,v);return v;}
static void write_mem(uint32 a,uint32 v,uint32 n,uint32 line){uint32 p=offset(a,n);memcpy(ram+p,&v,n);access_event(1,line,p,n,v);}
static void *ff_ptr(uint32 a,uint32 n){uint32 p=a&0x1fffffffu;if(!((a>>29)==0||(a>>29)==4||(a>>29)==5)||p>RAM_SIZE||n>RAM_SIZE-p)exit(20);return ram+p;}
#define ff_u32(a) read_mem(a,4,__LINE__)
#define ff_s32(a) ((sint32)read_mem(a,4,__LINE__))
#define ff_u16(a) ((uint16)read_mem(a,2,__LINE__))
#define ff_s16(a) ((sint16)read_mem(a,2,__LINE__))
#define ff_u8(a) ((uint8)read_mem(a,1,__LINE__))
#define ff_s8(a) ((sint8)read_mem(a,1,__LINE__))
#define ff_w32(a,v) write_mem(a,v,4,__LINE__)
#define ff_w16(a,v) write_mem(a,v,2,__LINE__)
#define ff_w8(a,v) write_mem(a,v,1,__LINE__)
'''
    result = '#define RAM_SIZE %d\n#define CFG_SIZE %d\n#define EVENT_CAP %d\n' % (size, 32+len(keys), cap) + prefix
    bindings = {key:'cfg[%d]' % (32+i) for i,key in enumerate(keys)}
    for hook in contract.get('hooks', []):
        symbol = identifier(hook['symbol']); arity = number(hook['arity'])
        if not 0 <= arity <= 8: raise ValueError('Invalid hook arity')
        rtype = hook.get('return_type', 'uint32')
        if rtype not in ('uint32', 'sint32', 'void'): raise ValueError('Unsupported hook return type')
        result += 'static %s %s(%s){\n' % (rtype, symbol, ','.join('uint32 arg%d'%i for i in range(arity)) or 'void')
        result += 'if(ncalls>=EVENT_CAP)exit(22);calls[ncalls][0]=%du;calls[ncalls][1]=%du;\n' % (number(hook['address']), arity)
        for i in range(arity): result += 'calls[ncalls][%d]=arg%d;\n' % (i+2, i)
        result += 'ncalls++;\n'
        environment = dict(bindings, **{'arg'+str(i):'arg'+str(i) for i in range(arity)})
        for write in hook.get('writes', []):
            width = number(write['width'])
            if width not in (1,2,4): raise ValueError('Invalid hook store')
            result += 'write_mem(%s,%s,%d,__LINE__);\n' % (cexpr(write['address'],environment), cexpr(write['value'],environment), width)
        if rtype != 'void': result += 'return (%s)(%s);\n' % (rtype, cexpr(hook.get('return',0),environment))
        result += '}\n'
    arguments = []
    for item in contract['native']['arguments']:
        ctype = item.get('type','uint32')
        if ctype not in TYPES: raise ValueError('Unsupported argument type')
        reg = number(item['register'])
        if not 0 <= reg < 32: raise ValueError('Invalid argument register')
        value = 'cfg[%d]' % reg
        arguments.append('(%s)ff_ptr(%s,4)'%(ctype,value) if '*' in ctype else '(%s)%s'%(ctype,value))
    invocation = identifier(contract['native']['symbol'])+'('+','.join(arguments)+')'
    invocation = invocation+';ret=0;' if contract['native'].get('return_type') == 'void' else 'ret=(uint32)'+invocation+';'
    result += '\n'+source+'\nint main(int argc,char **argv){FILE *in,*out;uint32 ret;\n'
    result += 'if(argc!=3)return 2;in=fopen(argv[1],"rb");out=fopen(argv[2],"wb");if(!in||!out)return 3;\n'
    result += 'while(fread(cfg,sizeof(cfg),1,in)==1){if(fread(ram,sizeof(ram),1,in)!=1)return 4;ncalls=naccess=0;memset(calls,0,sizeof(calls));'+invocation+'\n'
    result += 'fwrite(&ret,4,1,out);fwrite(&ncalls,4,1,out);fwrite(calls,40,ncalls,out);fwrite(&naccess,4,1,out);fwrite(access_log,20,naccess,out);fwrite(ram,sizeof(ram),1,out);}\nreturn ferror(in)||ferror(out);}\n'
    return result


def compile_native(directory):
    from code_refresh import msbuild_path
    msbuild = msbuild_path()
    installation = msbuild.parents[3]
    vcvars = installation/'VC/Auxiliary/Build/vcvars32.bat'
    if not vcvars.is_file(): raise ValueError('VS2022 x86 environment not found')
    script = directory/'build.cmd'
    script.write_text('@echo off\ncall "'+str(vcvars)+'" >nul\nif errorlevel 1 exit /b 1\ncl /nologo /TC /Od /W3 /wd4100 /D_CRT_SECURE_NO_WARNINGS native.c /Fe:native.exe\n', encoding='utf-8')
    with (directory/'build.log').open('w',encoding='utf-8') as log:
        result = subprocess.run(['cmd.exe','/d','/c',str(script)], cwd=directory, stdout=log, stderr=subprocess.STDOUT,
                                timeout=120, creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
    if result.returncode: raise ValueError('Audit native build failed; see build.log')


def first_memory_difference(expected, actual, ignored):
    exclusions = set()
    for start, length in ignored:
        start, length = number(start), number(length)
        if start < 0 or length < 0 or start+length > len(expected): raise ValueError('Invalid scratch range')
        exclusions.update(range(start,start+length))
    for offset, (a,b) in enumerate(zip(expected, actual)):
        if a != b and offset not in exclusions: return dict(address=offset, expected=a, actual=b)
    return None


@measured('repair_bench')
def run_contract(root, contract, directory, source_override=None):
    directory = Path(directory); directory.mkdir(parents=True,exist_ok=True)
    words = program(root, contract); source, sources = native_sources(root, contract)
    if source_override is not None: source = source_override
    selected = cases(contract); keys = sorted(set().union(*(x.keys() for x in selected)))
    if any(set(row) != set(keys) for row in selected): raise ValueError('Case bindings differ')
    generated = harness(contract, source, keys)
    (directory/'native.c').write_text(generated, encoding='utf-8')
    write_receipt(directory/'contract.json', contract)
    if len(selected)*number(contract.get('ram_size',2097152)) > 134217728:
        raise ValueError('Batch RAM budget exceeds 128 MiB; split the declarative cases')
    expected = []
    with (directory/'inputs.bin').open('wb') as stream:
        for case in selected:
            result = evaluate(root, contract, case)
            stream.write(struct.pack('<%dI'%(32+len(keys)), *result['initial_registers'], *(number(case[k])&0xffffffff for k in keys)))
            stream.write(result['initial_memory'])
            result.pop('initial_memory');result.pop('initial_registers')
            expected.append(result)
    compile_native(directory)
    with (directory/'native.log').open('w',encoding='utf-8') as log:
        child = subprocess.run([str(directory/'native.exe'),str(directory/'inputs.bin'),str(directory/'outputs.bin')],
                               cwd=directory,stdout=log,stderr=subprocess.STDOUT,timeout=120,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
    if child.returncode: raise ValueError('Audit native execution failed; see native.log')
    failures = []; size = number(contract.get('ram_size',2097152))
    with (directory/'outputs.bin').open('rb') as stream:
        def read(count):
            data = stream.read(count)
            if len(data) != count: raise ValueError('Truncated native audit output')
            return data
        for i, original in enumerate(expected):
            ret, count = struct.unpack('<II',read(8))
            if count > number(contract.get('max_native_events',20000)): raise ValueError('Invalid call count')
            calls_out = [list(struct.unpack('<10I',read(40))) for _ in range(count)]
            count = struct.unpack('<I',read(4))[0]
            if count > number(contract.get('max_native_events',20000)): raise ValueError('Invalid access count')
            accesses = [dict(zip(('kind','line','address','width','value'),struct.unpack('<5I',read(20)))) for _ in range(count)]
            memory = read(size)
            difference = first_memory_difference(original['memory'],memory,contract.get('scratch',[]))
            return_diff = contract.get('compare_return',True) and ret != original['result']
            if difference or return_diff or calls_out != original['calls']:
                failure = dict(case=i, bindings=selected[i], memory=difference,
                               return_value=dict(expected=original['result'],actual=ret) if return_diff else None,
                               calls=dict(expected=original['calls'],actual=calls_out) if calls_out != original['calls'] else None)
                failures.append(failure)
                if len(failures) == 1:
                    write_receipt(directory/'original-events.json',original['events'])
                    write_receipt(directory/'native-events.json',accesses)
            if stream.tell() > 2147483648: raise ValueError('Audit output limit exceeded')
        if stream.read(1): raise ValueError('Extra native audit records')
    # Recheck independent inputs after execution
    if program(root,contract) != words or native_sources(root,contract)[1] != sources:
        raise ValueError('Audit inputs changed while running')
    report = dict(schema=1,status='passed' if not failures else 'mismatch',cases=len(selected),
                  failures=failures[:8],failure_count=len(failures),contract_sha256=identity(contract),
                  source_sha256=sha(source.encode()),harness_sha256=sha(generated.encode()),
                  oracle_sha256=sha(Path(__file__).with_name('mips_audit_machine.py').read_bytes()),
                  executable_sha256=sha((directory/'native.exe').read_bytes()),
                  scope='Isolated finite cases with declared call models and scratch exclusions; not full-game MATCH',
                  scratch=contract.get('scratch',[]),hooks=contract.get('hooks',[]))
    write_receipt(directory/'report.json',report)
    if failures:
        from repair_causal import analyze
        write_receipt(directory/'causal.json',analyze(directory))
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--contract',required=True); parser.add_argument('--output',required=True)
    parser.add_argument('--run',action='store_true'); args=parser.parse_args()
    root, contract=load(args.contract); program(root,contract); native_sources(root,contract); selected=cases(contract)
    if not args.run:
        print(json.dumps(dict(status='planned',contract=contract['name'],cases=len(selected),scope='Static contract loading only'))); return 0
    directory=artifact_path(args.output)
    if directory.exists(): raise ValueError('Audit output must be new')
    result=run_contract(root,contract,directory)
    print(json.dumps(dict(status=result['status'],report=str(directory/'report.json'),cases=result['cases'])))
    return 0 if result['status']=='passed' else 1


if __name__=='__main__': raise SystemExit(main())
