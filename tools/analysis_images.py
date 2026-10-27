"""Resolve explicitly reviewed resident parents without inferring active overlay identities"""


def resident_parents(config, profiles):
    selected = config.get('analysis', {}).get('resident_parents', {})
    if not isinstance(selected, dict):
        raise ValueError('Resident parent selection must be an image mapping')
    for child, parent in selected.items():
        if child == parent or child not in profiles or parent not in profiles:
            raise ValueError('Resident parent refers to missing or identical images')
        left, right = profiles[child], profiles[parent]
        if (left.get('gp') != right.get('gp') or
                right.get('gp_evidence', {}).get('source') not in ('runtime_initializer', 'entry_runtime_initializer')):
            raise ValueError('Resident parent requires matching GP and a direct initializer')
        for a in left['segments']:
            for b in right['segments']:
                if a['start'] < b['end'] and b['start'] < a['end']:
                    raise ValueError('Resident parent and child load segments overlap')
    return selected


def resident_targets(image, target, parents, exported, by_address):
    parent = parents.get(image)
    if not parent:
        return []
    exact = [item for item in by_address.get(target, []) if item[0] == parent]
    if exact:
        return exact
    return [(parent, function['address']) for function in exported[parent]
            if any(start <= target < end for start, end in function['chunks'])]
