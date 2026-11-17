"""Select accepted supplemental IDA inputs without replacing canonical exports"""

import hashlib
import json
from pathlib import Path


def _read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def _digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _owned(root, value):
    if not isinstance(value, str) or not value:
        raise ValueError('Missing IDA index path')
    path = (root / value).resolve()
    if not path.is_relative_to(root):
        raise ValueError('IDA index path escapes project')
    return path


def validate_nop_chunk_normalization(original, current, raw, load, review, image):
    """Validate unchanged body coverage around an entry's unreachable NOP gap"""
    if (review.get('schema') != 1 or review.get('kind') != 'unreachable_nop_chunk_gap' or
            review.get('image') != image or review.get('address') != original['address'] or
            review.get('function_sha256') != original['sha256'] or
            review.get('before_chunks') != original['chunks'] or
            review.get('after_chunks') != current['chunks'] or not review.get('reason')):
        raise ValueError('Unbound original chunk normalization review')
    if any(current.get(key) != original.get(key) for key in ('address', 'sha256', 'size', 'end')):
        raise ValueError('Chunk normalization changes original body identity')
    old_words = [(w['address'], w['bytes'], w['word']) for w in original['raw_instructions']]
    new_words = [(w['address'], w['bytes'], w['word']) for w in current['raw_instructions']]
    if old_words != new_words:
        raise ValueError('Chunk normalization changes original instruction coverage')
    entry, end = original['address'], original['end']

    def word(address):
        offset = address - load['base'] + load.get('offset', 0)
        if address % 4 or offset < load.get('offset', 0) or offset + 4 > len(raw):
            raise ValueError('Chunk normalization escapes original bytes')
        return int.from_bytes(raw[offset:offset + 4], 'little')

    high, low, jump, delay = [word(entry + n * 4) for n in range(4)]
    register = high >> 16 & 31
    if (high >> 26 != 15 or high >> 21 & 31 or not register or
            low >> 26 != 9 or low >> 21 & 31 != register or low >> 16 & 31 != register or
            jump != (register << 21 | 8) or delay != 0):
        raise ValueError('Chunk normalization lacks a resolved entry tail jump')
    immediate = low & 0xffff
    target = (((high & 0xffff) << 16) + (immediate - 0x10000 if immediate & 0x8000 else immediate)) & 0xffffffff
    prefix_end = entry + 16
    if (target % 4 or not prefix_end < target < end or
            original['chunks'] != [[entry, end]] or
            current['chunks'] != [[entry, prefix_end], [target, end]]):
        raise ValueError('Chunk normalization does not match the skipped gap')
    if any(word(address) != 0 for address in range(prefix_end, target, 4)):
        raise ValueError('Chunk normalization excludes non-NOP original bytes')
    addresses = list(range(entry, prefix_end, 4)) + list(range(target, end, 4))
    if addresses != [w[0] for w in old_words] or original['size'] != len(addresses) * 4:
        raise ValueError('Chunk normalization changes original recorded extent')


