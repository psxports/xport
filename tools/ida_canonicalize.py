"""Preserve IDA listings and add independently decoded, byte-verified MIPS words."""
from pathlib import Path
import json,hashlib,capstone,re
from xport_project import project_root, project_path, artifact_path
R=project_root()
def main():
 import argparse
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--exports', type=Path, default=project_path('ida_exports', 'status/ida/images'))
 parser.add_argument('--configs', type=Path, default=project_path('ida_configs', 'tools/ida'))
 args=parser.parse_args()
 exports=artifact_path(args.exports)
 md=capstone.Cs(capstone.CS_ARCH_MIPS,capstone.CS_MODE_MIPS32|capstone.CS_MODE_LITTLE_ENDIAN)
 for out in sorted(p for p in exports.iterdir() if p.is_dir() and (p/'functions.json').is_file()):
  cfg=json.loads((args.configs/(out.name+'.json')).read_text());src=cfg['loads'][-1];base=src['base'];blob=(R/src['path']).read_bytes()[src.get('offset',0):]
  records=json.loads((out/'functions.json').read_text());full=[];all_c=[];raw_count=0;undecoded=[]
  expected={'%08X'%f['address'] for f in records}
  expected_pseudo={'%08X'%f['address'] for f in records if f['pseudocode']}
  for folder in ['functions','pseudocode']:
   for p in (out/folder).iterdir():
    if re.fullmatch(r'[0-9A-F]{8}\.(lst|mips\.txt|c)',p.name) and p.name[:8] not in (expected_pseudo if folder=='pseudocode' else expected):p.unlink()
  for rec in records:
   words=[];delay_next=None;lines=['; GENERATED IDA %s; image=%s entry=%08X end=%08X chunks=%s'%(json.loads((out/'export-report.json').read_text())['ida'],out.name,rec['address'],rec['end'],rec['chunks'])]
   for item in rec['instructions']:
    lines.append('%08X  %-23s %s'%(item['address'],bytes.fromhex(item['bytes']).hex(' '),item['text']))
    for ea in range(item['address'],item['address']+len(bytes.fromhex(item['bytes'])),4):
     b=blob[ea-base:ea-base+4];assert b==bytes.fromhex(item['bytes'])[ea-item['address']:ea-item['address']+4]
     w=int.from_bytes(b,'little');dec=list(md.disasm(b,ea));text=(dec[0].mnemonic+' '+dec[0].op_str).strip() if dec else '.word 0x%08X'%w
     if not dec:undecoded.append(ea)
     op=w>>26;fn=w&63;branch=op in [1,2,3,4,5,6,7] or op==0 and fn in [8,9] or op in [16,17,18] and (w>>21&31)==8
     words.append(dict(address=ea,bytes=b.hex(),word='%08X'%w,text=text,delay_slot=ea==delay_next))
     delay_next=ea+4 if branch else None
   assert hashlib.sha256(b''.join(bytes.fromhex(x['bytes']) for x in words)).hexdigest()==rec['sha256']
   rec['raw_instructions']=words;raw_count+=len(words)
   text='\n'.join(lines)+'\n';(out/'functions'/('%08X.lst'%rec['address'])).write_text(text);full.append(text)
   rawtext='; GENERATED independent Capstone %s; IDA listing is adjacent .lst\n; image=%s entry=%08X SHA256=%s\n'%(capstone.__version__,out.name,rec['address'],rec['sha256'])
   rawtext+='\n'.join('%08X %s %-10s %s%s'%(x['address'],x['bytes'],x['word'],x['text'],' ; DELAY SLOT' if x['delay_slot'] else '') for x in words)+'\n'
   (out/'functions'/('%08X.mips.txt'%rec['address'])).write_text(rawtext)
   if rec['pseudocode']:all_c.append((out/rec['pseudocode']).read_text())
  (out/'functions.json').write_text(json.dumps(records,indent=2));(out/(out.name+'.lst')).write_text('\n'.join(full));(out/(out.name+'.c')).write_text('\n'.join(all_c))
  report=json.loads((out/'export-report.json').read_text());report.update(raw_instruction_words=raw_count,source_bytes_verified=True,independent_decoder='capstone '+capstone.__version__,independent_undecoded_words=undecoded)
  (out/'export-report.json').write_text(json.dumps(report,indent=2));print(out.name,len(records),raw_count,'independent undecoded',len(undecoded))
if __name__=='__main__':main()
