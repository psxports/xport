"""Conservative test dependency identities with a full fallback for dynamic loading"""
import ast
import hashlib
import json
import sys
from pathlib import Path


def key(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()


def suite_identity(command, identity):
    files=identity['hashes'];directory=Path(command[command.index('-s')+1]);pattern=command[command.index('-p')+1]
    selected=[path for path in files if Path(path).parent==directory and Path(path).match(pattern)]
    modules={}
    for path in files:
        if Path(path).suffix=='.py':modules.setdefault(Path(path).stem,[]).append(path)
    pending=list(selected);seen=set();fallback=False
    while pending:
        path=pending.pop()
        if path in seen:continue
        seen.add(path);tree=ast.parse(Path(path).read_text(encoding='utf-8-sig'))
        for node in ast.walk(tree):
            names=[]
            if isinstance(node,ast.Import):names=[a.name.split('.')[0] for a in node.names]
            if isinstance(node,ast.ImportFrom):
                if node.level:fallback=True
                names=[(node.module or '').split('.')[0]]
            if isinstance(node,ast.Call):
                function=node.func
                label=function.id if isinstance(function,ast.Name) else function.attr if isinstance(function,ast.Attribute) else ''
                # Shell commands and dynamic imports cannot be proven by the static graph
                if label in ('__import__','import_module','run_path','exec','eval','Popen','run','check_call','check_output','system'):
                    fallback=True
            for name in names:
                if name and name not in modules and name not in sys.stdlib_module_names:fallback=True
                pending.extend(modules.get(name,[]))
    dependencies=files if fallback else {p:h for p,h in files.items() if p in seen or Path(p).suffix!='.py'}
    return dict(schema=1,command=command,hashes=dependencies,python=identity['python'],platform=identity['platform'],
                conservative_fallback=fallback)


def changed_components(previous, current, limit=12):
    before=(previous or {}).get('hashes',{});after=current.get('hashes',{})
    names=sorted(p for p in before.keys()|after.keys() if before.get(p)!=after.get(p))
    return dict(count=len(names),paths=names[:limit],omitted=max(0,len(names)-limit))
