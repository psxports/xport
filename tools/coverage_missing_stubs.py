"""Explicit project-owned authorization for missing system and SDK fail-fast boundaries"""

import hashlib
import json
from pathlib import Path
import re

from source_index import functions


CONTRACT = "xport-user-authorized-missing-stubs-v1"
INSTRUCTION = "разрешаю заглушки для системных и SDK функций которые отсутствуют в xport toolset"
REPLACEMENT = "user_authorized_missing_stub"


def policy_entries(root, proposal):
    reference = proposal.get("missing_stub_authorization")
    if reference is None:
        return []
    if not isinstance(reference, dict) or set(reference) != {"path", "sha256"}:
        raise ValueError("Missing stub authorization requires its exact path and SHA-256")
    relative = Path(reference["path"]) if isinstance(reference["path"], str) else Path()
    path = (Path(root) / relative).resolve()
    if (relative.is_absolute() or ".." in relative.parts or relative.parts[:1] != ("status",) or
            not path.is_relative_to((Path(root) / "status").resolve()) or not path.is_file()):
        raise ValueError("Missing stub authorization must be a project status JSON")
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != reference["sha256"]:
        raise ValueError("Missing stub authorization SHA-256 changed")
    document = json.loads(raw.decode("utf-8-sig"))
    if (set(document) != {"schema", "contract", "user_instruction", "entries"} or
            document["schema"] != 1 or document["contract"] != CONTRACT or
            document["user_instruction"] != INSTRUCTION or
            not isinstance(document["entries"], list) or not document["entries"]):
        raise ValueError("Missing stub policy has no explicit supported user authorization")
    seen = set()
    for entry in document["entries"]:
        common = {"kind", "symbol", "prototype", "source_file"}
        identity = {"image", "address", "function_sha256"}
        bridge = {"caller_image", "caller_address", "caller_function_sha256"}
        if (not isinstance(entry, dict) or entry.get("kind") not in {"sdk", "system_function", "system_bridge"} or
                set(entry) != common | (bridge if entry.get("kind") == "system_bridge" else identity)):
            raise ValueError("Invalid missing stub policy entry")
        if (not isinstance(entry["symbol"], str) or not re.fullmatch(r"[A-Za-z_]\w*", entry["symbol"]) or
                entry["symbol"] == "abort" or entry["symbol"] in seen or not isinstance(entry["prototype"], str) or
                functions(entry["prototype"], "prototype") or
                not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9 \t*]*\b" + re.escape(entry["symbol"]) + r"\s*\([^{};#]*\)\s*;", entry["prototype"].strip())):
            raise ValueError("Missing stub policy needs unique symbols and exact plain prototypes")
        if entry["kind"] == "system_bridge" and not entry["symbol"].startswith("xport_"):
            raise ValueError("Missing system bridge must name an explicit xport host service")
        seen.add(entry["symbol"])
    return document["entries"]


def signature(text, symbol):
    parsed = functions(text, "stub")
    if (len(parsed) != 1 or parsed[0]["name"] != symbol or
            text[:parsed[0]["start"]].strip() or text[parsed[0]["end"]:].strip()):
        raise ValueError("A missing stub must contain only its named complete function")
    opening = text.index("{", parsed[0]["start"])
    declaration = text[parsed[0]["start"]:opening].strip() + ";"
    return re.sub(r"\s+", "", declaration), text[opening + 1:text.rfind("}")]


def validate_stub_body(text, entry):
    prototype, body = signature(text, entry["symbol"])
    if prototype != re.sub(r"\s+", "", entry["prototype"]):
        raise ValueError("Missing stub body changes its explicitly authorized ABI")
    parameters = entry["prototype"][entry["prototype"].index("(") + 1:entry["prototype"].rindex(")")]
    names = set()
    if parameters.strip() not in {"", "void"}:
        for parameter in parameters.split(","):
            match = re.fullmatch(r"[A-Za-z_][A-Za-z_0-9 \t*]*\b([A-Za-z_]\w*)", parameter.strip())
            if not match:
                raise ValueError("Missing stub parameters need explicit plain names")
            if match.group(1) == "abort":
                raise ValueError("Missing stub cannot shadow its fail-fast service")
            names.add(match.group(1))
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S).strip()
    statements = [s.strip() for s in body.split(";") if s.strip()]
    if not statements or statements[-1] != "abort()":
        raise ValueError("A missing stub must end with unconditional abort()")
    suppressed = []
    for statement in statements[:-1]:
        match = re.fullmatch(r"\(void\)\s*([A-Za-z_]\w*)", statement)
        if match is None or match.group(1) not in names:
            raise ValueError("Missing stub permits only parameter suppressions and abort()")
        suppressed.append(match.group(1))
    if len(set(suppressed)) != len(suppressed):
        raise ValueError("Missing stub repeats a parameter suppression")
    if not body.endswith(";"):
        raise ValueError("Missing stub abort needs a complete statement")


def authorized_audit(root, proposal, binding, audit, kind):
    if audit.get("replacement_kind") != REPLACEMENT:
        return None
    entries = policy_entries(root, proposal)
    matches = [e for e in entries if e["kind"] == kind and
               e.get("image") == binding["image"] and e.get("address") == binding["address"] and
               e.get("function_sha256") == binding["function_sha256"] and
               e["symbol"] == audit.get("source_symbol") and e["source_file"] == audit.get("source_file")]
    if len(matches) != 1:
        raise ValueError("Missing stub audit lacks exact authorized original identity and source")
    if (audit.get("original_equivalence") is not False or
            not isinstance(audit.get("waived_original_effects"), list) or not audit["waived_original_effects"] or
            not isinstance(audit.get("unresolved_semantics"), list) or not audit["unresolved_semantics"] or
            not all(isinstance(s, str) and s for s in audit["waived_original_effects"] + audit["unresolved_semantics"])):
        raise ValueError("Missing stub audit must explicitly retain its waived effects and unresolved equivalence")
    if kind == "sdk":
        aliases = {r["alias"] for r in binding.get("library_classification") or []}
        if matches[0]["symbol"] not in aliases and not any(matches[0]["symbol"] == a + "PSX" for a in aliases):
            raise ValueError("Missing SDK stub has no reviewed canonical alias")
    return matches[0]


def system_function_allowed(context):
    words = []
    for line in context.get("mips", {}).get("lines", []):
        match = re.match(r"^[0-9A-Fa-f]{8}\s+((?:[0-9A-Fa-f]{2}\s+)+)", line)
        if match:
            raw = bytes.fromhex(match.group(1))
            words.extend(int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 4))
    if len(words) == 3 and words[1] == 0x01200008 and words[0] in {0x240900A0, 0x240900B0, 0x240900C0}:
        return words[2] >> 16 == 0x2408
    if len(words) == 3 and words[1] == 0x01400008 and words[0] in {0x240A00A0, 0x240A00B0, 0x240A00C0}:
        return words[2] >> 16 == 0x2409
    syscall = [w for w in words if w & 0xFC00003F == 0x0000000C]
    if not 2 <= len(words) <= 8 or len(syscall) != 1 or 0x03E00008 not in words:
        return False
    return all(w == 0 or w == 0x03E00008 or w & 0xFC00003F == 0x0000000C or
               w >> 16 in {0x2402, 0x2404, 0x2408} for w in words)


def stub_summary(proposal):
    rows = [a for a in [*(proposal.get("function_audits") or []), *(proposal.get("sdk_audits") or [])]
            if a.get("replacement_kind") == REPLACEMENT]
    result = [{k: a[k] for k in ("image", "address", "source_file", "source_symbol",
                               "replacement_kind", "original_equivalence", "waived_original_effects",
                               "unresolved_semantics")} for a in rows]
    result.extend({"replacement_kind": REPLACEMENT, "original_equivalence": False,
                   "source_file": row["path"], "source_symbol": row["symbol"],
                   "kind": "system_bridge"} for row in proposal.get("system_translations") or [])
    for row in result:
        row["implementation_status"] = "WIP"
    return result
