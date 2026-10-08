"""Summarize durable lockstep supervisor state without reading guest memory"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from lockstep_packet import boundary_evidence


def _post_call_candidate(attempt: dict) -> bool:
    if attempt.get("status") != "attention":
        return False
    try:
        return boundary_evidence(attempt.get("result") or {}) is not None
    except (KeyError, TypeError, ValueError):
        return False


def summarize(state: dict) -> dict:
    attempts = state.get("attempts") or []
    telemetry = state.get("telemetry") or {}
    completed = [attempt for attempt in attempts
                 if isinstance(attempt.get("started_at"), (int, float)) and
                 isinstance(attempt.get("finished_at"), (int, float))]
    seconds = sum(max(0.0, item["finished_at"] - item["started_at"])
                  for item in completed)
    reasons = telemetry.get("launch_reasons") or {}
    invocations = (telemetry.get("model_invocations_reported")
                   if telemetry.get("model_accounting_complete") is True else None)
    covered = telemetry.get("model_usage_reported_invocations", 0)
    revisions = {(item.get("source_identity"), item.get("implementation_identity"))
                 for item in attempts if item.get("source_identity")}
    checkpoint_attempts = [item for item in attempts
                           if type(item.get("checkpoint_captures")) is int and
                           item["checkpoint_captures"] > 0]
    checkpoint_captures = sum(item["checkpoint_captures"] for item in checkpoint_attempts)
    checkpoint_measured = all(
        type(item.get("checkpoint_capture_seconds")) in (int, float) and
        type(item.get("checkpoint_capture_bytes_total")) is int
        for item in checkpoint_attempts)
    checkpoint_seconds = (sum(item["checkpoint_capture_seconds"] for item in checkpoint_attempts)
                          if checkpoint_measured else None)
    return {
        "contract": "xport-lockstep-metrics-v1",
        "state_contract": state.get("contract"),
        "status": state.get("status"),
        "attempts": len(attempts),
        "completed_attempts": len(completed),
        "completed_attempt_seconds": round(seconds, 3),
        "duckstation_launches": telemetry.get("duckstation_launches", 0),
        "native_launches": telemetry.get("native_launches", 0),
        "diagnostic_refinement_pairs": reasons.get("duckstation:diagnostic_refinement", 0),
        "same_case_writer_refinement_pairs": reasons.get("duckstation:same_case_writer_refinement", 0),
        "external_refinement_pairs": telemetry.get("external_refinement_pairs", 0),
        "external_refinement_seconds": round(
            telemetry.get("external_refinement_seconds", 0.0), 3),
        "duckstation_launches_including_external": (
            telemetry.get("duckstation_launches", 0) +
            telemetry.get("external_refinement_pairs", 0)),
        "native_launches_including_external": (
            telemetry.get("native_launches", 0) +
            telemetry.get("external_refinement_pairs", 0)),
        "checkpoint_diagnostic_reuses": telemetry.get("checkpoint_diagnostic_reuses", 0),
        "checkpoint_writer_reuses": telemetry.get("checkpoint_writer_reuses", 0),
        "checkpoint_captures": checkpoint_captures,
        "checkpoint_capture_seconds": (round(checkpoint_seconds, 3)
                                       if checkpoint_seconds is not None else None),
        "checkpoint_capture_mean_ms": (round(1000.0 * checkpoint_seconds / checkpoint_captures, 3)
                                       if checkpoint_seconds is not None and checkpoint_captures else None),
        "checkpoint_capture_bytes_total": (sum(item["checkpoint_capture_bytes_total"]
                                               for item in checkpoint_attempts)
                                           if checkpoint_measured else None),
        "historical_post_call_refinement_candidates": sum(
            _post_call_candidate(item) for item in attempts),
        "distinct_source_implementation_revisions": len(revisions),
        "debug_builds": telemetry.get("debug_builds", 0),
        "debug_build_reuses": telemetry.get("debug_build_reuses", 0),
        "attention_packets": telemetry.get("attention_packets", telemetry.get("model_calls", 0)),
        "attention_packet_bytes": sum(item.get("attention_packet_bytes", 0) for item in attempts
                                      if type(item.get("attention_packet_bytes")) is int),
        "candidate_model_context_bytes": sum(item.get("candidate_model_context_bytes", 0)
                                             for item in attempts
                                             if type(item.get("candidate_model_context_bytes")) is int),
        "candidate_model_context_available": sum(
            type(item.get("candidate_model_context_bytes")) is int for item in attempts),
        'candidate_model_prompt_bytes': sum(item.get('candidate_model_prompt_bytes', 0)
                                            for item in attempts
                                            if type(item.get('candidate_model_prompt_bytes')) is int),
        'candidate_model_prompt_available': sum(
            type(item.get('candidate_model_prompt_bytes')) is int for item in attempts),
        "actual_model_invocations": invocations,
        "model_usage_reported_invocations": covered if invocations is not None else None,
        "actual_model_tokens": (telemetry.get("model_total_tokens_reported", 0)
                                if invocations is not None and covered == invocations else None),
        "reported_model_input_tokens": telemetry.get("model_input_tokens_reported"),
        "reported_model_output_tokens": telemetry.get("model_output_tokens_reported"),
        "reported_model_cached_input_tokens": telemetry.get("model_cached_input_tokens_reported"),
        "reported_model_reasoning_output_tokens": telemetry.get("model_reasoning_output_tokens_reported"),
        "phase_seconds": telemetry.get("phase_seconds") or {},
        "phase_counts": telemetry.get("phase_counts") or {},
    }


def current_prompt_metrics(state: dict, state_path: Path) -> dict:
    result = {"current_packet": None, "current_packet_bytes": None,
              "current_model_prompt_bytes": None,
              "current_model_prompt_status": "no_attention_packet"}
    if state.get("status") != "ATTENTION_REQUIRED" or not state.get("packet"):
        return result
    state_path = state_path.resolve()
    root = next((directory for directory in state_path.parents
                 if (directory / "xport-project.json").is_file()), None)
    if root is None:
        result["current_model_prompt_status"] = "project_config_missing"
        return result
    config = json.loads((root / "xport-project.json").read_text(encoding="utf-8"))
    status_root = (root / config["paths"]["status"] / "lockstep-converge").resolve()
    packet_path = Path(state["packet"]).resolve()
    if (not state_path.is_relative_to(status_root) or
            packet_path.parent != state_path.parent or
            not packet_path.is_file()):
        result["current_model_prompt_status"] = "packet_outside_project_run"
        return result
    result["current_packet"] = str(packet_path)
    result["current_packet_bytes"] = packet_path.stat().st_size
    packet = json.loads(packet_path.read_text(encoding="utf-8"))
    try:
        from lockstep_repair_context import build_prompt
        from lockstep_supervisor import shared_source_identity

        shared = (packet.get("identities") or {}).get("shared_source_identity")
        if shared is not None and shared != shared_source_identity(root, config):
            result["current_model_prompt_status"] = "shared_source_changed"
            return result
        roots = tuple(Path(value) for value in config["lockstep"].get("source_roots", ["src"]))
        prompt = build_prompt(packet, root=root, source_roots=roots)
    except (KeyError, TypeError, ValueError):
        result["current_model_prompt_status"] = "context_unavailable"
        return result
    result["current_model_prompt_bytes"] = len(prompt.encode("utf-8"))
    result["current_model_prompt_status"] = "available"
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("state", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    state = json.loads(args.state.read_text(encoding="utf-8"))
    report = summarize(state)
    report.update(current_prompt_metrics(state, args.state))
    content = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(content, encoding="utf-8")
    else:
        print(content, end="")


if __name__ == "__main__":
    main()
