"""Restricted instruction runner for isolated audits; not a PS1 emulator.

Implements branch/load delays, modulo-32 arithmetic and explicit call hooks.
Fails closed on unsupported instructions. No BIOS, GTE, devices or exceptions.
"""
import struct

MASK=0xffffffff
def s32(x):return (x&0x7fffffff)-(x&0x80000000)

class Machine:
 def __init__(self,words):
  self.words=words;self.mem=bytearray(0x200000);self.r=[0]*32
  self.coverage=set();self.branches=set();self.lo=0;self.hi=0
 def put(self,a,v,n=4):
  a&=0x1fffff;self.mem[a:a+n]=(v&((1<<(8*n))-1)).to_bytes(n,'little')
 def get(self,a,n=4,signed=False):
  a&=0x1fffff;return int.from_bytes(self.mem[a:a+n],'little',signed=signed)&MASK
 def run(self,pc,stops,hooks,observer=None,max_steps=10000):
  pending=load=None
  for step in range(max_steps):
   self.pc=pc
   if pc in stops:
    if getattr(self,'strict',False) and (pending is not None or load is not None):
     raise ValueError('Pending delay at audit exit')
    return self.r[2]
   if pc in hooks:
    assert pending is None and load is None
    hooks[pc](self);pc=self.r[31];continue
   self.coverage.add(pc)
   before=self.r.copy() if observer else None
   w=self.words[pc];op=w>>26;rs=(w>>21)&31;rt=(w>>16)&31;rd=(w>>11)&31
   shift=(w>>6)&31;fn=w&63;imm=w&65535;si=imm if imm<32768 else imm-65536
   a,b=self.r[rs],self.r[rt];target=pending;pending=None;newload=write=None
   if getattr(self,'strict',False) and target is not None and (op in (1,2,3,4,5,6,7) or (op==0 and fn in (8,9))):
    raise ValueError('Control transfer in delay slot')
   if op==0:
    if fn==0:write=(rd,b<<shift)
    elif fn==2:write=(rd,b>>shift)
    elif fn==3:write=(rd,s32(b)>>shift)
    elif fn==0x21:write=(rd,a+b)
    elif fn==0x23:write=(rd,a-b)
    elif fn==0x24:write=(rd,a&b)
    elif fn==0x25:write=(rd,a|b)
    elif fn==0x26:write=(rd,a^b)
    elif fn==0x2a:write=(rd,int(s32(a)<s32(b)))
    elif fn==0x2b:write=(rd,int(a<b))
    elif fn in (8,9):
     pending=a
     if fn==9:write=(rd,pc+8)
    elif fn==0x18:
     product=s32(a)*s32(b);self.lo=product&MASK;self.hi=(product>>32)&MASK
    elif fn==0x10:write=(rd,self.hi)
    elif fn==0x12:write=(rd,self.lo)
    else:raise AssertionError((hex(pc),hex(w)))
   elif op==9:write=(rt,a+si)
   elif op==10:write=(rt,int(s32(a)<si))
   elif op==11:write=(rt,int(a<(si&MASK)))
   elif op==12:write=(rt,a&imm)
   elif op==13:write=(rt,a|imm)
   elif op==14:write=(rt,a^imm)
   elif op==15:write=(rt,imm<<16)
   elif op in (2,3):
    pending=((pc+4)&0xf0000000)|((w&0x3ffffff)<<2)
    if op==3:write=(31,pc+8)
   elif op in (1,4,5,6,7):
    if op==1:
     assert rt in (0,1);take=s32(a)<0 if rt==0 else s32(a)>=0
    else:take=(a==b) if op==4 else (a!=b) if op==5 else s32(a)<=0 if op==6 else s32(a)>0
    self.branches.add((pc,take));pending=pc+4+4*si if take else pc+8
   elif op in (32,33,35,36,37):
    n={32:1,33:2,35:4,36:1,37:2}[op];newload=(rt,self.get(a+si,n,op in (32,33)))
   elif op in (40,41,43):self.put(a+si,b,{40:1,41:2,43:4}[op])
   else:raise AssertionError((hex(pc),hex(w)))
   if load:self.r[load[0]]=load[1]
   if write:self.r[write[0]]=write[1]&MASK
   load=newload;self.r[0]=0
   next_pc=target if target is not None else pc+4
   if observer:observer(dict(pc=pc,word=w,before=before,after=self.r.copy(),next_pc=next_pc,load=newload))
   pc=next_pc
  raise AssertionError('instruction limit')
