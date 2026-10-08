#!/usr/bin/env python3
"""Translate a deliberately small, reviewed IDA CTree subset to readable C"""

import argparse
import hashlib
import json
import re
from dataclasses import dataclass,asdict
from pathlib import Path

from ida_ctree_schema import validate as validate_ctree


CONTRACT="xport-ctree-transform-v1"


@dataclass(frozen=True)
class Gap:
    code:str
    node:int|None
    detail:str


class TranslationError(Exception):
    def __init__(self,gap):
        super().__init__(gap.detail)
        self.gap=gap


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _name(value):
    value=re.sub(r"[^A-Za-z0-9_]","_",value)
    if not value or value[0].isdigit():
        value="v_"+value
    return value


class Translator:
    binary={"add":"+","sub":"-","mul":"*","band":"&","bor":"|","bxor":"^"}
    compare={"eq":"==","ne":"!=","sgt":">","sge":">=","slt":"<","sle":"<=","ugt":">","uge":">=","ult":"<","ule":"<="}

    def __init__(self,document,manifest):
        self.document=document
        self.manifest=manifest
        self.nodes={n["id"]:n for n in document["nodes"]}
        self.gaps=[]
        self.variables={str(k):v for k,v in manifest.get("variables",{}).items()}
        self.objects={str(k).lower():v for k,v in manifest.get("objects",{}).items()}
        self.calls={str(k):v for k,v in manifest.get("calls",{}).items()}

    def fail(self,node,code,detail):
        raise TranslationError(Gap(code,None if node is None else node.get("id"),detail))

    def node(self,node_id):
        return self.nodes[node_id]

    @staticmethod
    def width(node):
        width=(node.get("type") or {}).get("width")
        if width not in (1,2,4):
            return None
        return width

    @staticmethod
    def signed(node):
        return bool((node.get("type") or {}).get("signed"))

    def variable(self,node):
        variable=node.get("variable") or {}
        if variable.get("kind")!="lvar":
            self.fail(node,"unsupported-variable","Only reviewed local variables are supported")
        spec=self.variables.get(str(variable.get("id")))
        if not spec:
            self.fail(node,"unreviewed-variable",f"Local variable {variable.get('id')} has no reviewed binding")
        return _name(spec["name"])

    def object_spec(self,node):
        variable=node.get("variable") or {}
        if variable.get("kind")!="object":
            self.fail(node,"unsupported-object","CTree object lacks an address")
        ea=variable.get("ea")
        spec=self.objects.get(str(ea).lower()) or self.objects.get(hex(ea).lower())
        if not spec:
            self.fail(node,"unreviewed-object",f"Object {hex(ea)} has no reviewed guest binding")
        if spec.get("kind")!="guest_address":
            self.fail(node,"host-guest-alias","CTree objects may not be rebound to host pointers")
        return spec

    def address(self,node):
        op=node["op_name"]
        children=[self.node(i) for i in node["children"]]
        if op=="obj":
            return f"UINT32_C({int(self.object_spec(node)['address']):#010x})"
        if op=="idx" and len(children)==2:
            width=self.width(node)
            if width is None:
                self.fail(node,"unsupported-index-type","Index element width must be 8, 16, or 32 bits")
            base=self.address(children[0])
            index=self.expr(children[1])
            return f"(uint32_t)({base} + (uint32_t)({index}) * UINT32_C({width}))"
        if op in ("add","sub") and len(children)==2:
            a=self.expr(children[0]);b=self.expr(children[1]);symbol="+" if op=="add" else "-"
            return f"(uint32_t)((uint32_t)({a}) {symbol} (uint32_t)({b}))"
        if op=="var":
            return f"(uint32_t)({self.variable(node)})"
        if op=="num":
            return f"UINT32_C({int(node['number']) & 0xffffffff:#010x})"
        self.fail(node,"unsupported-address",f"{op} cannot form a reviewed guest address")

    def load(self,node,address):
        width=self.width(node)
        if width is None:
            self.fail(node,"unsupported-load-width","Guest loads must be 8, 16, or 32 bits")
        suffix=("s" if self.signed(node) else "u")+str(width*8)
        return f"r_{suffix}(memory, {address})"

    def expr(self,node):
        op=node["op_name"]
        children=[self.node(i) for i in node["children"]]
        if op=="num":
            return f"UINT32_C({int(node['number']) & 0xffffffff:#010x})"
        if op=="var":
            return self.variable(node)
        if op=="obj":
            return self.load(node,self.address(node))
        if op=="ptr" and len(children)==1:
            return self.load(node,self.address(children[0]))
        if op=="idx" and len(children)==2:
            return self.load(node,self.address(node))
        if op in self.binary and len(children)==2:
            a=self.expr(children[0]);b=self.expr(children[1]);symbol=self.binary[op]
            return f"(uint32_t)((uint32_t)({a}) {symbol} (uint32_t)({b}))"
        if op in ("shl","shr","sshr") and len(children)==2:
            a=self.expr(children[0]);b=self.expr(children[1]);count=f"((uint32_t)({b}) & UINT32_C(31))"
            if op=="shl":
                return f"(uint32_t)((uint32_t)({a}) << {count})"
            if op=="shr":
                return f"(uint32_t)((uint32_t)({a}) >> {count})"
            return f"xport_ctree_asr32((uint32_t)({a}), {count})"
        if op in self.compare and len(children)==2:
            a=self.expr(children[0]);b=self.expr(children[1]);signed=op.startswith("s") or op in ("sgt","sge","slt","sle")
            cast="int32_t" if signed else "uint32_t"
            return f"(({cast})({a}) {self.compare[op]} ({cast})({b}))"
        if op=="lnot" and len(children)==1:
            return f"!({self.expr(children[0])})"
        if op=="neg" and len(children)==1:
            return f"(uint32_t)(UINT32_C(0) - (uint32_t)({self.expr(children[0])}))"
        if op=="cast" and len(children)==1:
            width=self.width(node)
            if width is None:
                self.fail(node,"unsupported-cast","Only fixed-width integer casts are supported")
            ctype=("int" if self.signed(node) else "uint")+str(width*8)+"_t"
            return f"({ctype})({self.expr(children[0])})"
        if op=="asg" and len(children)==2:
            return self.assignment(children[0],children[1],False)
        if op=="preinc" and len(children)==1:
            lhs=children[0]
            if lhs["op_name"]!="var":
                self.fail(node,"unsupported-update","Only a reviewed local may be incremented")
            name=self.variable(lhs)
            return f"({name} = (uint32_t)((uint32_t)({name}) + UINT32_C(1)))"
        if op=="call":
            spec=self.calls.get(str(node["id"]))
            if not spec or spec.get("reviewed") is not True or not spec.get("prototype"):
                self.fail(node,"unreviewed-call","Direct call requires a reviewed node-specific prototype")
            args=children[1:]
            if len(args)!=len(spec.get("arguments",args)):
                self.fail(node,"call-arity",f"Reviewed call arity does not match node {node['id']}")
            return f"{_name(spec['symbol'])}({', '.join(self.expr(v) for v in args)})"
        if op=="helper":
            self.fail(node,"unsupported-intrinsic","Hex-Rays helper/intrinsic has no independent semantics")
        self.fail(node,"unsupported-expression",f"Expression operation {op} is outside v1")

    def assignment(self,lhs,rhs,statement=True):
        value=self.expr(rhs)
        if lhs["op_name"]=="var":
            result=f"{self.variable(lhs)} = {value}"
        elif lhs["op_name"] in ("ptr","idx"):
            width=self.width(lhs)
            if width is None:
                self.fail(lhs,"unsupported-store-width","Guest stores must be 8, 16, or 32 bits")
            address=self.address(self.node(lhs["children"][0])) if lhs["op_name"]=="ptr" else self.address(lhs)
            result=f"w_u{width*8}(memory, {address}, (uint32_t)({value}))"
        elif lhs["op_name"]=="obj":
            width=self.width(lhs)
            if width is None:
                self.fail(lhs,"unsupported-store-width","Guest stores must be 8, 16, or 32 bits")
            result=f"w_u{width*8}(memory, {self.address(lhs)}, (uint32_t)({value}))"
        else:
            self.fail(lhs,"unsupported-lvalue",f"Assignment target {lhs['op_name']} is outside v1")
        return result+(';' if statement else '')

    def statement(self,node,level=1):
        indent="    "*level;op=node["op_name"];children=[self.node(i) for i in node["children"]]
        if op=="block":
            return "\n".join(filter(None,(self.statement(v,level) for v in children)))
        if op=="expr" and len(children)==1:
            child=children[0]
            if child["op_name"]=="asg":
                a=[self.node(i) for i in child["children"]]
                return indent+self.assignment(a[0],a[1])
            return indent+self.expr(child)+";"
        if op=="return":
            return indent+("return;" if not children else "return "+self.expr(children[0])+";")
        if op=="if" and len(children) in (2,3):
            result=indent+"if ("+self.expr(children[0])+")\n"+indent+"{\n"+self.statement(children[1],level+1)+"\n"+indent+"}"
            if len(children)==3:
                result+="\n"+indent+"else\n"+indent+"{\n"+self.statement(children[2],level+1)+"\n"+indent+"}"
            return result
        if op in ("while","do") and len(children)==2:
            if op=="while":
                return indent+"while ("+self.expr(children[0])+")\n"+indent+"{\n"+self.statement(children[1],level+1)+"\n"+indent+"}"
            return indent+"do\n"+indent+"{\n"+self.statement(children[0],level+1)+"\n"+indent+"} while ("+self.expr(children[1])+");"
        if op=="for" and len(children)==4:
            init=self.expr(children[0]);cond=self.expr(children[1]);step=self.expr(children[2])
            return indent+f"for ({init}; {cond}; {step})\n"+indent+"{\n"+self.statement(children[3],level+1)+"\n"+indent+"}"
        self.fail(node,"unsupported-statement",f"Statement operation {op} is outside v1")

    def translate(self):
        roots=[n for n in self.document["nodes"] if n.get("parent") is None]
        if len(roots)!=1:
            self.fail(None,"root-count",f"Expected one CTree root, found {len(roots)}")
        function=self.manifest["function"]
        args=[]
        for arg in function.get("arguments",[]):
            kind=arg["kind"]
            if kind=="guest_address": ctype="uint32_t"
            elif kind=="host_pointer": ctype=arg["type"]
            elif kind=="memory_context": continue
            elif kind=="integer": ctype=arg["type"]
            else: self.fail(None,"unsupported-argument-kind",kind)
            args.append(f"{ctype} {_name(arg['name'])}")
        body=self.statement(roots[0],1)
        locals_out=[]
        argument_names={a["name"] for a in function.get("arguments",[])}
        referenced_variables={str((n.get("variable") or {}).get("id")) for n in self.document["nodes"] if (n.get("variable") or {}).get("kind")=="lvar"}
        for key,spec in sorted(self.variables.items(),key=lambda v:int(v[0])):
            if key in referenced_variables and spec["name"] not in argument_names:
                locals_out.append(f"    {spec['type']} {_name(spec['name'])} = ({spec['type']})0;")
        helper="static uint32_t xport_ctree_asr32(uint32_t value, uint32_t count)\n{\n    if (count == 0u)\n        return value;\n    return (value >> count) | ((UINT32_C(0) - (value >> 31u)) << (32u - count));\n}\n\n" if any(n["op_name"]=="sshr" for n in self.document["nodes"]) else ""
        prototypes=[]
        for spec in self.calls.values():
            prototype=spec["prototype"].strip().rstrip(";")
            prototypes.append("extern "+prototype+";")
        declarations=("\n".join(sorted(set(prototypes)))+"\n\n") if prototypes else ""
        code='#include <stdint.h>\n\n#include "xport_trace.h"\n\n'+declarations+helper+f"{function['return_type']} {_name(function['name'])}({', '.join(args) if args else 'void'})\n{{\n"
        if locals_out: code+="\n".join(locals_out)+"\n\n"
        code+=body+"\n}\n"
        forbidden=[]
        for pattern in (r"\bswitch\s*\(",r"\bregisters\s*\[",r"\binstruction_[A-Za-z0-9_]*\s*\("):
            if re.search(pattern,code): forbidden.append(pattern)
        if forbidden:
            self.fail(None,"readability-rejection","Generated register VM or instruction dispatcher")
        return code


