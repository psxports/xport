"""Explicit single-call callback ABI exception plus separately reviewed GTE repair"""

import json
from pathlib import Path

from coverage_game_frame_delegation import _review, sha
from coverage_missing_stubs import policy_entries
from source_index import functions

ADDRESS = 0x80016364
CALLSITE = 0x800165AC
RISK_SHA256 = '0c9e39806ce1eb3d7945bdc1dd38ff331c746d7dd8c4ec951e97d525a820da16'
BEFORE_SHA256 = '70df4c3ae18215457ab6129205c09c353eb5158df0d6a7d842a8c7b405354f47'
AFTER_SHA256 = 'd1b71f884fdedc4e600e86cb622c2b53df69bc62a1584bdf18cbb08d6f89af5c'
GAPS = ['Unproved incoming callback GPR and HI/LO values', 'Effects of executing the callback']
CHECK_IDS = ['0x800165ac:call', '0x800165b0:delay']
PROTOTYPE = 'uint32 xport_guest_call_known_registers_post_sdk(uint32 target, uint32 *registers, uint32 known_register_mask);'
BACKEND_FUNCTIONS = {'InitGeom', 'SetGeomOffset', 'psx_gte_import_control_registers',
                     'xport_gte_write_control', 'native_gte_project',
                     'native_gte_screen_offset_raw', 'native_gte_floor_shift',
                     'clamp_ir', 'clamp_sxy', 'clamp_sz', 'truncate_mac0', 'finish_flag',
                     'gte_unr_divide', 'native_gte_wrap44', 'gte_import_matrix',
                     'xport_gte_execute', 'native_gte_vertex', 'xport_gte_write_data',
                     'psx_gte_snapshot'}


def validate_fixture_receipt(proof, apply, fixture):
    if fixture.get('contract') != 'xport-gte-projection-directed-fixture-run-v1' or fixture.get('result') != 'PASS' or type(fixture.get('build_exit_code')) is not int or fixture['build_exit_code'] != 0 or type(fixture.get('run_exit_code')) is not int or fixture['run_exit_code'] != 0 or type(fixture.get('directed_test_groups')) is not int or fixture['directed_test_groups'] != 11 or type(fixture.get('passed_test_groups')) is not int or fixture['passed_test_groups'] != 11:
        raise ValueError('16364 GTE review lacks successful separate numerical fixture')
    reference = proof.get('fixture_receipt')
    if not isinstance(reference, dict) or reference.get('sha256') != apply.get('fixture_receipt_sha256'):
        raise ValueError('16364 GTE fixture differs from applied repair receipt')
    expected = {'plan_sha256': apply.get('plan_sha256'),
                'proposal_sha256': apply.get('plan_sha256'),
                'predicted_source_sha256': apply.get('new_source_sha256'),
                'executable_sha256': apply.get('executable_sha256'),
                'compiled_source_sha256': apply.get('compiled_source_sha256')}
    if any(not isinstance(value, str) or len(value) != 64 or fixture.get(key) != value for key, value in expected.items()):
        raise ValueError('16364 GTE fixture plan/source/executable chain changed')


def compatible_backend_extension(root, proof, symbol, expected, actual_body):
    reference = proof.get('backend_compatibility_review')
    if reference is None:
        return False
    review = _review(root, reference)
    if review.get('contract') != 'xport-16364-disjoint-gte-extension-review-v1' or review.get('image') != 'SLUS_005.10' or review.get('address') != ADDRESS or review.get('covered_commands') != [0x00280030, 0x00180001] or review.get('covered_data_registers') != [0, 1, 2, 3, 4, 5]:
        raise ValueError('16364 backend compatibility scope changed')
    rows = review.get('bodies')
    if not isinstance(rows, list) or {r.get('symbol') for r in rows} != {'xport_gte_execute', 'xport_gte_write_data'} or len(rows) != 2:
        raise ValueError('16364 backend compatibility body closure changed')
    if symbol not in {'xport_gte_execute', 'xport_gte_write_data'}:
        return False
    row = next(r for r in rows if r['symbol'] == symbol)
    baseline, resulting = row.get('baseline_text'), row.get('result_text')
    if not isinstance(baseline, str) or not isinstance(resulting, str) or sha(baseline) != expected or row.get('baseline_sha256') != expected or row.get('result_sha256') != sha(resulting) or resulting != actual_body or '#' in resulting:
        raise ValueError('16364 backend compatibility original/current body changed')
    # Remove only branches unreachable for this exact owner, retaining all other text
    import re
    if symbol == 'xport_gte_execute':
        restored, count1 = re.subn(r'        case 0x14u:\n        \{.*?\n        \}\n(?=        case 0x10u:)', '', resulting, flags=re.S)
        restored, count2 = re.subn(r'        case 0x28u:\n.*?(?=        case 0x0Cu:)', '', restored, flags=re.S)
    else:
        restored, count1 = re.subn(r'    else if \(register_index == 30u\)\n    \{.*?\n    \}\n(?=    else if \(register_index == 31u\))', '', resulting, flags=re.S)
        restored, count2 = re.subn(r'    else if \(register_index == 31u\)\n    \{.*?\n    \}\n(?=    else\n        abort\(\);)', '', restored, flags=re.S)
    if count1 != 1 or count2 != 1 or restored != baseline:
        raise ValueError('16364 backend extension changed a covered branch or common effect')
    return True


