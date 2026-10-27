"""Compare complete indexed mnemonic sequences without operands or queue mutations"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import sqlite3


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def mnemonic(disassembly, word):
    name = disassembly.split()[0].lower()
    if name != '.word':
        return name
    instruction = int(word, 16)
    if instruction >> 26 == 0x12:
        transfer = (instruction >> 21) & 31
        if transfer in (0, 2, 4, 6):
            return {0: 'mfc2', 2: 'cfc2', 4: 'mtc2', 6: 'ctc2'}[transfer]
        if transfer >= 16:
            return 'gte_command_' + format(instruction & 63, '02x')
    return None


def compare(root, output):
    config_path = root / 'xport-project.json'
    config = json.loads(config_path.read_text(encoding='utf-8-sig'))
    database = root / config['paths']['analysis_database']
    manifest = root / 'status/decompilation-units.json'
    before = digest(manifest) if manifest.exists() else None
    groups = defaultdict(list)
    recovered, count = Counter(), 0
    incomplete = []
    with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as db:
        functions = {(image, address): {'image': image, 'address': address, 'name': name, 'original_instruction_sha256': sha}
                     for image, address, name, sha in db.execute('SELECT image,address,name,sha256 FROM functions')}
        units = {(image, address): unit for image, address, unit in db.execute('SELECT image,address,unit_id FROM function_work_unit_members')}
        current, names, unknown = None, [], []

        def save():
            if current is not None:
                member = functions.pop(current)
                member['exact_body_unit'] = units[current]
                if unknown:
                    incomplete.append({'function': member, 'undecoded_words': list(unknown), 'mnemonics': list(names)})
                else:
                    groups[tuple(names)].append(member)

        for image, function, address, word, disassembly in db.execute('SELECT image,function,address,word,disassembly FROM instructions ORDER BY image,function,address'):
            identity = (image, function)
            if current != identity:
                save()
                current, names, unknown = identity, [], []
            name = mnemonic(disassembly, word)
            names.append(name)
            if name is None:
                unknown.append({'address': address, 'word': word})
            elif disassembly.startswith('.word'):
                recovered[name] += 1
            count += 1
        save()
        if functions:
            raise ValueError('Functions without indexed instruction evidence')
    rows = []
    for sequence, members in groups.items():
        signature = hashlib.sha256(('\n'.join(sequence) + '\n').encode()).hexdigest()
        rows.append({'id': 'MNEMONIC-' + signature, 'instruction_count': len(sequence),
                     'occurrence_count': len(members), 'image_count': len({row['image'] for row in members}),
                     'exact_body_units': len({row['exact_body_unit'] for row in members}),
                     'distinct_instruction_bytes': len({row['original_instruction_sha256'] for row in members}),
                     'mnemonics': list(sequence), 'occurrences': members})
    rows.sort(key=lambda row: (-row['occurrence_count'], -row['instruction_count'], row['id']))
    repeated = [row for row in rows if row['occurrence_count'] > 1]
    cross_body = [row for row in rows if row['exact_body_units'] > 1]
    summary = {'functions': sum(row['occurrence_count'] for row in rows) + len(incomplete), 'instruction_words': count,
               'fully_decoded_functions': sum(row['occurrence_count'] for row in rows),
               'incomplete_decode_functions': len(incomplete),
               'unique_mnemonic_sequences': len(rows), 'repeated_sequence_groups': len(repeated),
               'functions_in_repeated_groups': sum(row['occurrence_count'] for row in repeated),
               'singleton_functions': sum(row['occurrence_count'] == 1 for row in rows),
               'cross_exact_body_groups': len(cross_body),
               'functions_in_cross_exact_body_groups': sum(row['occurrence_count'] for row in cross_body),
               'fully_decoded_exact_body_units': len({member['exact_body_unit'] for row in rows for member in row['occurrences']}),
               'total_exact_body_units': len(set(units.values())),
               'additional_body_units_grouped_by_mnemonics': sum(row['exact_body_units'] - 1 for row in rows),
               'recovered_word_mnemonics': dict(recovered)}
    if before != (digest(manifest) if manifest.exists() else None):
        raise ValueError('Exact-body queue manifest changed during comparison')
    document = {'schema': 'xport-mnemonic-sequences-v1', 'summary': summary,
                'method': {'sequence_order': 'Increasing original instruction address within each image-qualified function; all indexed chunks included',
                           'operands': 'All registers, immediates, addresses and GTE command options ignored',
                           'mnemonics': 'Canonical indexed names retained, including aliases, nop and delay-slot instructions; no alias folding or instruction removal',
                           'word_fallback': 'COP2 transfers identified by transfer selector; GTE commands identified by low six command bits; functions containing undecoded words are reported separately and excluded from matching',
                           'interpretation': 'Structural candidates only; does not prove equivalent control flow, data access, callee identity or semantics',
                           'queue_modified': False},
                'inputs': {'database': database.relative_to(root).as_posix(), 'database_sha256': digest(database),
                           'config_sha256': digest(config_path), 'exact_body_manifest_sha256': before}, 'groups': rows,
                'incomplete_decode_functions': incomplete}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('status/mnemonic-comparison.json'))
    args = parser.parse_args()
    root = args.project.resolve()
    output = args.output if args.output.is_absolute() else root / args.output
    compare(root, output)


if __name__ == '__main__':
    main()