def select_exports(root, project):
    """Return index inputs; original workflow consumers retain canonical inputs"""
    import startup

    root = Path(root).resolve()
    canonical = _owned(root, project['paths']['ida_exports'])
    exports = {path.name: path for path in sorted(canonical.iterdir())
               if path.is_dir() and (path / 'functions.json').is_file()}
    selected = set()
    for binding in project.get('analysis', {}).get('index_exports', []):
        image = binding.get('image')
        if image not in exports or image in selected:
            raise ValueError('IDA index selection requires a unique canonical image')
        selected.add(image)
        baseline_path = exports[image] / 'functions.json'
        if _digest(baseline_path) != binding.get('canonical_functions_sha256'):
            raise ValueError('Canonical IDA index baseline changed')
        receipt_path = _owned(root, binding.get('receipt'))
        if (not receipt_path.is_relative_to(root / 'status/ida/accepted') or
                _digest(receipt_path) != binding.get('receipt_sha256')):
            raise ValueError('Unbound IDA index acceptance receipt')
        receipt = _read(receipt_path)
        if (receipt.get('version') != 1 or
                receipt.get('role') != 'supplemental_original_analysis'):
            raise ValueError('IDA index selection requires an accepted supplemental snapshot')
        destination = _owned(root, receipt.get('destination'))
        if not destination.is_relative_to(root / 'status/ida/snapshots'):
            raise ValueError('IDA index selection is outside accepted snapshots')
        matches = [row for row in receipt.get('images', []) if row.get('image') == image]
        if len(matches) != 1:
            raise ValueError('Missing unique accepted IDA index image')
        accepted = matches[0]
        configs = _owned(root, project['paths']['ida_configs'])
        manifest = startup.verify_ida_image(root, destination, configs, image)
        for key in ('functions', 'config', 'config_sha256', 'files'):
            if manifest[key] != accepted.get(key):
                raise ValueError('Accepted IDA index artifact changed: ' + key)
        profile = _read(configs / (image + '.json'))
        folder = destination / image
        report = _read(folder / 'export-report.json')
        if profile.get('image') != image or report.get('gp') != profile.get('gp'):
            raise ValueError('IDA index image or GP differs from accepted profile')
        loads = profile.get('loads', [])
        if len(loads) != 1:
            raise ValueError('Supplemental IDA index requires one reviewed raw load')
        load = loads[0]
        original = _owned(root, load.get('path'))
        if _digest(original) != binding.get('original_sha256'):
            raise ValueError('Supplemental IDA index original input changed')
        raw = original.read_bytes()
        functions = _read(folder / 'functions.json')
        by_address = {row['address']: row for row in functions}
        if len(by_address) != len(functions):
            raise ValueError('Duplicate supplemental IDA index function')
        baseline = _read(baseline_path)
        if len({row['address'] for row in baseline}) != len(baseline):
            raise ValueError('Duplicate canonical IDA index function')
        normalizations = {}
        for item in binding.get('canonical_chunk_normalizations', []):
            path = _owned(root, item.get('review'))
            if (not path.is_relative_to(root / 'status') or
                    _digest(path) != item.get('review_sha256')):
                raise ValueError('Unbound canonical chunk normalization')
            review = _read(path)
            address = review.get('address')
            if type(address) is not int or address in normalizations:
                raise ValueError('Duplicate or invalid canonical chunk normalization')
            normalizations[address] = review
        used_normalizations = set()
        for row in baseline:
            current = by_address.get(row['address'])
            if current is None or any(current[key] != row[key]
                                      for key in ('sha256', 'size', 'end')):
                raise ValueError('Supplemental IDA index changes a canonical body')
            if current['chunks'] != row['chunks']:
                review = normalizations.get(row['address'])
                if review is None:
                    raise ValueError('Supplemental IDA index changes canonical chunks')
                validate_nop_chunk_normalization(row, current, raw, load, review, image)
                used_normalizations.add(row['address'])
        if used_normalizations != set(normalizations):
            raise ValueError('Unused canonical chunk normalization')
        for row in functions:
            words = row['raw_instructions']
            previous_end = -1
            for start, end in row['chunks']:
                if (type(start) is not int or type(end) is not int or
                        start % 4 or end % 4 or start >= end or start < previous_end):
                    raise ValueError('Invalid supplemental IDA index chunk')
                previous_end = end
            addresses = [address for start, end in row['chunks']
                         for address in range(start, end, 4)]
            if (addresses != [word['address'] for word in words] or
                    row['size'] != len(words) * 4):
                raise ValueError('Supplemental IDA index instruction/chunk mismatch')
            for word in words:
                offset = word['address'] - load['base'] + load.get('offset', 0)
                expected = bytes.fromhex(word['bytes'])
                if (len(expected) != 4 or offset < load.get('offset', 0) or
                        raw[offset:offset + 4] != expected or
                        int(word['word'], 16) != int.from_bytes(expected, 'little')):
                    raise ValueError('Supplemental IDA index instruction differs from original')
        exports[image] = folder
    for binding in additional_bindings(root, project):
        image, folder = validate_additional_export(root, binding)
        if image in exports:
            raise ValueError('Additional IDA image duplicates an existing index input')
        exports[image] = folder
    return [exports[image] for image in sorted(exports)]


def index_profile(root, project, image):
    """Resolve a retained additional image's original profile"""
    matches = [item for item in additional_bindings(Path(root).resolve(), project)
               if item.get('image') == image]
    if len(matches) > 1:
        raise ValueError('Duplicate additional IDA image profile')
    if matches:
        return _owned(Path(root).resolve(), matches[0].get('config'))
    return _owned(Path(root).resolve(), project.get('paths', {}).get('ida_configs', 'tools/ida')) / (image + '.json')


def additional_bindings(root, project):
    """Read an optional hash-bound manifest of retained additional images"""
    value = project.get('analysis', {}).get('additional_index_exports', [])
    if isinstance(value, dict):
        path = _owned(root, value.get('path'))
        if not path.is_relative_to(root / 'status') or _digest(path) != value.get('sha256'):
            raise ValueError('Additional IDA index manifest changed')
        value = _read(path)
    if not isinstance(value, list):
        raise ValueError('Additional IDA index inputs must be a list')
    return value


