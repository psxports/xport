"""Order channel failures on the absolute phase axis without guessing causal functions"""


def first_difference(reports, contract):
    failures=[]
    for channel,result in reports.items():
        if result['passed']:continue
        detail=result.get('first_difference') or {}
        ordinal=None
        if channel=='phases':
            ordinal=detail.get('ordinal')
        elif contract.get('schema')==1:
            # Schema 1 declares one contiguous gameplay interval
            index=detail.get('record_index',detail.get('ordinal'))
            if index is None and 'tick' in detail:index=detail['tick']-contract['start_tick']
            if type(index) is int and 0<=index<=contract['end_tick']-contract['start_tick']:
                ordinal=contract['phase_begin']+index
        elif contract.get('schema')==2:
            index=detail.get('record_index',detail.get('ordinal'))
            if type(index) is int and index>=0:
                for segment in contract['segments']:
                    size=segment['end_tick']-segment['start_tick']+1
                    if index<size:
                        ordinal=segment['start_phase']+index
                        break
                    index-=size
        if type(ordinal) is not int or not contract['phase_begin']<=ordinal<contract['phase_end']:
            ordinal=None
        failures.append(dict(channel=channel,detail=detail,ordinal=ordinal))
    if not failures:return None
    # A malformed channel with unknown position cannot certify a matching prefix
    if any(row['ordinal'] is None for row in failures):
        return dict(kind='unlocalized_or_incomplete',failures=failures,verified_prefix_end=None)
    first=min(failures,key=lambda row:row['ordinal'])
    return dict(kind='mismatch',**first,verified_prefix_end=first['ordinal'],
                failures=failures,axis='phase_ordinal')
