#!/usr/bin/env python3
"""Regenerate the single DuckStation patch from its prepared source tree"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path


DIFF_HEADER = re.compile(r"^diff --git a/(\S+) b/\1$", re.MULTILINE)
UNIFIED_HEADER = re.compile(r"^--- a/(\S+)$", re.MULTILINE)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckstation", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--add", action="append", default=[])
    args = parser.parse_args()

    root = args.duckstation.resolve()
    manifest_path = root / "duckstation-manifest.json"
    patch_path = root / "internal-trace-api.patch"
    source = args.source.resolve()
    if not source.is_dir():
        raise FileNotFoundError(source)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    patch_text = patch_path.read_text(encoding="utf-8")
    paths = [path.removeprefix("b/") for path in DIFF_HEADER.findall(patch_text)]
    paths.extend(path for path in UNIFIED_HEADER.findall(patch_text) if path not in paths)
    paths.extend(path for path in args.add if path not in paths)
    if not paths:
        raise RuntimeError("Patch has no tracked paths")

    with tempfile.TemporaryDirectory(prefix="xport-duckstation-patch-") as temporary:
        temporary_path = Path(temporary)
        before = temporary_path / "a"
        after = temporary_path / "b"
        patched = temporary_path / "patched"
        patched.mkdir()
        existing_paths = []
        for relative in paths:
            present = subprocess.run(
                ["git", f"--git-dir={args.cache.resolve()}", "cat-file", "-e",
                 f"{manifest['upstream']['commit']}:{relative}"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            ).returncode == 0
            if present:
                existing_paths.append(relative)
        archive_path = temporary_path / "base.tar"
        with archive_path.open("wb") as archive:
            subprocess.run(
                ["git", f"--git-dir={args.cache.resolve()}", "archive", "--format=tar",
                 manifest["upstream"]["commit"], *existing_paths],
                check=True, stdout=archive, stderr=subprocess.PIPE,
            )
        with tarfile.open(archive_path) as archive:
            archive.extractall(patched)
        applied = subprocess.run(
            ["git", "apply", "--ignore-space-change", "--ignore-whitespace", str(patch_path)],
            cwd=patched,
            capture_output=True,
        )
        if applied.returncode != 0:
            raise RuntimeError(applied.stderr.decode("utf-8", errors="replace"))
        for relative in paths:
            source_path = source / relative
            if not source_path.is_file():
                source_path = patched / relative
            if not source_path.is_file():
                raise FileNotFoundError(source_path)
            destination = after / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source_path.read_bytes().replace(b"\r\n", b"\n"))
            original = subprocess.run(
                ["git", f"--git-dir={args.cache.resolve()}", "show",
                 f"{manifest['upstream']['commit']}:{relative}"],
                capture_output=True,
            )
            if original.returncode == 0:
                base = before / relative
                base.parent.mkdir(parents=True, exist_ok=True)
                base.write_bytes(original.stdout)
        result = subprocess.run(
            ["git", "diff", "--no-index", "--binary", "--", "a", "b"],
            cwd=temporary_path, capture_output=True,
        )
        if result.returncode not in (0, 1):
            raise RuntimeError(result.stderr.decode("utf-8", errors="replace"))
        content = result.stdout.replace(b"a/a/", b"a/").replace(b"a/b/", b"a/").replace(b"b/b/", b"b/")
        patch_path.write_bytes(content)

    digest = hashlib.sha256(patch_path.read_bytes()).hexdigest()
    manifest["patch"]["sha256"] = digest
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"source": str(source), "paths": len(paths),
                      "bytes": patch_path.stat().st_size, "sha256": digest}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
