"""Caller and instruction scoped admission of unavailable native adapter boundaries"""

import hashlib
import json
import re
from pathlib import Path
import xml.etree.ElementTree as ET

from source_index import functions
from coverage_game_frame_delegation import FIELDS, _review
from coverage_missing_stubs import validate_stub_body
from lockstep_supervisor import source_snapshot

CONTRACT = 'xport-user-authorized-missing-native-adapters-v1'
REPLACEMENT = 'user_authorized_missing_adapter_scope'
INSTRUCTION = 'Для этого проекта разрешены fail-fast заглушки отсутствующих BIOS/SDK вызовов и адаптеров. Не реализуй их сейчас.'


def sha(value):
    return hashlib.sha256(value.encode('utf-8') if isinstance(value, str) else value).hexdigest()


def text_body(text, symbol):
    text = text.decode('utf-8-sig') if isinstance(text, bytes) else text
    text = text.replace('\r\n', '\n')
    matches = [f for f in functions(text, 'source') if f['name'] == symbol]
    if len(matches) != 1:
        raise ValueError('Missing adapter owner or service symbol is not unique')
    item = matches[0]
    return text[item['start']:item['end']], item


def owned(root, path, shared_root=None):
    relative = Path(path)
    shared = relative.parts[:1] == ('@xport',)
    if shared:
        relative = Path(*relative.parts[1:])
    owner = Path(shared_root if shared else root).resolve()
    resolved = (owner/relative).resolve()
    if relative.is_absolute() or '..' in relative.parts or not resolved.is_relative_to(owner/'src') or not resolved.is_file():
        raise ValueError('Missing adapter source escapes existing source tree')
    return resolved


def contexts(packet):
    return [packet['source_context'], *packet['source_context'].get('audit_cluster', [])]


def policy_entries(root, packet, proposal):
    reference = proposal.get('missing_adapter_authorization')
    if reference is None:
        return []
    policy = _review(root, reference)
    keys = {'schema', 'contract', 'user_instruction', 'project', 'packet', 'entries'}
    if set(policy) != keys or type(policy['schema']) is not int or policy['schema'] != 1 or policy['contract'] != CONTRACT or policy['user_instruction'] != INSTRUCTION:
        raise ValueError('Missing adapter policy lacks exact explicit user authorization')
    identity = policy['project']
    if set(identity) != {'path', 'xport_project_sha256'} or Path(identity['path']).resolve() != Path(root).resolve() or sha((Path(root)/'xport-project.json').read_bytes()) != identity['xport_project_sha256']:
        raise ValueError('Missing adapter project identity changed')
    packet_ref = policy['packet']
    saved = _review(root, packet_ref)
    if saved != packet:
        raise ValueError('Missing adapter immutable packet binding changed')
    entries = policy['entries']
    if not isinstance(entries, list) or not entries:
        raise ValueError('Missing adapter entries are empty')
    seen, symbols = set(), set()
    required = packet['source_context']['audit_contract']['required_functions']
    for entry in entries:
        fields = {'owner', 'check', 'kind', 'source_file', 'source_symbol', 'source_body_sha256', 'source_offset', 'before', 'after', 'bridge', 'missing_review', 'waived_effects'}
        if not isinstance(entry, dict) or set(entry) != fields or entry['kind'] not in ('callback_fallback', 'raw_mmio'):
            raise ValueError('Invalid missing adapter scope entry')
        owner = entry['owner']
        if set(owner) != set(FIELDS) or not any(all(owner[k] == b.get(k) for k in FIELDS) and not b.get('library_classification') for b in required):
            raise ValueError('Missing adapter is not an exact required game owner')
        key = (owner['image'], owner['address'], entry['check'].get('id'))
        bridge = entry['bridge']
        if key in seen or not isinstance(bridge, dict) or set(bridge) != {'symbol', 'prototype', 'source_file', 'header_file', 'header_anchor', 'text', 'invocation'} or bridge['symbol'] in symbols or not re.fullmatch(r'xport_missing_[A-Za-z_0-9]+', bridge['symbol']):
            raise ValueError('Missing adapter site or bridge is duplicate or foreign')
        seen.add(key)
        symbols.add(bridge['symbol'])
        validate_stub_body(bridge['text'], bridge)
        if not re.fullmatch(r'(?:void|uint32)\s+'+re.escape(bridge['symbol'])+r'\((?:void|uint32\s+[A-Za-z_]\w*(?:,\s*uint32\s+[A-Za-z_]\w*)*)\);', bridge['prototype']):
            raise ValueError('Missing adapter bridge requires an honest plain diagnostic ABI')
        if not isinstance(entry['waived_effects'], list) or not entry['waived_effects'] or any(not isinstance(v, str) or not v.strip() for v in entry['waived_effects']):
            raise ValueError('Missing adapter waived effects are not explicit')
    return entries


