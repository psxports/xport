"""Authorize only the reviewed rsincos quarter-mirror endpoint correction"""
import hashlib
import json
from pathlib import Path
import re
import struct
import xml.etree.ElementTree as ET
from source_index import functions
from lockstep_supervisor import source_snapshot

SKELETON_SHA256 = '44fe4066bcf28763961cec7103dd1c3f2030c448aa3ccdcb9a5140f535c368bd'
FIELDS = ('image','address','function_sha256','mips_sha256','pseudocode_sha256','machine_audit_sha256')

def _sha(data):
    return hashlib.sha256(data).hexdigest()

def _owned(root, relative, prefix):
    path = Path(relative) if isinstance(relative,str) else Path()
    resolved = (Path(root)/path).resolve()
    if path.is_absolute() or '..' in path.parts or path.parts[:1] != (prefix,) or not resolved.is_relative_to((Path(root)/prefix).resolve()) or not resolved.is_file():
        raise ValueError('Helper evidence escapes its project-owned scope')
    return resolved

def _body(source, symbol):
    rows = [f for f in functions(source,'helper') if f['name']==symbol]
    if len(rows)!=1:
        raise ValueError('Helper correction requires unique complete functions')
    f=rows[0]
    return source[f['start']:f['end']]

def _quarter(source):
    rows = re.findall(r'static const sint16 psyq_sine_quarter\[1025\]\s*=\s*\{.*?\};',source,re.S)
    if len(rows)!=1:
        raise ValueError('Helper correction needs the unchanged canonical quarter table')
    raw=rows[0];content=raw.split('{',1)[1].rsplit('}',1)[0]
    tokens=content.split(',')
    if tokens[-1].strip()=='':tokens.pop()
    if len(tokens)!=1025 or not all(re.fullmatch(r'\s*-?\d+\s*',t) for t in tokens):
        raise ValueError('Quarter table must contain exactly 1025 literal signed halfwords')
    values=[int(t) for t in tokens]
    if any(not -32768<=v<=32767 for v in values):
        raise ValueError('Quarter table value escapes signed halfword range')
    return raw,values

