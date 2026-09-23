"""Decode actual digital controller polls without merging by resettable game ticks"""
import argparse
import json
from pathlib import Path


def decode_polls(events):
    active = [[], []]
    seen = [False, False]
    leading = [0, 0]
    polls, issues = [], []
    ignored = 0
    previous_clock = -1
    for index, event in enumerate(events):
        port = event['port']
        clock = event['emulated_ticks']
        if port not in (0, 1) or clock < previous_clock:
            raise ValueError('Invalid port or reversed transfer clock')
        previous_clock = clock
        if event['device'] != 1:
            if active[port]:
                issues.append({'transfer': index, 'reason': 'Device changed during controller poll'})
                active[port] = []
            ignored += 1
            continue
        start = event['outgoing'] == 1 and event['incoming'] == 255 and event['ack'] == 1
        if not active[port]:
            if not start:
                if seen[port]:
                    issues.append({'transfer': index, 'reason': 'Unframed controller transfer'})
                else:
                    leading[port] += 1
                continue
            seen[port] = True
        elif start:
            issues.append({'transfer': index, 'reason': 'Controller poll restarted before completion'})
            active[port] = []
        active[port].append((index, event))
        if event['ack']:
            if len(active[port]) > 32:
                issues.append({'transfer': index, 'reason': 'Controller poll exceeds supported length'})
                active[port] = []
            continue
        packet = active[port]
        active[port] = []
        raw = [row['incoming'] for _, row in packet]
        if len(packet) != 5 or raw[:3] != [255, 65, 90] or packet[1][1]['outgoing'] != 0x42:
            issues.append({'transfer': packet[0][0], 'reason': 'Unsupported or malformed digital poll',
                           'incoming': raw})
            continue
        first = packet[0][1]
        polls.append({'port': port, 'first_transfer': packet[0][0], 'last_transfer': index,
                      'start_frame': first['frame'], 'end_frame': event['frame'],
                      'start_clock': first['emulated_ticks'], 'end_clock': clock,
                      'game_tick': event['game_tick'],
                      'buttons': (raw[3] | raw[4] << 8) ^ 65535})
    return {'polls': polls, 'issues': issues, 'leading_partial_bytes': leading,
            'trailing_partial_bytes': [len(packet) for packet in active],
            'non_controller_transfers': ignored,
            'digital_polls_valid': not issues,
            'comparison_ready': False,
            'scope': 'Complete digital response packets; native poll alignment and non-controller events remain separate'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--transfers', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    from xport_project import artifact_path
    args.output = artifact_path(args.output)
    result = decode_polls(json.loads(args.transfers.read_text()))
    with args.output.open('x') as stream:
        json.dump(result, stream)
    print(json.dumps({'polls': len(result['polls']), 'issues': len(result['issues']),
                      'digital_polls_valid': result['digital_polls_valid']}))


if __name__ == '__main__':
    main()
