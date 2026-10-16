"""Bounded immediate SDK caller chains for absent-service delegation"""

import hashlib
from pathlib import Path


_HASHES = ("function_sha256", "mips_sha256", "pseudocode_sha256", "machine_audit_sha256")


def nested_caller_chain(root, packet, proposal, binding):
    """Return verified earlier SDK parents ending at a required game owner"""
    dependencies = packet.get("sdk_dependencies") or []
    keys = [(d["binding"]["image"], d["binding"]["address"]) for d in dependencies]
    target = (binding["image"], binding["address"])
    if keys.count(target) != 1:
        raise ValueError("Nested SDK target requires one immutable dependency")
    required = packet.get("source_context", {}).get("audit_contract", {}).get("required_functions", [])
    chain, seen = [], {target}
    child = binding
    while True:
        key = (child["image"], child.get("caller_address"))
        owners = [r for r in required if (r.get("image"), r.get("address")) == key]
        if len(owners) == 1:
            if owners[0].get("function_sha256") != child.get("caller_function_sha256"):
                raise ValueError("Nested SDK chain has a stale game owner")
            return chain
        if key in seen or keys.count(key) != 1 or keys.index(key) >= keys.index((child["image"], child["address"])):
            raise ValueError("Nested SDK caller must be an earlier unique dependency")
        seen.add(key)
        parent = dependencies[keys.index(key)]
        parent_binding, context = parent["binding"], parent.get("context", {})
        if parent_binding.get("function_sha256") != child.get("caller_function_sha256"):
            raise ValueError("Nested SDK caller hash changed")
        child_dependency = dependencies[keys.index((child["image"], child["address"]))]
        _complete_context(root, child, child_dependency.get("context", {}))
        _complete_context(root, parent_binding, context)
        audits = [a for a in proposal.get("sdk_audits", []) if (a.get("image"), a.get("address")) == key]
        if len(audits) != 1:
            raise ValueError("Nested SDK caller requires one complete SDK audit")
        audit = audits[0]
        if (any(audit.get(k) != parent_binding.get(k) for k in _HASHES) or
                audit.get("complete_mips_audit") is not True or
                audit.get("pseudocode_used_as_baseline") is not parent_binding.get("pseudocode_complete") or
                [c for row in audit.get("machine_coverage", []) for c in row.get("check_ids", [])] !=
                [c["id"] for c in parent_binding["machine_audit"]["checks"]]):
            raise ValueError("Nested SDK caller audit conflicts with immutable context")
        aliases = {r["alias"] for r in parent_binding.get("library_classification", [])}
        symbol = audit.get("source_symbol")
        if symbol not in aliases and not any(symbol == a + "PSX" for a in aliases):
            raise ValueError("Nested SDK caller must name its canonical shared implementation")
        from lockstep_audit import words_from_listing
        words = words_from_listing(context["mips"]["lines"])
        sites = child.get("callsite_addresses")
        if not isinstance(sites, list) or not sites or len(set(sites)) != len(sites):
            raise ValueError("Nested SDK edge requires unique original sites")
        for pc in sites:
            word = words.get(pc)
            if type(pc) is not int or word is None or word >> 26 not in (2, 3) or (((pc + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)) != child["address"]:
                raise ValueError("Nested SDK edge differs from the original direct transfer")
        chain.append((parent_binding, audit))
        child = parent_binding


def _complete_context(root, binding, context):
    from lockstep_audit import checklist, words_from_listing
    function, mips, pseudo = context.get("function", {}), context.get("mips", {}), context.get("pseudocode", {})
    values = (function.get("sha256"), mips.get("sha256"), pseudo.get("sha256"), context.get("machine_audit", {}).get("sha256"))
    if (function.get("image") != binding["image"] or function.get("address") != binding["address"] or
            any(value != binding.get(k) for k, value in zip(_HASHES, values)) or mips.get("complete") is not True):
        raise ValueError("Nested SDK caller requires its full immutable context")
    words = words_from_listing(mips.get("lines", []))
    start, end = function.get("address"), function.get("end")
    if type(start) is not int or type(end) is not int or set(words) != set(range(start, end, 4)) or function.get("bytes") != end - start:
        raise ValueError("Nested SDK caller has incomplete original words")
    if hashlib.sha256(b"".join(words[pc].to_bytes(4, "little") for pc in range(start, end, 4))).hexdigest() != binding["function_sha256"]:
        raise ValueError("Nested SDK caller original bytes changed")
    for artifact, name in ((mips, "mips_sha256"), (pseudo, "pseudocode_sha256")):
        if binding.get(name) is None:
            if artifact.get("path") is not None:
                raise ValueError("Nested SDK unexpected original artifact")
            continue
        relative = Path(artifact.get("path", ""))
        source = (Path(root) / relative).resolve()
        if (relative.parts[:1] != ("orig",) or ".." in relative.parts or
                not source.is_relative_to((Path(root) / "orig").resolve()) or not source.is_file() or
                hashlib.sha256(source.read_bytes()).hexdigest() != binding[name] or artifact.get("complete") is not True or
                source.read_text(encoding="utf-8-sig").splitlines() != artifact.get("lines")):
            raise ValueError("Nested SDK original artifact is stale or incomplete")
    derived = checklist(mips["lines"])
    if derived != context.get("machine_audit") or derived != binding.get("machine_audit"):
        raise ValueError("Nested SDK checklist changed")
