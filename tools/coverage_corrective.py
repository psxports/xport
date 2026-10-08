"""Reaudit up to two accepted mapped game bodies without advancing coverage"""
from __future__ import annotations
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import coverage as supervisor
from source_index import functions
from coverage_corrective_prototypes import build_bindings, validate_repairs

FIELDS = ('image', 'address', 'function_sha256', 'mips_sha256', 'pseudocode_sha256', 'machine_audit_sha256')


def contexts(packet):
    return [packet['source_context'], *packet['source_context'].get('audit_cluster', [])]


def accepted_binding(root, state, image, address):
    matches = [r for r in state['functions'] if supervisor.image_of(state, r) == image and r['address'] == address]
    if len(matches) != 1 or matches[0].get('proof_status') != 'proved':
        raise ValueError('Corrective scope must be an already proved recorded function')
    row = matches[0]
    relative = Path(row.get('audit_receipt', ''))
    if relative.is_absolute() or '..' in relative.parts or not str(relative) or not (root / relative).is_file():
        raise ValueError('Accepted corrective proof receipt is missing')
    receipt_path = root / relative
    receipt = supervisor.read_json(receipt_path)
    audits = [a for a in receipt.get('function_audits', []) if a.get('image') == image and a.get('address') == address]
    if len(audits) != 1 or audits[0].get('replacement_kind') or audits[0].get('original_equivalence') is False:
        raise ValueError('Corrective route requires an ordinary accepted game proof')
    return {'image': image, 'address': address, 'receipt': relative.as_posix(),
            'receipt_sha256': supervisor.digest(receipt_path),
            'audit_binding': {key: audits[0].get(key) for key in FIELDS},
            'pseudocode_used_as_baseline': audits[0].get('pseudocode_used_as_baseline', audits[0].get('pseudocode_complete'))}


def prepare(name, request_path, budget=262144):
    root, project, state, selected = supervisor.load_state(name)
    request_path = request_path.resolve()
    if not request_path.is_relative_to((root / 'status').resolve()):
        raise ValueError('Corrective request must be project-owned status evidence')
    request = supervisor.read_json(request_path)
    if not {'schema', 'functions'} <= set(request) or set(request) - {'schema', 'functions', 'prototype_repairs'} or request['schema'] != 1 or not isinstance(request['functions'], list) or not 1 <= len(request['functions']) <= 2:
        raise ValueError('Corrective request needs schema1 and one or two functions')
    rows = request['functions']
    if any(set(r) != {'image', 'address'} or not isinstance(r['image'], str) or type(r['address']) is not int for r in rows) or len({(r['image'], r['address']) for r in rows}) != len(rows):
        raise ValueError('Corrective identities must be unique image-qualified integers')
    previous, views, bindings = [], [], []
    for row in rows:
        old = accepted_binding(root, state, row['image'], row['address'])
        view = supervisor.compact(row['image'], row['address'], budget, full_source=True, full_original=True)
        pseudo = view['pseudocode']
        if (not view['implementation'].get('complete') or not view['implementation'].get('text') or
                not view['mips'].get('complete') or
                (pseudo.get('path') and (pseudo.get('problem') or not pseudo.get('sha256') or pseudo.get('first_line') != 1 or
                 len(pseudo.get('lines', [])) != pseudo.get('total_lines') or
                 pseudo.get('content_complete', pseudo.get('complete')) is not True))):
            raise ValueError('Corrective scope requires complete mapped C, full MIPS and all available pseudocode text')
        baseline = bool(pseudo.get('path') and pseudo.get('complete'))
        if old.get('pseudocode_used_as_baseline') is not baseline:
            raise ValueError('Corrective pseudocode baseline differs from the accepted audit')
        machine = supervisor.checklist(view['mips']['lines']); view['machine_audit'] = machine
        binding = dict(image=row['image'], address=row['address'], function_sha256=view['function']['sha256'], mips_sha256=view['mips']['sha256'], pseudocode_sha256=view['pseudocode']['sha256'], machine_audit_sha256=machine['sha256'], pseudocode_complete=bool(view['pseudocode'].get('path') and view['pseudocode'].get('complete')))
        if any(binding[key] != old['audit_binding'][key] for key in FIELDS):
            raise ValueError('Corrective original evidence differs from accepted proof')
        previous.append(old);views.append(view);bindings.append(binding)
    views[0]['audit_contract'] = {'required_functions': bindings, 'complete_mips_audit_required': True, 'pseudocode_baseline_required_when_complete': True, 'semantic_simplification_allowed': False}
    views[0]['audit_cluster'] = views[1:]
    before = supervisor.source_snapshot(root, supervisor.SOURCE_ROOTS)
    shared = supervisor.source_snapshot(supervisor.shared_source_root(), supervisor.SOURCE_ROOTS)
    packet = dict(schema=1, contract='xport-function-corrective-packet-v1', coverage_name=name, source_identity=before['identity'], identities={'shared_source_identity':shared['identity']}, source_context=views[0], accepted_bindings=previous, coverage_state_sha256=supervisor.digest(selected['state']))
    repairs = request.get('prototype_repairs', [])
    packet['prototype_repair_bindings'] = build_bindings(root, project, state, views, repairs, before, supervisor.compact, budget,
                                                         shared_root=supervisor.shared_source_root(), shared_snapshot=shared)
    packet['failure_signature'] = supervisor.digest_bytes(json.dumps(packet,sort_keys=True,separators=(',', ':')).encode())
    number = 1
    while (selected['directory'] / f'coverage-corrective-{number:04d}.json').exists(): number += 1
    target = selected['directory'] / f'coverage-corrective-{number:04d}.json'
    supervisor.write_receipt(target, packet)
    return {'packet': str(target), 'failure_signature': packet['failure_signature'], 'progress': supervisor.progress_text(supervisor.progress(state)), 'active_packet': state.get('active_packet')}


