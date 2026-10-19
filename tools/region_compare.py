"""Prepare isolated PS-X IDA exports and compare regional code/data without semantic review."""
import argparse
from collections import Counter, defaultdict
import csv
import difflib
import hashlib
import html
import json
from pathlib import Path
import re
import struct
import subprocess
from mips_compare import MipsSimilarity
from mips_equiv import verify as verify_equivalence


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')


def signed16(word):
    return (word & 65535) - (65536 if word & 32768 else 0)


def ram(address):
    return 0x80000000 <= address < 0x80200000


def prepare_image(source, output, image):
    from startup import runtime_gp_initializers, validate_gp_config
    source = Path(source).resolve()
    raw = source.read_bytes()
    if raw[:8] != b'PS-X EXE':
        raise ValueError('Expected an unpacked PS-X EXE: '+str(source))
    entry, header_gp, base, size = struct.unpack_from('<4I', raw, 16)
    if size > len(raw)-2048 or not base <= entry < base+size:
        raise ValueError('Invalid PS-X EXE load span/entry')
    initializers = runtime_gp_initializers(raw[2048:2048+size], base)
    values = {row['value'] for row in initializers}
    if len(values) != 1:
        raise ValueError('GP requires manual review: '+str(initializers))
    gp = values.pop()
    if header_gp not in (0, gp):
        raise ValueError('Header and runtime GP disagree')
    words = struct.unpack_from('<9I', raw, 2048+entry-base)
    expected = (0x3c02, 0x2442, 0x3c03, 0x2463)
    if tuple(w >> 16 for w in words[:4]) != expected or words[4:] != (0xac400000, 0x24420004, 0x0043082b, 0x1420fffc, 0):
        raise ValueError('Unrecognized BSS zero-loop; review mapping before analysis')
    bss_start = ((words[0] & 65535) << 16)+signed16(words[1])
    bss_end = ((words[2] & 65535) << 16)+signed16(words[3])
    if not base < bss_start <= base+size < bss_end <= 0x80200000:
        raise ValueError('Unexpected BSS/load relationship')
    config = dict(image=image, entry=entry, gp=gp,
                  gp_evidence=dict(source='runtime_initializer', header_value=header_gp, runtime_initializers=initializers),
                  bss_evidence=dict(start=bss_start, end=bss_end, initializer=entry, words=[hex(w) for w in words]),
                  segments=[dict(start=base, end=base+size, name=image, **{'class': 'CODE'}),
                            dict(start=base+size, end=bss_end, name='.bss', **{'class': 'BSS'})],
                  loads=[dict(path=str(source), base=base, offset=2048)], seeds=[entry],
                  code_ranges=[[base, base+size]], rebuild=True, comparison_features=True, output=str(output))
    validate_gp_config(source.parent, config)
    return config


def analyze(pal, ntsc, output, ida):
    output = Path(output).resolve()
    exporter = Path(__file__).parent/'ida/ida_analyze_export.py'
    previous = read(output/'inputs.json') if (output/'inputs.json').exists() else []
    previous_hashes = {(r['region'], r['image']): r['sha256'] for r in previous}
    inputs = []
    for region, folder in [('pal', Path(pal)), ('ntsc-u', Path(ntsc))]:
        boot = read_boot(folder)
        for source in [folder/'MAIN.EXE', folder/boot]:
            source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
            if previous_hashes.get((region, source.name), source_hash) != source_hash:
                raise ValueError('Input changed; select a new output directory')
            destination = output/region/source.name
            config = prepare_image(source, destination, source.name)
            config_path = output/(region+'-'+source.name+'.json')
            database = output/(region+'-'+source.name+'.idb')
            write(config_path, config)
            inputs.append(dict(region=region, image=source.name, path=str(source.resolve()),
                               sha256=source_hash, gp=hex(config['gp']),
                               config=str(config_path), database=str(database)))
    write(output/'inputs.json', inputs)
    for row in inputs:
        report = output/row['region']/row['image']/'export-report.json'
        if report.exists() and report.with_name('comparison-features.json').exists():
            old = read(report)
            current = read(row['config'])
            if (old['gp'] != int(row['gp'], 16) or current['loads'] != old['config']['loads']
                    or current['segments'] != old['config']['segments']):
                raise ValueError('Existing export belongs to a different input/configuration')
            continue
        command = [str(ida), '-A', '-L'+str(output/(row['region']+'-'+row['image']+'.log')),
                   '-S"'+str(exporter)+'" "'+row['config']+'"', '-c', '-pmipsl',
                   '-o'+row['database'], row['path']]
        write(output/(row['region']+'-'+row['image']+'-command.json'), command)
        print('IDA:', row['region'], row['image'], flush=True)
        result = subprocess.run(command, capture_output=True, text=True)
        (output/(row['region']+'-'+row['image']+'-process.log')).write_text(result.stdout+result.stderr)
        if result.returncode:
            raise RuntimeError('IDA failed; inspect regional analysis log')