def authorized_scope(root, packet, proposal, binding, audit, shared_root=None):
    ids = audit.get('missing_adapter_checks')
    if ids is None:
        if audit.get('replacement_kind') == REPLACEMENT:
            raise ValueError('Missing adapter audit has no scoped checks')
        return None
    if binding.get('library_classification') or any(audit.get(k) != binding.get(k) for k in FIELDS) or audit.get('complete_mips_audit') is not True or audit.get('pseudocode_used_as_baseline') is not binding.get('pseudocode_complete'):
        raise ValueError('Missing adapter original owner audit is incomplete')
    if audit.get('replacement_kind') not in (REPLACEMENT, 'user_authorized_native_abi_frame') or audit.get('original_equivalence') is not False or audit.get('implementation_status') != 'WIP' or audit.get('retained_semantics_complete') is not True or audit.get('retained_unresolved_semantics') != []:
        raise ValueError('Missing adapter retained algorithm has unresolved effects or wrong disposition')
    entries = [e for e in policy_entries(root, packet, proposal) if all(e['owner'][k] == binding.get(k) for k in FIELDS)]
    context = next(c for c in contexts(packet) if (c['function']['image'], c['function']['address']) == (binding['image'], binding['address']))
    checks = context['machine_audit']['checks']
    selected = {e['check']['id']: e for e in entries}
    ordered = [c['id'] for c in checks if c['id'] in selected]
    if not entries or len(ordered) != len(entries) or ids != ordered or set(ids) & set(audit.get('waived_machine_checks', [])):
        raise ValueError('Missing adapter checks exceed or overlap the exact separate scope')
    gaps = [v for check in ids for v in selected[check]['waived_effects']]
    if audit.get('missing_adapter_unresolved_semantics') != gaps:
        raise ValueError('Missing adapter explicit non-equivalence effects changed')
    if audit.get('replacement_kind') == REPLACEMENT and (audit.get('unresolved_semantics') != gaps or audit.get('waived_original_effects') != gaps):
        raise ValueError('Missing adapter cannot claim full original equivalence')
    original = context.get('implementation', {})
    before_body = original.get('text', '').replace('\r\n', '\n')
    for entry in entries:
        check = entry['check']
        if check not in checks or entry['source_file'] != original.get('path') or entry['source_symbol'] != original.get('symbol') or audit.get('source_file') != entry['source_file'] or audit.get('source_symbol') != entry['source_symbol'] or sha(before_body) != entry['source_body_sha256']:
            raise ValueError('Missing adapter current native owner or machine check changed')
        offset = entry['source_offset']
        fragment = entry['before']
        if type(offset) is not int or offset < 0 or not isinstance(fragment, str) or not fragment or before_body[offset:offset+len(fragment)] != fragment:
            raise ValueError('Missing adapter exact source boundary changed')
        bridge = entry['bridge']
        word = int(check['word'], 0)
        if entry['kind'] == 'callback_fallback':
            if check['kind'] != 'call' or word >> 26 != 0 or word & 63 != 9 or (word >> 11) & 31 != 31 or word & 0x001F07C0:
                raise ValueError('Missing adapter callback is not an original JALR')
            if fragment.count('abort();') != 1 or not re.search(r'\b(?:if|switch)\s*\(', fragment) or entry['after'] != fragment.replace('abort();', bridge['invocation']+';', 1):
                raise ValueError('Missing adapter fallback changed its dispatch branches')
        else:
            op = word >> 26
            if check['kind'] not in ('load', 'store') or (word >> 21) & 31 == 29 or op not in (32, 33, 35, 36, 37, 40, 41, 43):
                raise ValueError('Missing adapter cannot waive ordinary memory or frame effects')
            old = re.match(r'(r_u(?:8|16|32)|w_u(?:8|16|32))\s*\(', fragment)
            if not old or entry['after'] != bridge['symbol']+fragment[len(old[1]):]:
                raise ValueError('Missing MMIO adapter must preserve exact access arguments')
            width = {32:1,33:2,35:4,36:1,37:2,40:1,41:2,43:4}[op]
            if old[1] != ('r' if check['kind']=='load' else 'w')+'_u'+str(width*8):
                raise ValueError('Missing MMIO width does not match original instruction')
        review = _review(root, entry['missing_review'])
        keys = {'schema', 'reviewed_complete', 'kind', 'capability', 'existing_real_adapter', 'known_real_preserved', 'native_source_identity', 'evidence'}
        if entry['kind'] == 'raw_mmio':
            keys |= {'guest_address', 'width', 'operation'}
        if set(review) != keys or type(review['schema']) is not int or review['schema'] != 1 or review['reviewed_complete'] is not True or review['kind'] != entry['kind'] or review['existing_real_adapter'] is not False or review['known_real_preserved'] is not True or not isinstance(review['capability'], str) or not review['capability'].strip() or review['native_source_identity'] != packet.get('identities', {}).get('shared_source_identity') or not isinstance(review['evidence'], list) or not review['evidence']:
            raise ValueError('Missing adapter absence and preserved-real review is incomplete')
        if entry['kind'] == 'callback_fallback' and review['capability'] == 'guest_callback_one_argument':
            raise ValueError('Existing real PAD owner registry cannot be bypassed as a missing one-argument adapter')
        if entry['kind'] == 'raw_mmio' and (type(review['guest_address']) is not int or not 0x1F801040 <= review['guest_address'] < 0x1F801050 or review['width'] != width or review['operation'] != check['kind']):
            raise ValueError('Missing adapter raw MMIO scope is outside unavailable SIO0 capability')
        for evidence in review['evidence']:
            _review(root, evidence)
    return [selected[check] for check in ids]


