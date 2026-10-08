#!/usr/bin/env python3
"""Validate project-local B18 evidence without promoting partial routes"""

import argparse
import hashlib
import json
from pathlib import Path


CONTRACT="xport-cross-project-acceptance-v1"
REGIONS=("dram","vram","scratchpad","spu_ram")
CLASSES=("c_addressing","caller_control_flow","shared_runtime_device")
NEGATIVE_GATES=("missing_frame","memory_outside_stack","omitted_region","changed_input","late_reseed","lost_event","changed_abi","changed_config","changed_source","unsupported_native_origin")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(document,project_root):
    project_root=Path(project_root).resolve()
    errors=[]
    if document.get("schema")!=1 or document.get("contract")!=CONTRACT:
        errors.append("unsupported contract")
    if document.get("claim") not in ("partial","full_match"):
        errors.append("invalid claim")
    capture=document.get("capture") or {}
    if tuple(capture.get("regions",()))!=REGIONS:
        errors.append("omitted region")
    if capture.get("stack_policy")!="exclude_declared_guest_stack_only" or not isinstance(capture.get("stack_ranges"),list):
        errors.append("changed stack policy")
    if not capture.get("input_sha256"):
        errors.append("changed input")
    if capture.get("initial_imports")!=1:
        errors.append("late reseed")
    native=document.get("native") or {}
    for field in ("executable_sha256","source_sha256","config_sha256","runtime_sha256"):
        if not native.get(field):errors.append("changed "+field.replace("_sha256",""))
    if native.get("configuration")!="Release":errors.append("non-Release native")
    if native.get("origin") not in ("translated_c","shared_runtime"):
        errors.append("unsupported native origin")
    cases=document.get("cases") or []
    if {row.get("class") for row in cases}!=set(CLASSES):
        errors.append("missing defect class")
    for row in cases:
        if row.get("real_defect") is not True:errors.append("fixture substituted for real defect")
        if row.get("result") not in ("passed","partial"):errors.append("invalid defect result")
        for role in ("mismatch","automated_evidence","baseline","candidate","release_regression","integration"):
            artifact=row.get(role) or {};relative=artifact.get("path")
            if not relative:
                errors.append(f"{row.get('id')} missing {role}");continue
            path=(project_root/relative).resolve()
            try:path.relative_to(project_root)
            except ValueError:
                errors.append(f"{row.get('id')} escaped project");continue
            if not path.is_file() or digest(path)!=artifact.get("sha256"):
                errors.append(f"{row.get('id')} changed {role}")
    gates=document.get("negative_gates") or {}
    for gate in NEGATIVE_GATES:
        if gates.get(gate)!="rejected":errors.append("negative gate not rejected: "+gate)
    scenario=document.get("scenario") or {}
    if scenario.get("started") is not True or scenario.get("executable_sha256")!=native.get("executable_sha256"):
        errors.append("native scenario identity mismatch")
    if document.get("claim")=="full_match":
        if capture.get("fresh") is not True or capture.get("continuous") is not True or capture.get("status")!="match":
            errors.append("full match lacks fresh continuous four-region capture")
        if scenario.get("continuous_route") is not True or any(row.get("result")!="passed" for row in cases):
            errors.append("full match has partial route")
        scope=document.get("implemented_scope") or {}
        if any(scope.get(key) not in ("passed","not_implemented") for key in ("image","audio","input","saves")):
            errors.append("invalid implemented scope")
    status="passed" if not errors else "failed"
    return {"schema":1,"contract":"xport-cross-project-result-v1","status":status,"claim":document.get("claim"),"portable_mechanism":status=="passed","full_match":status=="passed" and document.get("claim")=="full_match","errors":errors}


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--manifest",required=True);parser.add_argument("--project",required=True);parser.add_argument("--report",required=True);args=parser.parse_args()
    result=validate(json.loads(Path(args.manifest).read_text(encoding="utf-8")),args.project)
    Path(args.report).parent.mkdir(parents=True,exist_ok=True);Path(args.report).write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8");print(json.dumps(result,indent=2));return 0 if result["status"]=="passed" else 2


if __name__=="__main__":
    raise SystemExit(main())
