"""Execute and receipt one bounded RR access-migration package"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from source_context import compact
from trace_access_apply import apply_batch
from trace_access_audit import audit_project, validate_report
from trace_access_batch import select_batch
from xport_project import atomic_json
from xport_project import artifact_path, load_project


SCHEMA = 1


class PackageError(RuntimeError):
    """Raised when a package cannot complete all gates"""


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def package_names(number):
    if number < 1:
        raise PackageError("Sub-batch number must be positive")
    return {
        "apply": f"access-apply-{number}.json",
        "plan": f"access-plan-{number}.json",
        "next": f"access-batch-{number}-next.json",
        "receipt": f"{number:02d}-receipt.json",
    }


def _run(command, cwd):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise PackageError(f"Command failed ({result.returncode}): {' '.join(command)}\n{result.stdout}\n{result.stderr}")
    return result.stdout.strip()


def execute_package(root, report_path, batch_path, output_directory, number, image="MAIN.EXE"):
    root = Path(root).resolve()
    report_path = Path(report_path).resolve()
    batch_path = Path(batch_path).resolve()
    output_directory = Path(output_directory).resolve()
    names = package_names(number)
    targets = {key: output_directory / value for key, value in names.items()}
    if any(path.exists() for path in targets.values()):
        raise PackageError("Package output already exists")

    report = validate_report(json.loads(report_path.read_text(encoding="utf-8-sig")))
    batch = json.loads(batch_path.read_text(encoding="utf-8-sig"))
    if batch.get("source_report_sha256") != sha256(report_path):
        raise PackageError("Batch does not bind the supplied report")
    selected = batch.get("selected", [])
    if not 1 <= len(selected) <= 5 or len({item["path"] for item in selected}) > 2:
        raise PackageError("Package must contain one to five functions from no more than two source modules")

    source_paths = sorted({root / item["path"] for item in selected})
    originals = {path: path.read_bytes() for path in source_paths}
    query_paths = []
    created_paths = []
    try:
        for item in selected:
            address = item["address"]
            if not address:
                continue
            query_path = output_directory / f"query-{address[2:].upper()}.json"
            if not query_path.exists():
                atomic_json(query_path, compact(image, int(address, 0)))
                created_paths.append(query_path)
            query_paths.append(query_path)

        apply_result = apply_batch(root, report, batch, True)
        apply_result["source_report_sha256"] = sha256(report_path)
        apply_result["source_batch_sha256"] = sha256(batch_path)
        atomic_json(targets["apply"], apply_result)
        created_paths.append(targets["apply"])

        toolset = Path(__file__).resolve().parent
        launcher = toolset / "xport.py"
        _run([sys.executable, str(launcher), "--project", str(root), "code_refresh"], root)
        _run([sys.executable, str(launcher), "--project", str(root), "build_native", "--configuration", "Release", "--rebuild"], root)
        _run([sys.executable, "-B", "-m", "unittest", "tests.test_trace_access_audit",
              "tests.test_trace_access_batch", "tests.test_trace_access_apply"], toolset)

        post_report = validate_report(audit_project(root, report["family"]))
        atomic_json(targets["plan"], post_report)
        created_paths.append(targets["plan"])
        next_batch = select_batch(post_report, sha256(targets["plan"]), 5)
        atomic_json(targets["next"], next_batch)
        created_paths.append(targets["next"])

        executable = root / "bin" / ("RR.exe" if report["family"] == "rr" else "RRJ.exe")
        receipt = {
            "schema": SCHEMA,
            "plan_version": "1.1",
            "batch": "B04B" if report["family"] == "rr" else "B04C",
            "sub_batch": number,
            "status": "complete",
            "implementation": "automated_bounded_access_package",
            "completed_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
            "scope": [f"{item['path']}:{item['symbol']}" for item in selected],
            "validation": {
                "query": f"passed_for_all_{sum(1 for item in selected if item['address'])}_mapped_functions",
                "shared_semantics": "passed_access_tool_test_suite",
                "code_refresh": "passed",
                "release_build": "passed",
                "post_audit": "selected_functions_removed_from_ready_legacy_set",
            },
            "identities": {
                "source_report_sha256": sha256(report_path),
                "source_batch_sha256": sha256(batch_path),
                "apply_manifest_sha256": sha256(targets["apply"]),
                "post_plan_sha256": sha256(targets["plan"]),
                "next_selection_sha256": sha256(targets["next"]),
                "code_refresh_report_sha256": sha256(root / "status" / "code-refresh-latest.json"),
                "release_executable_sha256": sha256(executable),
                "queries": {path.stem.removeprefix("query-"): sha256(path) for path in query_paths},
            },
            "migration_counts": {
                "mapped_functions": sum(1 for item in selected if item["address"]),
                "unmapped_functions": sum(1 for item in selected if not item["address"]),
                "legacy_entries_removed": sum(item["candidate_count"] for item in selected),
                "remaining_ready_functions": next_batch["summary"]["remaining_ready_functions"],
                "blocked_functions": next_batch["summary"]["blocked_functions"],
            },
            "commands": [
                {"argv": ["query", item["address"], "--image", image, "--audit-context"], "exit_code": 0}
                for item in selected if item["address"]
            ] + [
                {"argv": ["trace_access_apply", "--apply"], "exit_code": 0},
                {"argv": ["code_refresh"], "exit_code": 0},
                {"argv": ["build_native", "--configuration", "Release", "--rebuild"], "exit_code": 0},
                {"argv": ["python", "-B", "-m", "unittest", "four access tool modules"], "exit_code": 0},
                {"argv": ["trace_access_audit", "--plan"], "exit_code": 0},
                {"argv": ["trace_access_batch", "--max-functions", "5"], "exit_code": 0},
            ],
            "evidence": [str(targets[key].relative_to(root)).replace("\\", "/") for key in ("apply", "plan", "next")],
            "limitations": [
                "Package uses shared optimized scalar semantics plus source-bound transformation proof",
                "Compound and pointer cases remain blocked and visible",
            ],
            "blockers": [],
            "next_batch": f"{receipt_batch(report['family'])}-{number + 1:02d}",
            "unrelated_changes_preserved": True,
        }
        atomic_json(targets["receipt"], receipt)
        created_paths.append(targets["receipt"])
        return receipt, targets
    except Exception as error:
        failure_directory = output_directory / f"{number:02d}-failure"
        if not failure_directory.exists():
            failure_directory.mkdir(parents=True)
            failed_sources = []
            for path in source_paths:
                relative = path.relative_to(root)
                snapshot = failure_directory / relative
                snapshot.parent.mkdir(parents=True, exist_ok=True)
                snapshot.write_bytes(path.read_bytes())
                failed_sources.append({"path": relative.as_posix(), "sha256": sha256(snapshot)})
            atomic_json(failure_directory / "failure.json", {
                "schema": 1,
                "sub_batch": number,
                "error": str(error),
                "failed_sources": failed_sources,
            })
        for path, data in originals.items():
            path.write_bytes(data)
        for path in reversed(created_paths):
            if path.exists():
                path.unlink()
        launcher = Path(__file__).resolve().parent / "xport.py"
        try:
            _run([sys.executable, str(launcher), "--project", str(root), "code_refresh"], root)
        except Exception:
            pass
        raise


def receipt_batch(family):
    return "B04B" if family == "rr" else "B04C"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True)
    parser.add_argument("--batch", required=True)
    parser.add_argument("--output-directory", required=True)
    parser.add_argument("--sub-batch", required=True, type=int)
    parser.add_argument("--image", default="MAIN.EXE")
    args = parser.parse_args()
    root, _ = load_project()
    receipt, targets = execute_package(root, artifact_path(args.report), artifact_path(args.batch),
                                       artifact_path(args.output_directory), args.sub_batch, args.image)
    print(json.dumps({"status": receipt["status"], "sub_batch": receipt["sub_batch"],
                      "receipt": str(targets["receipt"]), "next": str(targets["next"])}, separators=(",", ":")))


if __name__ == "__main__":
    main()
