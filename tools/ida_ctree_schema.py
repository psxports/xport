"""Validate minimal IDA CTree sidecars without importing IDA"""


def validate(document):
    if not isinstance(document,dict) or document.get("schema")!=1 or document.get("contract")!="xport-ida-ctree-v1":
        raise ValueError("Unsupported CTree document")
    required={"image","entry","function_sha256","gp","image_identity","ida","hexrays","prototype","lvars","nodes","source_mapping","missing_ea"}
    if not required<=set(document):
        raise ValueError("Incomplete CTree document")
    if not isinstance(document["gp"],int) or document["gp"]<=0 or set(document["image_identity"])!={"path","sha256","base","offset"}:
        raise ValueError("Invalid CTree image metadata")
    ids=[]
    for row in document["nodes"]:
        if set(row)-{"id","kind","op","op_name","ea","parent","children","type","variable","number"}:
            raise ValueError("Unknown CTree node field")
        if row.get("kind") not in ("expr","insn") or not isinstance(row.get("children"),list):
            raise ValueError("Invalid CTree node")
        if row["kind"]=="expr" and not isinstance(row.get("type"),dict):
            raise ValueError("Expression type is missing")
        ids.append(row["id"])
    if len(ids)!=len(set(ids)):
        raise ValueError("Duplicate CTree node id")
    known=set(ids)
    for row in document["nodes"]:
        if row.get("parent") is not None and row["parent"] not in known:
            raise ValueError("Unknown CTree parent")
        if any(child not in known for child in row["children"]):
            raise ValueError("Unknown CTree child")
    lvars={row["id"] for row in document["lvars"]}
    for row in document["nodes"]:
        variable=row.get("variable") or {}
        if variable.get("kind")=="lvar" and variable.get("id") not in lvars:
            raise ValueError("Unknown CTree local variable")
    return document
