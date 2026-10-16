"""Explicit native ABI admission for separately authorized original frame effects"""

import hashlib
import json
import re
from pathlib import Path

from source_index import functions
from coverage_game_frame_delegation import FIELDS, _review
from lockstep_audit import _register_effects

REPLACEMENT = 'user_authorized_native_abi_frame'
APPROVAL = 'Да, обычный C ABI с явным допуском'


def frame_effect(word):
    op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    offset = word & 65535
    offset -= 65536 if offset & 32768 else 0
    if op == 9 and rs == rt == 29 and offset:
        return ('sp_add', offset, None)
    if op in (35, 43) and rs == 29 and rt in (*range(16, 24), 30, 31) and offset >= 0 and offset % 4 == 0:
        return ('load' if op == 35 else 'store', offset, rt)
    return None


def original_words(context):
    rows = []
    for line in context.get('mips', {}).get('lines', []):
        match = re.match(r'^([0-9A-Fa-f]{8})\s+((?:[0-9A-Fa-f]{2}\s+)+)', line)
        if not match:
            continue
        raw = bytes.fromhex(match[2])
        if len(raw) % 4:
            raise ValueError('Native ABI original instruction is incomplete')
        rows.extend((int(match[1], 16) + i, int.from_bytes(raw[i:i+4], 'little')) for i in range(0, len(raw), 4))
    if not rows or any(b[0] != a[0] + 4 for a, b in zip(rows, rows[1:])):
        raise ValueError('Native ABI requires complete contiguous original words')
    return rows


