"""Produce bounded first-divergence context without dumping whole traces"""
import argparse
import json
from pathlib import Path
import sqlite3
from trace_cache import digest
from xport_project import load_project, artifact_path


def packet(report, address=None, image=None):
    root, config = load_project()
    value = json.loads(Path(report).read_text(encoding='utf-8-sig'))
    comparison = value.get('comparison', value)
    difference = comparison.get('first_difference')
    result = dict(report=str(Path(report).resolve()), report_sha256=digest(report),
                  passed=comparison.get('passed', value.get('passed')), first_difference=difference,
                  instruction='Locate the first causal branch; do not infer a function from a state address')
    result['artifacts'] = value.get('artifact_paths', value.get('artifacts', {}))
    identity = value.get('manifest_identity', {})
    result['dependencies'] = identity.get('spec', {}).get('dependencies', {})
    from trace_context import enrich
    result.update(enrich(comparison,Path(report).parent))
    if difference:
        position = difference.get('ordinal', difference.get('tick'))
        if isinstance(position, int):
            result['suggested_window'] = dict(begin=max(0, position-2), end_exclusive=position+3,
                axis='phase_ordinal' if 'ordinal' in difference else 'game_tick')
    if address is None and difference and isinstance(difference.get('ordinal'),int):
        manifest=Path(report).with_name('manifest.json')
        if manifest.is_file() and value.get('contract'):
            spec=json.loads(manifest.read_text())
            if spec.get('phase_index'):
                from trace_phase_index import rows
                ordinal=difference['ordinal']
                boundary=next(rows(spec['phase_index'],ordinal,ordinal+1,spec['raw_sha256']))
                image=config.get('analysis',{}).get('default_image')
                database=root/config['paths']['analysis_database']
                if image and database.is_file():
                    with sqlite3.connect(database.as_uri()+'?mode=ro',uri=True) as db:
                        candidates=db.execute('SELECT address,name FROM functions WHERE image=? AND address<=? AND end>?',
                            (image,boundary['pc'],boundary['pc'])).fetchall()
                    result['execution_boundary']=dict(pc=hex(boundary['pc']),image=image,phase=ordinal,
                        candidates=[dict(address=hex(a),name=n) for a,n in candidates],
                        meaning='Function containing the recorded boundary; not a causal attribution')
                    # A capture boundary does not identify the causal instruction
    if address is not None:
        from trace_audit_packet import audit
        result['audit']=audit(address,image)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--address', type=lambda v:int(v, 0))
    parser.add_argument('--image')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.address is not None and not args.image:
        parser.error('An explicit image is required with an address')
    result = packet(args.report, args.address, args.image)
    artifact_path(args.output).write_text(json.dumps(result, indent=2))
    print(json.dumps({'packet':str(args.output), 'passed':result['passed'],
                      'first_difference':result['first_difference']}))


if __name__ == '__main__':
    main()