def read_boot(folder):
    text = (Path(folder)/'SYSTEM.CNF').read_text()
    match = re.search(r'BOOT\s*=\s*cdrom:\\([^;\r\n]+)', text, re.I)
    if not match or Path(match[1]).name != match[1]:
        raise ValueError('Unsupported SYSTEM.CNF boot path')
    return match[1]


class Image:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.report = read(self.folder/'export-report.json')
        verification = self.report.get('gp_verification', {})
        if not self.report['gp'] or verification.get('mismatches') != 0:
            raise ValueError('Export lacks successful GP reference verification')
        self.gp = self.report['gp']
        load = self.report['config']['loads'][0]
        self.source = Path(load['path'])
        self.raw = self.source.read_bytes()
        self.base = load['base']
        self.offset = load.get('offset', 0)
        self.functions = {r['address']: r for r in read(self.folder/'functions.json')}
        self.extra = read(self.folder/'comparison-features.json')
        self.blocks = {r['address']: r['blocks'] for r in self.extra['functions']}
        self.data = {r['address']: r for r in self.extra['data']}
        self.strings = {r['address']: r['text'] for r in read(self.folder/'strings.json')}
        self.calls, self.callers = defaultdict(set), defaultdict(set)
        self.refs = defaultdict(dict)
        for row in read(self.folder/'callgraph.json'):
            if row['target'] in self.functions:
                self.calls[row['source']].add(row['target'])
                self.callers[row['target']].add(row['source'])
        for row in read(self.folder/'data-xrefs.json'):
            self.refs[row['source']][row['site']] = row['target']
        self.features = {a: self.feature(row) for a, row in self.functions.items()}

    def bytes(self, address, size):
        offset = self.offset+address-self.base
        if offset < self.offset or offset+size > len(self.raw):
            return None
        return self.raw[offset:offset+size]

    def feature(self, row):
        pairs = {}
        for item in row['instructions']:
            raw = bytes.fromhex(item['bytes'])
            if len(raw) % 4:
                raise ValueError('Non-MIPS32 instruction span')
            if raw != self.bytes(item['address'], len(raw)):
                raise ValueError('Exported instructions differ from original binary')
            for n in range(0, len(raw), 4):
                pairs[item['address']+n] = struct.unpack_from('<I', raw, n)[0]
        addresses = sorted(pairs)
        words = [pairs[a] for a in addresses]
        index = {a: i for i, a in enumerate(addresses)}
        tokens = [hex(w) for w in words]
        operands = {}
        upper = {}
        flow = []
        for i, (address, w) in enumerate(zip(addresses, words)):
            op, rs, rt, rd, fn = w >> 26, w >> 21 & 31, w >> 16 & 31, w >> 11 & 31, w & 63
            flow.append((op, fn if op == 0 else 0))
            if op in (2, 3):
                target = ((address+4) & 0xf0000000) | ((w & 0x3ffffff) << 2)
                if target in index:
                    tokens[i] = (op, 'local', index[target])
                else:
                    tokens[i] = (op, 'call' if op == 3 else 'tail')
                    operands[i] = ('code', target)
            elif op in (1, 4, 5, 6, 7) or op == 0x11 and rs == 8:
                target = address+4+signed16(w)*4
                tokens[i] = (w & 0xffff0000, 'branch', index.get(target, target-address))
            elif op == 15:
                upper[rt] = (i, (w & 65535) << 16)
            elif op in (8, 9, 13, 32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46):
                if rs == 28:
                    target = self.gp+signed16(w)
                    if ram(target):
                        tokens[i] = (w & 0xffff0000, 'gp')
                        operands[i] = ('data', target)
                elif rs in upper:
                    origin, hi = upper[rs]
                    target = hi | (w & 65535) if op == 13 else (hi+signed16(w)) & 0xffffffff
                    if ram(target) and i-origin <= 12:
                        tokens[origin] = (words[origin] & 0xffff0000, 'address_hi')
                        tokens[i] = (w & 0xffff0000, 'address_lo')
                        operands[i] = ('code' if target in self.functions else 'data', target)
            if op == 0 and fn not in (8, 9, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1a, 0x1b):
                # Indexed tables retain their LUI base through an address addition
                origin = (upper.get(rs) or upper.get(rt)) if fn == 0x21 else None
                upper.pop(rd, None)
                if origin:
                    upper[rd] = origin
            elif op != 15 and (op in (8, 9, 10, 11, 12, 13, 14) or 32 <= op <= 38):
                upper.pop(rt, None)
        cfg = tuple((index.get(b['start'], -1), sum(1 for a in addresses if b['start'] <= a < b['end']),
                     tuple(sorted(index.get(t, -1) for t in b['successors']))) for b in self.blocks.get(row['address'], []))
        return dict(words=words, addresses=addresses, tokens=tokens, normalized=repr(tokens),
                    flow=flow, cfg=cfg, operands=operands, size=len(words)*4,
                    hash=hashlib.sha256(b''.join(struct.pack('<I', w) for w in words)).hexdigest())


