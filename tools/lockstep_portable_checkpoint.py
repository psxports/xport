"""Single-slot, identity-bound container for a paired restart checkpoint"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import tempfile
from pathlib import Path


MAGIC = b"XPCK"
VERSION = 1
MAX_MANIFEST_SIZE = 4096
MAX_PAYLOAD_SIZE = 128 * 1024 * 1024
_HEADER = struct.Struct("<4sII")
_CHUNK_SIZE = 1024 * 1024


def _digest_file(path: Path) -> tuple[int, str]:
    size = 0
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(_CHUNK_SIZE), b""):
            size += len(chunk)
            if size > MAX_PAYLOAD_SIZE:
                raise ValueError("Checkpoint payload exceeds limit")
            digest.update(chunk)
    if not size:
        raise ValueError("Checkpoint payload is empty")
    return size, digest.hexdigest()


def _copy_file(source: Path, output) -> None:
    with source.open("rb") as stream:
        for chunk in iter(lambda: stream.read(_CHUNK_SIZE), b""):
            output.write(chunk)


def write_checkpoint(path: Path, *, record_id: str, frame: int,
                     duckstation_state: Path, native_state: Path,
                     duckstation_identity: str, native_abi: str) -> dict:
    if not isinstance(record_id, str) or len(record_id) != 64 or any(
            char not in "0123456789abcdef" for char in record_id):
        raise ValueError("Invalid checkpoint record identity")
    if type(frame) is not int or frame < 0 or frame >= 0xFFFFFFFF:
        raise ValueError("Invalid checkpoint frame")
    if not duckstation_identity or not native_abi:
        raise ValueError("Checkpoint runtime identities are required")
    payloads = (Path(duckstation_state), Path(native_state))
    sizes_and_hashes = [_digest_file(payload) for payload in payloads]
    manifest = {
        "contract": "xport-paired-restart-checkpoint-v1",
        "record_id": record_id,
        "frame": frame,
        "duckstation_identity": duckstation_identity,
        "native_abi": native_abi,
        "payloads": [
            {"name": name, "size": size, "sha256": digest}
            for name, (size, digest) in zip(("duckstation", "native"), sizes_and_hashes)
        ],
    }
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode("utf-8")
    if len(encoded) > MAX_MANIFEST_SIZE:
        raise ValueError("Checkpoint manifest exceeds limit")
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=path.name + ".", suffix=".tmp",
                                         dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(_HEADER.pack(MAGIC, VERSION, len(encoded)))
            output.write(encoded)
            for payload in payloads:
                _copy_file(payload, output)
            output.flush()
            os.fsync(output.fileno())
        verify_checkpoint(temporary, record_id=record_id,
                          duckstation_identity=duckstation_identity, native_abi=native_abi)
        os.replace(temporary, path)
        return manifest
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def verify_checkpoint(path: Path, *, record_id: str,
                      duckstation_identity: str, native_abi: str) -> dict:
    path = Path(path)
    with path.open("rb") as source:
        header = source.read(_HEADER.size)
        if len(header) != _HEADER.size:
            raise ValueError("Truncated checkpoint header")
        magic, version, manifest_size = _HEADER.unpack(header)
        if magic != MAGIC or version != VERSION or not 0 < manifest_size <= MAX_MANIFEST_SIZE:
            raise ValueError("Unsupported checkpoint header")
        encoded = source.read(manifest_size)
        if len(encoded) != manifest_size:
            raise ValueError("Truncated checkpoint manifest")
        manifest = json.loads(encoded)
        if (manifest.get("contract") != "xport-paired-restart-checkpoint-v1" or
                manifest.get("record_id") != record_id or
                manifest.get("duckstation_identity") != duckstation_identity or
                manifest.get("native_abi") != native_abi or
                type(manifest.get("frame")) is not int or manifest["frame"] < 0):
            raise ValueError("Checkpoint identity mismatch")
        payloads = manifest.get("payloads")
        if not isinstance(payloads, list) or len(payloads) != 2 or [
                item.get("name") for item in payloads if isinstance(item, dict)
        ] != ["duckstation", "native"]:
            raise ValueError("Invalid checkpoint payload table")
        offsets = {}
        for item in payloads:
            size = item.get("size")
            digest = item.get("sha256")
            if type(size) is not int or not 0 < size <= MAX_PAYLOAD_SIZE or \
                    not isinstance(digest, str) or len(digest) != 64:
                raise ValueError("Invalid checkpoint payload descriptor")
            offsets[item["name"]] = source.tell()
            actual = hashlib.sha256()
            remaining = size
            while remaining:
                chunk = source.read(min(remaining, _CHUNK_SIZE))
                if not chunk:
                    raise ValueError("Truncated checkpoint payload")
                actual.update(chunk)
                remaining -= len(chunk)
            if actual.hexdigest() != digest:
                raise ValueError("Checkpoint payload hash mismatch")
        if source.read(1):
            raise ValueError("Trailing checkpoint bytes")
    return {"manifest": manifest, "offsets": offsets}


def extract_checkpoint(path: Path, *, record_id: str, duckstation_identity: str,
                       native_abi: str, duckstation_state: Path,
                       native_state: Path) -> dict:
    verified = verify_checkpoint(path, record_id=record_id,
                                 duckstation_identity=duckstation_identity,
                                 native_abi=native_abi)
    targets = (Path(duckstation_state), Path(native_state))
    if targets[0] == targets[1] or Path(path) in targets:
        raise ValueError("Checkpoint extraction paths must be distinct")
    temporary_files = []
    try:
        with Path(path).open("rb") as source:
            for target, descriptor in zip(targets, verified["manifest"]["payloads"]):
                target.parent.mkdir(parents=True, exist_ok=True)
                source.seek(verified["offsets"][descriptor["name"]])
                with tempfile.NamedTemporaryFile(prefix=target.name + ".", suffix=".tmp",
                                                 dir=target.parent, delete=False) as output:
                    temporary_files.append(Path(output.name))
                    remaining = descriptor["size"]
                    digest = hashlib.sha256()
                    while remaining:
                        chunk = source.read(min(remaining, _CHUNK_SIZE))
                        if not chunk:
                            raise ValueError("Checkpoint changed during extraction")
                        output.write(chunk)
                        digest.update(chunk)
                        remaining -= len(chunk)
                    if digest.hexdigest() != descriptor["sha256"]:
                        raise ValueError("Checkpoint changed during extraction")
                    output.flush()
                    os.fsync(output.fileno())
        for temporary, target in zip(temporary_files, targets):
            os.replace(temporary, target)
        return verified["manifest"]
    finally:
        for temporary in temporary_files:
            temporary.unlink(missing_ok=True)
