"""Start manual recording from a numbered DuckStation game save slot"""

from __future__ import annotations

import argparse
import json

from interactive_record import start
from trace_workflow import status, stop_record
from xport_project import load_project


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action_or_slot")
    parser.add_argument("name", nargs="?")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    root, config = load_project()
    try:
        if args.action_or_slot == "stop" and args.name is None:
            result = stop_record(root, config, None, args.timeout)
        elif args.action_or_slot == "status" and args.name:
            result = status(root, config, args.name)
        elif args.name:
            slot = int(args.action_or_slot)
            result = start(root, config, None, args.name, args.timeout, slot=slot)
        else:
            parser.error("Use SLOT NAME, stop, or status NAME")
    except (OSError, RuntimeError, ValueError) as error:
        result = {"status": "attention_required", "error": str(error)}
    print(json.dumps(result, separators=(",", ":")))
    return 0 if result.get("status") in {"recording", "stopping", "complete", "idle"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
