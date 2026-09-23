"""Dispatch validated comparison contracts without conflating frames and phase ordinals"""
from trace_bundle import validate_bundle, compare_bundles
from trace_phases import iter_phases, compare_streams


def validate(spec, paths):
    if spec.get('comparison_kind', 'gameplay') == 'gameplay':
        return validate_bundle(paths, spec['start_tick'], spec['end_tick'])
    if spec['comparison_kind'] != 'phases':
        raise ValueError('Unknown comparison contract')
    begin = spec.get('phase_start', 0)
    count = 0
    for row in iter_phases(paths['phases'], sound=spec.get('phase_sound', False), gpu=spec.get('phase_gpu', False)):
        if row['emulated_ticks'] is None or row['ordinal'] >= begin:
            count += 1
    if spec['start_tick'] != begin or count != spec['end_tick']-begin or not count:
        raise ValueError('Phase count differs from ordinal contract')
    return {'complete': True, 'boundaries': count, 'axis': 'phase_ordinal'}


def compare_capture(spec, original, native, schedule=None):
    if spec.get('comparison_kind', 'gameplay') == 'gameplay':
        return compare_bundles(original, native, spec['start_tick'], spec['end_tick'],
                               spec.get('raw_sprite_rgb', False), schedule=schedule)
    result = {'passed': False, 'partial': True, 'axis': 'phase_ordinal',
              'start_tick': spec['start_tick'], 'end_tick': spec['end_tick'],
              'scope': 'Ordered execution boundaries and selected phase state; GPU, sound, pixels, PCM and interval tail not compared'}
    try:
        result['original_validation'] = validate(spec, original)
        result['native_validation'] = validate(spec, native)
        sound = spec.get('phase_sound', False)
        gpu = spec.get('phase_gpu', False)
        begin = spec.get('phase_start', 0)
        result.update(compare_streams(original['phases'], native['phases'], begin=begin, sound=sound, gpu=gpu))
        if result.get('first_raw_gpu_difference'):
            result['first_raw_gpu_difference']['ordinal'] += begin
        if result.get('first_difference'):
            result['first_difference']['ordinal'] += begin
            field = result['first_difference']['field']
            result['first_difference'].update(channel='phase_'+field if field in ('sound', 'gpu') else 'phase_state', tick=result['first_difference']['ordinal'])
        result['outcome'] = 'match' if result['passed'] else 'mismatch'
        result['verified_prefix_end'] = (spec['end_tick'] if result['passed'] else
                                         result['first_difference']['ordinal'])
    except (OSError, ValueError) as error:
        result.update(outcome='invalid_trace', error=str(error))
    return result
