"""Install the shared converge supervisor protocol configuration"""
import argparse
import copy
import json
from pathlib import Path


DEFAULTS = {
    'schema': 1,
    'enabled': True,
    'mode': 'enforced',
    'one_launch': True,
    'notify_on_revision_only': True,
    'batch_limits': {'functions': 5, 'instructions': 2048, 'modules': 2},
    'acceptance': {'unchanged_model_calls': 0, 'interventions_per_failure': 1,
                   'submissions_per_batch': 1, 'fast_refreshes_per_batch': 1,
                   'provisional_replays_per_batch': 1, 'full_gate_after_provisional_match': True,
                   'max_control_responses': 5, 'max_compactions': 2,
                   'deterministic_call_reduction_percent': 90},
}


def migrate(root, apply=False):
    path = Path(root).resolve() / 'xport-project.json'
    value = json.loads(path.read_text(encoding='utf-8-sig'))
    stage = value.setdefault('stage_pipeline', {})
    current = stage.get('supervisor_protocol')
    desired = copy.deepcopy(current) if isinstance(current, dict) else {}
    for key, item in DEFAULTS.items():
        if key in ('batch_limits', 'acceptance'):
            limits = desired.setdefault(key, {})
            for limit, amount in item.items():
                limits.setdefault(limit, amount)
        else:
            desired.setdefault(key, item)
    changed = current != desired
    if changed and apply:
        stage['supervisor_protocol'] = desired
        path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    return {'project': str(root), 'changed': changed, 'applied': changed and apply,
            'supervisor_protocol': desired}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    print(json.dumps(migrate(args.project, args.apply), separators=(',', ':')))


if __name__ == '__main__':
    main()