def validate_additional_export(root, binding):
    """Verify retained image inputs without running analysis or changing snapshots"""
    root = Path(root).resolve()
    folder = _owned(root, binding.get('path'))
    config = _owned(root, binding.get('config'))
    review = _owned(root, binding.get('review'))
    if not folder.is_relative_to(root / 'status') or not review.is_relative_to(root / 'status'):
        raise ValueError('Additional IDA input must retain project status provenance')
    if _digest(config) != binding.get('config_sha256') or _digest(review) != binding.get('review_sha256'):
        raise ValueError('Additional IDA profile or review changed')
    files = binding.get('files', {})
    required = {'functions.json', 'export-report.json', 'symbols.json', 'data-xrefs.json',
                'decompilation-failures.json'}
    if not required <= files.keys():
        raise ValueError('Additional IDA inventory lacks required input hashes')
    for name, expected in files.items():
        path = _owned(root, (folder / name).relative_to(root).as_posix())
        if not path.is_relative_to(folder) or _digest(path) != expected:
            raise ValueError('Additional IDA export file changed: ' + name)
    profile, report = _read(config), _read(folder / 'export-report.json')
    image = binding.get('image')
    if not image or profile.get('image') != image or report.get('image') != image:
        raise ValueError('Additional IDA image identity mismatch')
    reported_profile = report.get('config')
    inherited_thread = profile.get('gp_evidence', {}).get('source') == 'inherited_thread_resource_loader'
    if inherited_thread and isinstance(reported_profile, dict):
        reported_profile = json.loads(json.dumps(reported_profile))
        reported_profile.pop('output', None)
        reported_profile.pop('rebuild', None)
        for item in reported_profile.get('loads', []):
            item['path'] = _owned(root, item['path']).relative_to(root).as_posix()
        if reported_profile.get('external_symbols'):
            reported_profile['external_symbols'] = _owned(root, reported_profile['external_symbols']).relative_to(root).as_posix()
    if not profile.get('gp') or profile['gp'] != report.get('gp') or reported_profile != profile:
        raise ValueError('Additional IDA GP or original profile mismatch')
    loads = profile.get('loads', [])
    if len(loads) != 1:
        raise ValueError('Additional IDA input requires one retained image load')
    load = loads[0]
    original = _owned(root, load.get('path'))
    if _digest(original) != binding.get('original_sha256'):
        raise ValueError('Additional IDA original image changed')
    raw, seen = original.read_bytes(), set()
    if inherited_thread:
        import startup
        startup.validate_gp_config(root, profile)
    else:
        initializers = profile.get('gp_evidence', {}).get('runtime_initializers', [])
        if len(initializers) != 1 or initializers[0].get('value') != profile['gp']:
            raise ValueError('Additional IDA input lacks one reviewed GP initializer')
        gp_offset = initializers[0]['site'] - load['base'] + load.get('offset', 0)
        high = int.from_bytes(raw[gp_offset:gp_offset + 4], 'little')
        low = int.from_bytes(raw[gp_offset + 4:gp_offset + 8], 'little')
        immediate = low & 0xffff
        value = (((high & 0xffff) << 16) + (immediate - 0x10000 if immediate & 0x8000 else immediate)) & 0xffffffff
        if gp_offset < load.get('offset', 0) or high >> 16 != 0x3c1c or low >> 16 != 0x279c or value != profile['gp']:
            raise ValueError('Additional IDA GP initializer differs from original bytes')
    for row in _read(folder / 'functions.json'):
        address = row['address']
        if address in seen:
            raise ValueError('Duplicate additional IDA function')
        seen.add(address)
        previous_end = -1
        for start, end in sorted(row['chunks']):
            offset = start - load['base'] + load.get('offset', 0)
            if (start % 4 or end % 4 or start >= end or start < previous_end or
                    offset < load.get('offset', 0) or offset + end - start > len(raw)):
                raise ValueError('Additional IDA chunk escapes original image')
            previous_end = end
        addresses = [site for start, end in row['chunks'] for site in range(start, end, 4)]
        words = row['raw_instructions']
        if addresses != [word['address'] for word in words] or len(words) * 4 != row['size']:
            raise ValueError('Additional IDA chunk/instruction mismatch')
        body = bytearray()
        for word in words:
            offset = word['address'] - load['base'] + load.get('offset', 0)
            expected = bytes.fromhex(word['bytes'])
            if (len(expected) != 4 or offset < load.get('offset', 0) or
                    raw[offset:offset + 4] != expected or
                    int(word['word'], 16) != int.from_bytes(expected, 'little')):
                raise ValueError('Additional IDA instruction differs from original')
            body.extend(expected)
        if hashlib.sha256(body).hexdigest() != row['sha256']:
            raise ValueError('Additional IDA function hash mismatch')
        required_paths = ['functions/%08X.lst' % address]
        if row.get('pseudocode'):
            required_paths.append(row['pseudocode'])
        if not set(required_paths) <= files.keys():
            raise ValueError('Additional IDA function lacks listing/pseudocode binding')
    return image, folder
