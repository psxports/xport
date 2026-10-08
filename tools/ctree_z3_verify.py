#!/usr/bin/env python3
"""Optional bounded equivalence check between CTree and independent MIPS semantics"""

import argparse
import json
from pathlib import Path

from ida_ctree_schema import validate as validate_ctree


def verify(document,manifest):
    request=manifest.get("solver")
    if not request:
        return {"status":"not-requested","pass":False}
    try:
        import z3
    except ImportError:
        return {"status":"unavailable","pass":False,"detail":"Install z3-solver to run the optional proof"}
    nodes={n["id"]:n for n in document["nodes"]}
    variables={name:z3.BitVec(name,32) for name in request.get("variables",[])}

    def mips(ast):
        if "const" in ast:return z3.BitVecVal(int(ast["const"]) & 0xffffffff,32)
        if "var" in ast:
            if ast["var"] not in variables:raise ValueError("Unknown MIPS semantic variable")
            return variables[ast["var"]]
        op=ast.get("op");args=[mips(v) for v in ast.get("args",[])]
        if op=="add":return args[0]+args[1]
        if op=="sub":return args[0]-args[1]
        if op=="mul":return args[0]*args[1]
        if op=="and":return args[0]&args[1]
        if op=="or":return args[0]|args[1]
        if op=="xor":return args[0]^args[1]
        if op=="sll":return args[0] << (args[1]&31)
        if op=="srl":return z3.LShR(args[0],args[1]&31)
        if op=="sra":return args[0] >> (args[1]&31)
        raise ValueError(f"Unsupported MIPS semantic operation {op}")

    def ctree(node):
        op=node["op_name"];children=[ctree(nodes[i]) for i in node["children"]]
        if op=="num":return z3.BitVecVal(int(node["number"])&0xffffffff,32)
        if op=="var":
            key=str((node.get("variable") or {}).get("id"));name=(manifest.get("variables",{}).get(key) or {}).get("name")
            if name not in variables:raise ValueError("Unbound CTree proof variable")
            return variables[name]
        if op=="add":return children[0]+children[1]
        if op=="sub":return children[0]-children[1]
        if op=="mul":return children[0]*children[1]
        if op=="band":return children[0]&children[1]
        if op=="bor":return children[0]|children[1]
        if op=="bxor":return children[0]^children[1]
        if op=="shl":return children[0] << (children[1]&31)
        if op=="shr":return z3.LShR(children[0],children[1]&31)
        if op=="sshr":return children[0] >> (children[1]&31)
        if op=="neg":return -children[0]
        if op=="cast":return children[0]
        raise ValueError(f"CTree proof operation {op} is unsupported")

    solver=z3.Solver();solver.set(timeout=int(request.get("timeout_ms",1000)))
    for condition in request.get("preconditions",[]):
        left=mips(condition["left"]);right=mips(condition["right"]);relation=condition["relation"]
        if relation=="eq":solver.add(left==right)
        elif relation=="ne":solver.add(left!=right)
        elif relation=="ult":solver.add(z3.ULT(left,right))
        elif relation=="ule":solver.add(z3.ULE(left,right))
        else:raise ValueError("Unsupported proof precondition")
    solver.add(ctree(nodes[int(request["node"])])!=mips(request["mips"]))
    result=solver.check()
    if result==z3.unsat:return {"status":"proved","pass":True}
    if result==z3.sat:return {"status":"counterexample","pass":False,"model":str(solver.model())}
    return {"status":"unknown","pass":False,"detail":solver.reason_unknown()}


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--ctree",required=True);parser.add_argument("--manifest",required=True);parser.add_argument("--report",required=True);args=parser.parse_args()
    document=validate_ctree(json.loads(Path(args.ctree).read_text(encoding="utf-8")));manifest=json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    result=verify(document,manifest);Path(args.report).write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8");print(json.dumps(result,indent=2));return 0 if result["pass"] else 2


if __name__=="__main__":
    raise SystemExit(main())
