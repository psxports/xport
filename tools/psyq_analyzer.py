"""Recognize PsyQ symbols from original bytes without Ghidra or IDA"""
import argparse
import collections
import csv
import json
import math
from pathlib import Path
import re
import sqlite3
import struct
from contextlib import closing

from psyq_catalog import canonical, connect, file_sha, literal_runs, make_mask, sha


def read_exe(path):
    raw = Path(path).read_bytes()
    if len(raw) < 0x800 or raw[:8] != b'PS-X EXE':
        raise ValueError('Expected complete PS-X EXE')
    base, size = struct.unpack_from('<II', raw, 0x18)
    if not size or base % 4 or size % 4 or base + size > 0x100000000 or len(raw) < 0x800 + size:
        raise ValueError('Invalid or truncated PS-X EXE payload')
    return raw, base, raw[0x800:0x800 + size]


def read_image(path, raw_layout=None):
    if raw_layout is None:
        return read_exe(path)
    if set(raw_layout) != {'base', 'offset', 'bytes'}:
        raise ValueError('Raw image requires explicit base, offset and bytes')
    base, offset, size = (raw_layout[k] for k in ('base', 'offset', 'bytes'))
    if any(type(v) is not int for v in (base, offset, size)):
        raise ValueError('Raw image layout must contain integers')
    raw = Path(path).read_bytes()
    if (raw[:8] == b'PS-X EXE' or base < 0 or base % 4 or offset < 0 or offset % 4
            or size <= 0 or size % 4 or offset + size > len(raw) or base + size > 0x100000000):
        raise ValueError('Invalid raw image layout or PS-X EXE supplied as raw')
    return raw, base, raw[offset:offset + size]


def load_objects(catalog):
    with closing(connect(catalog)) as db:
        objects = []
        for row in db.execute('''SELECT o.*, f.path AS library,f.sdk_id,s.version,s.sha256 AS archive_sha256
                FROM objects o JOIN files f ON f.id=o.file_id JOIN sdks s ON s.id=f.sdk_id ORDER BY o.id'''):
            obj = dict(row)
            for key in ('sections', 'symbols', 'imports', 'relocations'):
                obj[key] = json.loads(obj[key])
            if (len(obj['text']) != len(obj['mask']) or make_mask(obj['text'], obj['relocations']) != obj['mask']
                    or sha(obj['mask'] + bytes(a & m for a, m in zip(obj['text'], obj['mask']))) != obj['normalized_sha256']):
                raise ValueError('Catalog object mask/hash mismatch')
            obj['coff'] = []
            for coff in db.execute('''SELECT c.procedures,c.sha256,f.path FROM coff c
                    JOIN files f ON f.id=c.file_id WHERE c.object_id IN
                    (SELECT id FROM objects WHERE sha256=? AND file_id IN
                     (SELECT id FROM files WHERE sdk_id=?)) AND c.equivalent=1''', (obj['sha256'], obj['sdk_id'])):
                obj['coff'].extend(dict(p, coff_sha256=coff[1], coff_library=coff[2]) for p in json.loads(coff[0]))
            for signature in db.execute('SELECT * FROM signatures WHERE object_id=? ORDER BY id', (obj['id'],)):
                start, size = signature['start'], signature['bytes']
                unit = dict(obj, id=signature['id'], catalog_object_id=obj['id'], origin_offset=start,
                            full_text_bytes=len(obj['text']), signature_kind=signature['kind'],
                            text=obj['text'][start:start + size], mask=obj['mask'][start:start + size],
                            normalized_sha256=signature['normalized_sha256'],
                            relocations=[dict(r, offset=r['offset'] - start) for r in obj['relocations']
                                         if start <= r['offset'] < start + size])
                if (start < 0 or size < 0 or start + size > len(obj['text']) or
                        sha(unit['mask'] + bytes(a & m for a, m in zip(unit['text'], unit['mask']))) != unit['normalized_sha256']):
                    raise ValueError('Catalog signature extent/hash mismatch')
                runs = literal_runs(unit['text'], unit['mask'])
                unit['anchor'] = max(runs, key=lambda r: len(r[1]), default=(0, b''))
                unit['fixed_bytes'] = sum(v == 255 for v in unit['mask'])
                fixed = [a for a, m in zip(unit['text'], unit['mask']) if m == 255]
                unit['entropy'] = byte_entropy(fixed)
                unit['strong'] = (unit['fixed_bytes'] >= 64 and len(unit['anchor'][1]) >= 16
                                  and unit['entropy'] >= 2.0)
                objects.append(unit)
        metadata = dict(db.execute('SELECT key,value FROM metadata'))
        sources = [dict(row) for row in db.execute('SELECT version,path,sha256,provenance FROM sdks ORDER BY version')]
        for source in sources:
            source['provenance'] = json.loads(source['provenance'])
    return objects, dict(sha256=file_sha(catalog), metadata=metadata, sources=sources,
                         summary=json.loads(metadata['summary']))


