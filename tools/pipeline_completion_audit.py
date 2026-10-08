#!/usr/bin/env python3
"""Verify the immutable receipt chain and required batch coverage"""

import argparse
import hashlib
import json
from pathlib import Path


def audit(state_path,requirements_path):
    state_path=Path(state_path).resolve();root=state_path.parent;state=json.loads(state_path.read_text(encoding="utf-8"));requirements=json.loads(Path(requirements_path).read_text(encoding="utf-8"));errors=[];seen=set()
    for row in state.get("completed_batches",[]):
        relative=row.get("receipt");path=(root/relative).resolve()
        try:path.relative_to(root)
        except ValueError:
            errors.append("receipt escaped state root: "+str(relative));continue
        if not path.is_file():errors.append("missing receipt: "+str(relative));continue
        actual=hashlib.sha256(path.read_bytes()).hexdigest()
        if actual!=row.get("sha256"):errors.append("changed receipt: "+str(relative))
        seen.add(row.get("batch"))
    missing=sorted(set(requirements.get("required_batches",[]))-seen)
    if missing:errors.append("missing batches: "+", ".join(missing))
    if state.get("status")!="complete" or state.get("current_batch") is not None:errors.append("state is not complete")
    return {"schema":1,"contract":"xport-pipeline-completion-audit-v1","status":"passed" if not errors else "failed","project":state.get("project"),"receipt_entries":len(state.get("completed_batches",[])),"distinct_batches":sorted(seen),"errors":errors}


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--state",required=True);parser.add_argument("--requirements",required=True);parser.add_argument("--report",required=True);args=parser.parse_args();result=audit(args.state,args.requirements)
    Path(args.report).write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8");print(json.dumps(result,indent=2));return 0 if result["status"]=="passed" else 2


if __name__=="__main__":raise SystemExit(main())