def authorized_native_abi_frame(root, packet, proposal, binding, audit):
    if audit.get('replacement_kind') != REPLACEMENT:
        return None
    if root is None or binding.get('library_classification') or packet.get('contract') != 'xport-function-audit-packet-v1':
        raise ValueError('Native ABI permission supports recorded game owners only')
    policy = _review(root, audit.get('native_abi_authorization'))
    keys = {'schema', 'contract', 'project', 'user_instruction', 'review', 'owners', 'original_equivalence', 'implementation_status'}
    if set(policy) != keys or type(policy['schema']) is not int or policy['schema'] != 1 or policy['contract'] != 'xport-user-authorized-native-abi-frames-v1' or policy['user_instruction'] != APPROVAL or policy['original_equivalence'] is not False or policy['implementation_status'] != 'WIP':
        raise ValueError('Native ABI explicit authorization contract is invalid')
    project = policy['project']
    if not isinstance(project, dict) or set(project) != {'path', 'xport_project_sha256'} or Path(project['path']).resolve() != Path(root).resolve() or hashlib.sha256((Path(root)/'xport-project.json').read_bytes()).hexdigest() != project['xport_project_sha256']:
        raise ValueError('Native ABI project identity changed')
    review = _review(root, policy['review'])
    if not isinstance(policy['owners'], list) or not policy['owners']:
        raise ValueError('Native ABI policy owners are missing')
    identities = [(o.get('image'), o.get('address')) for o in policy['owners']]
    if len(set(identities)) != len(identities):
        raise ValueError('Native ABI policy contains duplicate owners')
    owners = [o for o in policy['owners'] if (o.get('image'), o.get('address')) == (binding['image'], binding['address'])]
    if len(owners) != 1 or set(owners[0]) != {*FIELDS, 'frame_checks'} or any(owners[0].get(k) != binding.get(k) or audit.get(k) != binding.get(k) for k in FIELDS):
        raise ValueError('Native ABI exact original owner binding changed')
    if audit.get('complete_mips_audit') is not True or audit.get('pseudocode_used_as_baseline') is not binding.get('pseudocode_complete') or audit.get('original_equivalence') is not False or audit.get('implementation_status') != 'WIP' or audit.get('retained_semantics_complete') is not True or audit.get('retained_unresolved_semantics') != []:
        raise ValueError('Native ABI retained semantic audit is incomplete')
    gaps = audit.get('unresolved_semantics')
    if not isinstance(gaps, list) or not gaps or any(not isinstance(g, str) or not g.strip() for g in gaps) or audit.get('waived_original_effects') != gaps:
        raise ValueError('Native ABI guest memory non-equivalence must remain explicit')
    contexts = [packet['source_context'], *packet['source_context'].get('audit_cluster', [])]
    matched = [c for c in contexts if c.get('function', {}).get('image') == binding['image'] and c.get('function', {}).get('address') == binding['address']]
    if len(matched) != 1:
        raise ValueError('Native ABI complete original context missing')
    context = matched[0]
    symbol = context.get('implementation', {}).get('symbol') or context['function'].get('name')
    if not isinstance(symbol, str) or audit.get('source_symbol') != symbol:
        raise ValueError('Native ABI source symbol differs from exact original owner')
    mapped_path = context.get('implementation', {}).get('path')
    if mapped_path is not None and audit.get('source_file') != mapped_path:
        raise ValueError('Native ABI source path differs from mapped original owner')
    words = original_words(context)
    if words[0][0] != binding['address']:
        raise ValueError('Native ABI original entry address changed')
    if hashlib.sha256(b''.join(w.to_bytes(4, 'little') for _, w in words)).hexdigest() != binding['function_sha256']:
        raise ValueError('Native ABI raw original bytes changed')
    effects = [(pc, frame_effect(w)) for pc, w in words if frame_effect(w)]
    for _, word in words:
        op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        if op in (2, 3):
            continue
        register_effects = _register_effects(word)
        if register_effects is not None and 29 in (register_effects[0] | register_effects[1]) and not frame_effect(word):
            raise ValueError('Native ABI original exposes or changes the guest stack pointer')
        if rs == 29 and not frame_effect(word):
            raise ValueError('Native ABI original uses addressable stack arguments or locals')
        if rt == 29 and op not in (2, 3) and not frame_effect(word):
            raise ValueError('Native ABI original changes or exposes stack pointer')
    allocations = [e[1] for _, e in effects if e[0] == 'sp_add']
    if len(allocations) != 2 or allocations[0] >= 0 or allocations[1] != -allocations[0]:
        raise ValueError('Native ABI frame allocation and release are not paired')
    saved = {(e[1], e[2]) for _, e in effects if e[0] == 'store'}
    loaded = {(e[1], e[2]) for _, e in effects if e[0] == 'load'}
    if saved != loaded or any(offset >= -allocations[0] for offset, _ in saved):
        raise ValueError('Native ABI frame saves and restores are not paired')
    reviewed = [f for f in review.get('functions', []) if int(f.get('address', '0'), 0) == binding['address']]
    if len(reviewed) != 1 or reviewed[0].get('mips_sha256') != binding['mips_sha256'] or reviewed[0].get('other_sp_references') != [] or reviewed[0].get('local_frame_address_escape_in_original_instructions') is not False:
        raise ValueError('Native ABI frame includes stack arguments, locals or escaping addresses')
    checks = context['machine_audit']['checks']
    eligible = [c for c in checks if frame_effect(int(c['word'], 0))]
    if owners[0]['frame_checks'] != eligible or audit.get('waived_machine_checks') != [c['id'] for c in eligible]:
        raise ValueError('Native ABI waiver must contain exactly authorized frame checks')
    registers = sorted({reg for _, reg in saved if reg != 31})
    rows = audit.get('native_register_bindings')
    if not isinstance(rows, list) or [r.get('register') for r in rows] != registers:
        raise ValueError('Native ABI saved register representations are incomplete or unordered')
    return {'owner': owners[0], 'registers': rows}


