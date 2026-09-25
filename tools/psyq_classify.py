"""Compatibility CLI for the startup supervisor PsyQ classification stage"""
import argparse
import json
from pathlib import Path

import startup
from xport_project import load_project


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reports', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root, _ = load_project()
    print(json.dumps(startup.classify_psyq(root, args.reports, args.output)))


if __name__ == '__main__':
    main()
