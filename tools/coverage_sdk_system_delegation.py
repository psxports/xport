"""Explicit SDK delegations to authorized missing guest system services"""

from pathlib import Path
import hashlib
import re

from coverage_missing_stubs import policy_entries, validate_stub_body
from source_index import functions


REPLACEMENT = "user_authorized_system_delegation"


def _algorithm_check(check):
    word = int(check.get("word", "0"), 0)
    if check.get("kind") in {"load", "store"}:
        return (word >> 21) & 31 != 29
    if check.get("kind") == "call":
        return True
    if check.get("kind") == "control":
        return word != 0x03E00008
    return check.get("id") == "store"


_COP2_SERVICES = {
    "xport_gte_write_lzcs": ("void xport_gte_write_lzcs(uint32 value);", "mtc2", 30, 0x4884F000),
    "xport_gte_read_lzcr": ("uint32 xport_gte_read_lzcr(void);", "mfc2", 31, 0x4802F800),
}


def _original_cop2_sites(root, packet, binding, audit, rows):
    typed = [row for row in rows if "original_sites" in row]
    if not typed:
        return
    if packet is None or audit.get("source_symbol") not in {"SquareRoot0", "SquareRoot0PSX"} or not any(r.get("alias") == "SquareRoot0" for r in binding.get("library_classification", [])):
        raise ValueError("Original COP2 sites require the exact SquareRoot0 packet context")
    if [row.get("symbol") for row in typed] != list(_COP2_SERVICES):
        raise ValueError("Original COP2 sites require the ordered LZCS/LZCR pair")
    dependencies = [d for d in packet.get("sdk_dependencies", []) if d.get("binding") == binding]
    if len(dependencies) != 1:
        raise ValueError("Original COP2 sites require one immutable SDK binding")
    context = dependencies[0].get("context")
    if not isinstance(context, dict):
        raise ValueError("Original COP2 sites require full original context")
    function, mips = context.get("function", {}), context.get("mips", {})
    values = {"function_sha256": function.get("sha256"), "mips_sha256": mips.get("sha256"),
              "pseudocode_sha256": context.get("pseudocode", {}).get("sha256"),
              "machine_audit_sha256": context.get("machine_audit", {}).get("sha256")}
    if (function.get("image") != binding.get("image") or function.get("address") != binding.get("address") or
            any(values[k] != binding.get(k) or audit.get(k) != binding.get(k) for k in values) or mips.get("complete") is not True):
        raise ValueError("Original COP2 context conflicts with immutable SDK hashes")
    from lockstep_audit import words_from_listing
    words = words_from_listing(mips.get("lines", []))
    start, end = function.get("address"), function.get("end")
    if type(start) is not int or type(end) is not int or set(words) != set(range(start, end, 4)) or function.get("bytes") != end - start:
        raise ValueError("Original COP2 sites require complete contiguous function words")
    raw = b"".join(words[pc].to_bytes(4, "little") for pc in range(start, end, 4))
    if hashlib.sha256(raw).hexdigest() != binding["function_sha256"]:
        raise ValueError("Original COP2 function bytes changed")
    relative = Path(mips.get("path", ""))
    source = (Path(root) / relative).resolve()
    if relative.parts[:1] != ("orig",) or ".." in relative.parts or not source.is_relative_to((Path(root) / "orig").resolve()) or not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest() != binding["mips_sha256"]:
        raise ValueError("Original COP2 listing hash changed")
    if words_from_listing(source.read_text(encoding="utf-8-sig").splitlines()) != words:
        raise ValueError("Original COP2 packet words conflict with listing")
    from lockstep_audit import checklist as original_checklist
    derived = original_checklist(mips["lines"])
    if derived["sha256"] != binding["machine_audit_sha256"]:
        raise ValueError("Original COP2 checklist hash changed")
    checklist = context.get("machine_audit", {})
    if checklist.get("checks") != derived["checks"] or checklist.get("checks") != binding.get("machine_audit", {}).get("checks"):
        raise ValueError("Original COP2 checklist conflicts with packet binding")
    checked_pcs = {int(c["pc"], 0) for c in checklist.get("checks", [])}
    seen = []
    for row in typed:
        expected = _COP2_SERVICES[row["symbol"]]
        sites = row.get("original_sites")
        if row.get("prototype") != expected[0] or row.get("check_ids") != [] or not isinstance(sites, list) or len(sites) != 1:
            raise ValueError("Original COP2 delegation requires one exact typed site")
        site = sites[0]
        if (not isinstance(site, dict) or set(site) != {"pc", "word", "transfer", "data_register"} or
                type(site["pc"]) is not int or type(site["word"]) is not int or type(site["data_register"]) is not int or
                (site["transfer"], site["data_register"], site["word"]) != expected[1:] or
                words.get(site["pc"]) != site["word"] or site["pc"] in checked_pcs or site["pc"] in seen):
            raise ValueError("Original COP2 delegation has a foreign or already checked site")
        seen.append(site["pc"])
    if seen != sorted(seen) or seen[0] >= seen[1]:
        raise ValueError("Original COP2 transfers are not ordered write/read")


