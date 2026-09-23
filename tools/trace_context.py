"""Bounded diagnostic views and explicitly selected captured RAM queries"""
import argparse
import json
from pathlib import Path
import sqlite3

from trace_cache import digest
from trace_worker import write_receipt
from xport_project import artifact_path


def bounded(value, limit=12000):
    """Keep valid JSON and explicit omissions rather than slicing serialized text"""
    def trim(item, depth=0):
        if isinstance(item, dict):
            return {k: trim(v, depth+1) for k, v in item.items()}
        if isinstance(item, list):
            result = [trim(v, depth+1) for v in item[:8]]
            if len(item)>8: result.append({'omitted_items': len(item)-8})
            return result
        if isinstance(item, str) and len(item)>800:
            return item[:800]+' [truncated; read linked evidence]'
        return item
    result = trim(value)
    if len(json.dumps(result, ensure_ascii=True).encode()) <= limit:
        return result
    # Preserve all top-level keys and replace the largest values first
    omitted = []
    for key in sorted(result, key=lambda k: len(json.dumps(result[k])), reverse=True):
        if len(json.dumps(result, ensure_ascii=True).encode()) <= limit-256: break
        result[key] = {'omitted': True}
        omitted.append(key)
    result['bounded_view'] = dict(omitted_fields=omitted, full_evidence_required=True)
    return result


def channels(comparison):
    """Map each channel to its own first difference without re-running comparison"""
    from trace_stage_difference import first_difference
    contract = comparison.get('contract', {})
    result = []
    groups = comparison.get('segments') or [comparison]
    for group in groups:
        for name, row in group.get('channels', {}).items():
            detail = row.get('first_difference')
            ordinal = None
            if detail:
                if comparison.get('diagnostic_only'):
                    ordinal = detail.get('ordinal')
                else:
                    mapped = first_difference({name: row}, contract)
                    ordinal = mapped.get('ordinal') if mapped else None
            segment = next((s for s in contract.get('segments', [])
                            if type(ordinal) is int and s['start_phase']<=ordinal<s['end_phase']), None)
            result.append(dict(channel=name, passed=row.get('passed'),
                matched=row.get('matched', row.get('records', row.get('boundaries'))),
                ordinal=ordinal, stage=segment['stage'] if segment else None,
                tick=segment['start_tick']+ordinal-segment['start_phase'] if segment else None,
                first_difference=detail, problem=row.get('problem'),
                available_prefix_end=row.get('available_prefix_end')))
    # Put failing segments first so a bounded view does not hide a late-stage defect
    return sorted(result,key=lambda row:not bool(row['first_difference'] or row['problem']))


def enrich(comparison, folder=None):
    result = dict(channel_context=channels(comparison), data_references=[])
    addresses = set()
    def walk(value):
        if isinstance(value, dict):
            for key, item in value.items():
                if key=='address' and isinstance(item, str):
                    try: addresses.add(int(item, 0))
                    except ValueError: pass
                else: walk(item)
        elif isinstance(value, list):
            for item in value: walk(item)
    walk(result['channel_context'])
    from trace_audit_packet import candidates
    for address in sorted(addresses)[:4]:
        try: result['data_references'].append(candidates(address))
        except (OSError, ValueError, KeyError, sqlite3.Error) as error:
            result['data_references'].append(dict(address=hex(address), problem=str(error)))
    result['address_count'] = len(addresses)
    result['address_limit'] = 4
    if folder is not None:
        from trace_audit_packet import audit
        result['candidate_audits'] = []
        sites = set()
        for reference in result['data_references']:
            for candidate in reference.get('candidates', []):
                site = candidate['site']; image = candidate['image']
                if (image,site) in sites or len(sites)>=2: continue
                sites.add((image,site))
                try:
                    context = audit(site,image)
                    path = Path(folder)/f'data-candidate-{len(sites)}.json'
                    write_receipt(path,context)
                    result['candidate_audits'].append(dict(path=str(path),sha256=digest(path),site=hex(site),
                        meaning='Static reference candidate, not a proven causal writer'))
                except (OSError,ValueError,KeyError,sqlite3.Error) as error:
                    result['candidate_audits'].append(dict(site=hex(site),problem=str(error)))
    return bounded(result)


def ram_words(index, phase, addresses, count=4):
    """Read only explicit main-RAM addresses at an exact captured stage entry"""
    from trace_hash_session import hash_session
    if not 1<=len(addresses)<=8 or not 1<=count<=16:
        raise ValueError('Use 1..8 addresses and 1..16 words per address')
    index = Path(index).resolve()
    index_hash = digest(index)
    value = json.loads(index.read_text())
    if digest(index)!=index_hash: raise ValueError('Package index changed while reading')
    found = [p for p in value['packages'] if p['ordinal']==phase]
    if len(found)!=1: raise ValueError('Exactly one package at the requested phase is required')
    package = found[0]; section = package['sections']['ram']; path = Path(section['path'])
    if not path.is_absolute(): path = index.parent/path
    queries = []
    with hash_session():
        if digest(path)!=section['sha256']: raise ValueError('Captured RAM hash changed')
        with path.open('rb') as stream:
            size = path.stat().st_size
            if size!=section['size']: raise ValueError('Captured RAM size changed')
            for address in addresses:
                base = next((b for b in (0, 0x80000000, 0xA0000000)
                             if b<=address and address+count*4<=b+min(size, 0x200000)), None)
                if base is None: raise ValueError('Address is outside captured PSX main RAM')
                stream.seek(address-base); data = stream.read(count*4)
                if len(data)!=count*4: raise ValueError('Truncated RAM query')
                words = [int.from_bytes(data[i:i+4], 'little') for i in range(0,len(data),4)]
                queries.append(dict(address=hex(address), words=[hex(w) for w in words]))
        if digest(path)!=section['sha256']: raise ValueError('RAM changed while reading')
    from xport_project import load_project
    root, config = load_project(); image = config.get('analysis',{}).get('default_image')
    database = root/config.get('paths',{}).get('analysis_database','status/analysis.sqlite')
    if image and database.is_file():
        with sqlite3.connect(database.as_uri()+'?mode=ro',uri=True) as db:
            for query in queries:
                query['code_candidates'] = []
                for word in query['words']:
                    address = int(word,16)
                    rows = db.execute('SELECT address,name FROM functions WHERE image=? AND address<=? AND end>? LIMIT 4',
                                      (image,address,address)).fetchall()
                    if rows: query['code_candidates'].append(dict(value=word,image=image,
                        functions=[dict(address=hex(a),name=n) for a,n in rows]))
    return dict(index=str(index), index_sha256=index_hash, raw_sha256=value['raw_sha256'],
        phase=phase, stage=package['stage'], tick=package['game_tick'],
        ram_sha256=section['sha256'], queries=queries,
        scope='Captured stage-entry values only; pointer-like words are candidates, not proven callbacks or writers')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package-index', type=Path, required=True)
    parser.add_argument('--phase', type=int, required=True)
    parser.add_argument('--address', action='append', type=lambda s:int(s,0), required=True)
    parser.add_argument('--words', type=int, default=4)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    value = ram_words(args.package_index,args.phase,args.address,args.words)
    output = artifact_path(args.output); output.parent.mkdir(parents=True,exist_ok=True); write_receipt(output,value)
    print(json.dumps(dict(evidence=str(output), **bounded(value))))


if __name__=='__main__': main()