def validate_edits(root, packet, proposal):
    allowed = {'schema', 'failure_signature', 'edits', 'function_audits', 'summaries', 'game_prototype_updates', 'callee_abi_audits'}
    if set(proposal) - allowed or proposal.get('schema') != 1 or proposal.get('failure_signature') != packet['failure_signature']:
        raise ValueError('Corrective proposal has foreign fields or stale signature')
    edits = proposal.get('edits')
    views = contexts(packet)
    if not isinstance(edits, list) or len(edits) != len(views):
        raise ValueError('Corrective proposal needs exactly one whole-body edit per function')
    used = set()
    for edit in edits:
        if set(edit) != {'path', 'before', 'after'} or not all(isinstance(edit[k], str) for k in edit):
            raise ValueError('Invalid corrective whole-body edit')
        matches = [v for v in views if v['implementation']['path'] == edit['path'] and v['implementation']['text'].replace('\r\n','\n').strip() == edit['before'].replace('\r\n','\n').strip()]
        if len(matches) != 1 or matches[0]['address'] in used:
            raise ValueError('Corrective edit is outside its exact mapped body')
        view = matches[0]; used.add(view['address']); relative=Path(edit['path'])
        if relative.is_absolute() or '..' in relative.parts or not relative.is_relative_to(Path('src')) or relative.suffix != '.c':
            raise ValueError('Corrective body must be owned game C')
        after=edit['after']; parsed=functions(after, edit['path']); old=functions(edit['before'],edit['path'])
        if '#' in after or len(parsed)!=1 or len(old)!=1 or parsed[0]['name']!=view['implementation']['symbol'] or parsed[0]['markers']!=old[0]['markers'] or after[:parsed[0]['start']].strip() or after[parsed[0]['end']:].strip():
            raise ValueError('Corrective replacement changes symbol/marker or adds outside code')
        normalize=lambda s: re.sub(r'\s+','',s.split('{',1)[0])
        if normalize(after)!=normalize(edit['before']):
            raise ValueError('Corrective body cannot change its accepted native ABI')


