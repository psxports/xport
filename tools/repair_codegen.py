"""Assess bounded machine-code-to-C eligibility without generating game code"""
import argparse
from collections import Counter
import json
from repair_contract import identity, load, number, program
from trace_worker import write_receipt
from xport_project import artifact_path


def assess(root, contract):
    words=program(root,contract); reasons=[]; classes=Counter(); calls=set()
    supported_special={0,2,3,8,9,0x21,0x23,0x24,0x25,0x26,0x2a,0x2b}
    supported_ops={0,1,2,3,4,5,6,7,9,10,11,12,13,14,15,32,33,35,36,37,40,41,43}
    known_calls={number(x['address']) for x in contract.get('hooks',[])}
    for pc,w in words.items():
        op=w>>26; fn=w&63
        if op not in supported_ops or (op==0 and fn not in supported_special):
            reasons.append(dict(pc=pc,word=w,reason='Instruction outside initial integer emitter subset'))
        if op==1 and (w>>16)&31 not in (0,1):
            reasons.append(dict(pc=pc,reason='REGIMM variant outside initial subset'))
        if op==0 and fn==8 and (w>>21)&31!=31:
            reasons.append(dict(pc=pc,reason='Indirect jump requires a validated target set'))
        if op in (1,2,3,4,5,6,7) or (op==0 and fn in (8,9)):
            classes['control_transfer']+=1
            if pc+4 not in words: reasons.append(dict(pc=pc,reason='Missing delay slot'))
            elif (words[pc+4]>>26 in (1,2,3,4,5,6,7) or (words[pc+4]>>26==0 and words[pc+4]&63 in (8,9))):
                reasons.append(dict(pc=pc,reason='Nested delay-slot transfer'))
        if op==3:
            target=((pc+4)&0xf0000000)|((w&0x3ffffff)<<2);calls.add(target)
            if target not in known_calls:reasons.append(dict(pc=pc,reason='Call ABI is not declared',target=target))
        if op==0 and fn==9: reasons.append(dict(pc=pc,reason='Indirect call requires a validated target set'))
        if op in (32,33,35,36,37):classes['load_with_delay']+=1
        if op in (40,41,43):classes['store']+=1
        if op in (9,10,11):classes['signed_immediate']+=1
        if op in (12,13,14,15):classes['unsigned_immediate']+=1
    return dict(schema=1,status='requires_contract' if reasons else 'eligible_for_prototype',
                contract_sha256=identity(contract),instructions=len(words),classes=dict(classes),
                calls=sorted(calls),blockers=reasons,
                scope='Static feasibility only; no generated implementation, equivalence proof or runtime acceptance',
                required_evidence=['Independent DuckStation instruction semantics','Guest-width C arithmetic without undefined behavior',
                                   'Function call/memory differential cases','Fresh full-channel replay'])


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--contract',required=True);p.add_argument('--output',required=True);a=p.parse_args()
    root,c=load(a.contract); result=assess(root,c);target=artifact_path(a.output);target.parent.mkdir(parents=True,exist_ok=True)
    write_receipt(target,result);print(json.dumps(dict(status=result['status'],output=str(target))))


if __name__=='__main__':raise SystemExit(main())