def authorized_delegation(root, proposal, binding, audit, packet=None):
    if audit.get("replacement_kind") != REPLACEMENT:
        return None
    if root is None or not binding.get("library_classification"):
        raise ValueError("SDK system delegation requires a reviewed SDK dependency")
    if audit.get("original_equivalence") is not False:
        raise ValueError("SDK system delegation cannot claim original equivalence")
    for name in ("unresolved_semantics", "waived_original_effects"):
        values = audit.get(name)
        if not isinstance(values, list) or not values or not all(isinstance(v, str) and v.strip() for v in values):
            raise ValueError("SDK system delegation must retain explicit unresolved effects")
    rows = audit.get("system_delegations")
    if not isinstance(rows, list) or not rows:
        raise ValueError("SDK system delegation requires exact missing services")
    _original_cop2_sites(root, packet, binding, audit, rows)
    entries = policy_entries(root, proposal)
    expected = {c["id"] for c in binding.get("machine_audit", {}).get("checks", [])}
    aliases = {r["alias"] for r in binding["library_classification"]}
    symbol = audit.get("source_symbol")
    if symbol not in aliases and not any(symbol == a + "PSX" for a in aliases):
        raise ValueError("Delegated SDK must name its canonical implementation")
    seen_symbols, seen_checks, result = set(), set(), []
    for row in rows:
        if not isinstance(row, dict) or set(row) not in ({"symbol", "source_file", "prototype", "check_ids", "source_line"}, {"symbol", "source_file", "prototype", "check_ids", "source_line", "original_sites"}):
            raise ValueError("Invalid SDK system delegation row")
        checks = row["check_ids"]
        if (not isinstance(checks, list) or (not checks and "original_sites" not in row) or not all(isinstance(c, str) for c in checks)
                or len(set(checks)) != len(checks) or set(checks) - expected or set(checks) & seen_checks
                or type(row["source_line"]) is not int or row["source_line"] < 1
                or not isinstance(row["symbol"], str) or row["symbol"] in seen_symbols):
            raise ValueError("SDK delegation has duplicate or foreign original checks")
        matching = [e for e in entries if e["kind"] == "system_bridge" and
                    e["symbol"] == row["symbol"] and e["source_file"] == row["source_file"] and
                    e["prototype"] == row["prototype"] and e["caller_image"] == binding["image"] and
                    e["caller_address"] == binding.get("caller_address") and
                    e["caller_function_sha256"] == binding.get("caller_function_sha256")]
        if len(matching) != 1:
            raise ValueError("SDK delegation has no exact authorized recorded caller")
        seen_symbols.add(row["symbol"])
        seen_checks.update(checks)
        result.append(matching[0])
    if seen_checks == expected:
        raise ValueError("SDK delegation cannot waive the entire original algorithm")
    if not any(c["id"] not in seen_checks and _algorithm_check(c) for c in binding["machine_audit"]["checks"]):
        retained = audit.get("retained_real_implementation")
        if (not isinstance(retained, dict) or set(retained) != {"source_file", "symbol", "function_sha256"}
                or retained["source_file"] != audit.get("source_file") or retained["symbol"] not in aliases
                or retained["symbol"] == symbol or not isinstance(retained["function_sha256"], str)
                or not re.fullmatch(r"[0-9a-f]{64}", retained["function_sha256"])):
            raise ValueError("ABI-only SDK remainder requires its exact retained real canonical implementation")
    return result


