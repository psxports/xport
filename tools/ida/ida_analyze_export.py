"""IDA 7.7/32-bit bootstrap and evidence export; all discovered functions remain TODO."""
import os,sys,json,hashlib,traceback,struct
import ida_auto,ida_bytes,ida_funcs,ida_ida,ida_kernwin,ida_lines,ida_name,ida_segment,ida_loader,ida_idp,ida_hexrays
import idautils,idc

def hx(x):return '0x%08X'%x
def write(p,x):
 os.makedirs(os.path.dirname(p),exist_ok=True)
 with open(p,'w',encoding='utf8') as f:f.write(x)
def dump(p,x):write(p,json.dumps(x,indent=2)+'\n')
def seg(a,b,n,c):
 if not ida_segment.add_segm(0,a,b,n,c):raise RuntimeError('segment '+n)
 s=ida_segment.getseg(a);s.bitness=1;s.perm=7 if c=='CODE' else 6;ida_segment.update_segm(s)
def normalize_mips32(ranges):
 for _ in range(4):
  bad=sorted({ea&~3 for lo,hi in ranges for ea in idautils.Heads(lo,hi)
              if ida_bytes.is_code(ida_bytes.get_full_flags(ea)) and ida_bytes.get_item_size(ea)%4})
  if not bad:return
  for ea in bad:
   if not idc.split_sreg_range(ea,'mips16',0,idc.SR_user):raise RuntimeError('MIPS32 mode '+hx(ea))
   ida_bytes.del_items(ea,ida_bytes.DELIT_SIMPLE,4);idc.create_insn(ea)
  ida_auto.auto_wait()
 raise RuntimeError('MIPS16 items remain in PSX code ranges')
