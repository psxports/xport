"""Deferred failure-injection checks using the consuming project's capture profile"""
import argparse
import json
from pathlib import Path
import struct
import tempfile
from trace_actor_diff import RECORD_SIZE
from trace_cache import digest
from trace_hash_session import release_under
from trace_layout import PROFILE,PHASE_BYTES
from trace_phase_index import build
from trace_prefix import compare_channels
from xport_project import artifact_path


def fixtures(folder):
    folder=Path(folder);folder.mkdir(parents=True,exist_ok=True)
    native={k:str(folder/('native.'+k)) for k in ('actors','world','inputs','phases')}
    original={k:str(folder/('original.'+k)) for k in native}
    def envelope(kind,tick,payload):return struct.pack('<3I',kind,tick,len(payload))+payload
    header=struct.pack('<3I',0x31504646,3,PHASE_BYTES);phases=bytearray(header);actors=bytearray();world=bytearray();inputs=[]
    pools=PROFILE['world_a_size']+PROFILE['world_b_size']+PROFILE['world_c_size']+8
    for tick in range(8):
        payload=bytearray(PHASE_BYTES);struct.pack_into('<8I',payload,0,PROFILE['game_begin'],tick,0,0,0,0,0,0)
        phases.extend(envelope(15,tick,payload));phases.extend(envelope(17,tick,struct.pack('<2I',PROFILE['game_begin'],0)))
        actors.extend(struct.pack('<I',tick)+bytes(RECORD_SIZE-4));world.extend(struct.pack('<2I',tick,0)+bytes(pools)+struct.pack('<I',0));inputs.append([tick,0,0])
    footer=envelope(5,7,struct.pack('<I',1))
    Path(original['phases']).write_bytes(phases+footer);Path(native['phases']).write_bytes(phases)
    for side in (original,native):
        Path(side['actors']).write_bytes(actors);Path(side['world']).write_bytes(world)
    Path(original['inputs']).write_text(json.dumps(inputs));Path(native['inputs']).write_bytes(b''.join(struct.pack('<3I',*row) for row in inputs[:-1]))
    sha=digest(original['phases']);build(original['phases'],folder/'phase-index',sha)
    spec=dict(original=original,native=native,phase_index=str(folder/'phase-index/index.json'),raw_sha256=sha,
        contract=dict(schema=2,phase_begin=0,phase_end=8,segments=[dict(stage=0,start_phase=0,end_phase=8,start_tick=0,end_tick=7)]))
    (folder/'fixture.json').write_text(json.dumps(spec,indent=2));return spec,pools


def run():
    root=artifact_path('status/toolset-validation');root.mkdir(parents=True,exist_ok=True);results=[]
    with tempfile.TemporaryDirectory(prefix='minimization-',dir=root) as tmp:
        try:
            spec,pools=fixtures(tmp);native=spec['native'];baseline=Path(native['world']).read_bytes()
            data=bytearray(baseline);data[2*(12+pools)+8+pools-8]=1;Path(native['world']).write_bytes(data)
            report=compare_channels(spec,native,0,8)
            assert report['first_difference']['channel']=='world' and report['first_difference']['ordinal']==2
            assert report['passed'] is False and report['verified_prefix_end']==2
            results.append('Earlier world difference wins over late termination')
            Path(native['world']).write_bytes(baseline)
            report=compare_channels(spec,native,0,8)
            assert report['first_difference'] is None and report['passed'] is False and report['verified_prefix_end']==7
            results.append('Matching prefix without footer never grants MATCH')
            actors=Path(native['actors']).read_bytes();Path(native['actors']).write_bytes(actors[:3*RECORD_SIZE+11])
            report=compare_channels(spec,native,0,8)
            assert report['channels']['actors']['problem'] and report['verified_prefix_end']<=3
            results.append('Truncated actor tail limits checkpoint selection')
            Path(native['actors']).write_bytes(actors)
            from trace_world_index import seek
            from trace_bundle import worlds
            assert list(seek(spec['original']['world'],5))==list(worlds(spec['original']['world']))[5:]
            results.append('Indexed world seek equals sequential decode')
        finally:
            release_under(tmp)
    return dict(status='passed',checks=results,scope='Synthetic prefix/seek checks only; real record/converge, ABI/reset/cleanup/permissions and timings still required')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run',action='store_true');a=p.parse_args()
    if not a.run:p.error('Use --run only when functional tests are authorized')
    result=run();(artifact_path('status/toolset-validation')/'minimization-checks.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))


if __name__=='__main__':main()
