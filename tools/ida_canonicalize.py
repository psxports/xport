"""Compatibility CLI for the startup supervisor IDA canonicalization stage"""
import argparse
import json
from pathlib import Path

import startup
from xport_project import load_project, project_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, default=project_path('ida_exports', 'status/ida/images'))
    parser.add_argument('--configs', type=Path, default=project_path('ida_configs', 'tools/ida'))
    args = parser.parse_args()
    root, _ = load_project()
    print(json.dumps(startup.canonicalize_ida(root, args.exports, args.configs)))


if __name__ == '__main__':
    main()
