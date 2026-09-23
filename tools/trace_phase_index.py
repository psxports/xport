"""Build content-verified phase chunks and compare indexed ordinal windows"""
import argparse
import hashlib
import json
from itertools import islice, zip_longest
from pathlib import Path
import struct
import time

from trace_cache import digest
from trace_layout import PHASE_BYTES, PROFILE_PATH
from trace_phases import iter_phases, compare
from xport_project import artifact_path


def build(source, directory, expected_sha, chunk_size=128):
    source, directory = Path(source), artifact_path(directory)
    if not 1 <= chunk_size <= 4096:
        raise ValueError('Invalid chunk size')
    directory.mkdir(parents=True, exist_ok=False)
    source_hash = hashlib.sha256()
    chunks, data, count, start = [], bytearray(), 0, 0
    header = struct.pack('<3I', 0x31504646, 3, PHASE_BYTES)
    footer = struct.pack('<4I', 5, 0, 4, 1)
    def flush():
        if not data:
            return
        path = directory / f'{start:010d}.phases'
        path.write_bytes(header + data + footer)
        validated = sum(1 for _ in iter_phases(path, sound=True, gpu=True))
        if validated != count - start:
            raise ValueError('Chunk validation failed')
        chunks.append(dict(start=start, end=count, file=path.name, sha256=digest(path)))
    with source.open('rb') as stream:
        def read(size):
            value = stream.read(size)
            source_hash.update(value)
            if len(value) != size:
                raise ValueError('Truncated source')
            return value
        magic, version, size = struct.unpack('<3I', read(12))
        if magic not in (0x32544646, 0x31504646):
            raise ValueError('Unsupported source')
        if magic == 0x31504646 and (version != 3 or size != PHASE_BYTES):
            raise ValueError('Unsupported native phase schema')
        complete = False
        while stream.tell() < source.stat().st_size:
            raw = read(12)
            kind, tick, length = struct.unpack('<3I', raw)
            if complete or not 1 <= kind <= 18 or length > 32 * 1024 * 1024:
                raise ValueError('Invalid source envelope')
            if magic == 0x31504646 and kind not in (4, 5, 15, 17):
                raise ValueError('Unexpected native phase channel')
            payload = read(length)
            if kind == 5:
                if payload != struct.pack('<I', 1):
                    raise ValueError('Unsuccessful source')
                complete = True
            elif kind == 15:
                if count and count % chunk_size == 0:
                    flush()
                    data.clear()
                    start = count
                data.extend(raw + payload)
                count += 1
            elif kind in (4, 17) and count:
                data.extend(raw + payload)
        if not complete or not count:
            raise ValueError('Missing complete phase source')
    flush()
    if source_hash.hexdigest() != expected_sha:
        raise ValueError('Source hash differs; index not published')
    result = dict(schema=1, source_sha256=expected_sha, phases=count,
                  profile_sha256=digest(PROFILE_PATH), chunks=chunks,
                  scope='Derived phase state/GPU/sound only; raw source retained')
    (directory / 'index.json').write_text(json.dumps(result, indent=2))
    return result


def rows(index_path, begin, end, expected_sha):
    index_path = Path(index_path)
    index = json.loads(index_path.read_text())
    if index['schema'] != 1 or index['source_sha256'] != expected_sha:
        raise ValueError('Index identity differs')
    if digest(PROFILE_PATH) != index['profile_sha256']:
        raise ValueError('Profile changed')
    if not 0 <= begin < end <= index['phases']:
        raise ValueError('Window outside index')
    cursor = 0
    for chunk in index['chunks']:
        if chunk['start'] != cursor or chunk['end'] <= cursor:
            raise ValueError('Index gaps or overlaps')
        cursor = chunk['end']
    if cursor != index['phases']:
        raise ValueError('Incomplete index')
    for chunk in index['chunks']:
        if chunk['end'] <= begin or chunk['start'] >= end:
            continue
        path = (index_path.parent / chunk['file']).resolve()
        if path.parent != index_path.parent.resolve() or digest(path) != chunk['sha256']:
            raise ValueError('Chunk path or content changed')
        for offset, row in enumerate(iter_phases(path, sound=True, gpu=True)):
            ordinal = chunk['start'] + offset
            if begin <= ordinal < end:
                row['ordinal'] = ordinal
                yield row


def compare_window(index, source_sha, native, begin, end, excluded_sound_ordinals=()):
    original = rows(index, begin, end, source_sha)
    actual = iter_phases(native, sound=True, gpu=True)
    totals = dict(boundaries=0, sound_events=0, gpu_packets=0, raw_gpu_differences=0)
    for ordinal, pair in enumerate(zip_longest(original, actual), begin):
        left, right = pair
        if left is None or right is None:
            return dict(passed=False, first_difference=dict(ordinal=ordinal, field='boundary_count'))
        result = compare([left], [right], sound=True, gpu=True,
                         include_last_sound=ordinal < end-1 and ordinal not in excluded_sound_ordinals)
        if not result['passed']:
            result['first_difference']['ordinal'] = ordinal
            return result
        for key in totals:
            totals[key] += result[key]
    return dict(totals, passed=True, first_difference=None, start_phase=begin, end_phase=end)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['build', 'compare'])
    parser.add_argument('--source', type=Path)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--index', type=Path)
    parser.add_argument('--native', type=Path)
    parser.add_argument('--begin', type=int, default=0)
    parser.add_argument('--end', type=int)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    started = time.perf_counter()
    if args.action == 'build':
        value = build(args.source, args.directory, args.sha256)
        result = dict(phases=value['phases'], chunks=len(value['chunks']))
    else:
        result = compare_window(args.index, args.sha256, args.native, args.begin, args.end)
    result['seconds'] = time.perf_counter() - started
    if args.report:
        artifact_path(args.report).write_text(json.dumps(result, indent=2))
    print(json.dumps(result))
    if result.get('passed') is False:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