def _reviews(packet,proposal,shared_root,project_root,post=False):
    references=proposal.get('sdk_helper_corrections') or []
    if not isinstance(references,list) or len(references)>1:
        raise ValueError('Only one narrowly scoped rsincos helper correction is supported')
    if not references:return []
    if project_root is None or shared_root is None or packet.get('contract')!='xport-function-audit-packet-v1':
        raise ValueError('Helper correction requires a packet and project-owned evidence')
    if not post and source_snapshot(shared_root,(Path('src'),))['identity']!=packet.get('identities',{}).get('shared_source_identity'):
        raise ValueError('Helper correction shared source baseline changed')
    reference=references[0]
    if not isinstance(reference,dict) or set(reference)!={'path','sha256'}:
        raise ValueError('Helper correction requires an exact review reference and SHA-256')
    raw=_owned(project_root,reference['path'],'status').read_bytes()
    if _sha(raw)!=reference['sha256']:raise ValueError('Helper correction review hash changed')
    row=json.loads(raw.decode('utf-8-sig'))
    if type(row.get('schema')) is not int or row['schema']!=1 or row.get('accepted_claim') is not False:
        raise ValueError('Helper correction review is not an explicit unaccepted proposal')
    packet_reference=Path(row.get('packet',''))
    packet_path=(packet_reference if packet_reference.is_absolute() else Path(project_root)/packet_reference).resolve()
    if not packet_path.is_relative_to((Path(project_root)/'status').resolve()) or not packet_path.is_file():
        raise ValueError('Helper correction packet escapes project status')
    packet_raw=packet_path.read_bytes()
    if _sha(packet_raw)!=row.get('packet_sha256') or json.loads(packet_raw)!=packet or row.get('failure_signature')!=packet.get('failure_signature'):
        raise ValueError('Helper correction belongs to a stale packet')
    owner=row.get('owner_sdk')
    if not isinstance(owner,dict) or set(owner)!=set(FIELDS):raise ValueError('Helper correction lacks exact SDK owner hashes')
    matches=[d for d in packet.get('sdk_dependencies',[]) if {k:d['binding'].get(k) for k in FIELDS}==owner]
    if len(matches)!=1 or not any(c.get('alias') in ('RotMatrixYXZ','RotMatrixYXZ_gte') for c in matches[0]['binding'].get('library_classification',[])):
        raise ValueError('Helper correction owner is not the packet-bound RotMatrixYXZ SDK')
    audits=[a for a in proposal.get('sdk_audits',[]) if all(a.get(k)==owner[k] for k in FIELDS)]
    if len(audits)!=1 or audits[0].get('complete_mips_audit') is not True or audits[0].get('unresolved_semantics')!=[] or audits[0].get('source_symbol')!='RotMatrixYXZ' or audits[0].get('source_file')!='@xport/src/psx.c':
        raise ValueError('Helper correction requires its complete canonical SDK owner audit')
    if row.get('source_file')!='@xport/src/psx.c' or row.get('symbol')!='rsincos':raise ValueError('Helper correction is not the narrowly supported canonical helper')
    source_path=(Path(shared_root)/'src/psx.c').resolve();source=source_path.read_text(encoding='utf-8-sig')
    configuration=Path(project_root)/'xport-project.json'
    if not configuration.is_file():raise ValueError('Helper correction requires project native compilation configuration')
    project=json.loads(configuration.read_text(encoding='utf-8-sig'))
    solution_relative=Path(project.get('paths',{}).get('native_solution',''))
    solution=(Path(project_root)/solution_relative).resolve()
    if solution_relative.is_absolute() or '..' in solution_relative.parts or not solution.is_relative_to(Path(project_root).resolve()) or not solution.is_file() or solution.suffix!='.sln':raise ValueError('Helper correction native solution escapes project')
    projects=list(solution.parent.glob('*.vcxproj'))
    if len(projects)!=1:raise ValueError('Helper correction requires one native compilation project')
    tree=ET.fromstring(projects[0].read_text(encoding='utf-8-sig'))
    compiled={(projects[0].parent/e.attrib['Include'].replace('\\','/')).resolve() for e in tree.iter() if e.tag.split('}')[-1]=='ClCompile' and 'Include' in e.attrib}
    if source_path not in compiled:raise ValueError('SDK helper correction source is not registered for native compilation')
    before=row.get('function_before');after=row.get('function_after')
    if not isinstance(before,str) or _sha(before.encode())!=row.get('source_before_sha256') or _sha(re.sub(r'\s+','',before).encode())!=SKELETON_SHA256:
        raise ValueError('Helper correction changed its canonical rsincos skeleton')
    if before.count('0x7ffu - angle')!=2 or before.count('0xfffu - angle')!=2 or after!=before.replace('0x7ffu - angle','0x800u - angle').replace('0xfffu - angle','0x1000u - angle'):
        raise ValueError('Helper correction permits only the exact four mirror endpoints')
    current=_body(source,'rsincos')
    if current!=before and (not post or current!=after):raise ValueError('Helper correction before body differs from current source')
    declaration,quarter=_quarter(source)
    if _sha(declaration.encode())!=row.get('quarter_table_sha256'):raise ValueError('Helper correction quarter table changed')
    identity=row.get('original_image',{});image=_owned(project_root,identity.get('path'),'orig').read_bytes()
    if _sha(image)!=identity.get('sha256') or image[:8]!=b'PS-X EXE' or identity.get('payload_file_offset')!=2048:raise ValueError('Helper correction original executable identity changed')
    startup_path=(Path(project_root)/'status/startup.json').resolve()
    if not startup_path.is_file():raise ValueError('Helper original image lacks accepted startup inventory')
    startup=json.loads(startup_path.read_text(encoding='utf-8-sig'))
    inventory=startup.get('steps',{}).get('inventory',{})
    executables=inventory.get('result',{}).get('psx_executables',[])
    accepted=[e for e in executables if isinstance(e,dict) and e.get('path')==identity.get('path')]
    if startup.get('status')!='complete' or inventory.get('status')!='passed' or len(accepted)!=1 or accepted[0].get('sha256')!=identity.get('sha256') or accepted[0].get('bytes')!=len(image) or Path(identity['path']).name!=owner['image']:
        raise ValueError('Helper original image differs from accepted startup inventory')
    load,size=struct.unpack_from('<II',image,0x18)
    accepted_header=accepted[0].get('psx_exe',{})
    if accepted_header.get('load_address')!=load or accepted_header.get('payload_size')!=size or accepted_header.get('file_offset')!=2048:raise ValueError('Helper image header differs from accepted startup inventory')
    if load!=identity.get('header_load_address') or size!=identity.get('header_payload_size') or len(image)<2048+size:raise ValueError('Helper correction original payload metadata changed')
    table=row.get('original_table',{});address=table.get('address');offset=table.get('file_offset')
    if type(address) is not int or type(offset) is not int or table.get('size')!=16384 or offset!=2048+address-load or not 2048<=offset<=2048+size-16384:raise ValueError('Helper correction table escapes original payload')
    raw_table=image[offset:offset+16384]
    if _sha(raw_table)!=table.get('sha256'):raise ValueError('Helper correction original packed table changed')
    function=matches[0].get('context',{}).get('function',{})
    begin=owner['address'];end=function.get('end')
    if type(end) is not int or not load<=begin<end<=load+size or _sha(image[2048+begin-load:2048+end-load])!=owner['function_sha256']:raise ValueError('Helper correction SDK original function bytes changed')
    refs=row.get('owner_table_references',{});base=refs.get('base_instruction_pc');pcs=refs.get('packed_load_pcs')
    def word(pc):
        if type(pc) is not int or not begin<=pc<end or pc%4:raise ValueError('Helper table instruction leaves complete SDK owner')
        return struct.unpack_from('<I',image,2048+pc-load)[0]
    if word(base)!=0x3c030000|(address>>16) or word(base+4)!=0x24630000|(address&65535):raise ValueError('SDK owner does not form the exact original table base')
    if not isinstance(pcs,list) or len(pcs)!=3 or len(set(pcs))!=3:raise ValueError('SDK owner needs three exact packed table loads')
    for pc,destination in zip(pcs,(6,5,4)):
        if word(pc)!=(0x8d000000|(destination<<16)) or word(pc-4)!=0x01034020 or word(pc-8)!=0x31083ffc or word(pc-12)!=0x00084080:raise ValueError('SDK owner packed table lookup instructions changed')
    packed=struct.unpack('<4096I',raw_table)
    def lookup(a):
        if a<1024:return quarter[a]
        if a<2048:return quarter[2048-a]
        if a<3072:return -quarter[a-2048]
        return -quarter[4096-a]
    def signed(value):return value if value<32768 else value-65536
    for angle in range(-32768,32768):
        absolute=abs(angle)&4095;value=packed[absolute];sign=-1 if angle<0 else 1
        if (lookup(absolute)*sign,lookup((absolute+1024)&4095))!=(signed(value&65535)*sign,signed(value>>16)):
            raise ValueError('Corrected helper differs from original packed table for signed angle '+str(angle))
    verification=row.get('verification',{})
    if any(type(verification.get(k)) is not int or verification[k]!=v for k,v in {'domain_first':-32768,'domain_last':32767,'cases':65536,'mismatch_count':0}.items()):raise ValueError('Helper verification claims do not match recomputed full domain')
    return [(row,source_path)]

def authorized_helper_bodies(packet,proposal,shared_root,project_root):
    return [(row['source_file'],row['function_before']) for row,path in _reviews(packet,proposal,shared_root,project_root)]

def validate_helper_changes(packet,proposal,shared_root,project_root,changes):
    for row,path in _reviews(packet,proposal,shared_root,project_root,post=True):
        source=changes.get(path,path.read_text(encoding='utf-8-sig'))
        if isinstance(source,bytes):source=source.decode('utf-8')
        if _body(source,'rsincos')!=row['function_after']:raise ValueError('Predicted helper is not the exact endpoint correction')
        table,_=_quarter(source)
        if _sha(table.encode())!=row['quarter_table_sha256']:raise ValueError('Predicted quarter table changed')
        owner=_body(source,'RotMatrixYXZ');body=owner[owner.index('{')+1:owner.rfind('}')]
        body=re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"','',body,flags=re.S)
        calls=re.findall(r'(?:(?<=;)|(?<={)|(?<=})|^)\s*rsincos\s*\([^;{}]+\)\s*;',body)
        if len(calls)!=3:raise ValueError('Predicted canonical SDK owner lacks its three direct helper calls')
