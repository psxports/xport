"""Exact project authorization for user-requested absent-device replacements"""

import hashlib
import json
from pathlib import Path
import re

from source_index import functions

CONTRACT = 'xport-user-authorized-absent-devices-v1'
REPLACEMENT = 'user_authorized_absent_device'
INSTRUCTION = 'игнорируй карты памяти, pad tap, mdec видео, используй для них заглушки, будто карты нет, tap нет, видео тоже нет'
DEVICES = {'memory_cards', 'multitap', 'mdec_video'}


def policy_entries(root, proposal):
    reference = proposal.get('absent_device_authorization')
    if reference is None:
        return []
    if not isinstance(reference, dict) or set(reference) != {'path', 'sha256'}:
        raise ValueError('Absent-device policy needs its exact path and hash')
    relative = Path(reference['path']) if isinstance(reference['path'], str) else Path()
    path = (Path(root) / relative).resolve()
    if (relative.is_absolute() or '..' in relative.parts or relative.parts[:1] != ('status',)
            or not path.is_relative_to((Path(root) / 'status').resolve()) or not path.is_file()):
        raise ValueError('Absent-device policy must be a project status file')
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != reference['sha256']:
        raise ValueError('Absent-device policy hash changed')
    document = json.loads(raw.decode('utf-8-sig'))
    if (set(document) != {'schema', 'contract', 'user_instruction', 'entries'}
            or document['schema'] != 1 or document['contract'] != CONTRACT
            or document['user_instruction'] != INSTRUCTION
            or not isinstance(document['entries'], list) or not document['entries']):
        raise ValueError('Absent-device policy has no exact supported user instruction')
    seen = set()
    fields = {'device', 'image', 'address', 'function_sha256', 'symbol', 'prototype',
              'source_file', 'scope_evidence', 'replacement_sha256'}
    for entry in document['entries']:
        if (not isinstance(entry, dict) or set(entry) not in (fields, fields | {'omission'})
                or entry['device'] not in DEVICES):
            raise ValueError('Invalid absent-device entry')
        key = (entry['image'], entry['address'])
        if (type(entry['address']) is not int or key in seen
                or not all(isinstance(entry[k], str) and entry[k] for k in fields - {'address'})):
            raise ValueError('Absent-device entry needs a unique exact identity')
        if not re.fullmatch(r'[A-Za-z_]\w*', entry['symbol']):
            raise ValueError('Absent-device entry needs a plain C symbol')
        if not all(re.fullmatch(r'[0-9a-f]{64}', entry[k]) for k in ('function_sha256', 'replacement_sha256')):
            raise ValueError('Absent-device entry needs exact SHA-256 bindings')
        if 'omission' in entry:
            validate_omission_policy(entry)
        seen.add(key)
    return document['entries']


def validate_noop_body(text, entry):
    parsed = functions(text, entry['source_file'])
    if (len(parsed) != 1 or parsed[0]['name'] != entry['symbol']
            or text[:parsed[0]['start']].strip() or text[parsed[0]['end']:].strip()
            or hashlib.sha256(text.strip().encode()).hexdigest() != entry['replacement_sha256']):
        raise ValueError('Absent-device replacement differs from its exact scoped body')
    opening = text.index('{', parsed[0]['start'])
    prototype = text[parsed[0]['start']:opening].strip() + ';'
    if re.sub(r'\s+', '', prototype) != re.sub(r'\s+', '', entry['prototype']):
        raise ValueError('Absent-device replacement changes its scoped ABI')
    if 'omission' in entry:
        validate_omission_policy(entry)
        omission = entry['omission']
        expected = omission['baseline_text'].replace(omission['before'], omission['after'], 1)
        if text.strip() != expected.strip():
            raise ValueError('Device block omission changed retained game source')
        return
    if not prototype.startswith('void '):
        raise ValueError('No-op admission requires a void API; absence return codes need separate review')
    body = text[opening + 1:text.rfind('}')]
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', body, flags=re.S)
    marker = r'FUNCTION_MARKER\(0x([0-9A-Fa-f]+)u,\s*"([^"\n]+)"\)\s*;'
    markers = re.findall(marker, body)
    if markers != [(format(entry['address'], '08X'), entry['image'])]:
        raise ValueError('No-op requires its exact original function marker')
    body = re.sub(marker, '', body)
    parameters = prototype[prototype.index('(') + 1:prototype.rindex(')')]
    names = []
    if parameters.strip() != 'void':
        for parameter in parameters.split(','):
            match = re.fullmatch(r'[A-Za-z_][A-Za-z_0-9 \t*]*\b([A-Za-z_]\w*)', parameter.strip())
            if match is None:
                raise ValueError('No-op parameters need explicit names')
            names.append(match.group(1))
    statements = [s.strip() for s in body.split(';') if s.strip()]
    expected = ['(void)' + name for name in names]
    if [re.sub(r'\s+', '', s) for s in statements] != expected:
        raise ValueError('No-op must only suppress its parameters; calls and returns are forbidden')