def _code(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', "", text, flags=re.S)


def _calls(text, symbol):
    return re.search(r"\b" + re.escape(symbol) + r"\s*\(", _code(text)) is not None


def _direct_call(text, symbol):
    code = _code(text)
    if re.search(r"(?:^|\n)\s*(?:#|%:)", code):
        return False
    pattern = r"(?:^|[;{}])\s*(?:return\s+|(?:[A-Za-z_]\w*\s+)?[A-Za-z_]\w*\s*=\s*|\(void\)\s*)?" + re.escape(symbol) + r"\s*\("
    matches = list(re.finditer(pattern, code, re.M))
    if not matches or re.search(r"\bif\s*\(\s*(?:0[uUlL]*|false)\s*\)", code):
        return False
    return any(not re.search(r"\breturn\b", code[:m.start()]) for m in matches)


def validate_delegated_sources(root, packet, proposal, shared_root, changes):
    dependencies = {(d["binding"]["image"], d["binding"]["address"]): d["binding"]
                    for d in packet.get("sdk_dependencies") or []}
    for audit in proposal.get("sdk_audits") or []:
        if audit.get("replacement_kind") != REPLACEMENT:
            continue
        key = (audit["image"], audit["address"])
        if key not in dependencies:
            raise ValueError("SDK delegation is outside the packet")
        binding = dependencies[key]
        entries = authorized_delegation(root, proposal, binding, audit, packet)

        def body(path, symbol, with_source=False):
            relative = Path(path) if isinstance(path, str) else Path()
            if relative.parts[:2] != ("@xport", "src") or ".." in relative.parts or relative.suffix != ".c":
                raise ValueError("SDK delegation escapes canonical shared source")
            source = (Path(shared_root) / Path(*relative.parts[1:])).resolve()
            if not source.is_relative_to((Path(shared_root) / "src").resolve()) or not source.is_file():
                raise ValueError("SDK delegation source is unavailable")
            text = changes.get(source, source.read_text(encoding="utf-8-sig"))
            parsed = [f for f in functions(text, path) if f["name"] == symbol]
            if len(parsed) != 1:
                raise ValueError("SDK delegation requires one unique predicted body")
            f = parsed[0]
            result = text[f["start"]:f["end"]]
            return (result, text) if with_source else result

        adapter = body(audit["source_file"], audit["source_symbol"])
        caller = [t for t in proposal.get("translations") or [] if t["image"] == binding["image"] and
                  t["address"] == binding.get("caller_address")]
        if not caller:
            owner_audits = [a for a in proposal.get("function_audits") or [] if a.get("image") == binding["image"] and
                            a.get("address") == binding.get("caller_address") and
                            a.get("function_sha256") == binding.get("caller_function_sha256")]
            if len(owner_audits) == 1:
                owner = owner_audits[0]
                relative = Path(owner.get("source_file", ""))
                source = (Path(root) / relative).resolve()
                if relative.parts[:1] != ("src",) or ".." in relative.parts or not source.is_relative_to((Path(root) / "src").resolve()):
                    raise ValueError("Delegated SDK caller escapes project source")
                text = source.read_text(encoding="utf-8-sig")
                parsed = [f for f in functions(text, str(relative)) if f["name"] == owner.get("source_symbol")]
                if len(parsed) == 1:
                    caller = [{"text": text[parsed[0]["start"]:parsed[0]["end"]]}]
        if len(caller) != 1 or not _direct_call(caller[0]["text"][caller[0]["text"].index("{") + 1:], audit["source_symbol"]):
            raise ValueError("Recorded caller does not call the delegated SDK adapter")
        code = _code(adapter[adapter.index("{") + 1:adapter.rfind("}")])
        if re.search(r"\bgoto\b|\bwhile\s*\(\s*(?:0[uUlL]*|false)\s*\)", code):
            raise ValueError("Delegated SDK adapter uses unsupported control flow")
        if _calls(code, "abort"):
            raise ValueError("A delegated real SDK adapter cannot itself abort")
        retained = audit.get("retained_real_implementation")
        if retained is not None:
            if (not isinstance(retained, dict) or set(retained) != {"source_file", "symbol", "function_sha256"}
                    or retained["source_file"] != audit["source_file"]):
                raise ValueError("Invalid retained real SDK implementation")
            real = body(retained["source_file"], retained["symbol"])
            if (hashlib.sha256(real.encode()).hexdigest() != retained["function_sha256"] or
                    not _direct_call(code, retained["symbol"]) or _calls(real, "abort")):
                raise ValueError("SDK adapter does not preserve its exact real canonical implementation")
        for row, entry in zip(audit["system_delegations"], entries):
            if not _direct_call(code, row["symbol"]):
                raise ValueError("SDK adapter does not directly call its missing service")
            bridge_body, bridge_source = body(row["source_file"], row["symbol"], True)
            validate_stub_body(bridge_body, entry)
            lines = bridge_source.splitlines()
            if row["source_line"] > len(lines) or _code(lines[row["source_line"] - 1]).strip() != "abort();":
                raise ValueError("SDK delegated mapping must identify its unconditional abort line")
            groups = [g for g in audit.get("machine_coverage") or [] if set(g.get("check_ids") or []) & set(row["check_ids"])]
            mapped = [c for g in groups for c in g.get("check_ids") or [] if c in row["check_ids"]]
            if len(mapped) != len(row["check_ids"]) or set(mapped) != set(row["check_ids"]):
                raise ValueError("SDK delegated checks lack their exact native mappings")
            if any(set(g.get("check_ids") or []) - set(row["check_ids"]) or
                   g.get("source_file") != row["source_file"] or g.get("source_symbol") != row["symbol"]
                   or g.get("source_line") != row["source_line"] for g in groups):
                raise ValueError("SDK delegated checks must map to their authorized bridge line")
        typed_rows = [r for r in audit["system_delegations"] if "original_sites" in r]
        if typed_rows:
            positions = []
            for row in typed_rows:
                pattern = r"(?:^|[;{}])\s*(?:[A-Za-z_]\w*\s*=\s*)?" + re.escape(row["symbol"]) + r"\s*\("
                matches = list(re.finditer(pattern, code))
                if len(matches) != 1 or re.search(r"\b(?:if|for|while|switch|goto|return)\b", code[:matches[0].start()]):
                    raise ValueError("Original COP2 service calls must be unique and unconditionally reachable")
                positions.append(matches[0].start())
            if positions != sorted(positions):
                raise ValueError("Original COP2 services must execute write before read")
        independent = re.sub(r"\b(?:" + "|".join(re.escape(r["symbol"]) for r in audit["system_delegations"]) +
                             r")\s*\([^;]*\)\s*;", "", code)
        independent = re.sub(r"\(void\)\s*[A-Za-z_]\w*\s*;", "", independent)
        called = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", independent)) - {"if", "while", "for", "sizeof", "switch"}
        if not any(_direct_call(independent, name) for name in called):
            raise ValueError("SDK delegation cannot replace the entire real algorithm")


def delegation_summary(proposal):
    fields = ("image", "address", "source_file", "source_symbol", "replacement_kind", "original_equivalence",
              "waived_original_effects", "unresolved_semantics", "system_delegations")
    return [{**{key: audit[key] for key in fields},
             **({"retained_real_implementation": audit["retained_real_implementation"]} if "retained_real_implementation" in audit else {}),
             "missing_stub_authorization": proposal["missing_stub_authorization"],
             "implementation_status": "WIP"} for audit in proposal.get("sdk_audits") or []
            if audit.get("replacement_kind") == REPLACEMENT]