def reviewed_gte_substitution(root, binding, row, audit):
    proof = _review(root, audit.get('gte_projection_review'))
    original = row['original_hashes']
    if type(proof.get('schema')) is not int or proof['schema'] != 1 or proof.get('contract') != 'xport-16364-real-gte-substitution-review-v1' or proof.get('role') != 'supervisor' or proof.get('image') != binding['image'] or proof.get('address') != ADDRESS or proof.get('original_hashes') != original or proof.get('retained_numerical_static_audit_complete') is not True:
        raise ValueError('16364 GTE substitution lacks complete separate supervisor review')
    if proof.get('baseline_sha256') != BEFORE_SHA256 or proof.get('result_sha256') != AFTER_SHA256 or proof.get('baseline_text') != row['candidate_text'] or sha(proof['baseline_text']) != BEFORE_SHA256 or sha(proof.get('result_text', '')) != AFTER_SHA256:
        raise ValueError('16364 GTE substitution exceeds exact reviewed native bodies')
    before = proof['baseline_text']
    replacements = [
        ('uint32 registers[32]; sint32 flags, screen[3], depth[3]; SVECTOR vectors[3]; PsxGteSnapshot snapshot;', 'uint32 registers[32]; PsxGteSnapshot snapshot;'),
        ('            vectors[0].vx = (sint16)x; vectors[0].vy = (sint16)y; vectors[0].vz = 0;\n            vectors[1].vx = (sint16)(x + 16u); vectors[1].vy = (sint16)y; vectors[1].vz = 0;\n            vectors[2].vx = (sint16)x; vectors[2].vy = (sint16)(y + 16u); vectors[2].vz = 0;\n', ''),
        ('            gte_project3_full_depth(vectors, screen, depth, &flags);\n            xport_gte_complete_rtpt();', '            xport_gte_execute(0x00280030u);'),
        ('            vectors[0].vx = (sint16)(x + 16u); vectors[0].vy = (sint16)(y + 16u); vectors[0].vz = 0;\n            (void)gte_project_full_depth(&vectors[0], &screen[0], &flags);\n            xport_gte_complete_rtps();', '            xport_gte_execute(0x00180001u);')]
    for old, new in replacements:
        if before.count(old) != 1:
            raise ValueError('16364 GTE substitution anchors changed')
        before = before.replace(old, new)
    if before != proof['result_text']:
        raise ValueError('16364 GTE substitution changed ordinary game algorithm')
    config = json.loads((Path(root) / 'xport-project.json').read_text(encoding='utf-8-sig'))
    toolset = Path(config.get('toolset', ''))
    if not toolset.is_absolute() or toolset.name != 'tools' or not toolset.is_dir():
        raise ValueError('16364 GTE source requires configured shared toolset')
    source_path = toolset.parent / 'src/psx.c'
    raw = source_path.read_bytes()
    if not isinstance(proof.get('backend_source_sha256'), str) or len(proof['backend_source_sha256']) != 64:
        raise ValueError('16364 reviewed GTE source identity missing')
    body_hashes = proof.get('backend_body_hashes')
    if not isinstance(body_hashes, dict) or set(body_hashes) != BACKEND_FUNCTIONS:
        raise ValueError('16364 reviewed GTE body closure incomplete')
    text = raw.decode('utf-8-sig').replace('\r\n', '\n')
    if text.count('static PsxGteSnapshot gte;') != 1 or text.count('static uint16 native_gte_ofx_fraction, native_gte_ofy_fraction;') != 1:
        raise ValueError('16364 reviewed private GTE storage changed')
    # Normal source identity gates admit SDK appends; all numerical bodies remain exact
    parsed = functions(text, str(source_path))
    for symbol, expected in body_hashes.items():
        rows = [f for f in parsed if f['name'] == symbol]
        if len(rows) != 1:
            raise ValueError('16364 reviewed GTE body changed: ' + symbol)
        body = text[rows[0]['start']:rows[0]['end']] + '\n'
        if sha(body) != expected and not compatible_backend_extension(root, proof, symbol, expected, body):
            raise ValueError('16364 reviewed GTE body changed: ' + symbol)
    apply = _review(root, proof.get('apply_receipt'))
    if apply.get('contract') != 'xport-gte-projection-prerequisite-v1' or apply.get('role') != 'supervisor' or apply.get('result') != 'SOURCE_REPAIRED_REISSUE_REQUIRED' or apply.get('new_source_sha256') != proof['backend_source_sha256'] or apply.get('original_owner') != {'image': binding['image'], 'address': ADDRESS, **original}:
        raise ValueError('16364 GTE review lacks exact applied repair receipt')
    fixture = _review(root, proof.get('fixture_receipt'))
    validate_fixture_receipt(proof, apply, fixture)
    return proof['result_text']


