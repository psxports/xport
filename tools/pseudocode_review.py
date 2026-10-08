"""Apply hash-bound reviews of semantically incomplete pseudocode"""
import hashlib
import json


def apply_review(root, project, function, view):
    relative = project.get('analysis', {}).get('pseudocode_reviews')
    if not relative:
        return view
    root = root.resolve()
    path = (root / relative).resolve()
    if not path.is_relative_to(root / 'status') or not path.is_file():
        raise ValueError('Pseudocode reviews must be a project-owned status file')
    raw = path.read_bytes()
    data = json.loads(raw.decode('utf-8-sig'))
    if set(data) != {'schema', 'reviews'} or data['schema'] != 1 or not isinstance(data['reviews'], list):
        raise ValueError('Invalid pseudocode review schema')
    seen = set()
    for row in data['reviews']:
        if (set(row) != {'image', 'address', 'function_sha256', 'pseudocode_sha256', 'reason'} or
                not isinstance(row['image'], str) or type(row['address']) is not int or
                not isinstance(row['reason'], str) or not row['reason'].strip()):
            raise ValueError('Invalid incomplete pseudocode review')
        key = (row['image'], row['address'])
        if key in seen:
            raise ValueError('Duplicate pseudocode review')
        seen.add(key)
        if key != (function['image'], function['address']):
            continue
        if (not view.get('path') or row['function_sha256'] != function['sha256'] or
                row['pseudocode_sha256'] != view['sha256']):
            raise ValueError('Pseudocode review has stale original evidence')
        view = {**view, 'content_complete': view['complete'], 'complete': False,
                'review': {'path': path.relative_to(root).as_posix(),
                           'sha256': hashlib.sha256(raw).hexdigest(), 'reason': row['reason']}}
    return view
