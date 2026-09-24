"""Validate the required emulator capture profile without guessing game addresses"""
import argparse
import configparser
import json
from pathlib import Path
from xport_project import load_project


def validate(path):
    schema = json.loads((Path(__file__).parent/'duckstation/profile-schema.json').read_text())
    ini = configparser.ConfigParser()
    ini.optionxform = str
    with Path(path).open(encoding='utf-8-sig') as stream:
        ini.read_file(stream)
    section = ini['XportTrace']
    optional = schema.get('optional_u32', {})
    allowed = {'version', *schema['required_u32'], *optional}
    if not {'version', *schema['required_u32']} <= set(section) or set(section) - allowed:
        raise ValueError('Missing or unknown trace profile fields')
    values = {k:int(v, 10) for k,v in section.items()}
    if values.pop('version') != 1 or any(not 0 < v <= 0xffffffff for v in values.values()):
        raise ValueError('Invalid trace profile version or values')
    for key, default in optional.items():
        values.setdefault(key, default)
    offsets = {'gpu_pool_low','gpu_pool_split','gpu_pool_end'}
    limits = {'world_max_count','gpu_phase_mask','gpu_full_ram'}
    for key,value in values.items():
        if key == 'ot_end':
            continue
        if key in offsets or key in limits or key.endswith('_size'):
            if value > 0x200000:
                raise ValueError('Trace profile bound exceeds PS1 RAM: '+key)
        elif not 0x80000000 <= value < 0x80200000 or value % (2 if key=='pad_buttons' else 4):
            raise ValueError('Invalid RAM address: '+key)
    if not values['gpu_pool_low'] < values['gpu_pool_split'] < values['gpu_pool_end'] <= 0x200000:
        raise ValueError('Invalid GPU pool bounds')
    if values['ot_end'] > 0x200000 and values['ot_end'] != 0xffffff:
        raise ValueError('Invalid ordering-table terminator')
    if not 1 <= values['gpu_phase_mask'] <= 15:
        raise ValueError('Invalid GPU phase mask')
    if values['gpu_full_ram'] not in (0, 1):
        raise ValueError('Invalid GPU address scope')
    if values['world_max_count']*values['world_item_size'] > 0x200000:
        raise ValueError('Dynamic world range exceeds RAM')
    for key in ('actors','camera','menu_state','world_a','world_b','world_c'):
        if values[key]+values[key+'_size'] > 0x80200000:
            raise ValueError('Capture range exceeds RAM: '+key)
    hooks = ['game_begin', 'game_input_result', 'input_inject', 'sound_three_a', 'sound_three_b', 'sound_one', 'sound_two', 'menu_phase', 'sequence_phase', 'input_decode', 'aux_phase']
    if len({values[key] for key in hooks}) != len(hooks):
        raise ValueError("Duplicate trace hook PCs")
    if values["world_dynamic"]+values["world_max_count"]*values["world_item_size"] > 0x80200000:
        raise ValueError("Dynamic range exceeds RAM")
    if any(values[key]+values["pad_size"] > 0x80200000 for key in ("pad_first", "pad_second")):
        raise ValueError("Controller range exceeds RAM")
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path)
    args = parser.parse_args()
    root, config = load_project()
    path = args.profile or root/config['duckstation']['trace_profile']
    values = validate(path)
    print(json.dumps({'profile':str(path), 'validated_fields':len(values),
                      'scope':'Syntactic memory/hook bounds; semantic correctness requires MIPS and capture evidence'}))


if __name__ == '__main__':
    main()