def apply(name, packet_path, proposal_path):
    root, project, state, selected=supervisor.load_state(name)
    packet_path=packet_path.resolve()
    if not packet_path.is_relative_to(selected['directory'].resolve()) or not packet_path.is_file(): raise ValueError('Invalid corrective packet path')
    packet=supervisor.read_json(packet_path);proposal=supervisor.read_json(proposal_path.resolve())
    if packet.get('contract')!='xport-function-corrective-packet-v1' or packet.get('coverage_name')!=name or supervisor.digest(selected['state'])!=packet.get('coverage_state_sha256'):
        raise ValueError('Corrective packet state changed')
    signature_packet = dict(packet); signature_packet.pop('failure_signature', None)
    if supervisor.digest_bytes(json.dumps(signature_packet,sort_keys=True,separators=(',', ':')).encode()) != packet.get('failure_signature'):
        raise ValueError('Corrective packet identity changed')
    required=packet['source_context']['audit_contract']['required_functions']
    if len(required)!=len(packet['accepted_bindings']) or not 1<=len(required)<=2:
        raise ValueError('Corrective scope count changed')
    for binding in packet['accepted_bindings']:
        current=accepted_binding(root,state,binding['image'],binding['address'])
        if current!=binding: raise ValueError('Corrective accepted proof changed')
        matches=[v for v in contexts(packet) if v['function']['address']==binding['address'] and v['image']==binding['image']]
        fresh=supervisor.compact(binding['image'],binding['address'],262144,full_source=True,full_original=True)
        if len(matches)!=1 or any(matches[0].get(k)!=fresh.get(k) for k in ('function','implementation','mips','pseudocode')):
            raise ValueError('Corrective original or mapped context changed')
        recorded=[r for r in required if r['image']==binding['image'] and r['address']==binding['address']]
        if len(recorded)!=1 or any(recorded[0].get(k)!=binding['audit_binding'].get(k) for k in FIELDS):
            raise ValueError('Corrective accepted original binding changed')
        if supervisor.checklist(fresh['mips']['lines'])!=matches[0]['machine_audit']:
            raise ValueError('Corrective machine evidence changed')
        if bool(fresh['pseudocode'].get('path') and fresh['pseudocode'].get('complete')) is not binding.get('pseudocode_used_as_baseline'):
            raise ValueError('Corrective accepted pseudocode baseline changed')
    before=supervisor.source_snapshot(root,supervisor.SOURCE_ROOTS)
    if before['identity']!=packet['source_identity']: raise ValueError('Corrective source changed')
    supervisor.checked_shared_snapshot(packet)
    validate_edits(root,packet,proposal)
    repair_requests = [b['request'] for b in packet.get('prototype_repair_bindings', [])]
    refreshed = (build_bindings(root, project, state, contexts(packet), repair_requests, before, supervisor.compact,
                               shared_root=supervisor.shared_source_root(),
                               shared_snapshot=supervisor.source_snapshot(supervisor.shared_source_root(), supervisor.SOURCE_ROOTS))
                 if repair_requests else [])
    if refreshed != packet.get('prototype_repair_bindings', []):
        raise ValueError('Corrective callee, declarations or mapped callers changed')
    declaration_edits, callee_abi_audits = validate_repairs(root, packet, proposal)
    transaction_proposal = {**proposal, 'edits': [*proposal['edits'], *declaration_edits]}
    ledger=root/'status/translation-ledger.json';ledger_before=ledger.read_bytes();state_before=selected['state'].read_bytes()
    def validator(path,after,changed):
        supervisor.checked_shared_snapshot(packet)
        result=supervisor.validate_audit(path,{**after,'root':str(root)},changed)
        for audit,view in zip(sorted(proposal['function_audits'],key=lambda a:a['address']),sorted(contexts(packet),key=lambda v:v['function']['address'])):
            for group in audit.get('machine_coverage',[]):
                if group.get('source_file')!=view['implementation']['path'] or group.get('source_symbol')!=view['implementation']['symbol']:
                    raise ValueError('Corrective machine mapping names foreign source')
                source=root/group['source_file'];text=source.read_text(encoding='utf-8-sig');parsed=[f for f in functions(text,group['source_file']) if f['name']==group['source_symbol']]
                line=group['source_line'];first=text[:parsed[0]['start']].count('\n')+1;last=text[:parsed[0]['end']].count('\n')+1
                if not first<=line<=last: raise ValueError('Corrective mapping is outside its required body')
        return result
    def finalize(after):
        receipt_path=packet_path.with_suffix('.audit.json')
        receipt=supervisor.read_json(receipt_path)
        receipt['corrective_packet']=packet_path.relative_to(root).as_posix()
        receipt['previous_accepted_proofs']=packet['accepted_bindings']
        receipt['prototype_repair_bindings'] = packet.get('prototype_repair_bindings', [])
        receipt['callee_abi_audits'] = callee_abi_audits
        receipt['game_prototype_updates'] = declaration_edits
        supervisor.write_receipt(receipt_path,receipt)
        all_proved=all(row.get('proof_status')=='proved' for row in state['functions'])
        build=supervisor.finalize_batch(root,packet_path,packet,proposal,all_proved)
        updated=copy.deepcopy(state);receipt=packet_path.with_suffix('.audit.json').relative_to(root).as_posix()
        identities={(b['image'],b['address']) for b in packet['accepted_bindings']}
        for row in updated['functions']:
            if (supervisor.image_of(updated,row),row['address']) in identities:row['audit_receipt']=receipt
        if all_proved:
            updated['release_build']=supervisor.release_build(root)
            updated['status']='complete'
        updated.setdefault('corrective_audits',[]).append({'packet':packet_path.relative_to(root).as_posix(),'audit_receipt':receipt,'previous_proofs':packet['accepted_bindings'],'build':build})
        if supervisor.progress(updated)!=supervisor.progress(state) or updated.get('active_packet')!=state.get('active_packet'):raise ValueError('Corrective audit changed coverage progress')
        supervisor.save_state(selected['state'],updated)
        return build
    try:
        result=supervisor.apply_proposal(root,supervisor.SOURCE_ROOTS,packet_path,before,transaction_proposal,audit_validator=validator,post_apply=finalize,
                                         declaration_validator=lambda edit: edit in declaration_edits)
    except Exception:
        ledger.write_bytes(ledger_before);selected['state'].write_bytes(state_before)
        try:
            supervisor.source_refresh(root)
        except Exception:
            pass
        raise
    return {**result,'progress':supervisor.progress_text(supervisor.progress(state)),'active_packet':state.get('active_packet'),'active_packet_reissue_required':bool(state.get('active_packet'))}


def main():
    parser=argparse.ArgumentParser(description=__doc__);actions=parser.add_subparsers(dest='action',required=True)
    prepare_parser=actions.add_parser('prepare');prepare_parser.add_argument('name');prepare_parser.add_argument('--request',type=Path,required=True)
    apply_parser=actions.add_parser('apply');apply_parser.add_argument('name');apply_parser.add_argument('--packet',type=Path,required=True);apply_parser.add_argument('--proposal',type=Path,required=True)
    args=parser.parse_args();result=prepare(args.name,args.request) if args.action=='prepare' else apply(args.name,args.packet,args.proposal);print(json.dumps(result))

if __name__=='__main__':main()