def validate_native_abi_sources(root, packet, proposal, changes, bindings):
    for audit in proposal.get('function_audits', []):
        if audit.get('replacement_kind') != REPLACEMENT:
            continue
        matched = [b for b in bindings if (b['image'], b['address']) == (audit['image'], audit['address'])]
        if len(matched) != 1:
            raise ValueError('Native ABI source owner lacks supervisor binding')
        admission = authorized_native_abi_frame(root, packet, proposal, matched[0], audit)
        relative = Path(audit['source_file'])
        path = (Path(root)/relative).resolve()
        if relative.is_absolute() or '..' in relative.parts or not path.is_relative_to((Path(root)/'src').resolve()):
            raise ValueError('Native ABI source escapes project')
        text = changes.get(path, path.read_bytes() if path.is_file() else b'')
        if isinstance(text, bytes):
            text = text.decode('utf-8-sig')
        text = text.replace('\r\n', '\n')
        parsed = [f for f in functions(text, str(relative)) if f['name'] == audit['source_symbol']]
        if len(parsed) != 1:
            raise ValueError('Native ABI source function is not unique')
        function = parsed[0]
        if function['markers'] != [(audit['address'], audit['image'])]:
            raise ValueError('Native ABI owner requires its exact original marker')
        body = text[function['start']:function['end']]
        if hashlib.sha256(body.encode('utf-8')).hexdigest() != audit.get('native_body_sha256'):
            raise ValueError('Native ABI exact native body hash changed')
        for row in admission['registers']:
            if set(row) != {'register', 'native_representation', 'source_file', 'source_symbol', 'source_line', 'source_anchor', 'explanation'} or row['source_file'] != audit['source_file'] or row['source_symbol'] != audit['source_symbol'] or not isinstance(row['explanation'], str) or not row['explanation'].strip():
                raise ValueError('Native ABI register source binding is incomplete')
            line = row['source_line']
            lines = text.splitlines()
            representation = row['native_representation']
            supported = isinstance(representation, str) and re.fullmatch(r'(?:[A-Za-z_]\w*|(?:0[xX][0-9a-fA-F]+|[0-9]+)[uU])', representation)
            if type(line) is not int or not function['start_line'] <= line <= function['end_line'] or not row['source_anchor'].strip() or lines[line-1].strip() != row['source_anchor'].strip() or not supported or not re.search(r'(?<![A-Za-z_0-9])'+re.escape(representation)+r'(?![A-Za-z_0-9])', lines[line-1]):
                raise ValueError('Native ABI register representation source anchor changed')


def native_abi_summary(proposal):
    keys = (*FIELDS, 'source_file', 'source_symbol', 'native_body_sha256', 'replacement_kind', 'original_equivalence', 'implementation_status', 'native_abi_authorization', 'waived_machine_checks', 'waived_original_effects', 'unresolved_semantics', 'native_register_bindings')
    return [{k: a[k] for k in keys} for a in proposal.get('function_audits', []) if a.get('replacement_kind') == REPLACEMENT]


def validate_native_abi_state(root, state):
    admissions = state.get('native_abi_frames', [])
    if not isinstance(admissions, list) or type(state.get('native_abi_frame_count', 0)) is not int or state.get('native_abi_frame_count', 0) != len(admissions):
        raise ValueError('Native ABI persisted admission count is invalid')
    keys = [(a.get('image'), a.get('address')) for a in admissions]
    if len(set(keys)) != len(keys):
        raise ValueError('Native ABI persisted owners are duplicated')
    for admission in admissions:
        owners = [f for f in state['functions'] if (f.get('image', state['image']), f['address']) == (admission['image'], admission['address'])]
        if len(owners) != 1 or owners[0].get('proof_basis') != 'user_authorized_native_abi_frame_scoped_audit' or owners[0].get('original_equivalence') is not False:
            raise ValueError('Native ABI persisted proof disposition changed')
        relative = Path(owners[0]['audit_receipt'])
        receipt_path = (Path(root)/relative).resolve()
        if relative.is_absolute() or '..' in relative.parts or not receipt_path.is_relative_to((Path(root)/'status').resolve()):
            raise ValueError('Native ABI persisted receipt escapes project')
        receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
        receipt_sha = owners[0].get('audit_receipt_sha256')
        if receipt_sha is not None and hashlib.sha256(receipt_path.read_bytes()).hexdigest() != receipt_sha:
            raise ValueError('Native ABI persisted receipt hash changed')
        if admission not in native_abi_summary(receipt) or receipt.get('native_abi_frame_admissions') != native_abi_summary(receipt) or type(receipt.get('native_abi_frame_count')) is not int or receipt['native_abi_frame_count'] != len(native_abi_summary(receipt)):
            raise ValueError('Native ABI persisted receipt disposition changed')
        packet_path = receipt_path.with_suffix('.json')
        # Audit receipts use the packet stem followed by .audit
        packet_path = packet_path.with_name(packet_path.name.removesuffix('.audit.json')+'.json')
        packet = json.loads(packet_path.read_text(encoding='utf-8-sig'))
        bindings = packet['source_context']['audit_contract']['required_functions']
        validate_native_abi_sources(root, packet, receipt, {}, bindings)