def byte_entropy(data):
    counts = collections.Counter(data)
    return -sum((n / len(data)) * math.log2(n / len(data)) for n in counts.values()) if data else 0.0


def find_matches(objects, target, base):
    patterns = collections.defaultdict(list)
    for obj in objects:
        if obj['fixed_bytes'] >= 16 and len(obj['anchor'][1]) >= 4 and obj.get('entropy', 3) >= 1.5:
            patterns[obj['normalized_sha256']].append(obj)
    matches = []
    for variants in patterns.values():
        obj = variants[0]
        code, mask = obj['text'], obj['mask']
        anchor_at, anchor = obj['anchor']
        cursor = 0
        while True:
            hit = target.find(anchor, cursor)
            if hit < 0:
                break
            cursor = hit + 1
            offset = hit - anchor_at
            if offset < 0 or offset + len(code) > len(target) or (base + offset) % 4:
                continue
            actual = target[offset:offset + len(code)]
            if any((a & m) != (b & m) for a, b, m in zip(actual, code, mask)):
                continue
            for variant in variants:
                matches.append(dict(object=variant, address=base + offset, bytes=len(code),
                                    actual_sha256=sha(actual)))
    return matches, len(patterns)


def text_symbols(obj):
    start = obj.get('origin_offset', 0)
    symbols = [s for s in obj['symbols'] if s.get('section') == obj['text_section']
               and 'offset' in s and start <= s['offset'] < start + len(obj['text'])]
    symbols.extend(dict(s, kind='function') for s in obj['coff'] if start <= s['offset'] < start + len(obj['text']))
    return [dict(s, offset=s['offset'] - start) for s in symbols]


def linear_expression(expr, obj):
    """Keep unsupported expressions unresolved rather than guessing arithmetic"""
    op = expr[0]
    if op == 0:
        return {}, expr[1]
    if op in (4, 12):
        return {('section', str(expr[1])): 1}, 0
    if op == 22:
        sid = str(expr[1])
        if int(sid) == obj['text_section']:
            return {('section', sid): 1}, obj.get('full_text_bytes', len(obj['text']))
        return None
    if op == 2:
        symbol = next((s for s in obj['symbols'] if s.get('id') == expr[1]), None)
        if symbol and symbol.get('visibility') == 'bss':
            return {('common', symbol['name']): 1}, 0
        if symbol and 'offset' in symbol:
            return {('section', str(symbol['section'])): 1}, symbol['offset']
        name = obj['imports'].get(str(expr[1]))
        if name:
            return {('import', name): 1}, 0
        return None
    if op in (44, 46):
        left, right = linear_expression(expr[1], obj), linear_expression(expr[2], obj)
        if left is None or right is None:
            return None
        # lnk subtraction encodes the subtrahend before the minuend
        factor = -1 if op == 46 else 1
        terms = dict(right[0])
        for key, coefficient in left[0].items():
            terms[key] = terms.get(key, 0) + factor * coefficient
        return {key: value for key, value in terms.items() if value}, right[1] + factor * left[1]
    return None


