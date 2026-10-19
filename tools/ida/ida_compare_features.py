"""Export structural comparison evidence without assigning cross-version names."""
import hashlib
import ida_bytes
import ida_funcs
import ida_gdl
import ida_idaapi
import ida_segment
import idautils
import idc


def export_features(records, refs, symbols):
    functions = []
    for row in records:
        fn = ida_funcs.get_func(row['address'])
        blocks = []
        if fn:
            for block in ida_gdl.FlowChart(fn):
                blocks.append(dict(start=block.start_ea, end=block.end_ea,
                                   successors=sorted(b.start_ea for b in block.succs())))
        functions.append(dict(address=row['address'], blocks=blocks))
    targets = set(r['target'] for r in refs)
    targets.update(r['address'] for r in symbols if r['kind'] != 'code')
    for start in idautils.Segments():
        seg = ida_segment.getseg(start)
        targets.update(ea for ea in idautils.Heads(start, seg.end_ea)
                       if ida_bytes.is_data(ida_bytes.get_full_flags(ea)))
    data = {}
    for target in sorted(targets):
        if ida_bytes.is_code(ida_bytes.get_full_flags(target)):
            continue
        mapped = ida_bytes.is_mapped(target)
        head = ida_bytes.get_item_head(target) if mapped else target
        if head == ida_idaapi.BADADDR:
            head = target
        size = ida_bytes.get_item_size(head) if mapped else None
        raw = ida_bytes.get_bytes(head, size) if mapped and size else None
        data[head] = dict(address=head, size=size, mapped=bool(mapped),
                          initialized=raw is not None, name=idc.get_name(head),
                          sha256=hashlib.sha256(raw).hexdigest() if raw is not None else None,
                          boundary='IDA_item' if mapped else 'unmapped_reference_only')
    return dict(schema=1, functions=functions, data=list(data.values()))
