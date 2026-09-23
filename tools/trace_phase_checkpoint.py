"""Select only compatible checkpoints backed by a passing phase comparison"""
from trace_layout import PHASE_PCS
import json
from pathlib import Path
import struct
from trace_cache import digest


def context(path):
    data = Path(path).read_bytes()
    if len(data) not in (40, 48):
        raise ValueError('Invalid phase checkpoint context size')
    values = struct.unpack('<'+'I'*(len(data)//4), data)
    if values[0] != (0x31504346 if len(data)==40 else 0x32504346) or values[1] not in PHASE_PCS:
        raise ValueError('Invalid phase checkpoint context')
    result = {'pc':values[1], 'ordinal':values[2], 'input_ordinal':values[3], 'connected':values[4],
            'sequence_old_stage':values[5], 'active':values[6], 'title':values[7], 'pad':values[8], 'menu_job':values[9]}
    if len(values)==12:
        result['highscore_saved'] = list(values[10:12])
    return result


def verified_end(report):
    if report.get('axis') != 'phase_ordinal':
        raise ValueError('Phase checkpoints lack passing comparison evidence')
    if report.get('passed') is True:
        return report['end_tick']
    end = report.get('verified_prefix_end')
    if (report.get('outcome') != 'mismatch' or type(end) is not int or
            end != report.get('first_difference', {}).get('ordinal') or
            not report['start_tick'] <= end < report['end_tick'] or
            any(report.get(side, {}).get('complete') is not True
                for side in ('original_validation', 'native_validation'))):
        raise ValueError('Phase checkpoints lack passing comparison evidence')
    return end


def select(index, source_sha, exe_sha, target):
    if index.get('version') != 1 or index['raw_sha256'] != source_sha or index['exe_sha256'] != exe_sha:
        raise ValueError('Phase checkpoint index is incompatible')
    if digest(index['comparison_report']) != index['comparison_sha256']:
        raise ValueError('Phase checkpoint evidence changed')
    report = json.loads(Path(index['comparison_report']).read_text())
    end = verified_end(report)
    if not report['start_tick'] <= target < report['end_tick']:
        raise ValueError('Requested phase lies outside the proven interval')
    choices = [row for row in index['checkpoints']
               if report['start_tick'] <= row['ordinal'] <= target and row['ordinal'] < end]
    if not choices:
        raise ValueError('No verified checkpoint precedes the requested phase')
    row = max(choices, key=lambda item:item['ordinal'])
    if digest(row['checkpoint']) != row['checkpoint_sha256'] or digest(row['context']) != row['context_sha256']:
        raise ValueError('Phase checkpoint changed')
    actual = context(row['context'])
    if any(row[key] != value for key,value in actual.items()):
        raise ValueError('Phase checkpoint metadata differs from context')
    return row