def adapter_summary(proposal):
    keys = (*FIELDS, 'source_file', 'source_symbol', 'native_body_sha256', 'missing_adapter_checks', 'missing_adapter_unresolved_semantics', 'implementation_status', 'original_equivalence')
    return [{**{k:a[k] for k in keys}, 'missing_adapter_authorization':proposal['missing_adapter_authorization']} for a in proposal.get('function_audits', []) if a.get('missing_adapter_checks') is not None]


def validate_sources(root, packet, proposal, shared_root, changes, bindings):
    for audit in proposal.get('function_audits', []):
        if audit.get('missing_adapter_checks') is None:
            continue
        binding = next(b for b in bindings if (b['image'],b['address']) == (audit['image'],audit['address']))
        entries = authorized_scope(root, packet, proposal, binding, audit, shared_root)
        source = owned(root, audit['source_file'], shared_root)
        owner_text = changes.get(source, source.read_bytes())
        body, parsed = text_body(owner_text, audit['source_symbol'])
        if parsed['markers'] != [(audit['address'],audit['image'])] or sha(body) != audit.get('native_body_sha256'):
            raise ValueError('Missing adapter final native owner hash or marker changed')
        for entry in entries:
            bridge = entry['bridge']
            if body.count(entry['after']) != 1:
                raise ValueError('Missing adapter changed or duplicated its exact source boundary')
            path = owned(root, bridge['source_file'], shared_root)
            service_body, _ = text_body(changes.get(path, path.read_bytes()), bridge['symbol'])
            if service_body.strip() != bridge['text'].replace('\r\n','\n').strip():
                raise ValueError('Missing adapter actual bridge differs from its approved abort body')
            validate_stub_body(service_body, bridge)
            header = owned(root, bridge['header_file'], shared_root)
            text = changes.get(header, header.read_bytes())
            text = text.decode('utf-8-sig') if isinstance(text, bytes) else text
            if text.count(bridge['prototype']) != 1:
                raise ValueError('Missing adapter declaration is absent or duplicated')


