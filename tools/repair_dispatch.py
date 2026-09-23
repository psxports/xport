"""Resolve dispatcher paths from MIPS words and emit two restricted C repairs"""
import argparse
import json
from pathlib import Path

from repair_contract import cases, expr, identifier, identity, load, native_sources, number, program, sha
from repair_bench import cexpr
from trace_worker import write_receipt
from xport_project import artifact_path

MASK=0xffffffff


class Unresolved(ValueError):
    pass


def signed(value):
    value &= MASK
    return value-(1<<32) if value & (1<<31) else value


def operation(op, a, b):
    if isinstance(a,int) and isinstance(b,int):
        operations={'add':lambda:a+b,'sub':lambda:a-b,'and':lambda:a&b,'or':lambda:a|b,
                    'xor':lambda:a^b,'shl':lambda:a<<(b&31),'shr':lambda:a>>(b&31),
                    'sar':lambda:signed(a)>>(b&31),'lt':lambda:int(signed(a)<signed(b)),
                    'ltu':lambda:int(a<b)}
        return operations[op]() & MASK
    if op in ('add','or','xor') and b==0: return a
    if op in ('add','or','xor') and a==0: return b
    return dict(op=op,args=[a,b])


def symbolic_path(words, spec, selector, hooks):
    registers=[{'unknown':i} for i in range(32)]; registers[0]=0
    for reg,value in spec.get('registers',{}).items():
        index=number(reg)
        if not 0 < index < 32: raise ValueError('Invalid symbolic register')
        registers[index]=identifier(value['variable']) if isinstance(value,dict) else number(value)&MASK
    selector_reg=number(spec['selector_register'])
    if not 0 < selector_reg < 32: raise ValueError('Invalid selector register')
    registers[selector_reg]=selector&MASK
    pc=number(spec['entry']); stops={number(x) for x in spec['stops']}
    pending=None; delayed=None; effects=[]; visited=[]; memory={}
    for item in spec.get('memory',[]):
        key=identity([item['address'],number(item['width'])]); memory[key]=item['value']
    for step in range(number(spec.get('max_steps',2048))):
        if pc in stops:
            if pending is not None or delayed is not None: raise Unresolved('Pending delay at contract exit')
            return dict(status='resolved',selector=selector,effects=effects,return_value=registers[2],path=visited)
        if pc in hooks:
            if pending is not None or delayed is not None: raise Unresolved('Pending delay at call boundary')
            hook=hooks[pc]; arity=number(hook['arity'])
            if arity>4: raise Unresolved('Stack arguments require a separate contract')
            args=registers[4:4+arity]
            if any('unknown' in json.dumps(a) for a in args): raise Unresolved('Unknown call argument')
            if not isinstance(registers[31],int): raise Unresolved('Unknown return address')
            effects.append(dict(kind='call',target=pc,symbol=identifier(hook['symbol']),arguments=args))
            registers[2]={'call_result':len(effects)-1}
            # Calls may clobber volatile registers; retaining them would invent a proof
            for reg in [1,3,*range(4,16),24,25]: registers[reg]={'unknown':reg}
            pc=registers[31]; continue
        if pc not in words: raise Unresolved('Unresolved indirect target '+hex(pc))
        w=words[pc]; op=w>>26; rs=(w>>21)&31; rt=(w>>16)&31; rd=(w>>11)&31; fn=w&63
        imm=w&65535; si=imm if imm<32768 else imm-65536
        a,b=registers[rs],registers[rt]; old_pending=pending; pending=None; newload=None; write=None
        visited.append(pc)
        control=op in (1,2,3,4,5,6,7) or (op==0 and fn in (8,9))
        if old_pending is not None and control: raise Unresolved('Control transfer in delay slot')
        if op==0:
            if fn in (0,2,3): write=(rd,operation({0:'shl',2:'shr',3:'sar'}[fn],b,(w>>6)&31))
            elif fn in (0x21,0x23,0x24,0x25,0x26,0x2a,0x2b):
                write=(rd,operation({0x21:'add',0x23:'sub',0x24:'and',0x25:'or',0x26:'xor',0x2a:'lt',0x2b:'ltu'}[fn],a,b))
            elif fn in (8,9):
                if not isinstance(a,int): raise Unresolved('Symbolic indirect target')
                pending=a
                if fn==9: write=(rd,(pc+8)&MASK)
            else: raise Unresolved('Unsupported SPECIAL instruction '+hex(w))
        elif op in (9,10,11,12,13,14):
            write=(rt,operation({9:'add',10:'lt',11:'ltu',12:'and',13:'or',14:'xor'}[op],a,(si&MASK) if op in (9,10,11) else imm))
        elif op==15: write=(rt,imm<<16)
        elif op in (2,3):
            pending=((pc+4)&0xf0000000)|((w&0x3ffffff)<<2)
            if op==3: write=(31,(pc+8)&MASK)
        elif op in (1,4,5,6,7):
            if op in (4,5) and a==b: take=op==4
            elif not isinstance(a,int) or (op in (4,5) and not isinstance(b,int)):
                raise Unresolved('Branch depends on unconstrained input')
            elif op==1:
                if rt not in (0,1): raise Unresolved('Unsupported REGIMM')
                take=signed(a)<0 if rt==0 else signed(a)>=0
            else: take={4:lambda:a==b,5:lambda:a!=b,6:lambda:signed(a)<=0,7:lambda:signed(a)>0}[op]()
            pending=(pc+4+4*si if take else pc+8)&MASK
        elif op in (32,33,35,36,37):
            width={32:1,33:2,35:4,36:1,37:2}[op]; address=operation('add',a,si&MASK)
            key=identity([address,width])
            if key not in memory: raise Unresolved('Unmodelled load')
            value=memory[key]
            if isinstance(value,dict) and value=={'selector':True}: value=selector&MASK
            if isinstance(value,int):
                value &= (1<<(width*8))-1
                if op in (32,33) and value&(1<<(width*8-1)): value=(value-(1<<(width*8)))&MASK
            newload=(rt,value)
        elif op in (40,41,43):
            width={40:1,41:2,43:4}[op]; address=operation('add',a,si&MASK)
            if not isinstance(address,int) or not any(number(start)<=address and address+width<=number(start)+number(length) for start,length in spec.get('scratch',[])):
                raise Unresolved('Observable store outside call-only transformation class')
            if not isinstance(b,int): raise Unresolved('Symbolic scratch store')
            memory[identity([address,width])]=b&((1<<(8*width))-1)
        else: raise Unresolved('Unsupported instruction '+hex(w))
        if delayed: registers[delayed[0]]=delayed[1]
        if write: registers[write[0]]=write[1]
        delayed=newload; registers[0]=0; pc=old_pending if old_pending is not None else (pc+4)&MASK
    raise Unresolved('Symbolic path budget exhausted')