def main():
 cfg=json.load(open(idc.ARGV[1],encoding='utf-8-sig'));out=cfg['output'];os.makedirs(out,exist_ok=True)
 ida_auto.auto_wait()
 if cfg.get('rebuild',True):
  for a in list(idautils.Segments()):ida_segment.del_segm(a,ida_segment.SEGMOD_KILL)
  for part in cfg['segments']:seg(part['start'],part['end'],part['name'],part['class'])
  for im in cfg['loads']:
   raw=open(im['path'],'rb').read()[im.get('offset',0):];ida_bytes.put_bytes(im['base'],raw)
  ida_ida.inf_set_start_ea(cfg['entry']);ida_ida.inf_set_start_ip(cfg['entry'])
  # Processor-specific GP setting used by the installed MIPS module.
  try: idc.set_processor_type('mipsl',idc.SETPROC_USER)
  except Exception:pass
  ida_idp.process_config_directive('MIPS_GP=0x%X'%cfg['gp'])
  ida_idp.process_config_directive('MIPS_SIMPLIFY_GP=YES')
  ida_idp.process_config_directive('MIPS_SIMPLIFY=YES')
  ida_idp.process_config_directive('MIPS_STRICT=NO')
  # Pin PSX code ranges to MIPS32
  for lo,_ in cfg['code_ranges']:
   if not idc.split_sreg_range(lo,'mips16',0,idc.SR_user):raise RuntimeError('MIPS32 mode '+hx(lo))
  try:idc.set_inf_attr(idc.INF_AF,idc.get_inf_attr(idc.INF_AF)&~idc.AF_FLIRT)
  except Exception:pass
  if cfg.get('external_symbols'):
   for ext in json.load(open(cfg['external_symbols'],encoding='utf8')):
    ida_name.set_name(ext['address'],ext['name'],ida_name.SN_NOWARN)
  for ea in cfg['seeds']:

   idc.create_insn(ea);ida_funcs.add_func(ea)
  ida_auto.auto_wait()
  # Seed stack-frame candidates only in declared code ranges; record provenance.
  for lo,hi in cfg['code_ranges']:
   for ea in range(lo,hi-8,4):
    w=ida_bytes.get_dword(ea)
    if w>>16==0x27bd and w&0x8000 and (w&0xffff)%4==0 and not ida_funcs.get_func(ea):
     idc.create_insn(ea);ida_funcs.add_func(ea)
   # Decode direct calls from physical instruction words, including callers
   # which were not themselves reached by IDA's first recursive pass.
   for site in range(lo,hi,4):
    word=ida_bytes.get_dword(site)
    if word>>26==3:
     target=((site+4)&0xF0000000)|((word&0x03FFFFFF)<<2)
     if any(a<=target<b for a,b in cfg['code_ranges']) and not ida_funcs.get_func(target):
      idc.create_insn(target);ida_funcs.add_func(target)
   ida_auto.plan_and_wait(lo,hi)
  ida_auto.auto_wait()
  normalize_mips32(cfg['code_ranges'])
  ida_loader.save_database(idc.get_idb_path(),0)
 records=[];edges=[];xrefs=[];indirect=[];fail=[]
 hexok=ida_hexrays.init_hexrays_plugin()
 for ea in idautils.Functions():
  if not any(lo<=ea<hi for lo,hi in cfg['code_ranges']):continue
  fn=ida_funcs.get_func(ea);chunks=list(idautils.Chunks(ea));name=ida_funcs.get_func_name(ea)
  body=[];raw=b'';instructions=[]
  for a in idautils.FuncItems(ea):
   if not ida_bytes.is_code(ida_bytes.get_full_flags(a)):continue
   b=ida_bytes.get_bytes(a,ida_bytes.get_item_size(a)) or b'';raw+=b
   dis=ida_lines.tag_remove(idc.generate_disasm_line(a,0) or '')
   body.append('%08X  %-23s %s'%(a,b.hex(' '),dis));instructions.append(dict(address=a,bytes=b.hex(),text=dis))
   m=idc.print_insn_mnem(a).lower()
   for xr in idautils.XrefsFrom(a,0):
    if xr.iscode and xr.type in [16,17,18,19]:
     tf=ida_funcs.get_func(xr.to)
     if m in ['jal','jalr','bal'] or (m in ['j','b'] and (not tf or tf.start_ea!=ea)):
      edges.append(dict(source=ea,site=a,target=xr.to,kind='call' if m in ['jal','jalr','bal'] else 'tail_candidate'))
    elif not xr.iscode:xrefs.append(dict(source=ea,site=a,target=xr.to,type=xr.type))
   if m=='jalr' or m=='jr' and idc.print_operand(a,0) not in ['$ra','ra']:
    indirect.append(dict(source=ea,site=a,instruction=dis,status='unresolved_static'))
  prefix='; GENERATED; image=%s; function=%s; entry=%s; end=%s; chunks=%s; IDA=%s\n'%(cfg['image'],name,hx(ea),hx(fn.end_ea),chunks,ida_kernwin.get_kernel_version())
  write(out+'/functions/%08X.lst'%ea,prefix+'\n'.join(body)+'\n')
  pseudopath=None
  try:
   if not hexok:raise RuntimeError('Hex-Rays unavailable')
   ida_hexrays.mark_cfunc_dirty(ea,False);c=ida_hexrays.decompile(ea)
   if c is None:raise RuntimeError('no cfunc')
   pseudopath='pseudocode/%08X.c'%ea
   write(out+'/'+pseudopath,'/* GENERATED DRAFT image=%s entry=%s IDA=%s Hex-Rays=%s; verify MIPS. */\n'%(cfg['image'],hx(ea),ida_kernwin.get_kernel_version(),ida_hexrays.get_hexrays_version())+'\n'.join(ida_lines.tag_remove(l.line) for l in c.get_pseudocode())+'\n')
  except Exception as e:fail.append(dict(address=ea,error=str(e)))
  records.append(dict(address=ea,end=fn.end_ea,name=name,chunks=chunks,sha256=hashlib.sha256(raw).hexdigest(),size=len(raw),status='TODO',boundary_status='IDA_discovered_unreviewed',pseudocode=pseudopath,instructions=instructions))
  if len(records)%100==0:print('Exported',cfg['image'],len(records))
 names=[dict(address=a,name=n,kind='code' if ida_bytes.is_code(ida_bytes.get_full_flags(a)) else 'data',size=ida_bytes.get_item_size(a)) for a,n in idautils.Names()]
 dump(out+'/functions.json',records);dump(out+'/callgraph.json',edges);dump(out+'/data-xrefs.json',xrefs);dump(out+'/symbols.json',names);dump(out+'/indirect-calls.json',indirect);dump(out+'/decompilation-failures.json',fail)
 strings=[]
 for s in idautils.Strings():strings.append(dict(address=s.ea,length=s.length,text=str(s)))
 dump(out+'/strings.json',strings)
 segments=[dict(start=a,end=ida_segment.getseg(a).end_ea,name=ida_segment.get_segm_name(ida_segment.getseg(a)),kind=ida_segment.get_segm_class(ida_segment.getseg(a))) for a in idautils.Segments()]
 dump(out+'/export-report.json',dict(image=cfg['image'],ida=ida_kernwin.get_kernel_version(),hexrays=ida_hexrays.get_hexrays_version() if hexok else None,python=sys.version,gp=cfg['gp'],config=cfg,segments=segments,functions=len(records),pseudocode=len(records)-len(fail),failures=len(fail)))
 write(out+'/'+cfg['image']+'.lst','\n'.join(open(out+'/functions/%08X.lst'%r['address'],encoding='utf8').read() for r in records))
 write(out+'/'+cfg['image']+'.map','\n'.join('%08X %s'%(s['address'],s['name']) for s in names)+'\n')
 ida_loader.save_database(idc.get_idb_path(),0)
 print('EXPORT_COMPLETE',cfg['image'],len(records),len(fail))
try:main();idc.qexit(0)
except Exception:traceback.print_exc();idc.qexit(1)
