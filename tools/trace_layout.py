"""Resolve transport lengths and hook identities from the selected project profile"""
from xport_project import load_project, activate_adapters
from trace_profile import validate

activate_adapters()
ROOT, CONFIG = load_project()
PROFILE_PATH = ROOT/CONFIG['duckstation']['trace_profile']
PROFILE = validate(PROFILE_PATH)
PHASE_PCS = tuple(PROFILE[key] for key in ('game_begin','menu_phase','sequence_phase','aux_phase'))
ACTOR_BYTES = PROFILE['actors_size']+PROFILE['camera_size']
PHASE_BYTES = 32+PROFILE['menu_state_size']+ACTOR_BYTES
INPUT_CALL_BYTES = 24+PROFILE['pad_size']
SOUND_BYTES = {PROFILE[key]:size for key,size in
               [('sound_three_a',16),('sound_three_b',16),('sound_one',8),('sound_two',12)]}
