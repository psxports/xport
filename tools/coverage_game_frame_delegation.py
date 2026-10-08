"""Explicitly authorized, single-owner guest-frame non-equivalence admission"""

import hashlib,json,re

from pathlib import Path

import xml.etree.ElementTree as ET

from source_index import functions

from coverage_missing_stubs import policy_entries,validate_stub_body

REPLACEMENT='user_authorized_guest_frame_delegation'

APPROVAL='Разрешаю этот узкий маршрут'

ADDRESS=0x8001DE08

FIELDS=('image','address','function_sha256','mips_sha256','pseudocode_sha256','machine_audit_sha256')

GAPS=['Original guest stack address identity and alias effects','Nested guest frame allocation and release lifetime effects']

PROTOTYPES=('uint32 xport_guest_frame_acquire(size_t bytes);','void xport_guest_frame_release(uint32 address, size_t bytes);')

def sha(text):return hashlib.sha256(text.encode()).hexdigest()

def frame_transform(before):

    if not isinstance(before,str) or re.search(r'^\s*(?:#|%:)',before,re.M) or re.search(r'\b(?:goto|while|for|do|switch|frame_release|xport_guest_frame_acquire|xport_guest_frame_release)\b',before):raise ValueError('Frame baseline contains forbidden transfer/preprocessing')

    fs=functions(before,'frame')

    if len(fs)!=1 or fs[0]['name']!='sub_8001DE08' or before[:fs[0]['start']].strip() or before[fs[0]['end']:].strip():raise ValueError('Frame baseline is not exact approved owner')

    pairs=[('    uint8 frame[96];','    uint8 *frame;'),('    frame_address = xport_guest_buffer_address(frame, sizeof(frame));','    frame_address = xport_guest_frame_acquire(96u);\n    frame = (uint8 *)psx_addr(frame_address, 96u);'),('    if ((sint32)first > 0x3FFFFF)\n        return 1u;','    if ((sint32)first > 0x3FFFFF)\n    {\n        result = 1u;\n        goto frame_release;\n    }'),('        return sub_8003E520(model);','        result = sub_8003E520(model);\n        goto frame_release;')]

    pos=before.find(pairs[1][0]);pre=before[:pos];suffix=before[pos:]

    if pos<0 or pre.count('return result;')!=2 or re.findall(r'\breturn\s+([^;]+);',suffix)!=['1u','sub_8003E520(model)','result']:raise ValueError('Frame initialized exits differ from reviewed shape')

    # Acquire remains at function brace depth one, beyond both original early exits

    code=re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"','',pre,flags=re.S)

    if code.count('{')-code.count('}')!=1:raise ValueError('Frame acquisition is conditional or unreachable')

    after=before

    for old,new in pairs:

        if after.count(old)!=1:raise ValueError('Frame anchors must be unique')

        after=after.replace(old,new)

    trailing='\n' if after.endswith('\n') else ''
    if trailing:after=after[:-1]
    end='    return result;\n}'

    if not after.endswith(end):raise ValueError('Frame cleanup needs exact final return')

    return after[:-len(end)]+'frame_release:\n    xport_guest_frame_release(frame_address, 96u);\n'+end+trailing



def _eligible(c):

    w=int(c['word'],0);op=w>>26;base=(w>>21)&31;reg=(w>>16)&31;off=w&65535

    return (op==9 and base==reg==29 and off in {96,65440}) or (op in {35,43} and base==29 and reg in {16,17,31} and off in {80,84,88} and c['kind'] in {'load','store'})

def _review(root,ref):

    if not isinstance(ref,dict) or set(ref)!={'path','sha256'}:raise ValueError('Frame review requires path/hash')

    p=Path(ref['path']);resolved=(Path(root)/p).resolve()

    if p.is_absolute() or '..' in p.parts or p.parts[:1]!=('status',) or not resolved.is_relative_to((Path(root)/'status').resolve()) or not resolved.is_file():raise ValueError('Frame review escapes project status')

    raw=resolved.read_bytes()

    if hashlib.sha256(raw).hexdigest()!=ref['sha256']:raise ValueError('Frame review hash changed')

    return json.loads(raw.decode('utf-8-sig'))

def scope_hash(owner):return sha(json.dumps(owner,sort_keys=True,separators=(',',':')))

