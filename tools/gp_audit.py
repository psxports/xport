"""Audit translated C addresses against gp-relative MIPS memory operands"""
import argparse
import difflib
import json
from pathlib import Path
import re
import sqlite3

from xport_project import artifact_path, load_project


GP_MEMORY = re.compile(r'\b(?:lb|lbu|lh|lhu|lw|lwl|lwr|sb|sh|sw|swl|swr)\s+\$[A-Za-z0-9]+,\s*(-?0x[0-9A-Fa-f]+|-?\d+)\(\$gp\)')
MEMORY_LITERAL = re.compile(r'\b(?:byte_|word_|dword_)\(\s*(0x[0-9A-Fa-f]+)u?')
RAW_INSTRUCTIONS = re.compile(r'^\s*[0-9A-Fa-f]{8}\s+((?:[0-9A-Fa-f]{2}\s+)+)')
GP_MEMORY_OPCODES = {32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46,
                     48, 49, 50, 51, 56, 57, 58, 59}


def listing_gp_offsets(listing):
    offsets = set()
    for line in listing.splitlines():
        raw = RAW_INSTRUCTIONS.match(line)
        if raw:
            data = bytes.fromhex(raw.group(1))
            if len(data) % 4:
                raise ValueError('Listing instruction bytes are not complete MIPS words')
            for index in range(0, len(data), 4):
                word = int.from_bytes(data[index:index + 4], 'little')
                if word >> 26 in GP_MEMORY_OPCODES and (word >> 21) & 31 == 28:
                    offsets.add(signed_offset(hex(word & 0xffff)))
        else:
            offsets.update(signed_offset(match.group(1)) for match in GP_MEMORY.finditer(line))
    return sorted(offsets)



def signed_offset(text):
    value = int(text, 0)
    if value >= 0x8000:
        value -= 0x10000
    if not -0x8000 <= value <= 0x7fff:
        raise ValueError('GP displacement is outside signed 16-bit range')
    return value


def literal_count(source, address):
    return len(re.findall(rf'0x0*{address:X}u?\b', source, re.IGNORECASE))


def audit(root, config, suspect_bases):
    database = root/config['paths']['analysis_database']
    db = sqlite3.connect(database)
    rows = db.execute("""SELECT image,address,source_path,start_offset,end_offset
                         FROM implementations WHERE mapping_status='mapped' ORDER BY image,address""").fetchall()
    profiles = root/config.get('paths', {}).get('ida_configs', 'tools/ida')
    profile_cache = {}
    source_cache = {}
    functions = []
    candidate_bases = {}
    inferred_bases = {}
    totals = {'mapped': len(rows), 'with_gp_memory': 0, 'operands': 0, 'wrong_literals': 0,
              'unresolved_operands': 0, 'ambiguous_literals': 0}
    for image, address, source_path, start, end in rows:
        listing_path = root/config['paths'].get('ida_exports', 'orig/images')/image/'functions'/f'{address:08X}.lst'
        if not listing_path.is_file():
            continue
        listing = listing_path.read_text(encoding='utf-8', errors='replace')
        offsets = listing_gp_offsets(listing)
        if not offsets:
            continue
        totals['with_gp_memory'] += 1
        totals['operands'] += len(offsets)
        if image not in profile_cache:
            profile_cache[image] = json.loads((profiles/(image+'.json')).read_text(encoding='utf-8'))
        gp = profile_cache[image].get('gp')
        if type(gp) is not int or not 0 < gp <= 0xffffffff:
            raise ValueError('Missing reviewed runtime GP for '+image)
        if source_path not in source_cache:
            source_cache[source_path] = (root/source_path).read_bytes()
        source = source_cache[source_path][start:end].decode('utf-8', errors='replace')
        expected_addresses = {(gp+offset) & 0xffffffff for offset in offsets}
        memory_literals = [int(match.group(1), 16) for match in MEMORY_LITERAL.finditer(source)]
        listing_absolutes = {int(value, 16) for value in re.findall(r'\b0x(8[0-9A-Fa-f]{7})\b', listing)}
        for offset in offsets:
            for literal in memory_literals:
                base = (literal-offset) & 0xffffffff
                if 0x80000000 <= base < 0x80200000:
                    candidate_bases[base] = candidate_bases.get(base, 0)+1
        operands = []
        for offset in offsets:
            expected = (gp+offset) & 0xffffffff
            expected_count = literal_count(source, expected)
            wrong = []
            for base in suspect_bases:
                candidate = (base+offset) & 0xffffffff
                count = literal_count(source, candidate)
                if count and candidate not in expected_addresses:
                    direct = bool(re.search(rf'(?:0x|_){candidate:08X}\b', listing, re.IGNORECASE))
                    wrong.append({'base': base, 'address': candidate, 'occurrences': count,
                                  'also_direct_absolute_in_listing': direct})
                    if direct:
                        totals['ambiguous_literals'] += count
                    else:
                        totals['wrong_literals'] += count
            if not expected_count and not wrong:
                totals['unresolved_operands'] += 1
            operands.append({'offset': offset, 'expected_address': expected,
                             'expected_occurrences': expected_count, 'wrong': wrong})
        unresolved = [row['offset'] for row in operands if not row['expected_occurrences'] and not row['wrong']]
        inferred = {}
        for offset in unresolved:
            for literal in set(memory_literals)-expected_addresses-listing_absolutes:
                base = (literal-offset) & 0xffffffff
                if 0x80000000 <= base < 0x80200000:
                    inferred.setdefault(base, set()).add(offset)
        inferred = sorted(inferred.items(), key=lambda item: (-len(item[1]), item[0]))
        inferred_rows = [{'base': base, 'matched_offsets': len(found)} for base, found in inferred[:8]]
        if inferred and len(inferred[0][1]) >= 2 and (len(inferred) == 1 or len(inferred[0][1]) > len(inferred[1][1])):
            base = inferred[0][0]
            entry = inferred_bases.setdefault(base, {'functions': 0, 'matched_offsets': 0})
            entry['functions'] += 1;entry['matched_offsets'] += len(inferred[0][1])
        functions.append({'image': image, 'address': address, 'source': source_path,
                          'start_offset': start, 'end_offset': end,
                          'gp': gp, 'operands': operands, 'inferred_base_candidates': inferred_rows})
    ranked = sorted(candidate_bases.items(), key=lambda item: (-item[1], item[0]))[:32]
    inferred_ranked = sorted(inferred_bases.items(), key=lambda item: (-item[1]['matched_offsets'], item[0]))
    return {'schema': 1, 'suspect_bases': suspect_bases, 'totals': totals,
            'candidate_bases': [{'base': base, 'pair_count': count} for base, count in ranked],
            'inferred_bases': [{'base': base, **counts} for base, counts in inferred_ranked],
            'functions': functions}