def index(root, contract):
    words=program(root,contract); spec=contract['dispatch']
    hooks={number(h['address']):h for h in contract.get('hooks',[])}
    rows=[]
    for selector in spec['values']:
        try: row=symbolic_path(words,spec,number(selector),hooks)
        except Unresolved as error: row=dict(status='unresolved',selector=number(selector),reason=str(error))
        rows.append(row)
    return dict(schema=1,contract_sha256=identity(contract),kind=contract['kind'],
                status='indexed',rows=rows,scope='Conditional on declared entry/register/memory/call contract; no inferred ABI')


def transform(root, contract, requested):
    if requested['kind'] not in ('callback_binding','state_transition'):
        raise ValueError('Unknown deterministic transformation')
    selector=number(requested['selector']); indexed=index(root,contract)
    register=number(contract['dispatch']['selector_register'])
    inputs={number(k):v for k,v in contract.get('registers',{}).items()}
    if register not in inputs or selector not in {expr(inputs[register],case) for case in cases(contract)}:
        raise ValueError('Repair selector is absent from differential input cases')
    matches=[r for r in indexed['rows'] if r['selector']==selector]
    if len(matches)!=1 or matches[0]['status']!='resolved': raise ValueError('Selector is not resolved')
    row=matches[0]
    if len(row['effects'])!=1: raise ValueError('Transformation requires exactly one call')
    effect=row['effects'][0]
    if row['return_value'] != {'call_result':0} and contract.get('compare_return',True):
        raise ValueError('Call result is not the function result')
    native=contract['native']; variables={name:identifier(name) for name in native.get('variables',[])}
    if native.get('return_type','uint32')!='void' and not contract.get('compare_return',True):
        raise ValueError('Automatic non-void repair must compare its return')
    call=effect['symbol']+'('+', '.join(cexpr(v,variables) for v in effect['arguments'])+')'
    selector_name=identifier(requested['selector_name']); style=requested['style']
    if selector_name not in variables:raise ValueError('Selector is not a declared native variable')
    returning=native.get('return_type','uint32')!='void'
    statement='return '+call+';' if returning else call+'; return;'
    if style=='switch': replacement='case %du: %s\n' % (selector&MASK,statement)
    elif style=='if': replacement='if (%s == %du) { %s }\n' % (selector_name,selector&MASK,statement)
    else: raise ValueError('Supported insertion styles: switch, if')
    if requested['kind']=='callback_binding' and effect['target'] != selector:
        raise ValueError('Callback selector is not the call target')
    if requested['kind']=='state_transition' and effect['target'] != number(requested['transition_target']):
        raise ValueError('State transition target is not proven by path')
    source, files=native_sources(root,contract)
    anchor=requested['anchor']
    if not anchor or source.count(anchor)!=1: raise ValueError('Insertion anchor must be unique')
    if sha(source.encode()) != requested['source_sha256']: raise ValueError('Repair source changed')
    proposed=source.replace(anchor,replacement+anchor,1)
    return dict(schema=1,status='candidate',kind=requested['kind'],selector=selector,
                contract_sha256=identity(contract),before_sha256=sha(source.encode()),
                after_sha256=sha(proposed.encode()),source=proposed,source_files=files,
                insertion=replacement,anchor=anchor,evidence=row,
                scope='Deterministic candidate only; differential bench and full replay required')


def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--contract',required=True)
    p.add_argument('--output',required=True); p.add_argument('--transform',action='store_true'); a=p.parse_args()
    root,c=load(a.contract); result=transform(root,c,c['repair']) if a.transform else index(root,c)
    target=artifact_path(a.output); target.parent.mkdir(parents=True,exist_ok=True); write_receipt(target,result)
    print(json.dumps(dict(status=result['status'],output=str(target),scope=result['scope'])))


if __name__=='__main__': raise SystemExit(main())