def validate_manifest(manifest,ctree_path):
    if manifest.get("schema")!=1 or manifest.get("contract")!=CONTRACT:
        raise ValueError("Unsupported transformation manifest")
    binding=manifest.get("ctree") or {}
    if binding.get("sha256")!=sha256(ctree_path):
        raise ValueError("CTree identity mismatch")
    if manifest.get("application"):
        app=manifest["application"]
        source=Path(app["source"])
        if sha256(source)!=app.get("sha256"):
            raise ValueError("Application source identity mismatch")
        if app.get("reviewed") is not True:
            raise ValueError("Application manifest is not reviewed")


def run(ctree_path,manifest_path,output_dir):
    ctree_path=Path(ctree_path);manifest_path=Path(manifest_path);output_dir=Path(output_dir)
    document=validate_ctree(json.loads(ctree_path.read_text(encoding="utf-8")))
    manifest=json.loads(manifest_path.read_text(encoding="utf-8"));validate_manifest(manifest,ctree_path)
    if any(part.lower()=="src" for part in output_dir.resolve().parts):
        raise ValueError("Generated output may not target a hand-edited src directory")
    output_dir.mkdir(parents=True,exist_ok=True)
    report={"schema":1,"contract":"xport-ctree-result-v1","ctree_sha256":sha256(ctree_path),"manifest_sha256":sha256(manifest_path),"status":"unsupported","candidate":None,"gaps":[]}
    try:
        code=Translator(document,manifest).translate()
        target=output_dir/(_name(manifest["function"]["name"])+".c")
        target.write_text(code,encoding="utf-8",newline="\n")
        report.update(status="candidate",candidate=str(target.resolve()))
    except TranslationError as error:
        report["gaps"]=[asdict(error.gap)]
    report_path=output_dir/"report.json";report_path.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    return report


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--ctree",required=True);parser.add_argument("--manifest",required=True);parser.add_argument("--output",required=True)
    args=parser.parse_args();report=run(args.ctree,args.manifest,args.output)
    print(json.dumps(report,indent=2));return 0 if report["status"]=="candidate" else 2


if __name__=="__main__":
    raise SystemExit(main())