def authorized_frame_delegation(root,packet,proposal,binding,audit):

    if audit.get('replacement_kind')!=REPLACEMENT:return None

    if root is None or packet.get('contract')!='xport-function-audit-packet-v1' or binding.get('address')!=ADDRESS or binding.get('image')!='SLUS_005.10' or binding.get('library_classification'):raise ValueError('Frame waiver exceeds explicitly approved single ordinary owner')

    if any(audit.get(k)!=binding.get(k) for k in FIELDS) or audit.get('complete_mips_audit') is not True or audit.get('pseudocode_used_as_baseline') is not binding.get('pseudocode_complete'):raise ValueError('Frame whole-original audit binding changed')

    if audit.get('implementation_status')!='WIP':raise ValueError('Frame admission must retain WIP')
    if audit.get('original_equivalence') is not False or audit.get('unresolved_semantics')!=GAPS or audit.get('waived_original_effects')!=GAPS or audit.get('retained_semantics_complete') is not True or audit.get('retained_unresolved_semantics')!=[]:raise ValueError('Frame retained proof or explicit gaps missing')

    row=_review(root,audit.get('guest_frame_review'));owner={k:binding.get(k) for k in FIELDS}

    auth=_review(root,audit.get('guest_frame_authorization'))

    if set(auth)!={'schema','contract','user_instruction','scope','owner','review','permission_question_answer','baseline_sha256','baseline_review'} or type(auth['schema']) is not int or auth['schema']!=1 or auth['contract']!='xport-user-authorized-guest-frame-delegation-v1' or auth['user_instruction']!=APPROVAL or auth['permission_question_answer']!=APPROVAL or auth['owner']!=owner or not auth['scope'].startswith('Only recorded1DE08 retained complete game algorithm audit'):

        raise ValueError('Frame route lacks exact separate human authorization')

    _review(root,auth['review'])
    baseline_review=_review(root,auth['baseline_review'])
    if auth['baseline_sha256']!=row['baseline_sha256'] or baseline_review.get('native_sha256')!=row['baseline_sha256'] or baseline_review.get('address')!=ADDRESS or baseline_review.get('full_original_mips_pseudocode_native_read') is not True or baseline_review.get('accepted_claim') is not False:raise ValueError('Frame baseline is not exact independently reviewed algorithm authorized by human')

    keys={'schema','accepted_claim','failure_signature','owner','frame_size','baseline_text','baseline_sha256','result_text','result_sha256','user_approval','approval_scope_sha256'}

    if set(row)!=keys or type(row['schema']) is not int or row['schema']!=1 or row['accepted_claim'] is not False or row['failure_signature']!=packet.get('failure_signature') or row['owner']!=owner or type(row['frame_size']) is not int or row['frame_size']!=96 or row['user_approval']!=APPROVAL or row['approval_scope_sha256']!=scope_hash(owner):raise ValueError('Frame review lacks exact explicit human scope or current binding')

    if sha(row['baseline_text'])!=row['baseline_sha256'] or sha(row['result_text'])!=row['result_sha256'] or frame_transform(row['baseline_text'])!=row['result_text']:raise ValueError('Frame rewrite changed retained algorithm')

    if audit.get('source_symbol')!='sub_8001DE08':raise ValueError('Frame native owner changed')

    contexts=[packet.get('source_context',{}),*packet.get('source_context',{}).get('audit_cluster',[])]

    ctx=[c for c in contexts if c.get('function',{}).get('address')==ADDRESS and c.get('function',{}).get('image')==binding['image'] and c.get('function',{}).get('sha256')==binding['function_sha256']]

    if len(ctx)!=1:raise ValueError('Frame complete original context missing')

    words=[]

    for line in ctx[0].get('mips',{}).get('lines',[]):

        m=re.match(r'^[0-9A-Fa-f]{8}\s+((?:[0-9A-Fa-f]{2}\s+)+)',line)

        if m:

            raw=bytes.fromhex(m.group(1));words.extend(int.from_bytes(raw[i:i+4],'little') for i in range(0,len(raw),4))

    if words.count(0x27bdffa0)!=1 or words.count(0x27bd0060)!=1:raise ValueError('Frame original allocation is not exact96bytes')

    machine=ctx[0].get('machine_audit',{})
    if machine.get('sha256')!=binding.get('machine_audit_sha256'):raise ValueError('Frame immutable context checklist hash differs from owner binding')
    checks=machine.get('checks',[]);eligible=[c['id'] for c in checks if _eligible(c)]

    if not eligible or audit.get('waived_machine_checks')!=eligible or not any(c['id'] not in eligible and c['kind'] in {'load','store','call','control'} for c in checks):raise ValueError('Frame waiver includes non-ABI game effects')

    entries=policy_entries(root,proposal);found=[]

    for proto in PROTOTYPES:

        symbol=proto.split('(')[0].split()[-1];matches=[e for e in entries if e.get('kind')=='system_bridge' and e['symbol']==symbol and e['prototype']==proto and e['caller_image']==binding['image'] and e['caller_address']==ADDRESS and e['caller_function_sha256']==binding['function_sha256'] and e['source_file']=='@xport/src/psx.c']

        if len(matches)!=1:raise ValueError('Frame paired policy missing or foreign')

        found.append(matches[0])

    return {'review':row,'entries':found}



