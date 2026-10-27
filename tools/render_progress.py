"""Compatibility command returning the SQLite queue without generating HTML"""
import json
from project_context import queue
from xport_project import load_project


def render():
    root, config = load_project()
    return queue(root, config)


if __name__ == '__main__':
    print(json.dumps(render(), separators=(',', ':')))