def callback_scope(root, packet, proposal, binding, audit, row, context):
    auth = _review(root, audit.get('callback_abi_authorization'))
    keys = {'schema', 'contract', 'user_instruction', 'review', 'image', 'owner_address', 'callsite', 'scope', 'retained_requirements', 'no_general_waiver'}
    if set(auth) != keys or type(auth['schema']) is not int or auth['schema'] != 1 or auth['contract'] != 'xport-user-authorized-16364-callback-abi-scope-v1' or auth['user_instruction'] != 'Разрешаю узкое исключение' or auth['image'] != 'SLUS_005.10' or auth['owner_address'] != ADDRESS or binding['address'] != ADDRESS or auth['callsite'] != CALLSITE or auth['scope'] != GAPS or auth['retained_requirements'] != ['Complete audit of all other game algorithm effects', 'Absent dispatcher unconditional abort', 'WIP', 'original_equivalence:false'] or auth['no_general_waiver'] is not True:
        raise ValueError('16364 callback scope lacks exact separate human authorization')
    if not isinstance(auth['review'], dict) or auth['review'].get('sha256') != RISK_SHA256:
        raise ValueError('16364 callback risk review changed')
    risk = _review(root, auth['review'])
    if risk.get('contract') != 'xport-review-batch10-16364-masked-dispatch-boundary-risk-v1' or risk.get('owner_original', {}).get('sha256') != binding['function_sha256'] or risk.get('candidate_mask') != '0x9007003D':
        raise ValueError('16364 callback risk differs from original owner')
    checks = context['machine_audit']['checks']
    target = [c for c in checks if c['id'] in CHECK_IDS]
    if [c['id'] for c in target] != CHECK_IDS or [int(c['word'], 0) for c in target] != [0x00A0F809, 0]:
        raise ValueError('16364 callback original JALR or delay changed')
    entries = [e for e in policy_entries(root, proposal) if e.get('kind') == 'system_bridge' and e.get('symbol') == 'xport_guest_call_known_registers_post_sdk' and e.get('prototype') == PROTOTYPE and e.get('source_file') == '@xport/src/psx.c' and e.get('caller_image') == binding['image'] and e.get('caller_address') == ADDRESS and e.get('caller_function_sha256') == binding['function_sha256']]
    if len(entries) != 1:
        raise ValueError('16364 callback lacks exact missing dispatcher policy')
    text = reviewed_gte_substitution(root, binding, row, audit)
    return {'result_text': text, 'gaps': GAPS.copy(), 'check_ids': CHECK_IDS.copy(), 'entry': entries[0]}