def correction_patch(root, result):
    edits = {}
    for function in result['functions']:
        mapping = {}
        for operand in function['operands']:
            for wrong in operand['wrong']:
                if wrong['also_direct_absolute_in_listing']:
                    continue
                mapping.setdefault(wrong['address'], set()).add(operand['expected_address'])
        mapping = {wrong: values for wrong, values in mapping.items() if len(values) == 1}
        if mapping:
            edits.setdefault(function['source'], []).append((function['start_offset'], function['end_offset'],
                {wrong: next(iter(expected)) for wrong, expected in mapping.items()}))
    chunks = ['*** Begin Patch']
    for relative, functions in sorted(edits.items()):
        path = root/relative
        old = path.read_text(encoding='utf-8')
        new = old
        for start, end, mapping in sorted(functions, reverse=True):
            body = new[start:end]
            for wrong, expected in mapping.items():
                pattern = re.compile(rf'(\b(?:byte_|word_|dword_)\(\s*)0x{wrong:08X}(u?\b)', re.IGNORECASE)
                body = pattern.sub(rf'\g<1>0x{expected:08X}\g<2>', body)
            new = new[:start]+body+new[end:]
        if new == old:
            continue
        diff = list(difflib.unified_diff(old.splitlines(), new.splitlines(), lineterm=''))
        chunks.append('*** Update File: '+str(path))
        chunks.extend('@@' if line.startswith('@@ ') else line for line in diff[2:])
    chunks.append('*** End Patch')
    return '\n'.join(chunks)+'\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suspect-base', action='append', default=[], type=lambda value: int(value, 0))
    parser.add_argument('--report', type=Path)
    parser.add_argument('--emit-patch', action='store_true')
    parser.add_argument('--strict', action='store_true')
    args = parser.parse_args()
    root, config = load_project()
    result = audit(root, config, args.suspect_base)
    if args.emit_patch:
        print(correction_patch(root, result), end='')
        return 0
    if args.report:
        artifact_path(args.report).write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'totals': result['totals'], 'report': str(args.report) if args.report else None}))
    problems = result['totals']['wrong_literals']+result['totals']['unresolved_operands']
    return 1 if args.strict and problems else 0


if __name__ == '__main__':
    raise SystemExit(main())