def relocation_checks(match, target, base, known, gp=None):
    obj, address = match['object'], match['address']
    bindings = {('section', str(obj['text_section'])): address - obj.get('origin_offset', 0)}
    checks, conflicts = [], []
    groups = collections.defaultdict(list)
    for r in obj['relocations']:
        word = struct.unpack_from('<I', target, address - base + r['offset'])[0]
        groups[canonical(r['expression'])].append((r, word))
    for group in groups.values():
        expr = group[0][0]['expression']
        linear = linear_expression(expr, obj)
        for r, word in group:
            kind, site = r['kind'], address + r['offset']
            expected, actual, status = None, None, 'unresolved'
            if kind == 74:
                actual = ((site + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
            elif kind == 16:
                actual = word
            elif kind == 84:
                high = [w for rel, w in group if rel['kind'] == 82]
                if high:
                    low = word & 0xFFFF
                    signed_low = low - 0x10000 if low & 0x8000 else low
                    actual = (((high[0] & 0xFFFF) << 16) + signed_low) & 0xFFFFFFFF
            elif kind == 100 and gp is not None:
                low = word & 0xFFFF
                actual = (gp + (low - 0x10000 if low & 0x8000 else low)) & 0xFFFFFFFF
            if linear:
                terms, constant = linear
                unknown, value = [], constant
                for key, coefficient in terms.items():
                    if key in bindings:
                        value += coefficient * bindings[key]
                    elif key[0] == 'import' and len(known.get(key[1], set())) == 1:
                        value += coefficient * next(iter(known[key[1]]))
                    else:
                        unknown.append((key, coefficient))
                if not unknown:
                    expected = value & 0xFFFFFFFF
                    if kind == 82:
                        # PsyQ HI16 pairs with a signed LO16 immediate
                        status = 'verified' if (word & 0xFFFF) == ((expected + 0x8000) >> 16) & 0xFFFF else 'conflict'
                    elif kind == 84:
                        status = 'verified' if (word & 0xFFFF) == (expected & 0xFFFF) else 'conflict'
                    elif actual is not None:
                        status = 'verified' if actual == expected else 'conflict'
                elif actual is not None and len(unknown) == 1 and unknown[0][1] == 1:
                    key = unknown[0][0]
                    if kind != 74:
                        bindings[key] = (actual - value) & 0xFFFFFFFF
                        status = 'inferred_data_binding'
            check = dict(offset=r['offset'], kind=kind, expression=expr, actual=actual,
                         expected=expected, status=status)
            checks.append(check)
            if status == 'conflict':
                conflicts.append(check)
    # Recheck high halves after an inferred low-half binding
    for check in checks:
        if check['kind'] == 82 and check['status'] == 'unresolved':
            linear = linear_expression(check['expression'], obj)
            if linear and all(k in bindings for k in linear[0]):
                expected = (linear[1] + sum(bindings[k] * c for k, c in linear[0].items())) & 0xFFFFFFFF
                word = struct.unpack_from('<I', target, address - base + check['offset'])[0]
                check['expected'] = expected
                check['status'] = 'verified' if word & 0xFFFF == ((expected + 0x8000) >> 16) & 0xFFFF else 'conflict'
                if check['status'] == 'conflict':
                    conflicts.append(check)
    return checks, conflicts


def bios_stub(obj):
    if obj['relocations'] or len(obj['text']) != 16:
        return False
    first, jump, selector, delay = struct.unpack('<4I', obj['text'])
    vector = (first in (0x240A00A0, 0x240A00B0, 0x240A00C0) and jump == 0x01400008
              and selector & 0xFFFFFF00 == 0x24090000 and delay == 0)
    critical = (first in (0x24040001, 0x24040002) and jump == 0x0000000C
                and selector == 0x03E00008 and delay == 0)
    return vector or critical


def anchors(matches):
    names_at, strength = collections.defaultdict(set), {}
    for match in matches:
        obj = match['object']
        if obj['strong'] or bios_stub(obj):
            for symbol in text_symbols(obj):
                if symbol.get('visibility') == 'export':
                    address, score = match['address'] + symbol['offset'], obj['fixed_bytes']
                    if score > strength.get(address, -1):
                        names_at[address].clear()
                        strength[address] = score
                    if score == strength[address]:
                        names_at[address].add(symbol['name'])
    known = collections.defaultdict(set)
    for address, names in names_at.items():
        if len(names) == 1:
            known[next(iter(names))].add(address)
    return known


def refine_matches(matches, target, base, gp=None):
    rejected = []
    while True:
        known, survivors = anchors(matches), []
        for match in matches:
            checks, conflicts = relocation_checks(match, target, base, known, gp)
            if conflicts:
                rejected.append(dict(signature_id=match['object']['id'],
                                     object_id=match['object'].get('catalog_object_id', match['object']['id']), address=match['address'],
                                     reason='relocation_conflict', conflicts=conflicts))
            else:
                match['checks'] = checks
                survivors.append(match)
        if len(survivors) == len(matches):
            verify_short_call_imports(survivors, target, base, known)
            return survivors, rejected
        matches = survivors


def direct_call_import(match, check, target, base):
    if check['kind'] != 74:
        return None
    word = struct.unpack_from('<I', target, match['address'] - base + check['offset'])[0]
    linear = linear_expression(check['expression'], match['object'])
    if word >> 26 != 3 or linear is None or linear[1] != 0 or len(linear[0]) != 1:
        return None
    (kind, name), coefficient = next(iter(linear[0].items()))
    return name if kind == 'import' and coefficient == 1 else None


def anchor_reference(match, name, address):
    obj = match['object']
    return dict(name=name, address=address, signature_id=obj['id'], sdk_id=obj.get('sdk_id'),
                match_address=match['address'], bytes=match['bytes'],
                actual_sha256=match['actual_sha256'], fixed_bytes=obj['fixed_bytes'])


def verify_short_call_imports(matches, target, base, known):
    """Verify strong callers through bounded leaf evidence without promoting weak bodies"""
    roots, positions = {}, collections.defaultdict(set)
    for match in matches:
        obj = match['object']
        if obj['strong'] or bios_stub(obj):
            if not all(c['status'] in ('verified', 'inferred_data_binding') for c in match['checks']):
                continue
            for symbol in text_symbols(obj):
                address, name = match['address'] + symbol['offset'], symbol['name']
                if symbol.get('visibility') == 'export' and known.get(name) == {address}:
                    proof = anchor_reference(match, name, address)
                    if name not in roots or proof['fixed_bytes'] > roots[name]['fixed_bytes']:
                        roots[name] = proof
        else:
            for symbol in text_symbols(obj):
                if symbol.get('visibility') == 'export' and symbol['offset'] == 0:
                    positions[obj.get('sdk_id'), symbol['name']].add(match['address'])
    leaves = {}
    for match in matches:
        obj, checks = match['object'], match['checks']
        sdk = obj.get('sdk_id')
        if (obj['strong'] or bios_stub(obj) or not 16 <= obj['fixed_bytes'] < 64
                or type(sdk) is not int or sdk <= 0 or not checks):
            continue
        calls = []
        for check in checks:
            name = direct_call_import(match, check, target, base)
            proof = roots.get(name)
            if check['status'] != 'verified' or proof is None or check['actual'] != proof['address']:
                break
            calls.append(dict(offset=check['offset'], target=proof))
        else:
            for symbol in text_symbols(obj):
                name = symbol['name']
                key = sdk, name
                if (symbol.get('visibility') == 'export' and symbol['offset'] == 0
                        and positions[key] == {match['address']} and not known.get(name)):
                    leaves.setdefault(key, dict(kind='short_leaf_call',
                        leaf=anchor_reference(match, name, match['address']), calls=calls))
    # only positive JAL checks change; no new rejection, data binding or recursive anchor
    for match in matches:
        obj = match['object']
        if not obj['strong']:
            continue
        for check in match['checks']:
            if check['status'] != 'unresolved':
                continue
            name = direct_call_import(match, check, target, base)
            proof = leaves.get((obj.get('sdk_id'), name))
            if proof is not None and check['actual'] == proof['leaf']['address']:
                check.update(expected=check['actual'], status='verified', anchor_evidence=proof)


def summarize_symbols(matches):
    by_address = collections.defaultdict(list)
    for match in matches:
        obj = match['object']
        unresolved = [c for c in match['checks'] if c['status'] == 'unresolved']
        eligible = (obj['strong'] or bios_stub(obj)) and not unresolved
        for symbol in text_symbols(obj):
            if symbol['name'].startswith(('loc_', 'text_')):
                continue
            by_address[match['address'] + symbol['offset']].append(dict(
                name=symbol['name'], kind=symbol['kind'], visibility=symbol['visibility'],
                bytes=symbol.get('bytes'), signature_id=obj['id'],
                object_id=obj.get('catalog_object_id', obj['id']), version=obj['version'],
                library=obj['library'], object_name=obj['name'], object_address=match['address'],
                object_bytes=match['bytes'], object_sha256=match['actual_sha256'],
                signature_kind=obj.get('signature_kind', 'object'),
                sdk_text_offset=obj.get('origin_offset', 0),
                fixed_bytes=obj['fixed_bytes'],
                symbol_offset=symbol['offset'], eligible=eligible,
                unresolved_relocations=len(unresolved), coff_sha256=symbol.get('coff_sha256')))
    rows = []
    for address, evidence in sorted(by_address.items()):
        names = sorted({e['name'] for e in evidence})
        best = max((e['fixed_bytes'] for e in evidence if e['eligible']), default=0)
        eligible_names = sorted({e['name'] for e in evidence if e['eligible'] and e['fixed_bytes'] == best})
        # Weak incidental patterns cannot override a unique fully checked identity
        status = 'confirmed' if len(eligible_names) == 1 else 'ambiguous' if len(names) > 1 else 'candidate'
        rows.append(dict(address=address, name=eligible_names[0] if status == 'confirmed' else None,
                         names=names, status=status, versions=sorted({e['version'] for e in evidence}),
                         function_extent=sorted({e['bytes'] for e in evidence if e['bytes'] is not None}),
                         evidence=evidence))
    return rows


def scan(catalog, image, image_name=None, gp=None, raw_layout=None):
    objects, catalog_info = load_objects(catalog)
    return scan_loaded(objects, catalog_info, image, image_name, gp, raw_layout)


def scan_loaded(objects, catalog_info, image, image_name=None, gp=None, raw_layout=None):
    """Reuse one verified catalog for a batch of independent image scans"""
    raw, base, target = read_image(image, raw_layout)
    matches, patterns = find_matches(objects, target, base)
    initial = len(matches)
    matches, rejected = refine_matches(matches, target, base, gp)
    symbols = summarize_symbols(matches)
    counts = collections.Counter(s['status'] for s in symbols)
    full_objects = [m for m in matches if m['object'].get('signature_kind', 'object') == 'object']
    return dict(schema='xport-psyq-analysis-v2', image=image_name or Path(image).name,
                input=str(Path(image).resolve()), input_sha256=sha(raw), base=base, payload_bytes=len(target),
                gp=gp, input_layout=dict(raw_layout) if raw_layout is not None else None,
                analyzer_sha256=file_sha(__file__),
                catalog_reader_sha256=file_sha(Path(__file__).with_name('psyq_catalog.py')), catalog=catalog_info,
                summary=dict(patterns_scanned=patterns, initial_signature_candidates=initial,
                             signature_matches=len(matches), unique_signature_ranges=len({(m['address'], m['bytes']) for m in matches}),
                             object_matches=len(full_objects), unique_object_ranges=len({(m['address'], m['bytes']) for m in full_objects}),
                             rejected_relocation_matches=len(rejected), symbol_addresses=len(symbols),
                             confirmed=counts['confirmed'], ambiguous=counts['ambiguous'], candidate=counts['candidate']),
                matches=[dict(signature_id=m['object']['id'], version=m['object']['version'],
                              sdk_id=m['object'].get('sdk_id'),
                              library=m['object']['library'], name=m['object']['name'],
                              object_id=m['object'].get('catalog_object_id', m['object']['id']),
                              signature_kind=m['object'].get('signature_kind', 'object'),
                              sdk_text_offset=m['object'].get('origin_offset', 0),
                              address=m['address'], bytes=m['bytes'], actual_sha256=m['actual_sha256'],
                              strong=m['object']['strong'], bios=bios_stub(m['object']), checks=m['checks']) for m in matches],
                rejected=rejected, symbols=symbols,
                policy=dict(automatic_names='Unique strongest eligible name: >=64 fixed bytes, >=16-byte anchor, >=2.0 fixed-byte entropy or exact BIOS stub; no unresolved relocations',
                            short_call_imports='Positive direct JAL verification in strong callers only: same-source unique short leaf, only direct imports to baseline fully checked strong/BIOS anchors; no weak-body promotion, data anchors or recursive inference',
                            data_bindings='HI/LO destinations inferred and checked for consistency within each object',
                            functions='Original exported labels are symbols; only SDK procedure metadata supplies function extents',
                            coverage='Identity does not attest native ABI or implementation equivalence'))


def save_scan(report, output):
    output = Path(output)
    if output.exists():
        raise ValueError('Refusing to overwrite analysis directory')
    output.mkdir(parents=True)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    with (output / 'symbols.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.writer(stream)
        writer.writerow(('address', 'status', 'name', 'alternatives', 'versions'))
        for s in report['symbols']:
            writer.writerow((f"{s['address']:08X}", s['status'], s['name'] or '', '|'.join(s['names']), '|'.join(s['versions'])))
    db = sqlite3.connect(output / 'analysis.sqlite')
    with db:
        db.executescript('''PRAGMA user_version=2;
            CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
            CREATE TABLE symbols(image TEXT,address INTEGER,status TEXT,name TEXT,evidence TEXT,
                PRIMARY KEY(image,address));
            CREATE TABLE matches(image TEXT,address INTEGER,signature_id INTEGER,object_id INTEGER,evidence TEXT,
                PRIMARY KEY(image,address,signature_id));
            CREATE TABLE entries(image TEXT,address INTEGER,bytes INTEGER,original_sha256 TEXT,
                library_status TEXT,name TEXT,evidence TEXT,PRIMARY KEY(image,address));''')
        db.executemany('INSERT INTO metadata VALUES (?,?)', [(key, canonical(report[key]))
                       for key in ('schema', 'input', 'input_sha256', 'catalog', 'analyzer_sha256',
                                   'catalog_reader_sha256', 'summary', 'policy')])
        db.executemany('INSERT INTO symbols VALUES (?,?,?,?,?)', [(report['image'], s['address'], s['status'], s['name'], canonical(s))
                       for s in report['symbols']])
        db.executemany('INSERT INTO matches VALUES (?,?,?,?,?)', [(report['image'], m['address'], m['signature_id'], m['object_id'], canonical(m))
                       for m in report['matches']])
        db.executemany('INSERT INTO entries VALUES (?,?,?,?,?,?,?)', [(report['image'], e['address'], e['bytes'],
                       e['original_sha256'], e['library_status'], e['name'], canonical(e)) for e in report.get('entries', [])])
    db.close()
    export_scripts(report, output)


def read_inventory(report, inventory):
    raw, base, target = read_image(report['input'], report.get('input_layout'))
    if sha(raw) != report['input_sha256']:
        raise ValueError('Image changed since analysis')
    listings = sorted(Path(inventory).glob('*.mips.txt'))
    if not listings:
        raise ValueError('Expected original .mips.txt inventory')
    rows = []
    for listing in listings:
        text = listing.read_text(encoding='utf-8-sig')
        identity = re.search(r'image=([^\s]+)', text)
        if not identity or identity[1] != report['image']:
            raise ValueError('Listing image identity mismatch: ' + str(listing))
        header = re.search(r'entry=([0-9A-Fa-f]{8}) SHA256=([0-9a-f]{64})', text)
        if not header:
            raise ValueError('Missing raw identity: ' + str(listing))
        address = int(header[1], 16)
        words = re.findall(r'^([0-9A-Fa-f]{8}) ([0-9a-fA-F]{8}) [0-9A-Fa-f]{8} ', text, re.M)
        body = b''.join(bytes.fromhex(w) for _, w in words)
        if not body or sha(body) != header[2]:
            raise ValueError('Listing hash mismatch or empty body: ' + str(listing))
        addresses = [int(a, 16) for a, _ in words]
        if address not in addresses or len(set(addresses)) != len(addresses) or any(a % 4 for a in addresses):
            raise ValueError('Invalid or overlapping instruction addresses: ' + str(listing))
        if any(a < base or a + 4 > base + len(target) for a in addresses):
            continue
        if any(bytes.fromhex(w) != target[a-base:a-base+4] for a, (_, w) in zip(addresses, words)):
            raise ValueError('Listing bytes differ from original image: ' + str(listing))
        chunks = []
        for at in addresses:
            if chunks and at == chunks[-1][1]:
                chunks[-1][1] += 4
            else:
                chunks.append([at, at + 4])
        row = dict(address=address, bytes=len(body), original_sha256=header[2])
        if chunks != [[address, address + len(body)]]:
            row['chunks'] = chunks
        rows.append(row)
    return rows


def bind_inventory(report, inventory):
    symbols = {s['address']: s for s in report['symbols']}
    entries = read_inventory(report, inventory)
    for entry in entries:
        address = entry['address']
        chunks = entry.get('chunks', [[address, address + entry['bytes']]])
        covering = [m for m in report['matches']
                    if all(m['address'] <= start < end <= m['address'] + m['bytes'] for start, end in chunks)]
        overlapping = [m for m in report['matches']
                       if any(m['address'] < end and start < m['address'] + m['bytes'] for start, end in chunks)]
        eligible = [m for m in covering if (m['strong'] or m.get('bios', False))
                    and all(c['status'] != 'unresolved' for c in m['checks'])]
        symbol = symbols.get(address, {})
        entry.update(name=symbol.get('name'), names=symbol.get('names', []),
                     library_status='confirmed' if eligible else 'candidate' if overlapping else 'unrecognized',
                     boundary_status='contained' if covering else 'sdk_overlap' if overlapping else 'unrecognized',
                     signatures=[m['signature_id'] for m in eligible or covering or overlapping])
    report['entries'] = entries
    report['summary']['original_entries_verified'] = len(entries)
    report['summary']['confirmed_library_entries'] = sum(e['library_status'] == 'confirmed' for e in entries)
    return entries


def export_scripts(report, output):
    rows = []
    for symbol in report['symbols']:
        if symbol['status'] != 'confirmed':
            continue
        evidence = max((e for e in symbol['evidence'] if e['eligible'] and e['name'] == symbol['name']),
                       key=lambda e: e['fixed_bytes'])
        rows.append([symbol['address'], symbol['name'], evidence['object_address'],
                     evidence['object_bytes'], evidence['object_sha256']])
    ida = '''# Apply verified PsyQ names without replacing user-defined names
import hashlib
import ida_bytes
import ida_name
ROWS = %s
for address, name, start, size, expected in ROWS:
    raw = ida_bytes.get_bytes(start, size)
    if raw is None or hashlib.sha256(raw).hexdigest() != expected:
        raise RuntimeError('PsyQ original-byte mismatch at %%08X' %% start)
for address, name, start, size, expected in ROWS:
    flags = ida_bytes.get_full_flags(address)
    if not ida_bytes.has_user_name(flags):
        ida_name.set_name(address, name, ida_name.SN_CHECK | ida_name.SN_AUTO)
''' % repr(rows)
    (output / 'apply_ida.py').write_text(ida, encoding='utf-8')
    literals = ',\n'.join('new String[]{' + ','.join(json.dumps(str(v)) for v in row) + '}' for row in rows)
    ghidra = '''// Apply verified PsyQ labels while preserving existing imported and user names
// @category PSX
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.SourceType;
import java.security.MessageDigest;
import java.util.HexFormat;
public class ApplyPsyqNames extends GhidraScript {
 public void run() throws Exception {
  String[][] rows = new String[][]{%s};
  for (String[] row : rows) {
   byte[] raw = getBytes(toAddr(Long.parseLong(row[2])), Integer.parseInt(row[3]));
   String hash = HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(raw));
   if (!hash.equals(row[4])) throw new Exception("PsyQ original-byte mismatch at " + row[2]);
  }
  for (String[] row : rows) {
   var address = toAddr(Long.parseLong(row[0]));
   boolean preserve = false;
   for (var symbol : currentProgram.getSymbolTable().getSymbols(address))
    if (symbol.getSource() == SourceType.USER_DEFINED || symbol.getSource() == SourceType.IMPORTED) preserve = true;
   if (!preserve) createLabel(address, row[1], true, SourceType.ANALYSIS);
  }
 }
}
''' % literals
    (output / 'ApplyPsyqNames.java').write_text(ghidra, encoding='utf-8')


def ghidra_symbols(report, target, base):
    """Validate detector patterns against the same original payload before comparison"""
    if report.get('base') != base:
        raise ValueError('Ghidra base differs from original image')
    symbols, ranges = collections.defaultdict(set), set()
    for match in report['matches']:
        pattern, mask = bytes.fromhex(match['pattern']), bytes.fromhex(match['mask'])
        at = match['address'] - base
        if not pattern or len(pattern) != len(mask) or at < 0 or at + len(pattern) > len(target):
            raise ValueError('Ghidra match outside original payload')
        if any((a & m) != (p & m) for a, p, m in zip(target[at:at + len(pattern)], pattern, mask)):
            raise ValueError('Ghidra pattern does not match original bytes')
        ranges.add((match['address'], len(pattern)))
        for label in match['labels']:
            name, offset = label['name'], label['offset']
            if name and not name.startswith(('loc_', 'text_')) and 0 <= offset < len(pattern):
                symbols[match['address'] + offset].add(name)
    return symbols, ranges


def compare(report, ghidra_path, inventory=None):
    raw, base, target = read_image(report['input'], report.get('input_layout'))
    if sha(raw) != report['input_sha256']:
        raise ValueError('Image changed since analysis')
    ghidra_path = Path(ghidra_path)
    original = json.loads(ghidra_path.read_text(encoding='utf-8-sig'))
    if original.get('image') != report['image']:
        raise ValueError('Ghidra image differs from selected image')
    old, ranges = ghidra_symbols(original, target, base)
    new = {s['address']: s for s in report['symbols']}
    confirmed = {a for a, s in new.items() if s['status'] == 'confirmed'}
    common = set(old) & set(new)
    agreements, conflicts, confirmed_conflicts = [], [], []
    for address in sorted(common):
        row = dict(address=address, ghidra_names=sorted(old[address]),
                   analyzer_names=new[address]['names'], status=new[address]['status'])
        (agreements if old[address] & set(new[address]['names']) else conflicts).append(row)
        if new[address]['status'] == 'confirmed' and new[address]['name'] not in old[address]:
            confirmed_conflicts.append(dict(row, assigned_name=new[address]['name']))
    entries = {e['address'] for e in (read_inventory(report, inventory) if inventory else [])}
    return dict(schema='xport-psyq-comparison-v1', image=report['image'], image_sha256=sha(raw),
                ghidra=dict(path=str(ghidra_path.resolve()), sha256=file_sha(ghidra_path),
                            detected_version=original.get('detected_version'), raw_matches=len(original['matches']),
                            unique_object_ranges=len(ranges), named_addresses=len(old)),
                analyzer=report['summary'],
                summary=dict(common_named_addresses=len(common), compatible_names=len(agreements),
                             confirmed_name_differences=len(confirmed_conflicts),
                             differing_names=len(conflicts), analyzer_only=len(set(new) - set(old)),
                             analyzer_only_confirmed=len(confirmed - set(old)), ghidra_only=len(set(old) - set(new)),
                             original_entries_verified=len(entries), ghidra_named_entries=len(entries & set(old)),
                             analyzer_named_entries=len(entries & set(new)), analyzer_confirmed_entries=len(entries & confirmed)),
                agreements=agreements, differences=conflicts, confirmed_name_differences=confirmed_conflicts,
                analyzer_only=[new[a] for a in sorted(set(new) - set(old))],
                ghidra_only=[dict(address=a, names=sorted(old[a])) for a in sorted(set(old) - set(new))],
                notes=['Counts deduplicate SDK versions and distinguish symbol addresses from functions',
                       'Ghidra is a comparison source, not ground truth',
                       'Technical text_/loc_ labels excluded on both sides',
                       'Only immutable loaded-image entries enter function counts; overlays remain separate'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--image-name')
    parser.add_argument('--gp', type=lambda x: int(x, 0))
    parser.add_argument('--raw-base', type=lambda x: int(x, 0))
    parser.add_argument('--raw-offset', type=lambda x: int(x, 0))
    parser.add_argument('--raw-size', type=lambda x: int(x, 0))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compare-ghidra', type=Path)
    parser.add_argument('--inventory', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Refusing to overwrite analysis directory')
    layout = None
    if any(v is not None for v in (args.raw_base, args.raw_offset, args.raw_size)):
        if any(v is None for v in (args.raw_base, args.raw_offset, args.raw_size)):
            parser.error('Raw input requires --raw-base, --raw-offset and --raw-size together')
        layout = dict(base=args.raw_base, offset=args.raw_offset, bytes=args.raw_size)
    report = scan(args.catalog, args.image, args.image_name, args.gp, layout)
    if args.inventory:
        bind_inventory(report, args.inventory)
    comparison = compare(report, args.compare_ghidra, args.inventory) if args.compare_ghidra else None
    save_scan(report, args.output)
    if comparison:
        (args.output / 'comparison.json').write_text(json.dumps(comparison, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(output=str(args.output), **report['summary'],
                          comparison=comparison['summary'] if comparison else None)))


if __name__ == '__main__':
    main()
