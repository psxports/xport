"""Report optimization targets without weakening functional acceptance"""
from xport_project import load_project


def evaluate(report):
    _,project=load_project()
    config=project.get('stage_pipeline',{}).get('optimization_targets',{})
    defaults=dict(responses=35,wall_seconds=900,uncached_plus_output=200000,compactions=0,finalization_seconds=30,after_match_seconds=180)
    targets={k:config.get(k,v) for k,v in defaults.items()}
    tokens=report['tokens'];sessions=report.get('sessions',[])
    actual=dict(responses=report['requests'],wall_seconds=report['wall_seconds'],
        uncached_plus_output=tokens['uncached_input_tokens']+tokens['output_tokens'],
        compactions=sum(s.get('local_items',{}).get('counts',{}).get('ContextCompaction',0) for s in sessions),
        finalization_seconds=(report.get('milestones',{}).get('finalization') or {}).get('seconds_after_match'),
        after_match_seconds=report.get('milestones',{}).get('after_match_seconds'))
    baseline=config.get('baseline',{})
    return dict(targets=targets,actual=actual,over_target=[k for k,v in actual.items() if v is not None and v>targets[k]],
        baseline=baseline,reduction_percent={k:100*(1-v/baseline[k]) for k,v in actual.items() if v is not None and baseline.get(k,0)>0},
        scope='Advisory targets for comparable small-defect traces; not correctness gates or promised savings')
