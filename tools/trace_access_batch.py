"""Select the next bounded function package from an access-audit report"""

import argparse
import hashlib
import json
from pathlib import Path

from trace_access_audit import validate_report
from xport_project import atomic_json
from xport_project import artifact_path


SCHEMA = 1
MIGRATION_CATEGORIES = {"data_access", "packed_access", "direct_region", "bulk_access"}


class BatchError(ValueError):
    """Raised when a bounded package cannot be selected safely"""


def _sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def select_batch(report, report_sha256, maximum=5):
    validate_report(report)
    if report["schema"] < 2:
        raise BatchError("Access report does not contain function ownership")
    if not 1 <= maximum <= 5:
        raise BatchError("Function limit must be between one and five")

    groups = {}
    file_scope = []
    for entry in report["entries"]:
        if entry["category"] not in MIGRATION_CATEGORIES or entry["category"] == "common_access":
            continue
        if not entry["function_symbol"]:
            file_scope.append(entry["id"])
            continue
        key = (entry["path"], entry["function_symbol"], entry["function_address"])
        groups.setdefault(key, []).append(entry)

    ready = []
    blocked = []
    for (path, symbol, address), entries in groups.items():
        legacy = [item for item in entries if item["category"] != "common_access"]
        unresolved = [item for item in legacy if not item["supported"]]
        row = {
            "path": path,
            "symbol": symbol,
            "address": address,
            "candidate_count": len(legacy),
            "entry_ids": [item["id"] for item in legacy],
            "unresolved_count": len(unresolved),
            "unresolved_reasons": sorted({item["unsupported_reason"] for item in unresolved}),
        }
        if legacy and not unresolved:
            ready.append(row)
        elif legacy:
            blocked.append(row)

    ready.sort(key=lambda item: (item["address"] is None, item["candidate_count"], item["path"],
                                 item["address"] or "", item["symbol"]))
    blocked.sort(key=lambda item: (item["path"], item["address"] or "", item["symbol"]))
    selected = []
    selected_paths = set()
    for row in ready:
        if len(selected) >= maximum:
            break
        if row["path"] not in selected_paths and len(selected_paths) >= 2:
            continue
        selected.append(row)
        selected_paths.add(row["path"])
    return {
        "schema": SCHEMA,
        "status": "ready" if selected else "exhausted",
        "family": report["family"],
        "source_report_sha256": report_sha256,
        "maximum_functions": maximum,
        "selected": selected,
        "summary": {
            "selected_functions": len(selected),
            "selected_candidates": sum(item["candidate_count"] for item in selected),
            "remaining_ready_functions": len(ready) - len(selected),
            "blocked_functions": len(blocked),
            "file_scope_candidates": len(file_scope),
        },
        "blocked": blocked,
        "file_scope_entry_ids": file_scope,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--max-functions", type=int, default=5)
    args = parser.parse_args()
    report_path = artifact_path(args.report)
    report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    batch = select_batch(report, _sha256(report_path), args.max_functions)
    output = artifact_path(args.output)
    if output.exists():
        if json.loads(output.read_text(encoding="utf-8-sig")) != batch:
            raise BatchError("Batch output already contains different immutable evidence")
    else:
        atomic_json(output, batch)
    print(json.dumps({"status": batch["status"], "summary": batch["summary"], "output": str(output)}, separators=(",", ":")))


if __name__ == "__main__":
    main()