def ratio(a, b):
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()


class Matcher:
    def __init__(self, pal, ntsc, labels=None):
        self.a, self.b = pal, ntsc
        self.labels = labels or {}
        self.matches, self.reverse, self.evidence = {}, {}, {}
        self.data_map, self.data_reverse, self.data_evidence = {}, {}, {}
        self.ambiguities = []
        self.mips = None

    def add(self, a, b, reason, score=1, evidence=None):
        if a in self.matches or b in self.reverse:
            return False
        self.matches[a], self.reverse[b] = b, a
        self.evidence[a] = dict(method=reason, score=round(score, 5), evidence=evidence or [])
        return True

    def add_data(self, a, b, method, evidence):
        if a in self.data_map or b in self.data_reverse:
            return False
        self.data_map[a], self.data_reverse[b] = b, a
        self.data_evidence[a] = dict(method=method, evidence=evidence)
        return True

    def unique(self, field, reason):
        left, right = defaultdict(list), defaultdict(list)
        for a, f in self.a.features.items():
            if a not in self.matches and f['size'] >= 16:
                left[f[field]].append(a)
        for b, f in self.b.features.items():
            if b not in self.reverse and f['size'] >= 16:
                right[f[field]].append(b)
        for key in sorted(left.keys() & right.keys(), key=repr):
            if len(left[key]) == len(right[key]) == 1:
                a, b = left[key][0], right[key][0]
                self.add(a, b, reason)

    def seed_data(self):
        for source, destination, method in [(self.a.strings, self.b.strings, 'unique_string')]:
            left, right = defaultdict(list), defaultdict(list)
            for address, text in source.items():
                if len(text) >= 4:
                    left[text].append(address)
            for address, text in destination.items():
                if len(text) >= 4:
                    right[text].append(address)
            for text in sorted(left.keys() & right.keys()):
                if len(left[text]) == len(right[text]) == 1:
                    self.add_data(left[text][0], right[text][0], method, [text])
        left, right = defaultdict(list), defaultdict(list)
        for image, groups in [(self.a, left), (self.b, right)]:
            for address, row in image.data.items():
                if row['size'] and row['size'] >= 8 and row['sha256']:
                    raw = image.bytes(address, row['size'])
                    if raw and len(set(raw)) >= 3:
                        groups[(row['size'], row['sha256'])].append(address)
        for key in sorted(left.keys() & right.keys()):
            if len(left[key]) == len(right[key]) == 1:
                self.add_data(left[key][0], right[key][0], 'unique_item_bytes', [key[0], key[1]])

    def propagate_data(self):
        votes, left, right = defaultdict(set), defaultdict(set), defaultdict(set)
        for a, b in self.matches.items():
            if self.evidence[a]['method'] not in ('exact_bytes', 'address_normalized'):
                continue
            af, bf = self.a.features[a], self.b.features[b]
            if len(af['words']) != len(bf['words']):
                continue
            for i, (kind, target) in af['operands'].items():
                other = bf['operands'].get(i)
                if kind == 'data' and other and other[0] == 'data':
                    votes[target, other[1]].add(a)
                    left[target].add(other[1]); right[other[1]].add(target)
        for (a, b), functions in votes.items():
            if len(left[a]) == len(right[b]) == 1:
                self.add_data(a, b, 'matched_instruction_reference', [hex(f) for f in sorted(functions)])

    def score(self, a, b):
        if self.mips is None:
            self.mips = MipsSimilarity(self.a, self.b, self.matches)
        af, bf = self.a.features[a], self.b.features[b]
        size = min(af['size'], bf['size'])/max(af['size'], bf['size'])
        shape = ratio(af['tokens'], bf['tokens'])
        flow = ratio(af['flow'], bf['flow'])
        left, right = self.mips.left[a], self.mips.right[b]
        ev = self.mips.evidence(a, b)
        data_flow = ratio(left['events'], right['events'])
        parts = [(shape, .25), (flow, .15), (data_flow, .30)]
        for key, weight in (('memory', .20), ('calls', .07), ('topology', .03)):
            if left[key] or right[key]:
                parts.append((ev[key], weight))
        score = sum(value*weight for value, weight in parts)/sum(weight for _, weight in parts)
        return score, dict(instructions=round(shape, 4), opcodes=round(flow, 4), size=round(size, 4),
                           data_flow=round(data_flow, 4), memory_accesses=round(ev['memory'], 4),
                           known_calls=round(ev['calls'], 4), control_flow=round(ev['topology'], 4),
                           similarity_pct=round(score*100, 2), size_similarity_pct=round(size*100, 2))

    def candidates(self):
        if self.mips is None:
            self.mips = MipsSimilarity(self.a, self.b, self.matches)
        groups = defaultdict(list)
        for b, f in self.b.features.items():
            if b not in self.reverse:
                groups[f['size']].append(b)
        output = {}
        for a, f in sorted(self.a.features.items(), key=lambda row: (row[1]['size'], row[0])):
            if a in self.matches:
                continue
            candidates = []
            # Search all sizes by MIPS fingerprints, retaining equal-size ties separately
            available = [b for size, addresses in groups.items() for b in addresses]
            shortlist = set(self.mips.shortlist(a, available)) | set(groups.get(f['size'], []))
            for b in sorted(shortlist):
                value, evidence = self.score(a, b)
                candidates.append(dict(address=b, score=round(value, 5), similarity_pct=round(value*100, 2), evidence=evidence))
            output[a] = sorted(candidates, key=lambda r: (-r['score'], abs(self.b.features[r['address']]['size']-f['size']), r['address']))[:5]
        return output

    def run(self):
        self.seed_data()
        self.unique('hash', 'exact_bytes')
        self.unique('normalized', 'address_normalized')
        self.propagate_data()
        self.mips = MipsSimilarity(self.a, self.b, self.matches)
        self.rounds = []
        self.remaining = self.candidates()
        return self

    def result(self):
        matches = []
        for a, b in sorted(self.matches.items()):
            af, bf = self.a.features[a], self.b.features[b]
            diffs = []
            if len(af['words']) == len(bf['words']):
                for i, (w, v) in enumerate(zip(af['words'], bf['words'])):
                    if w != v:
                        diffs.append(dict(pal_site=hex(af['addresses'][i]), ntsc_site=hex(bf['addresses'][i]),
                                          pal_word=hex(w), ntsc_word=hex(v),
                                          pal_address_operand=af['operands'].get(i), ntsc_address_operand=bf['operands'].get(i)))
            method = self.evidence[a]['method']
            kind = 'identical_bytes' if af['hash'] == bf['hash'] else 'address_operands_only' if method == 'address_normalized' else 'potential_code_change'
            conflicts, unresolved = [], 0
            for change in diffs:
                left, right = change['pal_address_operand'], change['ntsc_address_operand']
                if not left or not right:
                    continue
                mapping = self.matches if left[0] == 'code' else self.data_map
                expected = mapping.get(left[1])
                if expected is None:
                    unresolved += 1
                elif left[0] != right[0] or expected != right[1]:
                    conflicts.append(change)
            if conflicts and kind != 'identical_bytes':
                kind = 'target_change_candidate'
            matches.append(dict(pal=hex(a), ntsc=hex(b), name=self.labels.get(a, self.a.functions[a]['name']),
                                pal_size=af['size'], ntsc_size=bf['size'], category=kind,
                                **self.evidence[a], similarity_pct=100.0, changed_words=diffs, target_conflicts=conflicts,
                                unresolved_changed_address_operands=unresolved))
        pending = []
        for a, candidates in sorted(self.remaining.items()):
            pending.append(dict(pal=hex(a), name=self.labels.get(a, self.a.functions[a]['name']),
                                pal_size=self.a.features[a]['size'], category='ambiguous' if candidates else 'no_candidate',
                                candidates=[dict(ntsc=hex(r['address']), ntsc_size=self.b.features[r['address']]['size'],
                                                 score=r['score'], similarity_pct=r['similarity_pct'], evidence=r['evidence']) for r in candidates]))
        data = []
        for a, b in sorted(self.data_map.items()):
            ar, br = self.a.data.get(a), self.b.data.get(b)
            size = min(ar['size'], br['size']) if ar and br and ar['size'] and br['size'] else None
            ab, bb = (self.a.bytes(a, size), self.b.bytes(b, size)) if size else (None, None)
            status = 'reference_only'
            if ab is not None and bb is not None:
                status = 'identical_item' if ab == bb and ar['size'] == br['size'] else 'potential_data_change'
                if size >= 4 and size % 4 == 0 and ar['size'] == br['size'] and ab != bb:
                    aw = struct.unpack('<'+'I'*(size//4), ab); bw = struct.unpack('<'+'I'*(size//4), bb)
                    if all(x == y or self.matches.get(x) == y or self.data_map.get(x) == y for x, y in zip(aw, bw)):
                        status = 'mapped_pointer_relocation'
            data.append(dict(pal=hex(a), ntsc=hex(b), category=status, pal_item=ar, ntsc_item=br,
                             **self.data_evidence[a]))
        summary = dict(pal_functions=len(self.a.functions), ntsc_functions=len(self.b.functions),
                       matched=len(matches), categories=dict(Counter(r['category'] for r in matches)),
                       unmatched_pal=len(pending), unmatched_ntsc=len(self.b.functions)-len(self.reverse),
                       mapped_data=len(data), data_categories=dict(Counter(r['category'] for r in data)), rounds=self.rounds)
        unmatched_a = [r for a, r in self.a.data.items() if a not in self.data_map]
        unmatched_b = [r for b, r in self.b.data.items() if b not in self.data_reverse]
        summary['unmatched_pal_data_items'] = len(unmatched_a)
        summary['unmatched_ntsc_data_items'] = len(unmatched_b)
        return dict(summary=summary, matches=matches, pending=pending, data=data,
                    unmatched_pal_data=unmatched_a, unmatched_ntsc_data=unmatched_b,
                    unmatched_ntsc=[dict(ntsc=hex(b), size=self.b.features[b]['size'], name=self.b.functions[b]['name'])
                                    for b in sorted(self.b.functions) if b not in self.reverse])


def labels(project, image):
    output = {}
    config = project/'status/ghidra/classification.json'
    if config.exists():
        for row in read(config).get('accepted', []):
            if row['image'] == image and row.get('matches'):
                output[row['address']] = row['matches'][0]['name']
    for source in (project/'src').glob('*.c'):
        text = source.read_text(encoding='utf-8-sig')
        functions = list(re.finditer(r'^\w[^;\n]*\b(\w+)\([^;\n]*\)\s*\n\{', text, re.M))
        for match in re.finditer(r'FUNCTION_MARKER\((0x[0-9a-fA-F]+),\s*"'+re.escape(image)+'"\)', text):
            owner = next((fn for fn in reversed(functions) if fn.start() < match.start()), None)
            if owner:
                output[int(match[1], 16)] = owner[1]
    return output


def compare(project, output):
    project, output = Path(project).resolve(), Path(output).resolve()
    inputs = read(output/'inputs.json')
    result = dict(schema=2, scope='Automated MIPS-only triage; no decompiler or PAL/NTSC game changes',
                  similarity_method='MIPS bounded register provenance, memory fields, control flow and known call identities', images={})
    for kind in ('MAIN.EXE', 'boot'):
        pair = [next(r for r in inputs if r['region'] == region and (r['image'] == 'MAIN.EXE') == (kind == 'MAIN.EXE')) for region in ('pal', 'ntsc-u')]
        images = [Image(output/r['region']/r['image']) for r in pair]
        for image, row in zip(images, pair):
            if hashlib.sha256(image.raw).hexdigest() != row['sha256']:
                raise ValueError('Input binary changed after IDA analysis')
        comparison = Matcher(*images, labels(project, pair[0]['image'])).run().result()
        verify_equivalence(*images, comparison)
        comparison['functions_by_size'] = {region: [dict(address=hex(a), size=f['size'], name=image.functions[a]['name'])
                                                   for a, f in sorted(image.features.items(), key=lambda row: (row[1]['size'], row[0]))]
                                           for region, image in zip(('pal', 'ntsc-u'), images)}
        comparison['inputs'] = pair
        result['images'][kind] = comparison
        print(kind, json.dumps(comparison['summary']), flush=True)
    result['limitations'] = ['IDA boundaries are discovered, not manually accepted',
                             'Matching address operands does not establish semantic equality of their target data',
                             'Size-selected similarity candidates require review; similarity is not a probability',
                             'Mapped data items are IDA item boundaries, not proven complete C arrays/structures',
                             'Reference-only BSS objects have no original initialized bytes to compare',
                             'Small duplicated functions may remain ambiguous; unmatched does not mean changed']
    export_result(output, result)
    return result


def export_result(output, result):
    output = Path(output)
    write(output/'comparison.json', result)
    rows = []
    for image, report in result['images'].items():
        for row in report['matches']:
            rows.append(dict(image=image, pal=row['pal'], ntsc=row['ntsc'], name=row['name'], category=row['category'],
                             pal_size=row['pal_size'], ntsc_size=row['ntsc_size'], score=row['score'], similarity_pct=row['similarity_pct'], method=row['method'],
                             verification=row['verification']['status'], verification_reason=row['verification']['reason']))
        for row in report['pending']:
            best = row['candidates'][0] if row['candidates'] else {}
            rows.append(dict(image=image, pal=row['pal'], ntsc=best.get('ntsc', ''), name=row['name'], category=row['category'],
                             pal_size=row['pal_size'], ntsc_size=best.get('ntsc_size', ''), score=best.get('score', ''), similarity_pct=best.get('similarity_pct', ''), method='candidate_only',
                             verification=row['verification']['status'], verification_reason=row['verification']['reason']))
    rows.sort(key=lambda row: (row['image'], row['pal_size'], -float(row['similarity_pct'] or 0), row['pal']))
    with (output/'functions.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)
    changes = [r for r in rows if r['verification'] != 'equivalent']
    changes.sort(key=lambda r: (r['category'] != 'potential_code_change', -float(r['score'] or 0), r['image'], r['pal']))
    write(output/'potential-differences.json', dict(scope=result['scope'], limitations=result['limitations'],
                                                  functions=changes, data={image: [r for r in report['data'] if r['category'] == 'potential_data_change'] for image, report in result['images'].items()},
                                                  unmatched_ntsc={image: report['unmatched_ntsc'] for image, report in result['images'].items()},
                                                  unmatched_data={image: dict(pal=report['unmatched_pal_data'], ntsc=report['unmatched_ntsc_data']) for image, report in result['images'].items()}))
    render_html(output, result, rows)
    return result


def render_html(output, result, rows):
    content = json.dumps(rows, ensure_ascii=False).replace('</', '<\\/')
    summary = ''
    for image, report in result['images'].items():
        s = report['summary']
        summary += '<p><b>'+html.escape(image)+'</b>: PAL '+str(s['pal_functions'])+' функций, NTSC-U '+str(s['ntsc_functions'])+'; сопоставлено '+str(s['matched'])+'; неоднозначны или не сопоставлены '+str(s['unmatched_pal'])+' PAL / '+str(s['unmatched_ntsc'])+' NTSC-U. Объектов данных сопоставлено: '+str(s['mapped_data'])+'.</p>'
        summary += '<p>Строгая проверка: '+html.escape(json.dumps(report['strict_summary'], ensure_ascii=False))+'.</p>'
    data_rows = [dict(image=image, **row) for image, report in result['images'].items() for row in report['data'] if row['category'] == 'potential_data_change']
    data_summary = ''.join('<li>'+html.escape(row['image'])+': '+html.escape(row['pal'])+' → '+html.escape(row['ntsc'])+' ('+html.escape(row['method'])+')</li>' for row in data_rows)
    page = '''<!doctype html><meta charset="utf-8"><title>PAL / NTSC-U — automated triage</title>
<style>body{font:14px system-ui;margin:32px;color:#252525;background:#fafafa}h1{font-size:24px}p{max-width:1100px;line-height:1.6}input,select{padding:8px;margin:12px 8px 12px 0}table{border-collapse:collapse;width:100%;background:white}th,td{padding:9px;text-align:left;border-bottom:1px solid #ddd}th{position:sticky;top:0;background:#eee}code{font-family:monospace}</style>
<h1>PAL / NTSC-U: карта соответствий и кандидаты различий</h1>
<p>Автоматическая первичная оценка. Кандидаты не являются доказанными соответствиями. Изменения логики ещё не разбирались. <a href="comparison.json">Полные признаки и операнды</a> · <a href="potential-differences.json">Список для оценки</a> · <a href="functions.csv">CSV функций</a>.</p>SUMMARY
<p>Сравнение только по MIPS, без декомпиляции. Кандидаты ищутся среди всех размеров по потоку данных, обращениям к полям, переходам и уже установленным соответствиям вызываемых функций. Для подробной оценки выбираются 32 лучших кандидата и все функции одинакового размера. Числовая близость адресов не используется; размер служит сортировкой, а не ограничением поиска.</p>
<p>Similarity 0–100%: инструкции 25%, опкоды 15%, поток данных 30%, обращения к памяти 20%, известные вызовы 7%, структура переходов 3%. Отсутствующие у обеих функций признаки исключаются из весов. Нормализация регистров отслеживает ограниченное происхождение значений, копирование, аргументы, стек и задержанные загрузки/вызовы. Это эвристика сходства, не доказательство эквивалентности.</p>
<p>identical_bytes — совпадают байты; address_operands_only — совпадает нормализованный код (содержимое целевых данных может отличаться); ambiguous / no_candidate — соответствие не установлено. Полные отсортированные списки обеих версий находятся в comparison.json, functions_by_size.</p>
<p>Строгие статусы: equivalent — подтверждено совпадение кода в поддерживаемой модели и замкнуты зависимости вызовов; difference_detected — найдены конкретные изменения операций или данных в установленной паре; unknown — проверка не завершена. Это не доказательство одинакового поведения всей игры: предполагаются соответствующие входные состояния и приватные стековые кадры ABI; CD-ресурсы не сравниваются. Скрыты только equivalent. Даже similarity 100% остаётся видимым, если строгая проверка не пройдена.</p>
<details><summary>Потенциальные изменения сопоставленных данных (DATA_COUNT)</summary><ul>DATA_SUMMARY</ul><p>Границы элементов установлены IDA и могут охватывать только часть таблицы. Несопоставленные данные и функции NTSC-U перечислены в JSON.</p></details>
<input id="search" placeholder="Имя, адрес или причина" size="40"><select id="category"><option value="">Все статусы проверки</option></select><label><input type="checkbox" id="changes" checked> Скрыть подтверждённые совпадения</label> <span id="count"></span>
<table><thead><tr><th>Образ</th><th>PAL</th><th>NTSC-U / кандидат</th><th>Имя PAL</th><th>Проверка</th><th>Причина</th><th>Размер PAL ↑</th><th>Размер NTSC-U</th><th>Similarity</th></tr></thead><tbody id="rows"></tbody></table>
<script>const data=DATA;const q=document.querySelector('#search'),c=document.querySelector('#category'),b=document.querySelector('#rows');
for(const kind of [...new Set(data.map(r=>r.verification))].sort()){const o=document.createElement('option');o.value=o.textContent=kind;c.append(o)}
const changes=document.querySelector('#changes');function draw(){const selected=data.filter(r=>(!changes.checked||r.verification!=='equivalent')&&(!c.value||r.verification===c.value)&&JSON.stringify(r).toLowerCase().includes(q.value.toLowerCase()));b.replaceChildren();for(const r of selected){const tr=document.createElement('tr');for(const k of ['image','pal','ntsc','name','verification','verification_reason','pal_size','ntsc_size','similarity_pct']){const td=document.createElement('td');td.textContent=k==='similarity_pct'&&r[k]!==''?Number(r[k]).toFixed(2)+'%':r[k];tr.append(td)}b.append(tr)}document.querySelector('#count').textContent=selected.length+' функций'}q.oninput=c.onchange=changes.onchange=draw;draw();</script>'''
    page = page.replace('DATA_COUNT', str(len(data_rows))).replace('DATA_SUMMARY', data_summary)
    (Path(output)/'index.html').write_text(page.replace('SUMMARY', summary).replace('DATA', content), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pal', type=Path)
    parser.add_argument('--ntsc', type=Path)
    parser.add_argument('--ida', type=Path)
    parser.add_argument('--compare-only', action='store_true')
    parser.add_argument('--verify-only', action='store_true', help='Recheck existing pairs without rerunning similarity or IDA')
    args = parser.parse_args()
    if args.verify_only:
        result = read(args.output/'comparison.json')
        for report in result['images'].values():
            images = [Image(args.output/row['region']/row['image']) for row in report['inputs']]
            for image, row in zip(images, report['inputs']):
                if hashlib.sha256(image.raw).hexdigest() != row['sha256']:
                    raise ValueError('Input binary changed after IDA analysis')
            verify_equivalence(*images, report)
            print(report['inputs'][0]['image'], report['strict_summary'])
        export_result(args.output, result)
        return
    if not args.compare_only:
        if not all((args.pal, args.ntsc, args.ida)):
            parser.error('Analysis requires --pal, --ntsc and --ida')
        analyze(args.pal, args.ntsc, args.output, args.ida)
    compare(args.project, args.output)


if __name__ == '__main__':
    main()