def validate_frame_delegated_sources(root,packet,proposal,shared_root,changes,bindings):

    for audit in proposal.get('function_audits',[]):

        if audit.get('replacement_kind')!=REPLACEMENT:continue

        matches=[b for b in bindings if b.get('address')==audit.get('address') and b.get('image')==audit.get('image')]

        if len(matches)!=1:raise ValueError('Frame actual owner requires supervisor bindings')

        admitted=authorized_frame_delegation(root,packet,proposal,matches[0],audit)

        p=Path(audit['source_file']);source=(Path(root)/p).resolve()

        if p.is_absolute() or '..' in p.parts or p.parts[:1]!=('src',) or not source.is_relative_to((Path(root)/'src').resolve()):raise ValueError('Frame owner escapes project source')

        text=changes.get(source,source.read_text(encoding='utf-8-sig') if source.is_file() else '')

        if isinstance(text,bytes):text=text.decode('utf-8-sig')

        fs=[f for f in functions(text,str(p)) if f['name']==audit['source_symbol']]

        if len(fs)!=1 or text[fs[0]['start']:fs[0]['end']]!=admitted['review']['result_text'].removesuffix('\n'):raise ValueError('Frame predicted/actual body differs from bounded rewrite')

        config=json.loads((Path(root)/'xport-project.json').read_text(encoding='utf-8-sig'))
        relative=Path(config.get('paths',{}).get('native_solution',''))
        solution=(Path(root)/relative).resolve()
        if relative.is_absolute() or '..' in relative.parts or not relative.suffix=='.sln' or not solution.is_relative_to(Path(root).resolve()) or not solution.is_file():raise ValueError('Frame compiled solution escapes configured project')
        projects=list(solution.parent.glob('*.vcxproj'))

        if len(projects)!=1:raise ValueError('Frame compiled project ambiguous')

        tree=ET.fromstring(projects[0].read_text(encoding='utf-8-sig'));compiled={(projects[0].parent/e.attrib['Include'].replace('\\','/')).resolve() for e in tree.iter() if e.tag.split('}')[-1]=='ClCompile' and 'Include' in e.attrib}

        path=(Path(shared_root)/'src/psx.c').resolve()

        if path not in compiled:raise ValueError('Frame services not compiled')

        text=changes.get(path,path.read_text(encoding='utf-8-sig'))

        if isinstance(text,bytes):text=text.decode('utf-8-sig')

        for entry in admitted['entries']:

            fs=[f for f in functions(text,str(path)) if f['name']==entry['symbol']]

            if len(fs)!=1:raise ValueError('Frame service lacks unique actual body')

            validate_stub_body(text[fs[0]['start']:fs[0]['end']],entry)
            function=fs[0];start_line=text[:function['start']].count('\n')+1
            body=text[function['start']:function['end']]
            abort_line=start_line+next(i for i,line in enumerate(body.splitlines()) if line.strip()=='abort();')
            waived=[]
            owner_context=next(c for c in [packet['source_context'],*packet['source_context'].get('audit_cluster',[])] if c['function']['image']==matches[0]['image'] and c['function']['address']==matches[0]['address'] and c['function']['sha256']==matches[0]['function_sha256'])
            original_checks=owner_context['machine_audit']['checks']
            owner_checks={c['id']:c for c in original_checks}
            for group in audit.get('machine_coverage',[]):
                ids=group.get('check_ids',[])
                if group.get('check_range') is not None:
                    lo,hi=group['check_range'];ids=[c['id'] for c in original_checks[lo:hi+1]]
                overlap=[i for i in ids if i in audit['waived_machine_checks']]
                if not overlap:continue
                required=[]
                for i in overlap:
                    w=int(owner_checks[i]['word'],0);op=w>>26
                    release=op==35 or (op==9 and (w&65535)==96)
                    if release==('release' in entry['symbol']):required.append(i)
                if required:
                    if len(required)!=len(ids) or group.get('source_file')!=entry['source_file'] or group.get('source_symbol')!=entry['symbol'] or group.get('source_line')!=abort_line:raise ValueError('Frame waived checks must map only to exact corresponding service abort line')
                    waived.extend(required)
            expected=[i for i in audit['waived_machine_checks'] if ((int(owner_checks[i]['word'],0)>>26)==35 or ((int(owner_checks[i]['word'],0)>>26)==9 and (int(owner_checks[i]['word'],0)&65535)==96))==('release' in entry['symbol'])]
            if waived!=expected:raise ValueError('Frame waived source mappings missing or out of order')

def frame_delegation_summary(proposal):

    keys=('image','address','source_file','source_symbol','replacement_kind','original_equivalence','unresolved_semantics','waived_original_effects','waived_machine_checks','guest_frame_review','guest_frame_authorization','implementation_status')

    return [{k:a[k] for k in keys} for a in proposal.get('function_audits',[]) if a.get('replacement_kind')==REPLACEMENT]

