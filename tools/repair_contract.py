"""Load pinned declarative repair inputs without interpreting pseudocode"""
import hashlib
import itertools
import json
from pathlib import Path
import re
import sqlite3

from xport_project import load_project


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def sha(value):
    return hashlib.sha256(value).hexdigest()


def identity(value):
    return sha(json.dumps(value, sort_keys=True, separators=(',', ':')).encode())


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z_]\w*', value):
        raise ValueError('Invalid C identifier')
    return value


def local(root, relative):
    path = (Path(root) / relative).resolve()
    if not path.is_relative_to(Path(root).resolve()):
        raise ValueError('Contract path escapes project')
    return path


def pinned(root, record):
    path = local(root, record['path'])
    data = path.read_bytes()
    if sha(data) != record['sha256']:
        raise ValueError('Pinned input changed: ' + str(path))
    return path, data


def load(path):
    root, _ = load_project()
    path = local(root, path)
    value = json.loads(path.read_text(encoding='utf-8-sig'))
    if value.get('schema') != 1 or value.get('kind') not in ('fixture', 'project'):
        raise ValueError('Expected repair contract schema 1 and explicit fixture/project kind')
    identifier(value['name'])
    return root, value


def program(root, contract):
    if contract.get('schema')!=1 or contract.get('kind') not in ('fixture','project'):
        raise ValueError('Invalid contract schema/kind')
    identifier(contract['name'])
    spec = contract['original']
    if contract['kind'] == 'fixture':
        words = {number(a): number(w) for a, w in spec['words'].items()}
    else:
        _, config = load_project(root)
        database = local(root, config['paths']['analysis_database'])
        image = spec['image']
        if Path(image).name!=image:raise ValueError('Invalid image identity')
        config_path=local(root,config['paths'].get('ida_configs','tools/ida'))/(image+'.json')
        image_config=json.loads(config_path.read_text(encoding='utf-8-sig'))
        allowed={(str(local(root,item['path'])),number(item['base']),number(item.get('offset',0)))
                 for item in image_config['loads']}
        words = {}
        mappings = []
        if not spec['functions'] or not spec['segments']:raise ValueError('Missing original identity')
        for segment in spec['segments']:
            path, data = pinned(root, segment)
            if (str(path),number(segment['base']),number(segment.get('offset',0))) not in allowed:
                raise ValueError('Image segment is not a configured original load')
            mappings.append((number(segment['base']), number(segment.get('offset', 0)), data))
        with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as db:
            for function in spec['functions']:
                entry = number(function['address'])
                rows = db.execute('SELECT address,bytes FROM instructions WHERE image=? AND function=? ORDER BY address', (image, entry)).fetchall()
                raw = b''.join(bytes.fromhex(b) for _, b in rows)
                if not rows or sha(raw) != function['sha256']:
                    raise ValueError('Original function identity changed')
                for address, encoded in rows:
                    expected = bytes.fromhex(encoded)
                    if len(expected) != 4 or address in words:
                        raise ValueError('Overlapping or invalid instruction')
                    candidates = [data[offset + address - base:offset + address - base + 4]
                                  for base, offset, data in mappings
                                  if 0 <= offset + address - base <= len(data) - 4 and address >= base]
                    if len(candidates) != 1 or candidates[0] != expected:
                        raise ValueError('Instruction bytes do not match one pinned image segment')
                    words[address] = int.from_bytes(expected, 'little')
    if not words or any(a % 4 or w < 0 or w > 0xffffffff for a, w in words.items()):
        raise ValueError('Invalid MIPS word map')
    if number(spec['entry']) not in words:
        raise ValueError('Missing entry instruction')
    return words


def cases(contract):
    values = list(contract.get('cases', []))
    axes = contract.get('axes', {})
    size = 1
    for axis in axes.values():
        size *= len(axis)
    limit = min(number(contract.get('max_cases', 256)), 4096)
    if size + len(values) > limit and axes:
        raise ValueError('Case product exceeds declared limit')
    if axes:
        values.extend(dict(zip(axes, row)) for row in itertools.product(*axes.values()))
    if not values or len(values) > limit:
        raise ValueError('Missing or excessive cases')
    for row in values:
        if not all(isinstance(k, str) and isinstance(v, (int, str)) for k, v in row.items()):
            raise ValueError('Cases must contain scalar bindings')
    return values


def expr(value, bindings):
    if isinstance(value, str) and value in bindings:
        return number(bindings[value]) & 0xffffffff
    if not isinstance(value, dict):
        return number(value) & 0xffffffff
    if set(value) != {'op', 'args'}:
        raise ValueError('Invalid expression')
    args = [expr(x, bindings) for x in value['args']]
    if len(args) != 2:
        raise ValueError('Binary expression required')
    a, b = args
    operations = {'add': lambda: a+b, 'sub': lambda: a-b, 'and': lambda: a&b,
                  'or': lambda: a|b, 'xor': lambda: a^b,
                  'shl': lambda: a << (b&31), 'shr': lambda: a >> (b&31)}
    if value['op'] not in operations:
        raise ValueError('Unsupported expression')
    return operations[value['op']]() & 0xffffffff


def native_sources(root, contract):
    spec = contract['native']
    if contract['kind'] == 'fixture':
        return spec['source'], {}
    from source_context import checked_text, slice_record
    if not spec['functions']:raise ValueError('Missing native function identity')
    _, config = load_project(root)
    database = local(root, config['paths']['analysis_database'])
    bodies = []; hashes = {}; cache = {}
    with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as db:
        db.row_factory = sqlite3.Row
        for item in spec['functions']:
            row = db.execute('SELECT * FROM implementations WHERE image=? AND address=?',
                             (item['image'], number(item['address']))).fetchone()
            if row is None or row['mapping_status'] != 'mapped':
                raise ValueError('Native implementation is not uniquely mapped')
            if len(spec['functions'])==1 and row['symbol']!=spec['symbol']:
                raise ValueError('Native entry symbol does not match the source index')
            body = slice_record(root, db, row, cache)
            if sha(body.encode()) != item['sha256']:
                raise ValueError('Native function changed')
            bodies.append(body)
            hashes[row['source_path']] = sha(local(root,row['source_path']).read_bytes())
        for hook in contract.get('hooks',[]):
            row=db.execute('SELECT symbol,mapping_status FROM implementations WHERE image=? AND address=?',
                           (contract['original']['image'],number(hook['address']))).fetchone()
            if row is None or row['mapping_status']!='mapped' or row['symbol']!=hook['symbol']:
                raise ValueError('Hook symbol/address lacks a unique native mapping')
    return '\n'.join(bodies), hashes
