"""Bind a connected original call cluster to a proven paired owner entry"""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path

from lockstep_audit import checklist, words_from_listing
from lockstep_packet import _audit_function_binding
from lockstep_paired_entry import _address, _evidence, _memory
from source_context import compact


def read_cluster_request(root: Path, directory: Path, request: dict,
                         previous: dict, source_roots: tuple[Path, ...]) -> dict:
    required = {'kind', 'failure_signature', 'image', 'functions', 'evidence'}
    if set(request) != required or request.get('kind') != 'audit_cluster':
        raise ValueError('Invalid causal audit cluster request')
    if request['failure_signature'] != previous.get('failure_signature'):
        raise ValueError('Causal audit cluster belongs to another failure')
    context = previous.get('source_context') or {}
    image = request['image']
    if not isinstance(image, str) or not image or len(image) > 40:
        raise ValueError('Causal audit cluster image is invalid')
    addresses = request['functions']
    if (not isinstance(addresses, list) or not 1 <= len(addresses) <= 8 or
            any(not isinstance(value, str) for value in addresses)):
        raise ValueError('Causal audit cluster functions are invalid')
    parsed = [_address(value) for value in addresses]
    if len(set(parsed)) != len(parsed):
        raise ValueError('Causal audit cluster functions are duplicated')
    evidence = request['evidence']
    if not isinstance(evidence, list) or not 1 <= len(evidence) <= 8:
        raise ValueError('Causal audit cluster evidence is invalid')
    frame = (previous.get('expected_original_event') or {}).get('frame')
    if type(frame) is not int:
        raise ValueError('Causal audit cluster lacks a divergent frame')
    probes = []
    for item in evidence:
        if not isinstance(item, dict) or set(item) != {'path', 'sha256'}:
            raise ValueError('Causal audit cluster evidence identity is invalid')
        probe = _evidence(directory, item['path'], item['sha256'])
        alignment = probe.get('alignment') or {}
        comparison = alignment.get('comparison') or (alignment.get('last_step') or {}).get('comparison') or {}
        if (probe.get('target_frame') != frame - 1 or
                alignment.get('aligned') is not True or comparison.get('status') != 'match'):
            raise ValueError('Causal audit cluster evidence is not aligned')
        probes.append(probe)
    contexts = [context, *(context.get('audit_cluster') or [])]
    known = {(row['function']['image'], row['function']['address']) for row in contexts}
    for address in parsed:
        if (image, address) in known:
            raise ValueError('Causal audit cluster repeats an existing function')
        row = compact(image, address, 262144, True, address, full_original=True)
        implementation = row.get('implementation') or {}
        if (implementation.get('complete') is not True or
                (row.get('mips') or {}).get('complete') is not True or
                not any(Path(implementation.get('path', '')).is_relative_to(base) for base in source_roots)):
            raise ValueError('Causal audit cluster source is incomplete or unowned')
        row['machine_audit'] = checklist(row['mips']['lines'])
        contexts.append(row)
        known.add((image, address))
    by_address = {row['function']['address']: row for row in contexts}
    if len(by_address) != len(contexts):
        raise ValueError('Causal audit cluster contains ambiguous overlay addresses')
    edges = {address: set() for address in by_address}
    for address, row in by_address.items():
        for pc, word in words_from_listing(row['mips']['lines']).items():
            if word >> 26 != 3:
                continue
            target = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            if target in edges:
                edges[address].add(target)
                edges[target].add(address)
    reached = set()
    pending = [context['function']['address']]
    while pending:
        address = pending.pop()
        if address in reached:
            continue
        reached.add(address)
        pending.extend(edges[address] - reached)
    if reached != set(edges):
        raise ValueError('Causal audit cluster is disconnected from the current owner')
    paired = False
    observed = set()
    for probe in probes:
        for hit in probe.get('hits') or []:
            pc = _address(hit.get('pc'))
            observed.update(address for address, row in by_address.items()
                            if address <= pc < row['function']['end'])
        native = probe.get('native_entry') or {}
        for row in contexts:
            address = row['function']['address']
            symbol = (row.get('implementation') or {}).get('symbol')
            if native.get('symbol') != symbol:
                continue
            arguments = native.get('arguments') or []
            samples = native.get('probe_memory') or native.get('memory') or []
            for hit in probe.get('hits') or []:
                registers = hit.get('registers') or {}
                if (_address(hit.get('pc')) != address or not arguments or
                        _address(arguments[0]) != _address(registers.get('r4'))):
                    continue
                if samples and all(_memory(hit.get('memory'), _address(sample['address']), sample['size']) ==
                                   bytes.fromhex(sample['data']) for sample in samples):
                    paired = True
    if not paired or not observed.intersection(parsed):
        raise ValueError('Causal audit cluster lacks a matched owner entry and observed dependency')
    packet = copy.deepcopy(previous)
    target = packet['source_context']
    target['audit_cluster'] = contexts[1:]
    target['audit_contract']['required_functions'] = [_audit_function_binding(row) for row in contexts]
    packet['causal_cluster_provenance'] = {'evidence': evidence, 'frame': frame,
                                         'functions': addresses, 'paired_entry': True}
    packet['failure_signature'] = hashlib.sha256(json.dumps({
        'prior': previous['failure_signature'], 'cluster': addresses, 'evidence': evidence,
    }, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    packet['serialized_bytes'] = len(json.dumps(packet, sort_keys=True, separators=(',', ':')).encode())
    if packet['serialized_bytes'] > packet['budget_bytes']:
        raise ValueError('Causal audit cluster exceeds the attention packet budget')
    return packet
