"""Compatibility CLI for the startup supervisor IDA export stage"""
import argparse
import json
from pathlib import Path

import startup
from xport_project import load_project


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ida', type=Path, required=True)
    parser.add_argument('--image', action='append')
    parser.add_argument('--label', required=True)
    parser.add_argument('--export-existing', action='store_true')
    parser.add_argument('--plan', action='store_true')
    args = parser.parse_args()
    root, _ = load_project()
    result = startup.prepare_ida_export(root, args.ida, args.label, args.image,
                                        args.export_existing, args.plan)
    print(json.dumps(result))


if __name__ == '__main__':
    main()