def prepare_additions(root, packet, proposal, shared_root, shared_before, solution):
    entries = policy_entries(root, packet, proposal)
    if not entries:
        return {}
    if shared_root is None or shared_before is None or source_snapshot(shared_root,(Path('src'),))['identity'] != shared_before['identity'] or shared_before['identity'] != packet.get('identities',{}).get('shared_source_identity'):
        raise ValueError('Missing adapter additions require unchanged bound shared source')
    projects = list(Path(solution).parent.glob('*.vcxproj'))
    if len(projects) != 1:
        raise ValueError('Missing adapter additions require one configured native project')
    tree = ET.fromstring(projects[0].read_text(encoding='utf-8-sig'))
    compiled = {(projects[0].parent/e.attrib['Include'].replace('\\','/')).resolve() for e in tree.iter() if e.tag.split('}')[-1]=='ClCompile' and 'Include' in e.attrib}
    names = {f['name'] for path in shared_before['files'] for f in functions((Path(shared_root)/path).read_text(encoding='utf-8-sig'),path)}
    changes = {}
    for entry in entries:
        bridge = entry['bridge']
        if bridge['symbol'] in names:
            raise ValueError('Missing adapter cannot replace an existing host API or bridge')
        path = owned(root, bridge['source_file'], shared_root)
        if not bridge['source_file'].startswith('@xport/src/') or path.suffix != '.c' or path not in compiled:
            raise ValueError('Missing adapter bridge must use existing compiled shared C source')
        current = changes.get(path,path.read_bytes()).decode('utf-8-sig')
        changes[path] = (current.rstrip()+'\n\n'+bridge['text'].strip()+'\n').encode('utf-8')
        header = owned(root,bridge['header_file'],shared_root)
        if bridge['header_file'].startswith('@xport/') or header.suffix != '.h':
            raise ValueError('Missing adapter prototypes require an existing project header')
        current = changes.get(header,header.read_bytes()).decode('utf-8-sig')
        anchor = bridge['header_anchor']
        if not isinstance(anchor,str) or not anchor or current.count(anchor) != 1 or bridge['symbol'] in current:
            raise ValueError('Missing adapter prototype anchor changed or declaration exists')
        changes[header] = current.replace(anchor,anchor+'\n'+bridge['prototype'],1).encode('utf-8')
    return changes


def validate_state(root, state, shared_root):
    admissions = state.get('missing_native_adapters', [])
    if not isinstance(admissions,list) or type(state.get('missing_native_adapter_count',0)) is not int or state.get('missing_native_adapter_count',0) != len(admissions):
        raise ValueError('Missing adapter persisted inventory count changed')
    for admission in admissions:
        owner = next((f for f in state['functions'] if (f.get('image',state['image']),f['address']) == (admission['image'],admission['address'])),None)
        if owner is None or owner.get('original_equivalence') is not False:
            raise ValueError('Missing adapter persisted owner disposition changed')
        ref = Path(owner['audit_receipt'])
        receipt_path = (Path(root)/ref).resolve()
        if ref.is_absolute() or '..' in ref.parts or not receipt_path.is_relative_to((Path(root)/'status').resolve()):
            raise ValueError('Missing adapter persisted receipt escapes project')
        receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
        if admission not in adapter_summary(receipt) or receipt.get('missing_native_adapter_admissions') != adapter_summary(receipt):
            raise ValueError('Missing adapter persisted receipt scope changed')
        packet_path = receipt_path.with_name(receipt_path.name.removesuffix('.audit.json')+'.json')
        packet = json.loads(packet_path.read_text(encoding='utf-8-sig'))
        validate_sources(root,packet,receipt,shared_root,{},packet['source_context']['audit_contract']['required_functions'])
