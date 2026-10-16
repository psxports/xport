"""Apply one supervisor-reviewed GTE arithmetic prerequisite without admitting coverage"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

import coverage as supervisor
from lockstep_audit import checklist, words_from_listing
from source_index import functions
from xport_project import allocation_lock, load_project

CONTRACT = 'xport-gte-projection-prerequisite-v1'
PLAN_SHA256 = '8d32fbcb75c04d0206c33e01b81702930139ba2fec77f8bd3fa00b274b426524'
PACKET_SHA256 = '921c0565dc18fedcc68e1940f3e86eb6a51a734c03765211375e5c18215ed1c7'
SYMBOLS = ('InitGeom', 'SetGeomOffset', 'psx_gte_import_control_registers', 'xport_gte_write_control', 'native_gte_project')
HELPERS = ('native_gte_screen_offset_raw', 'native_gte_floor_shift')
DEPENDENCIES = ('clamp_ir', 'clamp_sxy', 'clamp_sz', 'truncate_mac0', 'finish_flag', 'gte_unr_divide', 'native_gte_wrap44', 'gte_import_matrix')
LIMITATIONS = ('Public snapshot/export omits fractional OFX/OFY', 'Legacy projection APIs remain outside this repair', 'Directed fixture is not a full numerical equivalence oracle', 'No game proof or coverage counter admission')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def owned(root, path):
    path = Path(path).resolve()
    if not path.is_relative_to((root / 'status').resolve()) or path.name.lower() == 'readme.md':
        raise ValueError('Prerequisite artifact must be project-owned status')
    return path


def normalize(text):
    return text.replace('\r\n', '\n')


def body(text, symbol):
    rows = [row for row in functions(text, 'src/psx.c') if row['name'] == symbol]
    if len(rows) != 1:
        raise ValueError('Require one complete body: ' + symbol)
    row = rows[0]
    return text[row['start']:row['end']].rstrip() + '\n', row


def predict(source, plan):
    if tuple(row['symbol'] for row in plan.get('replacements', [])) != SYMBOLS:
        raise ValueError('Foreign replacement manifest')
    text = normalize(source)
    for row in plan['replacements']:
        actual, _ = body(text, row['symbol'])
        if actual != row['before'] or sha(actual.encode()) != row['before_sha256']:
            raise ValueError('Shared whole-body drift: ' + row['symbol'])
        if text.count(row['before'].rstrip()) != 1:
            raise ValueError('Ambiguous whole-body occurrence')
        after, _ = body(row['after'], row['symbol'])
        if after != row['after'] or len(functions(row['after'], 'src/psx.c')) != 1:
            raise ValueError('Replacement is not one exact complete body')
        text = text.replace(row['before'].rstrip(), row['after'].rstrip(), 1)
    anchor = plan['private_storage_insert_after']
    if anchor != 'static PsxGteSnapshot gte;' or text.count(anchor) != 1:
        raise ValueError('Private storage anchor changed')
    if plan['private_storage_text'] != 'static uint16 native_gte_ofx_fraction, native_gte_ofy_fraction;\n':
        raise ValueError('Foreign private declaration')
    if any(name in source for name in ('native_gte_ofx_fraction', 'native_gte_ofy_fraction', *HELPERS)):
        raise ValueError('Private names already exist')
    text = text.replace(anchor, anchor + '\n' + plan['private_storage_text'].rstrip(), 1)
    if tuple(row['name'] for row in functions(plan['helper_text'], 'src/psx.c')) != HELPERS:
        raise ValueError('Foreign helper manifest')
    _, target = body(text, 'native_gte_project')
    if plan['helper_insert_before'] != 'native_gte_project':
        raise ValueError('Foreign helper insertion')
    text = text[:target['start']] + plan['helper_text'] + '\n' + text[target['start']:]
    return text


def predict_preserving_newlines(source, plan):
    expected = predict(source, plan)
    text = source
    for row in plan['replacements']:
        actual, position = body(text, row['symbol'])
        if normalize(actual) != row['before']:
            raise ValueError('Raw whole-body changed')
        original = text[position['start']:position['end']]
        first_lf = original.find('\n')
        newline = '\r\n' if first_lf > 0 and original[first_lf - 1] == '\r' else '\n'
        after = row['after'].rstrip().replace('\n', newline)
        text = text[:position['start']] + after + text[position['end']:]
    anchor = plan['private_storage_insert_after']
    start = text.index(anchor) + len(anchor)
    newline = '\r\n' if text[start:start + 2] == '\r\n' else '\n'
    declaration = newline + plan['private_storage_text'].rstrip()
    text = text[:start] + declaration + text[start:]
    _actual, position = body(text, 'native_gte_project')
    original = text[position['start']:position['end']]
    first_lf = original.find('\n')
    newline = '\r\n' if first_lf > 0 and original[first_lf - 1] == '\r' else '\n'
    helpers = (plan['helper_text'] + '\n').replace('\n', newline)
    text = text[:position['start']] + helpers + text[position['start']:]
    if normalize(text) != expected:
        raise ValueError('Raw prediction differs from exact authorized normalized edits')
    return text


def original_owner(root, packet):
    context = packet['source_context']
    rows = [context, *context.get('audit_cluster', [])]
    owners = [row for row in rows if row['image'] == 'SLUS_005.10' and row['function']['address'] == 0x80016364]
    if len(owners) != 1:
        raise ValueError('Missing exact original GTE owner')
    owner = owners[0]
    fresh = supervisor.compact('SLUS_005.10', 0x80016364, 262144, full_source=True, full_original=True)
    if any(owner.get(key) != fresh.get(key) for key in ('function', 'mips', 'pseudocode')):
        raise ValueError('Original full context changed')
    words = words_from_listing(owner['mips']['lines'])
    first, end = owner['function']['address'], owner['function']['end']
    if sorted(words) != list(range(first, end, 4)) or sha(b''.join(struct.pack('<I', words[pc]) for pc in sorted(words))) != owner['function']['sha256']:
        raise ValueError('Original raw range or function hash changed')
    if words.get(0x800164E8) != 0x4A280030 or words.get(0x80016518) != 0x4A180001:
        raise ValueError('Exact RTPT/RTPS instructions changed')
    if checklist(owner['mips']['lines']) != owner['machine_audit']:
        raise ValueError('Original ordered machine audit changed')
    binding = {'image': 'SLUS_005.10', 'address': first, 'function_sha256': owner['function']['sha256'], 'mips_sha256': owner['mips']['sha256'], 'pseudocode_sha256': owner['pseudocode']['sha256'], 'machine_audit_sha256': owner['machine_audit']['sha256']}
    matches = [row for row in context['audit_contract']['required_functions'] if row['image'] == binding['image'] and row['address'] == first]
    if len(matches) != 1 or any(matches[0].get(key) != value for key, value in binding.items()):
        raise ValueError('Four original packet bindings changed')
    return binding


def validate(root, packet_path, plan_path):
    root, config = load_project(root)
    os.environ['XPORT_PROJECT'] = str(root)
    directory = root / 'status/function-coverage/first'
    packet_path, plan_path = owned(root, packet_path), owned(root, plan_path)
    if packet_path != (directory / 'coverage-batch-0010-revision-0010.json').resolve():
        raise ValueError('Require reviewed batch10 revision10')
    if sha(packet_path.read_bytes()) != PACKET_SHA256:
        raise ValueError('Reviewed exact immutable packet changed')
    if sha(plan_path.read_bytes()) != PLAN_SHA256:
        raise ValueError('Reviewed exact plan changed')
    plan, packet = read(plan_path), read(packet_path)
    state_path = directory / 'coverage.json'
    state = read(state_path)
    if (root / state['active_packet']).resolve() != packet_path:
        raise ValueError('Active packet changed')
    shared = Path(config['toolset'])
    shared = (shared if shared.is_absolute() else root / shared).resolve().parent
    project_before = supervisor.source_snapshot(root, supervisor.SOURCE_ROOTS)
    shared_before = supervisor.source_snapshot(shared, supervisor.SOURCE_ROOTS)
    if project_before['identity'] != packet['source_identity'] or shared_before['identity'] != packet['identities']['shared_source_identity']:
        raise ValueError('Packet source identity drift')
    source = shared / 'src/psx.c'
    raw = source.read_bytes()
    if sha(raw) != plan['actual_source_sha256']:
        raise ValueError('Reviewed full shared file changed')
    solution = (root / config['paths']['native_solution']).resolve()
    projects = list(solution.parent.glob('*.vcxproj'))
    if len(projects) != 1:
        raise ValueError('Ambiguous compiled project')
    tree = ET.fromstring(projects[0].read_text(encoding='utf-8-sig'))
    compiled = {(projects[0].parent / row.attrib['Include'].replace('\\', '/')).resolve() for row in tree.iter() if row.tag.split('}')[-1] == 'ClCompile' and 'Include' in row.attrib}
    if source.resolve() not in compiled:
        raise ValueError('Shared psx.c is not compiled')
    owner = original_owner(root, packet)
    decoded = raw.decode('utf-8-sig')
    if '\r' in decoded.replace('\r\n', ''):
        raise ValueError('Bare CR source newlines require separate review')
    predicted_text = predict_preserving_newlines(decoded, plan)
    predicted = predicted_text.encode('utf-8-sig' if raw.startswith(b'\xef\xbb\xbf') else 'utf-8')
    index = (root / config['paths']['analysis_database']).resolve()
    if not index.is_relative_to((root / 'status').resolve()) or not index.is_file():
        raise ValueError('Missing project-owned source index')
    for suffix in ('-wal', '-shm', '-journal'):
        extra = Path(str(index) + suffix)
        if extra.exists():
            raise ValueError('Source index must have no live SQLite sidecars')
    invariants = [root / 'xport-project.json', state_path, root / 'status/translation-ledger.json', packet_path, plan_path, projects[0], solution]
    document = {'schema': 1, 'contract': CONTRACT, 'role': 'supervisor', 'plan_sha256': PLAN_SHA256, 'packet_sha256': sha(packet_path.read_bytes()), 'source_identity': project_before['identity'], 'shared_source_identity': shared_before['identity'], 'old_source_sha256': sha(raw), 'predicted_source_sha256': sha(predicted), 'original_owner': owner, 'symbols': list(SYMBOLS), 'helpers': list(HELPERS), 'source_file': '@xport/src/psx.c', 'limitations': list(LIMITATIONS), 'coverage_admission': False, 'active_packet_reissue_required': True, 'source_index_refresh_required': True}
    return {'root': root, 'shared': shared, 'source': source, 'old': raw, 'predicted': predicted, 'index': index, 'invariants': {path: sha(path.read_bytes()) for path in invariants}, 'project_before': project_before, 'shared_before': shared_before, 'plan': plan, 'document': document}


def prerequisites(data, review_path, fixture_path):
    root, document = data['root'], data['document']
    review_path, fixture_path = owned(root, review_path), owned(root, fixture_path)
    review, fixture = read(review_path), read(fixture_path)
    if review.get('contract') != 'xport-gte-projection-supervisor-review-v1' or review.get('role') != 'supervisor' or review.get('complete_source_review') is not True:
        raise ValueError('Full supervisor source review is required')
    for key in ('plan_sha256', 'packet_sha256', 'source_identity', 'shared_source_identity', 'predicted_source_sha256'):
        if review.get(key) != document[key]:
            raise ValueError('Supervisor review binding changed: ' + key)
    if fixture.get('contract') != 'xport-gte-projection-directed-fixture-run-v1' or fixture.get('result') != 'PASS' or fixture.get('proposal_sha256') != PLAN_SHA256:
        raise ValueError('Actual successful compiled fixture is required')
    if fixture.get('build_exit_code') != 0 or fixture.get('run_exit_code') != 0 or fixture.get('directed_test_groups') != 11 or fixture.get('passed_test_groups') != 11:
        raise ValueError('All eleven actual directed groups must pass')
    compiled_source = (root / fixture['compiled_source_path']).resolve()
    if not compiled_source.is_relative_to((root / 'status').resolve()) and not compiled_source.is_relative_to((root / '_build').resolve()):
        raise ValueError('Compiled fixture source must be project-owned status or _build')
    fixture_source = owned(root, root / fixture['fixture_path'])
    executable = (root / fixture['executable_path']).resolve()
    if (not executable.is_relative_to((root / '_build').resolve()) and not executable.is_relative_to((root / 'status').resolve())) or not executable.is_file():
        raise ValueError('Fixture executable must be project-owned status or _build')
    if sha(compiled_source.read_bytes()) != fixture['compiled_source_sha256'] or sha(fixture_source.read_bytes()) != fixture['fixture_sha256'] or compiled_source.read_bytes() != fixture_source.read_bytes() or sha(executable.read_bytes()) != fixture['executable_sha256']:
        raise ValueError('Actual compiled fixture/executable bytes changed')
    text = normalize(compiled_source.read_text(encoding='utf-8-sig'))
    for row in data['plan']['replacements']:
        if body(text, row['symbol'])[0] != row['after']:
            raise ValueError('Compiled fixture does not contain exact proposed body')
    for name in HELPERS:
        if body(text, name)[0] != body(data['plan']['helper_text'], name)[0]:
            raise ValueError('Compiled fixture helper changed')
    actual = normalize(data['old'].decode('utf-8-sig'))
    for name in DEPENDENCIES:
        if body(text, name)[0] != body(actual, name)[0]:
            raise ValueError('Compiled actual arithmetic dependency changed')
    if text.count(data['plan']['private_storage_text'].rstrip()) != 1:
        raise ValueError('Compiled fixture private storage changed')
    return {'supervisor_review_sha256': sha(review_path.read_bytes()), 'fixture_receipt_sha256': sha(fixture_path.read_bytes()), 'executable_sha256': fixture['executable_sha256'], 'compiled_source_sha256': fixture['compiled_source_sha256']}


def unchanged(data):
    if any(not path.is_file() or sha(path.read_bytes()) != value for path, value in data['invariants'].items()):
        raise ValueError('Prerequisite protected state changed')


def atomic_bytes(path, value):
    temporary = path.with_name(path.name + '.gte-prerequisite.tmp')
    if temporary.exists():
        raise ValueError('Prerequisite temporary collision')
    try:
        temporary.write_bytes(value)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def transact(data, receipt_path, prerequisite_bindings, refresh=None):
    refresh = refresh or supervisor.source_refresh
    root, source, index = data['root'], data['source'], data['index']
    receipt_path = owned(root, receipt_path)
    if receipt_path.exists():
        raise ValueError('Prerequisite receipt already exists')
    unchanged(data)
    if supervisor.source_snapshot(root, supervisor.SOURCE_ROOTS) != data['project_before'] or supervisor.source_snapshot(data['shared'], supervisor.SOURCE_ROOTS) != data['shared_before']:
        raise ValueError('Prewrite project/shared source identity drift')
    if source.read_bytes() != data['old']:
        raise ValueError('Prewrite shared source drift')
    index_before = index.read_bytes()
    wrote = False
    try:
        atomic_bytes(source, data['predicted'])
        wrote = True
        result = refresh(root)
        unchanged(data)
        if supervisor.source_snapshot(root, supervisor.SOURCE_ROOTS) != data['project_before']:
            raise ValueError('Refresh changed project sources')
        after = supervisor.source_snapshot(data['shared'], supervisor.SOURCE_ROOTS)
        expected = dict(data['shared_before']['files']); expected['src/psx.c'] = sha(data['predicted'])
        if after['files'] != expected:
            raise ValueError('Refresh changed unexpected shared sources')
        receipt = {**data['document'], **prerequisite_bindings, 'result': 'SOURCE_REPAIRED_REISSUE_REQUIRED', 'new_source_sha256': sha(source.read_bytes()), 'new_shared_source_identity': after['identity'], 'source_index_before_sha256': sha(index_before), 'source_index_after_sha256': sha(index.read_bytes()), 'source_index_refresh': result, 'proof_status_changed': False, 'coverage_counter_changed': False, 'unresolved_semantics_cleared': False}
        atomic_bytes(receipt_path, (json.dumps(receipt, indent=2) + '\n').encode('utf-8'))
        return receipt
    except BaseException:
        if wrote:
            if source.read_bytes() != data['predicted']:
                raise RuntimeError('External shared drift prevents safe rollback; preserve state for supervisor review')
            atomic_bytes(source, data['old'])
            atomic_bytes(index, index_before)
        if receipt_path.exists():
            receipt_path.unlink()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('action', choices=('prepare', 'apply'))
    parser.add_argument('--packet', type=Path, required=True)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--supervisor-review', type=Path)
    parser.add_argument('--fixture-receipt', type=Path)
    args = parser.parse_args()
    root, _ = load_project(args.project)
    with allocation_lock(root), allocation_lock(Path(load_project(root)[1]['toolset']).resolve().parent):
        data = validate(root, args.packet, args.plan)
        output = owned(root, args.output)
        if args.action == 'prepare':
            if output.suffix.lower() != '.txt':
                raise ValueError('Prepare output must be status TXT')
            atomic_bytes(output, data['predicted'])
            result = {**data['document'], 'result': 'PREPARED_SOURCE_ONLY', 'admission_ready': False, 'predicted_source_path': str(output)}
        else:
            if not args.supervisor_review or not args.fixture_receipt or output.suffix.lower() != '.json':
                raise ValueError('Apply requires supervisor and actual fixture receipts')
            bindings = prerequisites(data, args.supervisor_review, args.fixture_receipt)
            result = transact(data, output, bindings)
    print(json.dumps(result))


if __name__ == '__main__':
    main()
