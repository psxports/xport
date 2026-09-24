"""Partition gameplay boundaries into monotonic stage segments"""


def stage_segments(game):
    segments=[]
    for row in game:
        if not segments or row['stage']!=segments[-1]['stage'] or row['game_tick']<segments[-1]['end_tick']:
            segments.append(dict(stage=row['stage'],start_phase=row['ordinal'],start_tick=row['game_tick'],
                                 end_tick=row['game_tick'],gameplay_count=0))
        segment=segments[-1]
        segment['end_tick']=row['game_tick']
        segment['end_phase']=row['ordinal']+1
        segment['gameplay_count']+=1
    return segments
