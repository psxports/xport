"""Query reads, writes and overlapping writers in current game C/C++ sources.

Positional addresses below 0x400 are scratchpad offsets. Pass full guest
addresses to query DRAM, or use repeatable --range ADDRESS:SIZE.
The generated SQLite cache refreshes automatically on each invocation.
"""

import argparse
import ast
from bisect import bisect_right
from collections import Counter, defaultdict, deque
from contextlib import closing
import hashlib
import json
import operator
from pathlib import Path
import re
import sqlite3
import sys
from tempfile import NamedTemporaryFile

from source_index import functions, mask_c
from xport_project import load_project

BASE, END = 0x1f800000, 0x1f800400
DRAM_SIZE = 0x200000
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
SCALAR = re.compile(r'([rw])_(?:masked_)?[us](8|16|32)')
LITERAL = re.compile(r'\b0x[0-9a-f]+[ul]*\b', re.IGNORECASE)
INTEGER_PARAMETER = re.compile(r'(?:const\s+)?(?:u?int(?:8|16|32)|sint(?:8|16|32))\s+([A-Za-z_]\w*)')
ADDRESS_LOCAL = re.compile(r'(?m)(?:^|(?<=[;{]))[ \t]*(?:const\s+)?uint32\s+([A-Za-z_]\w*)\s*=\s*([^;]+);')
ORDER_BARRIER = re.compile(r'\b(?:if|else|for|while|do|switch|goto|return|break|continue)\b|[{}?:]|&&|\|\|')
SCHEMA_VERSION = 7
SCALAR_SIZES = {prefix + str(bits): bits // 8
                for prefix in ('uint', 'sint') for bits in (8, 16, 32, 64)}
SCALAR_PREFIX = re.compile(r'\s*(?:const\s+)?(?P<type>uint(?:8|16|32|64)|sint(?:8|16|32|64))'
                           r'\s+(?P<name>[A-Za-z_]\w*)\s*;')
LIMITS = ('Only exact scalar accesses in a straight-line prefix have order classification. '
          'Overlap and argument chains are candidates, not execution order. '
          'Dynamic pointers, preprocessing, control flow and indirect calls are not resolved. '
          'Only game source is indexed; no result proves absence of writers.')
NON_CALLS = {'if', 'for', 'while', 'switch', 'sizeof', '_Alignof', 'FUNCTION_MARKER'}
BINOPS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
          ast.LShift: operator.lshift, ast.BitOr: operator.or_, ast.BitAnd: operator.and_}
SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (fingerprint TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS call_resolution (symbol TEXT NOT NULL, parameter INTEGER NOT NULL,
    address INTEGER NOT NULL, witness TEXT NOT NULL,
    PRIMARY KEY (symbol, parameter, address));
CREATE TABLE IF NOT EXISTS call_resolution_state (truncated INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS files (source TEXT PRIMARY KEY, sha256 TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS definitions (source TEXT NOT NULL, symbol TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS definitions_symbol ON definitions(symbol);
CREATE TABLE IF NOT EXISTS sites (
    source TEXT NOT NULL, symbol TEXT NOT NULL, line INTEGER NOT NULL,
    operation TEXT NOT NULL, direction TEXT NOT NULL, expression TEXT NOT NULL,
    lo INTEGER, hi INTEGER, width INTEGER, parameter INTEGER, displacement INTEGER,
    sequence INTEGER, byte_mask INTEGER
);
CREATE INDEX IF NOT EXISTS sites_range ON sites(lo, hi);
CREATE INDEX IF NOT EXISTS sites_symbol ON sites(symbol);
CREATE INDEX IF NOT EXISTS sites_source ON sites(source);
CREATE INDEX IF NOT EXISTS sites_pending ON sites(symbol) WHERE hi IS NULL;
CREATE TABLE IF NOT EXISTS ingress (
    source TEXT NOT NULL, caller TEXT NOT NULL, line INTEGER NOT NULL,
    callee TEXT NOT NULL, parameter INTEGER NOT NULL, address INTEGER,
    caller_parameter INTEGER, displacement INTEGER
);
CREATE INDEX IF NOT EXISTS ingress_target ON ingress(callee, parameter);
CREATE INDEX IF NOT EXISTS ingress_source ON ingress(source);
CREATE TABLE IF NOT EXISTS calls (
    source TEXT NOT NULL, caller TEXT NOT NULL, line INTEGER NOT NULL, callee TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS calls_source ON calls(source);
CREATE INDEX IF NOT EXISTS calls_caller ON calls(caller);
CREATE TABLE IF NOT EXISTS native_members (
    source TEXT NOT NULL, symbol TEXT NOT NULL, line INTEGER NOT NULL,
    field TEXT NOT NULL, direction TEXT NOT NULL, body_sha256 TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS native_members_field ON native_members(field);
CREATE INDEX IF NOT EXISTS native_members_source ON native_members(source);
"""


def native_members(text, source):
    text = text.replace('\r\n', '\n')
    code = mask_c(text)
    newlines = [hit.start() for hit in re.finditer('\n', text)]
    member = r'[A-Za-z_]\w*(?:\s*(?:\.|->)\s*[A-Za-z_]\w*)+'
    # include bare root address loans and whole-root assignments as candidates
    address = r'(?<![\w&])&\s*([A-Za-z_]\w*(?:\s*(?:\.|->)\s*[A-Za-z_]\w*)*)'
    pattern = re.compile(member + '|' + address + r'|\b([A-Za-z_]\w*)\b(?=\s*=(?!=))')
    rows = []
    for function in functions(text, source):
        start, end = function['start'], function['end']
        opening = code.index('{', start, end) + 1
        for hit in pattern.finditer(code, opening, end):
            field = re.sub(r'\s+', '', hit[0]).lstrip('&')
            before = code[opening:hit.start()].rstrip()
            after = code[hit.end():end].lstrip()
            if hit[1] or (before.endswith('&') and not before.endswith('&&')):
                direction = 'address_candidate'
            elif after.startswith('['):
                direction = 'indexed_candidate'
            elif re.match(r'(?:\+\+|--|<<=|>>=|[+*/%&|^\-]=)', after) or before.endswith(('++', '--')):
                direction = 'read_write'
            elif re.match(r'=(?!=)', after):
                direction = 'write'
            else:
                direction = 'read'
            rows.append((source, function['name'], bisect_right(newlines, hit.start()) + 1,
                         field, direction, function['sha256']))
    return rows


def native_field_query(db, fields, *, limit=40, symbols=(), reads=False, writes=False, dependencies=False):
    reports = []
    for field in dict.fromkeys(fields):
        if not re.fullmatch(r'[A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)*', field):
            raise ValueError('native fields require an identifier/member path without indexes')
        parts = re.split(r'(\.|->)', field)
        ancestors = [''.join(parts[:end]) for end in range(1, len(parts), 2)]
        placeholders = ','.join('?' for _ in ancestors)
        condition = 'n.field = ? OR substr(n.field,1,?) = ? OR substr(n.field,1,?) = ?'
        params = [field, len(field) + 1, field + '.', len(field) + 2, field + '->']
        if ancestors:
            condition += f' OR n.field IN ({placeholders})'
            params += ancestors
        selected = db.execute(
            'SELECT n.source,n.symbol,n.line,n.field,n.direction,n.body_sha256,f.sha256 '
            'FROM native_members n JOIN files f ON f.source=n.source WHERE (' + condition + ') '
            'ORDER BY n.source,n.line,n.field', params).fetchall()
        sites = []
        counts = Counter()
        for source, symbol, line, matched, direction, body_hash, source_hash in selected:
            if symbols and symbol not in symbols:
                continue
            if reads and direction not in ('read', 'read_write', 'address_candidate', 'indexed_candidate'):
                continue
            if writes and direction not in ('write', 'read_write', 'address_candidate', 'indexed_candidate'):
                continue
            counts[direction] += 1
            relation = 'exact' if matched == field else 'ancestor' if matched in ancestors else 'descendant'
            sites.append({'source': source, 'symbol': symbol, 'line': line, 'field': matched,
                          'relation': relation, 'direction': direction,
                          'body_sha256': body_hash, 'source_sha256': source_hash})
        report = {'field': field, 'site_count': len(sites), 'direction_counts': dict(counts), 'sites': sites[:limit]}
        if dependencies:
            owners = {site['symbol'] for site in sites}
            edges = [dict(zip(('source', 'caller', 'line', 'callee'), row))
                     for row in db.execute('SELECT source,caller,line,callee FROM calls ORDER BY source,line,callee')
                     if row[3] in owners]
            report.update(incoming_call_count=len(edges), incoming_calls=edges[:limit])
        reports.append(report)
    return {'native_fields': reports, 'tool_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'limits': 'Lexical direct-member, root-assignment and possible address-loan candidates only. '
                      'Ancestors/descendants are included; address/indexed candidates may read or write. '
                      'No type/alias, whole-object read, preprocessor, macro, indirect-call, reachability, '
                      'ordering or absence-of-writers proof. Incoming calls identify callers, not value propagation. '
                      'Member accesses through indexes/parentheses may require separate alias review. '
                      'Function hashes normalize CRLF to LF; source hashes bind raw file bytes.'}


def arguments(code, opening, *, complete=False):
    stack = ['(']
    parts = []
    start = opening + 1
    for end in range(start, len(code)):
        char = code[end]
        if char in '([{':
            stack.append(char)
        elif char in ')]}':
            if stack[-1] != {')': '(', ']': '[', '}': '{'}[char]:
                return []
            stack.pop()
            if not stack:
                if complete and code[end + 1:].strip():
                    return []
                return parts + [code[start:end].strip()]
        elif char == ',' and len(stack) == 1:
            parts.append(code[start:end].strip())
            start = end + 1
    return []


def expression_tree(expr):
    expr = re.sub(r'\b(0x[0-9a-f]+|[0-9]+)[ul]+\b', r'\1', expr, flags=re.IGNORECASE)
    if not re.fullmatch(r'[0-9a-zA-Z_+*<>&|()\s-]+', expr):
        return None
    try:
        return ast.parse(expr.strip(), mode='eval').body
    except SyntaxError:
        return None


def literal(tree, names=None):
    if isinstance(tree, ast.Name) and names is not None:
        return names.get(tree.id)
    if isinstance(tree, ast.Constant) and type(tree.value) is int:
        return tree.value if 0 <= tree.value <= 0xffffffff else None
    if isinstance(tree, ast.UnaryOp) and isinstance(tree.op, (ast.UAdd, ast.USub)):
        value = literal(tree.operand, names)
        return None if value is None else (-value if isinstance(tree.op, ast.USub) else value)
    if isinstance(tree, ast.BinOp) and type(tree.op) in BINOPS:
        lhs, rhs = literal(tree.left, names), literal(tree.right, names)
        if lhs is None or rhs is None or (isinstance(tree.op, ast.LShift) and not 0 <= rhs < 32):
            return None
        return BINOPS[type(tree.op)](lhs, rhs)
    return None


def param_address(tree, params):
    if isinstance(tree, ast.Name) and tree.id in params:
        return params[tree.id]
    if isinstance(tree, ast.BinOp) and isinstance(tree.op, (ast.Add, ast.Sub)):
        parameter, offset = param_address(tree.left, params)
        delta = literal(tree.right)
        if parameter is not None and delta is not None and abs(delta) < DRAM_SIZE:
            return parameter, offset + (delta if isinstance(tree.op, ast.Add) else -delta)
    return None, None


def guest_address(value, operation=None):
    if value is None or not 0 <= value <= 0xffffffff:
        return None
    physical = value & 0x1fffffff
    segment = value & 0xe0000000
    if segment not in (0, 0x80000000, 0xa0000000):
        return None
    if physical < 0x00800000 and (operation != 'psx_addr' or segment or physical < DRAM_SIZE):
        return physical % DRAM_SIZE
    return physical if BASE <= physical < END else None


def span_end(address, width):
    if address is None or width is None or width <= 0:
        return None
    limit = DRAM_SIZE if address < DRAM_SIZE else END
    return address + width if address + width <= limit else None


def variable_rebound(code, name):
    return re.search(
        rf'\b{name}\b\s*(?:=(?!=)|[+*/%&|^-]=|<<=|>>=|\+\+|--)'
        rf'|(?:\+\+|--|(?<!&)&(?!&))\s*\b{name}\b'
        rf'|\b(?:u?int\w*|sint\w*|char|short|long|size_t|auto|float|double)\s+\**\s*{name}\b',
        code,
    )


def local_addresses(code, body, prefix_end, params):
    aliases = {}
    for match in ADDRESS_LOCAL.finditer(code[body:prefix_end]):
        name, expression = match.groups()
        if name in params or variable_rebound(
            code[body:body + match.start()] + code[body + match.end():], name
        ):
            continue
        value, parameter, offset = address_expression(expression, params, aliases, body + match.start())
        if value is not None or parameter is not None:
            aliases[name] = body + match.end(), value, parameter, offset
    return aliases


def address_expression(expression, params, aliases, position):
    tree = expression_tree(expression)
    values = {}
    variables = dict(params)
    for name, (declared, value, parameter, offset) in aliases.items():
        if declared <= position:
            if value is not None:
                values[name] = value
            else:
                variables[name] = (parameter, offset)
    parameter, offset = param_address(tree, variables)
    return literal(tree, values), parameter, offset


def copy_address(expression, width, objects=None):
    match = re.match(r'^psx_addr\s*\(', expression)
    if match:
        args = arguments(expression, match.end() - 1, complete=True)
        if len(args) == 2:
            capacity = (literal(expression_tree(args[1])) if objects is None else
                        copy_extent(args[1], objects))
            if capacity is None or width is None:
                return args[0], None
            if capacity is not None and width is not None and width <= capacity:
                return args[0], capacity
    return expression, None


def scalar_size_names(code, body, macros):
    """recognize only simple leading C scalar objects, excluding lexical shadowing"""
    if re.search(r'\benum\b', code[body:]):
        return {}
    objects, position = {}, body
    while match := SCALAR_PREFIX.match(code, position):
        kind, name = match['type'], match['name']
        if kind in SCALAR_SIZES and not {kind, name, 'sizeof'} & macros:
            objects[name] = (SCALAR_SIZES[kind], match.end('name'))
        position = match.end()
    for name, (_, declaration_end) in list(objects.items()):
        valid = True
        for use in re.finditer(r'\b' + re.escape(name) + r'\b', code):
            if use.end() == declaration_end:
                continue
            prefix, suffix = code[:use.start()].rstrip(), code[use.end():].lstrip()
            address = prefix.endswith('&') and not prefix.endswith('&&')
            sizeof = bool(re.search(r'\bsizeof\s*\(\s*$', prefix)) and suffix.startswith(')')
            bare_sizeof = bool(re.search(r'\bsizeof\s+$', code[:use.start()]))
            assignment = suffix.startswith('=') and not suffix.startswith('==') and prefix[-1:] in '({;:,'
            copied_value = suffix.startswith(')') and bool(re.search(
                r'\bsizeof\s*\(\s*' + re.escape(name) + r'\s*\)\s*\)\s*,\s*$', prefix))
            if not (address or sizeof or bare_sizeof or assignment or copied_value):
                valid = False
                break
        if not valid:
            del objects[name]
    return {name: size for name, (size, _) in objects.items()}


def copy_extent(expression, objects):
    def size(match):
        name = match[1] or match[2]
        value = objects.get(name)
        return str(value) if value is not None else match[0]
    reduced = re.sub(r'\bsizeof\s*(?:\(\s*([A-Za-z_]\w*)\s*\)|([A-Za-z_]\w*))', size, expression)
    return literal(expression_tree(reduced))


def summarize(text, source):
    sites, ingress, calls, definitions = [], [], [], []
    masked = mask_c(text)
    newlines = [m.start() for m in re.finditer('\n', text)]
    macros = set(re.findall(r'(?m)^\s*@\s*define\s+(\w+)',
                            mask_c(re.sub(r'\\\r?\n', '', text).replace('#', '@'))))
    for function in functions(text, source):
        name = function['name']
        definitions.append((source, name))
        start, end = function['start'], function['end']
        code = masked[start:end]
        body = code.index('{') + 1
        barrier = ORDER_BARRIER.search(code, body)
        directive = re.search(r'(?m)^\s*#', text[start + body:end])
        prefix_end = min(barrier.start() if barrier else len(code),
                         body + directive.start() if directive else len(code))
        params = {}
        signature = re.search(r'\b' + re.escape(name.split('::')[-1]) + r'\s*\(', code[:body])
        for index, arg in enumerate(arguments(code, signature.end() - 1) if signature else ()):
            match = INTEGER_PARAMETER.fullmatch(arg)
            if match and not variable_rebound(code[body:], match[1]):
                params[match[1]] = index, 0
        aliases = local_addresses(code, body, prefix_end, params)
        objects = scalar_size_names(code, body, macros) if Path(source).suffix == '.c' else {}
        for call in CALL.finditer(code, body):
            operation = call[1]
            if operation in NON_CALLS:
                continue
            scalar = SCALAR.fullmatch(operation)
            line = bisect_right(newlines, start + call.start()) + 1
            args = arguments(code, call.end() - 1)
            if not args:
                continue
            if scalar:
                accesses = [(args[0], scalar[1], int(scalar[2]) // 8)]
            elif operation == 'psx_addr' and len(args) == 2:
                accesses = [(args[0], 'view', copy_extent(args[1], objects))]
            elif operation in ('memcpy', 'memmove', 'memset') and len(args) == 3:
                width = copy_extent(args[2], objects)
                accesses = [(args[0], 'w', width)]
                if operation != 'memset':
                    accesses.append((args[1], 'r', width))
            else:
                calls.append((source, name, line, operation))
                for index, arg in enumerate(args):
                    value, parameter, offset = address_expression(arg, params, aliases, call.start())
                    address = value if guest_address(value) is not None else None
                    if address is not None or parameter is not None:
                        ingress.append((source, name, line, operation, index, address, parameter, offset))
                continue
            for expr, direction, width in accesses:
                capacity = width
                if operation in ('memcpy', 'memmove', 'memset'):
                    expression, capacity = copy_address(expr, width, objects)
                    if capacity is None and re.match(r'^psx_addr\s*\(', expr):
                        width = None
                    source_api = 'psx_addr'
                else:
                    expression, source_api = expr, operation
                value, parameter, offset = address_expression(expression, params, aliases, call.start())
                address = guest_address(value, source_api)
                limit = span_end(address, width) if span_end(address, capacity) is not None else None
                if parameter is not None and width is not None and (width != capacity or not 0 < width <= DRAM_SIZE):
                    parameter = offset = None
                sequence = None
                if scalar and limit is not None and call.start() < prefix_end:
                    # standalone statements avoid unspecified order between nested calls
                    statement = code.rfind(';', body, call.start()) + 1 or body
                    end_statement = code.find(';', call.end())
                    if end_statement != -1 and end_statement < prefix_end:
                        fragment = code[statement:end_statement]
                        if len(CALL.findall(fragment)) == 1:
                            if direction == 'r' or not code[statement:call.start()].strip():
                                sequence = start + call.start()
                byte_mask = None
                if operation == 'w_masked_u32':
                    mask = literal(expression_tree(args[2])) if len(args) == 3 else None
                    if mask is None:
                        limit, parameter, offset = None, None, None
                    elif not mask & 15:
                        continue
                    elif limit is not None:
                        for lane in range(4):
                            if mask & (1 << lane):
                                start_lane = address + lane
                                sites.append((source, name, line, operation, direction, expr, start_lane,
                                              start_lane + 1, 1, None, None, None, None))
                        continue
                    elif parameter is not None:
                        byte_mask = mask & 15
                sites.append((source, name, line, operation, direction, expr, address, limit,
                              width, parameter, offset, sequence, byte_mask))
        for match in LITERAL.finditer(code, body):
            value = int(match[0].rstrip('uUlL'), 16)
            address = guest_address(value) if value >= 0x80000000 or value >= BASE else None
            if address is not None:
                line = bisect_right(newlines, start + match.start()) + 1
                sites.append((source, name, line, 'literal', 'anchor', match[0], address, None,
                              None, None, None, None, None))
    return sites, ingress, calls, definitions


def refresh(db, root, rebuild=False, *, cache_calls=False):
    parser_path = Path(sys.modules['source_index'].__file__)
    fingerprint = hashlib.sha256(Path(__file__).read_bytes() + parser_path.read_bytes()).hexdigest()
    tables = ('sites', 'ingress', 'calls', 'definitions', 'native_members', 'files', 'meta',
              'call_resolution', 'call_resolution_state')
    with db:
        db.execute('BEGIN IMMEDIATE')
        if db.execute('PRAGMA user_version').fetchone()[0] != SCHEMA_VERSION:
            for table in tables:
                db.execute(f'DROP TABLE IF EXISTS {table}')
            for statement in SCHEMA.split(';'):
                if statement.strip():
                    db.execute(statement)
            db.execute(f'PRAGMA user_version = {SCHEMA_VERSION}')
        current = db.execute('SELECT fingerprint FROM meta').fetchone()
        if rebuild or current != (fingerprint,):
            for table in tables:
                db.execute(f'DELETE FROM {table}')
            db.execute('INSERT INTO meta VALUES (?)', (fingerprint,))
        prior = dict(db.execute('SELECT source, sha256 FROM files'))
        indexed = reused = 0
        for path in sorted((root / 'src').rglob('*')):
            if path.suffix not in ('.c', '.cpp', '.h', '.hpp'):
                continue
            source = path.relative_to(root).as_posix()
            raw = path.read_bytes()
            digest = hashlib.sha256(raw).hexdigest()
            if prior.pop(source, None) == digest:
                reused += 1
                continue
            text = raw.decode('utf-8-sig')
            sites, ingress, calls, definitions = summarize(text, source)
            members = native_members(text, source)
            for table in ('sites', 'ingress', 'calls', 'definitions', 'native_members'):
                db.execute(f'DELETE FROM {table} WHERE source = ?', (source,))
            db.execute('INSERT OR REPLACE INTO files VALUES (?, ?)', (source, digest))
            db.executemany('INSERT INTO sites VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)', sites)
            db.executemany('INSERT INTO ingress VALUES (?, ?, ?, ?, ?, ?, ?, ?)', ingress)
            db.executemany('INSERT INTO calls VALUES (?, ?, ?, ?)', calls)
            db.executemany('INSERT INTO definitions VALUES (?, ?)', definitions)
            db.executemany('INSERT INTO native_members VALUES (?, ?, ?, ?, ?, ?)', members)
            indexed += 1
        for source in prior:
            for table in ('sites', 'ingress', 'calls', 'definitions', 'native_members', 'files'):
                db.execute(f'DELETE FROM {table} WHERE source = ?', (source,))
        if indexed or prior:
            db.execute('DELETE FROM call_resolution')
            db.execute('DELETE FROM call_resolution_state')
        state = {'indexed_files': indexed, 'reused_files': reused}
        if cache_calls:
            cached = db.execute('SELECT 1 FROM call_resolution_state').fetchone() is not None
            if not cached:
                bindings, truncated = bound_parameters(db, None)
                db.executemany('INSERT INTO call_resolution VALUES (?, ?, ?, ?)', (
                    (symbol, parameter, address, json.dumps(witness, separators=(',', ':')))
                    for (symbol, parameter), addresses in bindings.items()
                    for address, witness in addresses.items()
                ))
                db.execute('INSERT INTO call_resolution_state VALUES (?)', (truncated,))
            state['call_cache'] = 'reused' if cached else 'rebuilt'
    return state


def reachable(db, root):
    if root is None:
        return None
    graph = defaultdict(set)
    for caller, callee in db.execute('SELECT DISTINCT caller, callee FROM calls'):
        graph[caller].add(callee)
    seen, queue = {root}, deque([root])
    while queue:
        for callee in graph[queue.popleft()] - seen:
            seen.add(callee)
            queue.append(callee)
    return seen


def bound_parameters(db, scope):
    routes, by_target = defaultdict(list), defaultdict(dict)
    ambiguous = {symbol for symbol, in db.execute(
        'SELECT symbol FROM definitions GROUP BY symbol HAVING count(*) > 1'
    )}
    needed = {(symbol, parameter) for symbol, parameter in db.execute(
        'SELECT DISTINCT symbol, parameter FROM sites WHERE parameter IS NOT NULL'
    ) if scope is None or symbol in scope}
    seeds, parents = [], defaultdict(set)
    for source, caller, line, callee, parameter, address, parent, delta in db.execute(
        'SELECT * FROM ingress ORDER BY source, line, callee, parameter'
    ):
        if caller in ambiguous or callee in ambiguous or (scope is not None and caller not in scope):
            continue
        edge = {'source': source, 'caller': caller, 'line': line,
                'callee': callee, 'argument': parameter + 1}
        target = callee, parameter
        if address is not None:
            seeds.append((target, address, edge))
        else:
            origin = caller, parent
            parents[target].add(origin)
            routes[origin].append((target, delta, edge))
    pending = deque(needed)
    while pending:
        for origin in parents[pending.popleft()] - needed:
            needed.add(origin)
            pending.append(origin)

    queue = deque()
    seen = set()
    truncated = False

    def enqueue(target, address, witness):
        nonlocal truncated
        key = target, address
        if target not in needed or key in seen or not 0 <= address <= 0xffffffff:
            return
        if len(witness) > 8 or len(seen) >= 100000:
            truncated = True
            return
        seen.add(key)
        queue.append((target, address, witness))

    for target, address, edge in seeds:
        enqueue(target, address, [edge])
    while queue:
        target, address, witness = queue.popleft()
        by_target[target][address] = witness
        for next_target, delta, edge in routes[target]:
            enqueue(next_target, address + delta, [*witness, edge])
    return by_target, truncated


def read_call_cache(db):
    state = db.execute('SELECT truncated FROM call_resolution_state').fetchone()
    if state is None:
        return bound_parameters(db, None)
    bindings = defaultdict(dict)
    for symbol, parameter, address, witness in db.execute('SELECT * FROM call_resolution ORDER BY rowid'):
        bindings[symbol, parameter][address] = json.loads(witness)
    return bindings, bool(state[0])


def classify_order(db, exact):
    # use only standalone accesses before the first control barrier
    ambiguous = {tuple(row) for row in db.execute(
        'SELECT source, symbol FROM definitions GROUP BY source, symbol HAVING count(*) > 1'
    )}
    writes = defaultdict(list)
    for source, symbol, sequence, lo, hi in db.execute(
        "SELECT source, symbol, sequence, lo, hi FROM sites "
        "WHERE direction = 'w' AND sequence IS NOT NULL AND operation IN ('w_u8','w_u16','w_u32')"
    ):
        writes[source, symbol].append((sequence, lo, hi))
    for item in exact:
        sequence = item.pop('sequence')
        if item['direction'] != 'r':
            continue
        key = item['source'], item['symbol']
        if sequence is None or key in ambiguous:
            item['order'] = 'unresolved'
            continue
        prior = set()
        for earlier, lo, hi in writes[key]:
            if earlier < sequence:
                prior.update(range(max(lo, item['lo']), min(hi, item['hi'])))
        item['order'] = ('covered_by_prior_exact_writes' if len(prior) == item['hi'] - item['lo']
                         else 'no_prior_exact_write_for_some_bytes')


def bound_span(operation, address, displacement, width):
    api = 'psx_addr' if operation in ('memcpy', 'memmove', 'memset') else operation
    start = guest_address(address + displacement, api)
    return start, span_end(start, width)


def select_sites(db, lo, hi, scope, bindings, function=None, direction=None, unresolved=False):
    condition = ('hi IS NULL AND direction != \'anchor\'' if unresolved else
                 '((lo < ? AND hi > ?) OR hi IS NULL)')
    params = [] if unresolved else [hi, lo]
    if function:
        condition += ' AND symbol = ?'
        params.append(function)
    if direction:
        condition += ' AND direction = ?'
        params.append(direction)
    rows = db.cursor()
    rows.row_factory = sqlite3.Row
    rows.execute(f'SELECT * FROM sites WHERE {condition} ORDER BY source, line, rowid', params)
    exact, pending, anchors, bound = [], [], [], []
    for row in rows:
        if scope is not None and row['symbol'] not in scope:
            continue
        info = {key: row[key] for key in ('source', 'symbol', 'line', 'operation', 'direction', 'expression')}
        start, end = row['lo'], row['hi']
        if start is not None and end is not None:
            if start < hi and end > lo:
                exact.append(dict(info, lo=start, hi=end, sequence=row['sequence']))
            continue
        if row['direction'] != 'anchor':
            pending.append(dict(info, known_start=start))
        if start is not None and lo <= start < hi:
            anchors.append(dict(info, lo=start))
        if row['parameter'] is not None:
            for address, chain in bindings.get((row['symbol'], row['parameter']), {}).items():
                start, end = bound_span(row['operation'], address, row['displacement'], row['width'])
                if end is None and row['width'] is None and start is not None and lo <= start < hi:
                    anchors.append(dict(info, lo=start, extent='unknown', call_chain=chain))
                if end is None or start >= hi or end <= lo:
                    continue
                spans = [(start, end)] if row['byte_mask'] is None else [
                    (start + lane, start + lane + 1)
                    for lane in range(4) if row['byte_mask'] & (1 << lane)
                ]
                for first, last in spans:
                    if first < hi and last > lo:
                        bound.append(dict(info, lo=first, hi=last, call_chain=chain, order='unresolved'))
    return exact, pending, anchors, bound


def dependency_writers(db, lo, hi, scope, bindings):
    exact, pending, _, bound = select_sites(db, lo, hi, scope, bindings, direction='w')
    for writer in exact:
        writer.pop('sequence')
    return exact + bound, len(pending)


def ownership_regions(reads, writes, lo, hi):
    events = defaultdict(list)
    for direction, sites in enumerate((reads, writes)):
        for site in sites:
            owner = site['source'], site['symbol'], 'call_chain' in site
            events[max(lo, site['lo'])].append((direction, owner, 1))
            events[min(hi, site['hi'])].append((direction, owner, -1))
    active = [Counter(), Counter()]
    previous = lo
    for address in sorted(events):
        readers, writers = active
        if address > previous and (readers or writers):
            owners = []
            for group in active:
                counts = defaultdict(Counter)
                for (source, symbol, inferred), count in group.items():
                    counts[source, symbol]['via_call' if inferred else 'exact'] += count
                owners.append([
                    dict(source=source, symbol=symbol, exact=count['exact'], via_call=count['via_call'])
                    for (source, symbol), count in sorted(counts.items())
                ])
            if not readers:
                category = 'writes_only'
            elif not writers:
                category = 'reads_without_indexed_writer'
            elif len(owners[1]) > 1:
                category = 'multiple_writer_functions'
            else:
                category = 'one_writer_function'
            yield category, dict(lo=previous, hi=address, readers=owners[0], writers=owners[1])
        for direction, owner, delta in events[address]:
            active[direction][owner] += delta
            if active[direction][owner] == 0:
                del active[direction][owner]
        previous = address


def unresolved_summary(db, scope, functions, bindings, limit):
    rows = db.execute(
        "SELECT source, symbol, direction, line, operation, parameter, displacement, width "
        "FROM sites WHERE hi IS NULL AND direction != 'anchor' ORDER BY source, line, rowid"
    )
    grouped = {}
    for source, symbol, direction, line, operation, parameter, delta, width in rows:
        if (scope is not None and symbol not in scope) or (functions and symbol not in functions):
            continue
        group = grouped.setdefault((source, symbol, direction), dict(
            source=source, symbol=symbol, direction=direction, count=0, first_line=line,
            without_candidate=0, first_without_candidate_line=None,
        ))
        group['count'] += 1
        if not any(bound_span(operation, address, delta, width)[1] is not None
                   for address in bindings.get((symbol, parameter), ())):
            group['without_candidate'] += 1
            if group['first_without_candidate_line'] is None:
                group['first_without_candidate_line'] = line
    groups = sorted(grouped.values(), key=lambda group: (
        -group['without_candidate'], -group['count'], group['source'], group['symbol'], group['direction']
    ))
    return dict(count=sum(group['count'] for group in groups),
                without_candidate=sum(group['without_candidate'] for group in groups),
                group_count=len(groups), groups=groups[:limit])


def triage(db, args, ranges, scope, resolution):
    bindings, truncated = resolution
    functions = set(args.function)
    reports = []
    categories = ('reads_without_indexed_writer', 'multiple_writer_functions',
                  'one_writer_function', 'writes_only')
    for lo, hi in ranges:
        reads, _, _, bound_reads = select_sites(db, lo, hi, scope, bindings, direction='r')
        writes, _, _, bound_writes = select_sites(db, lo, hi, scope, bindings, direction='w')
        readers = [site for site in reads + bound_reads if not functions or site['symbol'] in functions]
        regions = {category: [] for category in categories}
        counts = dict.fromkeys(categories, 0)
        sizes = dict.fromkeys(categories, 0)
        for category, region in ownership_regions(readers, writes + bound_writes, lo, hi):
            if functions and category == 'writes_only':
                continue
            counts[category] += 1
            sizes[category] += region['hi'] - region['lo']
            if len(regions[category]) < args.limit:
                regions[category].append(region)
        _, _, anchors, _ = select_sites(db, lo, hi, scope, bindings)
        call_anchors = [site for site in anchors if 'call_chain' in site and
                        (not functions or site['symbol'] in functions)]
        reports.append(dict(address_range=[lo, hi], region_counts=counts, byte_counts=sizes, regions=regions,
                            call_anchors=call_anchors[:args.limit], call_anchors_count=len(call_anchors)))
    return dict(ranges=reports, call_search_truncated=truncated,
                unresolved_by_function=unresolved_summary(db, scope, functions, bindings, args.limit),
                source_sha256=hashlib.sha256(json.dumps(list(db.execute(
                    'SELECT source, sha256 FROM files ORDER BY source'
                )), separators=(',', ':')).encode('utf-8')).hexdigest())


def query(db, args, *, lo=None, hi=None, scope=None, resolution=None, writer_cache=None):
    if lo is None:
        lo, hi = BASE + args.offset, BASE + args.offset + args.size
    if resolution is None:
        scope = reachable(db, args.root)
        resolution = ({}, False) if args.unresolved else bound_parameters(db, scope)
    bindings, truncated = resolution
    direction = 'r' if args.reads else 'w' if getattr(args, 'writes', False) else None
    exact, pending, anchors, bound = select_sites(
        db, lo, hi, scope, bindings, args.function, direction, args.unresolved)
    result = {'unresolved_count': len(pending)}
    if args.unresolved:
        return dict(result, unresolved=pending[:args.limit])
    classify_order(db, exact)
    if args.reads_before_write:
        exact = [site for site in exact if site['order'] != 'covered_by_prior_exact_writes']
    call_anchors = [site for site in anchors if 'call_chain' in site]
    literal_anchors = [site for site in anchors if 'call_chain' not in site]
    for label, items in (('exact_spans', exact), ('call_candidates', bound), ('literal_anchors', literal_anchors),
                         ('call_anchors', call_anchors)):
        result[label] = items[:args.limit]
        result[label + '_count'] = len(items)
    result['call_search_truncated'] = truncated
    if getattr(args, 'dependencies', False):
        key = lo, hi
        if writer_cache is not None and key in writer_cache:
            writers, pending_count = writer_cache[key]
        else:
            writers, pending_count = dependency_writers(db, lo, hi, scope, bindings)
            if writer_cache is not None:
                writer_cache[key] = writers, pending_count
        result['unresolved_writer_count'] = pending_count
        result['read_dependencies'] = []
        for reader in exact[:args.limit] + bound[:args.limit]:
            matches = []
            for writer in writers:
                start = max(reader['lo'], writer['lo'], lo)
                end = min(reader['hi'], writer['hi'], hi)
                if start < end:
                    matches.append({'writer': writer, 'overlap': [start, end]})
            result['read_dependencies'].append({
                'reader': reader, 'overlapping_writer_count': len(matches),
                'overlapping_writers': matches[:args.limit],
            })
    return result


def number(value):
    return int(value, 0)


def address_range(address, size):
    start = BASE + address if 0 <= address < END - BASE else guest_address(address)
    end = span_end(start, size)
    if end is None:
        raise ValueError('provide a scratchpad offset or guest RAM address and a size inside that region')
    return start, end


def write_report(output, report):
    payload = json.dumps(report, indent=2) + '\n'
    if output is None:
        print(payload, end='')
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = None
    try:
        with NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n', dir=output.parent,
                                prefix=output.name + '.', delete=False) as stream:
            temp = Path(stream.name)
            stream.write(payload)
        temp.replace(output)
    finally:
        if temp is not None:
            temp.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', help='project root or xport-project.json')
    parser.add_argument('address', type=number, nargs='?')
    parser.add_argument('size', type=number, nargs='?')
    parser.add_argument('--range', action='append', metavar='ADDRESS:SIZE', default=[],
                        help='batch multiple ranges in one refresh and call search')
    parser.add_argument('--inventory', action='store_true', help='search all DRAM and scratchpad addresses')
    parser.add_argument('--native-field', action='append', default=[], metavar='OBJECT.MEMBER',
                        help='query cached lexical native-field producers/consumers and ancestor loans; repeat to batch')
    parser.add_argument('--function', action='append', default=[], metavar='SYMBOL',
                        help='restrict readers or listed accesses; repeat to batch multiple functions')
    parser.add_argument('--root', help='restrict candidates to this named call tree')
    parser.add_argument('--unresolved', action='store_true')
    parser.add_argument('--output', type=Path, help='atomically replace a JSON report instead of writing to stdout')
    direction = parser.add_mutually_exclusive_group()
    direction.add_argument('--reads', action='store_true')
    direction.add_argument('--writes', action='store_true')
    direction.add_argument('--dependencies', action='store_true',
                           help='for each read, list overlapping writer candidates, including other functions')
    direction.add_argument('--reads-before-write', action='store_true',
                        help='include reads not covered by known earlier writes in the same straight-line prefix')
    direction.add_argument('--triage', action='store_true',
                           help='group residual accesses by byte-range owners and unresolved function')
    parser.add_argument('--limit', type=int, default=40, help='maximum rows per list; 0 returns counts only')
    parser.add_argument('--rebuild', action='store_true', help='reindex all files before running the query')
    parser.add_argument('--refresh-only', action='store_true', help='update source and call caches without querying')
    parser.add_argument('--summary', action='append', default=[], metavar='KEY',
                        help='validate a checked summary and its dependencies without refreshing the access index')
    parser.add_argument('--summary-put', type=Path, metavar='JSON',
                        help='atomically import scoped checked summaries with exact input hashes')
    parser.add_argument('--worklist', type=Path, metavar='PLAN.json',
                        help='rank analysis_worklist jobs from an existing plan using checked summaries')
    args = parser.parse_args()
    root, config = load_project(args.project)
    cache_path = root / config.get('paths', {}).get('status', 'status') / 'refactoring/memory-access.sqlite'
    if args.summary or args.summary_put or args.worklist:
        if args.worklist and (args.summary or args.summary_put):
            parser.error('--worklist cannot be combined with summary query/import')
        if (args.address is not None or args.size is not None or args.range or args.inventory or args.native_field
                or args.function or args.root or args.unresolved or args.reads or args.writes or args.dependencies
                or args.reads_before_write or args.triage or args.rebuild or args.refresh_only):
            parser.error('summary operations cannot be combined with access-index operations')
        if args.output is not None and (args.output.suffix.lower() != '.json'
                                       or args.output.resolve() == cache_path.resolve()):
            parser.error('--output requires a .json path outside the SQLite cache')
        from analysis_summaries import SummaryStore, analysis_worklist
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with closing(sqlite3.connect(cache_path, timeout=20)) as db:
                store = SummaryStore(db, root)
                if args.worklist:
                    plan = json.loads(args.worklist.read_text(encoding='utf-8-sig'))
                    result = analysis_worklist(store, plan['analysis_worklist'])
                    db.commit()
                    write_report(args.output, dict(result, parsed_files=store.parsed_files))
                    return
                imported = store.put_many(json.loads(args.summary_put.read_text(encoding='utf-8'))) if args.summary_put else []
                results = [store.get(key) for key in args.summary] or imported
                db.commit()
                if args.output is None:
                    for result in results:
                        if 'record' in result:
                            record = result['record']
                            result['record'] = {k: v for k, v in record.items() if k not in ('inputs', 'output')}
                            result['record']['input_count'] = len(record['inputs'])
                write_report(args.output, {'summaries': results, 'parsed_files': store.parsed_files,
                                          'limits': 'Freshness checks only declared inputs; it is not semantic proof'})
        except (ValueError, KeyError, TypeError, OSError) as error:
            parser.error(str(error))
        return
    if args.limit < 0:
        parser.error('--limit must be nonnegative')
    if args.size is not None and args.address is None:
        parser.error('size requires an address')
    if args.refresh_only and (args.address is not None or args.range or args.inventory or args.native_field or args.function
                              or args.root or args.unresolved or args.reads or args.writes
                              or args.dependencies or args.reads_before_write or args.triage):
        parser.error('--refresh-only cannot be combined with a query')
    if args.triage and args.unresolved:
        parser.error('--triage already includes an unresolved inventory')
    if args.output is not None and (args.output.suffix.lower() != '.json' or args.output.resolve() == cache_path.resolve()):
        parser.error('--output requires a .json path outside the SQLite cache')
    if args.inventory and (args.address is not None or args.range):
        parser.error('--inventory cannot be combined with explicit ranges')
    if args.native_field and (args.address is not None or args.range or args.inventory or args.root
                              or args.unresolved or args.reads_before_write or args.triage):
        parser.error('--native-field cannot be combined with address, root, unresolved or triage queries')
    inventory = args.inventory or ((args.function or args.triage) and args.address is None and not args.range)
    try:
        ranges = [(0, DRAM_SIZE), (BASE, END)] if inventory else [
            address_range(*map(number, item.split(':'))) for item in args.range]
        if args.address is not None or not ranges:
            ranges.insert(0, address_range(args.address or 0, 4 if args.size is None else args.size))
    except (ValueError, TypeError) as error:
        parser.error(str(error))
    args.reads = args.reads or args.reads_before_write or args.dependencies
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    with closing(sqlite3.connect(cache_path, timeout=20)) as db:
        state = refresh(db, root, args.rebuild, cache_calls=not (args.unresolved or args.root or args.native_field))
        if args.refresh_only:
            write_report(args.output, state)
            return
        db.execute('BEGIN')
        for symbol in (args.root, *args.function):
            if symbol and not db.execute('SELECT 1 FROM definitions WHERE symbol = ?', (symbol,)).fetchone():
                parser.error(f'no indexed definition for {symbol}')
        if args.native_field:
            try:
                result = native_field_query(db, args.native_field, limit=args.limit, symbols=args.function,
                                            reads=args.reads and not args.dependencies, writes=args.writes,
                                            dependencies=args.dependencies)
            except ValueError as error:
                parser.error(str(error))
            write_report(args.output, {**state, **result})
            return
        scope = reachable(db, args.root)
        if args.unresolved:
            resolution = {}, False
        elif scope is None:
            resolution = read_call_cache(db)
        else:
            resolution = bound_parameters(db, scope)
        if args.triage:
            result = triage(db, args, ranges, scope, resolution)
            write_report(args.output, {**state, **result, 'limits': LIMITS})
            return
        if args.unresolved:
            ranges = ranges[:1]
        functions = list(dict.fromkeys(args.function)) or [None]
        reports, writer_cache = {}, {}
        for symbol in functions:
            query_args = argparse.Namespace(**(vars(args) | {'function': symbol}))
            results = [
                dict(query(db, query_args, lo=lo, hi=hi, scope=scope, resolution=resolution,
                           writer_cache=writer_cache), address_range=[lo, hi])
                for lo, hi in ranges]
            reports[symbol] = results[0] if len(results) == 1 else {'ranges': results}
    result = next(iter(reports.values())) if len(reports) == 1 else {'functions': reports}
    write_report(args.output, {**state, **result, 'limits': LIMITS})


if __name__ == '__main__':
    main()