def validate_omission_policy(entry):
    omission = entry['omission']
    fields = {'baseline_text', 'baseline_sha256', 'before', 'after', 'original_range'}
    if not isinstance(omission, dict) or set(omission) != fields:
        raise ValueError('Device omission needs an exact baseline, block and original range')
    baseline, before, after = (omission.get(k) for k in ('baseline_text', 'before', 'after'))
    if not all(isinstance(value, str) and value for value in (baseline, before, after)):
        raise ValueError('Device omission has empty source bindings')
    if hashlib.sha256(baseline.encode()).hexdigest() != omission['baseline_sha256']:
        raise ValueError('Device omission baseline hash changed')
    parsed = functions(baseline, entry['source_file'])
    if (len(parsed) != 1 or parsed[0]['name'] != entry['symbol']
            or baseline[:parsed[0]['start']].strip() or baseline[parsed[0]['end']:].strip()
            or baseline.count(before) != 1):
        raise ValueError('Device omission needs one original function and one exact block')
    opening = baseline.index('{', parsed[0]['start'])
    block_start = baseline.index(before)
    if block_start <= opening or block_start + len(before) > baseline.rfind('}'):
        raise ValueError('Device omission must lie inside the function body')
    comments = {'memory_cards':'Memory cards are absent by user instruction',
                'multitap':'Multitap is absent by user instruction',
                'mdec_video':'MDEC video is absent by user instruction'}
    if after.strip() != '/* ' + comments[entry['device']] + ' */':
        raise ValueError('Device omission may replace its block only with the device comment')
    marker = f'FUNCTION_MARKER(0x{entry["address"]:08X}u, "{entry["image"]}");'
    if baseline.count(marker) != 1 or marker in before:
        raise ValueError('Device omission must preserve the original marker')
    span = omission['original_range']
    if (not isinstance(span, list) or len(span) != 2 or any(type(v) is not int for v in span)
            or not entry['address'] <= span[0] < span[1] or any(v % 4 for v in span)):
        raise ValueError('Device omission needs a nonempty aligned original instruction range')


def validate_omission_checks(entry, audit, checks):
    if 'omission' not in entry:
        return
    first, last = entry['omission']['original_range']
    expected = [check['id'] for check in checks if first <= int(check['pc'], 0) < last]
    if not expected or audit.get('waived_machine_checks') != expected:
        raise ValueError('Device omission must name exactly its ordered original machine checks')
    if (audit.get('retained_semantics_complete') is not True
            or audit.get('retained_unresolved_semantics') != []):
        raise ValueError('Device omission cannot waive unresolved retained game semantics')


def authorized_audit(root, proposal, binding, audit):
    if audit.get('replacement_kind') != REPLACEMENT:
        return None
    matches = [entry for entry in policy_entries(root, proposal)
               if entry['image'] == binding['image'] and entry['address'] == binding['address']
               and entry['function_sha256'] == binding['function_sha256']
               and entry['symbol'] == audit.get('source_symbol')
               and entry['source_file'] == audit.get('source_file')
               and entry['device'] == audit.get('device')]
    if len(matches) != 1:
        raise ValueError('Absent-device audit has no exact authorized original and source')
    if (audit.get('original_equivalence') is not False
            or audit.get('complete_mips_audit') is not True
            or audit.get('pseudocode_used_as_baseline') is not binding.get('pseudocode_complete')):
        raise ValueError('Absent-device audit must retain full original read and explicit non-equivalence')
    for field in ('waived_original_effects', 'unresolved_semantics'):
        items = audit.get(field)
        if not isinstance(items, list) or not items or not all(isinstance(v, str) and v for v in items):
            raise ValueError('Absent-device audit must record waived effects and unresolved equivalence')
    for field in ('function_sha256', 'mips_sha256', 'pseudocode_sha256', 'machine_audit_sha256'):
        if audit.get(field) != binding.get(field):
            raise ValueError('Absent-device audit differs from original ' + field)
    return matches[0]


def admission_summary(proposal):
    fields = ('image', 'address', 'source_file', 'source_symbol', 'device',
              'replacement_kind', 'original_equivalence', 'waived_original_effects',
              'unresolved_semantics')
    rows = [audit for audit in proposal.get('function_audits') or []
            if audit.get('replacement_kind') == REPLACEMENT]
    return [{**{field:audit[field] for field in fields}, 'implementation_status':'WIP'}
            for audit in rows]


def retain_admissions(state, admissions):
    retained = state.setdefault('absent_devices', [])
    for admission in admissions:
        identity = (admission['image'], admission['address'])
        if admission.get('original_equivalence') is not False or admission.get('implementation_status') != 'WIP':
            raise ValueError('Absent-device inventory cannot claim original equivalence or DONE')
        prior = next((row for row in retained if (row['image'], row['address']) == identity), None)
        if prior is None:
            retained.append(dict(admission))
        else:
            prior.update(admission)
    state['absent_device_count'] = len(retained)
    return retained
