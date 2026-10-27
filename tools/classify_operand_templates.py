"""Build lossless operand-template certificates without accepting native semantic reuse"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import sqlite3

from compare_mnemonic_sequences import mnemonic


MEMORY = {32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46, 48, 50, 56, 58}
BRANCH = {1, 4, 5, 6, 7}
TIERS = ('addresses_calls', 'scalar_constants', 'memory_offsets')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def signed(value):
    return (value & 65535) - (65536 if value & 32768 else 0)


def encoded(value):
    return json.dumps(value, separators=(',', ':'), sort_keys=True).encode()


def register_mask(word):
    op = word >> 26
    if op == 0:
        return 0x03fff800
    if op in (2, 3):
        return 0
    if op in (16, 17, 18):
        return 0x001ff800 if (word >> 21 & 31) < 16 else 0
    if op == 1:
        return 0x03e00000
    return 0x03ff0000


def address_pairs(instructions, data_refs):
    found = {}
    for index, (address, word, text, delay) in enumerate(instructions[:-1]):
        if word >> 26 != 15:
            continue
        register = word >> 16 & 31
        if register in (0, 28, 29):
            continue
        next_address, low, _, _ = instructions[index + 1]
        op = low >> 26
        if next_address != address + 4 or low >> 21 & 31 != register or op not in ({9, 13} | MEMORY):
            continue
        target = (((word & 65535) << 16) + (low & 65535 if op == 13 else signed(low))) & 0xffffffff
        if 0x80000000 <= target < 0x80200000 and target in (data_refs.get(address, set()) | data_refs.get(next_address, set())):
            found[index] = ('address_hi16', target)
            found[index + 1] = ('address_lo16', target)
    return found


def normalize(instructions, tier, resolve, data_refs=None):
    addresses = {address: index for index, (address, _, _, _) in enumerate(instructions)}
    pairs = address_pairs(instructions, data_refs or {})
    tokens, parameters, reasons, features = [], [], set(), set()
    for index, (address, word, text, delay) in enumerate(instructions):
        op, rs, rt, fn = word >> 26, word >> 21 & 31, word >> 16 & 31, word & 63
        mask, role, value, target_binding = 0, None, None, None
        extra = None
        name = mnemonic(text, format(word, '08X'))
        if name is None:
            reasons.add('undecoded_instruction')
        if op in BRANCH or op in (2, 3) or op == 0 and fn in (8, 9):
            if address + 4 not in addresses:
                reasons.add('missing_delay_slot')
        if op in BRANCH:
            target = (address + 4 + signed(word) * 4) & 0xffffffff
            if target in addresses:
                mask, role, value, extra = 65535, 'local_branch', word & 65535, addresses[target]
            else:
                reasons.add('branch_outside_function')
        elif op in (2, 3):
            target = ((address + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2)
            if op == 2 and target in addresses:
                mask, role, value, extra = 0x03ffffff, 'local_jump', word & 0x03ffffff, addresses[target]
            else:
                target_binding = resolve(target)
                if target_binding is None:
                    reasons.add('unresolved_direct_call_or_tail')
                else:
                    mask, role, value = 0x03ffffff, 'direct_call' if op == 3 else 'tail_call', word & 0x03ffffff
                    extra = 'self' if target == instructions[0][0] else 'external'
        elif op == 0 and fn in (8, 9):
            if not (fn == 8 and rs == 31):
                reasons.add('indirect_dispatch')
        elif index in pairs:
            mask, role, value = 65535, pairs[index][0], word & 65535
            target_binding = {'address': pairs[index][1], 'evidence': 'adjacent_lui_pair_and_indexed_data_reference'}
        elif op in MEMORY:
            if rs == 28:
                mask, role, value = 65535, 'gp_address', word & 65535
            elif tier == 'memory_offsets' and rs != 29:
                mask, role, value = 65535, 'memory_field_offset', word & 65535
        elif op in range(8, 16) and tier != 'addresses_calls':
            if rs not in (28, 29) and rt not in (28, 29):
                mask, role, value = 65535, 'scalar_immediate', word & 65535
        elif op == 0 and fn in (0, 2, 3) and word != 0 and tier != 'addresses_calls':
            mask, role, value = 0x7c0, 'shift_amount', word & 0x7c0
        if op in (16, 17, 18):
            features.add('coprocessor')
        if op == 0 and fn in (12, 13):
            features.add('exception_instruction')
        ra_use = False
        if op == 0 and fn not in (8, 9, 12, 13):
            ra_use = rs == 31 or rt == 31 or (word >> 11 & 31) == 31
        elif op in (8, 9, 10, 11, 12, 13, 14, 15) or op in BRANCH:
            ra_use = rs == 31 or (op != 1 and rt == 31)
        elif op in MEMORY:
            ra_use = rs == 31 or (op < 48 and rt == 31 and not (op in (35, 43) and rs == 29))
        elif op in (16, 17, 18) and rs in (0, 2, 4, 6):
            ra_use = rt == 31
        if ra_use:
            reasons.add('return_address_observed_or_repurposed')
        token = [word & ~mask, mask, role, extra, name, delay]
        tokens.append(token)
        if mask:
            parameters.append({'index': index, 'role': role, 'value_bits': value, 'binding': target_binding})
            features.add(role)
        restored = (word & ~mask) | (value if mask else 0)
        if restored != word:
            raise ValueError('Operand-template word reconstruction failed')
    return tokens, parameters, sorted(reasons), sorted(features)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('status/operand-templates'))
    args = parser.parse_args()
    root = args.project.resolve()
    out = args.output if args.output.is_absolute() else root / args.output
    out.mkdir(parents=True, exist_ok=True)
    config_path = root / 'xport-project.json'
    config = json.loads(config_path.read_text(encoding='utf-8-sig'))
    database = root / config['paths']['analysis_database']
    exact_path = root / 'status/decompilation-units.json'
    exact_sha = sha(exact_path.read_bytes())
    exact = json.loads(exact_path.read_text(encoding='utf-8-sig'))
    units = {row['unit']: row for row in exact['units']}
    membership = {(member['image'], member['address']): row['unit'] for row in exact['units'] for member in row['occurrences']}
    parents = config['analysis'].get('resident_parents', {})
    signatures = {tier: defaultdict(set) for tier in TIERS}
    unit_signatures = {tier: defaultdict(set) for tier in TIERS}
    blockers, feature_counts, input_hashes = Counter(), Counter(), []
    shapes, records = {}, {}
    first_operands, operand_differences = {}, defaultdict(set)
    db = sqlite3.connect(database.as_uri() + '?mode=ro', uri=True)
    functions = {(image, address): name for image, address, name in db.execute('SELECT image,address,name FROM functions')}
    sources = {}
    for image, source, original_sha, base, offset, gp in db.execute('SELECT id,source,sha256,base,file_offset,gp FROM images'):
        path = Path(source)
        raw = (path if path.is_absolute() else root / path).read_bytes()
        if sha(raw) != original_sha:
            raise ValueError('Original source changed: ' + image)
        sources[image] = (raw, base, offset, gp)
        input_hashes.append({'image': image, 'sha256': original_sha})
    chunks, references = defaultdict(list), defaultdict(lambda: defaultdict(set))
    for image, function, start, end in db.execute('SELECT image,function,start,end FROM chunks ORDER BY image,function,start'):
        chunks[(image, function)].append((start, end))
    for image, function, site, target in db.execute('SELECT image,function,site,target FROM data_refs'):
        references[(image, function)][site].add(target)

    def analyze(identity, instructions, stream):
        image, function = identity
        raw, base, offset, gp = sources[image]
        unit = membership[identity]
        record = units[unit]
        layout = [[a - function, z - function] for a, z in chunks[identity]]
        boundaries = set()
        independent = record.get('independent_ghidra_body')
        if not independent or independent[0] == 'unavailable' or independent[1] != layout:
            boundaries.add('independent_boundary_disagreement')
        covered = set()
        for address, word, text, delay in instructions:
            first = address - base + offset
            if raw[first:first + 4] != word.to_bytes(4, 'little'):
                raise ValueError('Indexed instruction differs from original')
            covered.update(range(address, address + 4))
        embedded = []
        for a, z in chunks[identity]:
            for address in range(a, z):
                if address not in covered:
                    embedded.append([address - function, raw[address - base + offset]])

        def resolve(target):
            if (image, target) in functions:
                return {'image': image, 'address': target}
            if image in parents and (parents[image], target) in functions:
                return {'image': parents[image], 'address': target}
            return None

        all_tiers = {}
        for tier in TIERS:
            tokens, parameters, reasons, features = normalize(instructions, tier, resolve, references[identity])
            reasons = sorted(set(reasons) | boundaries)
            structural = [gp, layout, embedded, tokens]
            signature = sha(encoded(structural))
            if reasons:
                signature = 'SEPARATE-' + unit
            all_tiers[tier] = {'signature': signature, 'parameters': parameters, 'blockers': reasons}
            signatures[tier][signature].add(unit)
            unit_signatures[tier][unit].add(signature)
            if tier == 'scalar_constants':
                blockers.update(reasons)
                feature_counts.update(features)
        sequence = tuple(mnemonic(text, format(word, '08X')) for _, word, text, _ in instructions)
        register_shape = tuple(word & register_mask(word) for _, word, _, _ in instructions)
        flow = []
        addresses = {a: n for n, (a, _, _, _) in enumerate(instructions)}
        for index, (address, word, _, _) in enumerate(instructions):
            op = word >> 26
            if op in BRANCH:
                target = address + 4 + signed(word) * 4
                flow.append([index, addresses.get(target, 'outside')])
            elif op == 2:
                target = ((address + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)
                flow.append([index, addresses.get(target, 'tail')])
        shapes[identity] = (sha(encoded(sequence)), sha(encoded(register_shape)), sha(encoded(flow)))
        sequence_id = shapes[identity][0]
        roles = {row['index']: row['role'] for row in all_tiers['memory_offsets']['parameters']}
        operands = [(word, roles.get(index)) for index, (_, word, _, _) in enumerate(instructions)]
        if sequence_id not in first_operands:
            first_operands[sequence_id] = operands
        else:
            for index, ((first, first_role), (word, role)) in enumerate(zip(first_operands[sequence_id], operands)):
                if first == word:
                    continue
                delta = first ^ word
                if delta & register_mask(word):
                    operand_differences[sequence_id].add('register_operands_differ')
                if word >> 26 in (16, 17, 18) and delta:
                    operand_differences[sequence_id].add('coprocessor_options_or_selectors_differ')
                for item in (role, first_role):
                    if item:
                        operand_differences[sequence_id].add(item + '_operand_differs')
                if word >> 26 in MEMORY and delta & 65535 and not role:
                    operand_differences[sequence_id].add('fixed_memory_or_stack_offset_differs')
                if word >> 26 in range(8, 16) and delta & 65535 and not role:
                    operand_differences[sequence_id].add('fixed_stack_or_gp_adjustment_differs')
        records[identity] = {'unit': unit, 'signatures': {tier: all_tiers[tier]['signature'] for tier in TIERS}}
        stream.write(json.dumps({'image': image, 'address': function, 'exact_body_unit': unit,
                                 'instruction_words': len(instructions), 'original_words_sha256': sha(b''.join(w.to_bytes(4, 'little') for _, w, _, _ in instructions)),
                                 'tiers': all_tiers}, separators=(',', ':')) + '\n')

    current, instructions, word_count = None, [], 0
    variants_path = out / 'variants.jsonl'
    with variants_path.open('w', encoding='utf-8') as stream:
        for image, function, address, word, text, delay in db.execute('SELECT image,function,address,word,disassembly,delay_slot FROM instructions ORDER BY image,function,address'):
            identity = (image, function)
            if current is not None and identity != current:
                analyze(current, instructions, stream)
                if len(records) % 2000 == 0:
                    print(json.dumps({'analyzed': len(records)}), flush=True)
                instructions = []
            current = identity
            instructions.append((address, int(word, 16), text, delay))
            word_count += 1
        if current is not None:
            analyze(current, instructions, stream)
    if set(records) != set(functions) or set(records) != set(membership):
        raise ValueError('Incomplete function inventory')
    db.close()
    summaries, family_ids = {}, {}
    for tier in TIERS:
        conflicting = {unit for unit, values in unit_signatures[tier].items() if len(values) != 1}
        groups = defaultdict(list)
        for unit in units:
            signature = 'SEPARATE-' + unit if unit in conflicting else next(iter(unit_signatures[tier][unit]))
            groups[signature].append(unit)
            family_ids[(tier, unit)] = signature
        families = []
        for signature, members in sorted(groups.items()):
            families.append({'id': signature, 'exact_body_units': members,
                             'occurrences': sum(len(units[unit]['occurrences']) for unit in members),
                             'representative': units[members[0]]['representative'],
                             'shared_template': len(members) > 1})
        result = {'templates': len(families), 'shared_families': sum(len(row['exact_body_units']) > 1 for row in families),
                  'exact_body_units_in_shared_families': sum(len(row['exact_body_units']) for row in families if row['shared_template']),
                  'additional_implementations_avoided_if_specialization_is_audited': len(units) - len(families),
                  'context_conflicting_exact_units_kept_separate': len(conflicting)}
        summaries[tier] = result
        (out / (tier + '-families.json')).write_text(json.dumps({'summary': result, 'families': families}, indent=2) + '\n', encoding='utf-8')
    mnemonic_groups = defaultdict(list)
    for identity, (sequence, registers, flow) in shapes.items():
        mnemonic_groups[sequence].append(identity)
    differences, group_rows = Counter(), []
    for sequence, identities in mnemonic_groups.items():
        if len(identities) < 2:
            continue
        flags = set(operand_differences[sequence])
        if len({shapes[identity][1] for identity in identities}) > 1:
            flags.add('register_operands_differ')
        if len({shapes[identity][2] for identity in identities}) > 1:
            flags.add('local_control_flow_differs')
        tier_counts = {tier: len({family_ids[(tier, membership[identity])] for identity in identities}) for tier in TIERS}
        differences.update(flags)
        group_rows.append({'mnemonic_signature': sequence, 'occurrences': len(identities), 'difference_flags': sorted(flags),
                           'template_counts': tier_counts, 'functions': [{'image': identity[0], 'address': identity[1], 'exact_body_unit': membership[identity]} for identity in identities]})
    if sha(exact_path.read_bytes()) != exact_sha:
        raise ValueError('Existing queue changed')
    report = {'schema': 'xport-lossless-operand-templates-v1', 'status': 'complete',
              'functions': len(records), 'instruction_words_reconstructed': word_count, 'exact_body_units': len(units),
              'tiers': summaries, 'mnemonic_group_difference_counts': dict(differences),
              'blocked_occurrences': dict(blockers), 'features_in_occurrences': dict(feature_counts),
              'method': {'fixed': 'Physical opcodes, all register fields, direct SP/GP adjustments and direct SP-relative memory slots, branch instruction-index targets, chunk layout, embedded bytes and COP/GTE options',
                         'addresses_calls': 'Normalize internal branch/jump encodings; parameterize indexed adjacent LUI address pairs, GP offsets and resolved direct call/tail destinations',
                         'scalar_constants': 'Additionally parameterize non-SP/non-GP arithmetic/logical immediates and immediate shift amounts',
                         'memory_offsets': 'Additionally parameterize non-stack memory field offsets; layout/access review required',
                         'blockers': 'Unresolved direct targets, non-return indirect transfers, observed/repurposed return address, external conditional branches, missing delay slots, unknown instructions, or independent boundary disagreement keep the existing exact-body unit separate',
                         'proof': 'Every indexed word matches original bytes and is exactly reconstructed from a fixed instruction skeleton plus original operand bits',
                         'scope': 'Count of lossless source-specialization templates, not a minimum number of semantically distinct algorithms or accepted complete native C bodies',
                         'remaining_review': 'Audit representative complete MIPS; preserve per-image data, all call bindings and fixed-width semantics; validate native ABI/types, aliasing, RA observability, embedded data and indirect dispatch before sharing C',
                         'queue_modified': False},
              'inputs': {'config_sha256': sha(config_path.read_bytes()), 'database_sha256': sha(database.read_bytes()),
                         'exact_body_manifest_sha256': exact_sha, 'originals': input_hashes},
              'artifacts': {'variants': {'path': variants_path.name, 'sha256': sha(variants_path.read_bytes())},
                            **{tier: {'path': tier + '-families.json', 'sha256': sha((out / (tier + '-families.json')).read_bytes())} for tier in TIERS}},
              'groups': group_rows}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: report[key] for key in ('functions', 'instruction_words_reconstructed', 'exact_body_units', 'tiers', 'mnemonic_group_difference_counts', 'blocked_occurrences')}), flush=True)


if __name__ == '__main__':
    main()
