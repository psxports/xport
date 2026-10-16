#!/usr/bin/env python3
"""Persistent CDB controller for native xport lockstep boundaries"""

from __future__ import annotations

import hashlib
import math
import os
import queue
import re
import struct
import subprocess
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path

from lockstep_contract import (EVENT_CONTRACT, EXCEPTION_CONTRACT, SCHEMA,
                               validate_event, validate_exceptions)
from xport_process import normalized_environment

BOUNDARY_CONTRACT = "xport-lockstep-native-boundary-v1"

class CdbError(RuntimeError):
    pass


class CdbProtocolError(CdbError):
    pass


class CdbTimeout(CdbError):
    def __init__(self, message: str, *, process_id: int | None = None, instruction: int | None = None):
        super().__init__(message)
        self.process_id = process_id
        self.instruction = instruction


class PdbIdentityError(CdbError):
    pass


@dataclass(frozen=True)
class CodeViewIdentity:
    guid: bytes
    age: int

    @property
    def text(self) -> str:
        data1, data2, data3 = struct.unpack_from("<IHH", self.guid)
        tail = self.guid[8:]
        return f"{data1:08x}-{data2:04x}-{data3:04x}-{tail[:2].hex()}-{tail[2:].hex()}-{self.age}"


@dataclass
class Invocation:
    invocation: int
    parent_invocation: int
    depth: int
    call_ordinal: int
    image: str
    function: str
    address: int
    file: str
    line: int
    return_kind: int
    return_address: int
    frame: int
    arguments: list[dict]


def _u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise PdbIdentityError("Truncated debug identity")
    return struct.unpack_from("<I", data, offset)[0]


def _pe_rva_to_offset(data: bytes, pe_offset: int, rva: int) -> int:
    section_count = struct.unpack_from("<H", data, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    sections = pe_offset + 24 + optional_size
    for index in range(section_count):
        entry = sections + index * 40
        virtual_size = _u32(data, entry + 8)
        virtual_address = _u32(data, entry + 12)
        raw_size = _u32(data, entry + 16)
        raw_address = _u32(data, entry + 20)
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_address + rva - virtual_address
    raise PdbIdentityError(f"PE RVA 0x{rva:x} is outside sections")


def read_pe_codeview(path: Path) -> CodeViewIdentity:
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise PdbIdentityError("Executable has no DOS header")
    pe = _u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise PdbIdentityError("Executable has no PE header")
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directory = optional + (96 if magic == 0x10B else 112 if magic == 0x20B else 0)
    if not directory:
        raise PdbIdentityError("Unsupported PE optional header")
    debug_rva = _u32(data, directory + 6 * 8)
    debug_size = _u32(data, directory + 6 * 8 + 4)
    if not debug_rva or debug_size < 28:
        raise PdbIdentityError("Executable has no debug directory")
    debug_offset = _pe_rva_to_offset(data, pe, debug_rva)
    for offset in range(debug_offset, debug_offset + debug_size, 28):
        if _u32(data, offset + 12) != 2:
            continue
        raw_size = _u32(data, offset + 16)
        raw_offset = _u32(data, offset + 24)
        record = data[raw_offset:raw_offset + raw_size]
        if record[:4] == b"RSDS" and len(record) >= 24:
            return CodeViewIdentity(record[4:20], _u32(record, 20))
    raise PdbIdentityError("Executable has no RSDS CodeView record")


def read_pdb_identity(path: Path) -> CodeViewIdentity:
    data = path.read_bytes()
    signature = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"
    if not data.startswith(signature) or len(data) < 56:
        raise PdbIdentityError("Unsupported PDB container")
    block_size = _u32(data, 32)
    block_count = _u32(data, 40)
    directory_size = _u32(data, 44)
    block_map = _u32(data, 52)
    if block_size < 512 or block_size > 65536 or block_size & (block_size - 1):
        raise PdbIdentityError("Invalid PDB block size")
    if block_count * block_size > len(data) + block_size:
        raise PdbIdentityError("Truncated PDB blocks")
    directory_blocks = math.ceil(directory_size / block_size)
    map_offset = block_map * block_size
    indices = [_u32(data, map_offset + index * 4) for index in range(directory_blocks)]
    directory = b"".join(data[index * block_size:(index + 1) * block_size] for index in indices)
    directory = directory[:directory_size]
    stream_count = _u32(directory, 0)
    if stream_count < 2 or stream_count > 0x10000:
        raise PdbIdentityError("PDB has no information stream")
    sizes = [_u32(directory, 4 + index * 4) for index in range(stream_count)]
    cursor = 4 + stream_count * 4
    streams: list[list[int]] = []
    for size in sizes:
        count = 0 if size == 0xFFFFFFFF else math.ceil(size / block_size)
        blocks = [_u32(directory, cursor + index * 4) for index in range(count)]
        cursor += count * 4
        streams.append(blocks)
    info = b"".join(data[index * block_size:(index + 1) * block_size] for index in streams[1])
    info = info[:sizes[1]]
    if len(info) < 28:
        raise PdbIdentityError("Truncated PDB information stream")
    return CodeViewIdentity(info[12:28], _u32(info, 8))


def verify_pdb_identity(executable: Path, pdb: Path) -> CodeViewIdentity:
    executable_identity = read_pe_codeview(executable)
    pdb_identity = read_pdb_identity(pdb)
    if executable_identity != pdb_identity:
        raise PdbIdentityError(
            f"PDB mismatch: executable={executable_identity.text} pdb={pdb_identity.text}"
        )
    return executable_identity


def resolve_cdb(path: Path | None = None) -> Path:
    candidates = [path] if path else []
    candidates.extend([
        Path(r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe"),
        Path(r"C:\Program Files\Windows Kits\10\Debuggers\x86\cdb.exe"),
    ])
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate.resolve()
    raise CdbError("x86 CDB was not found")


class CdbController:
    """Own one debugger and expose stopped semantic native events"""

    _MARKER = re.compile(r"XPORT\|([^%\r\n]+)\r?$")

    def __init__(self, executable: Path, pdb: Path, *, arguments: tuple[str, ...] = (),
                 cwd: Path | None = None, cdb: Path | None = None, timeout: float = 10.0,
                 memory_ready: bool = False, environment: dict[str, str] | None = None,
                 boundary_addresses: frozenset[int] | None = None):
        self.executable = executable.resolve()
        self.pdb = pdb.resolve()
        self.arguments = arguments
        self.cwd = (cwd or executable.parent).resolve()
        self.cdb = resolve_cdb(cdb)
        self.timeout = timeout
        self.memory_ready = memory_ready
        self.environment = environment or {}
        self.boundary_addresses = boundary_addresses
        self.identity = verify_pdb_identity(self.executable, self.pdb)
        self.token = uuid.uuid4().hex
        self.module = self.executable.stem
        self.process: subprocess.Popen[bytes] | None = None
        self._reader: threading.Thread | None = None
        self._markers: queue.Queue[str] = queue.Queue()
        self._output = bytearray()
        self._output_lock = threading.Lock()
        self._running = False
        self._closed = False
        self._target_pid: int | None = None
        self._event_sequence = 0
        self._mode = "DETAILED"
        self._epoch = 0
        self._boundary_sequence = 0
        self._next_invocation = 1
        self._stack: list[Invocation] = []
        self._ordinals: dict[int, int] = {}
        self._return_breakpoints: set[int] = set()
        self._refinement_watchpoint = False

    @property
    def output(self) -> str:
        with self._output_lock:
            return self._output.decode("utf-8", errors="replace")

    @property
    def target_pid(self) -> int | None:
        return self._target_pid

    def start(self) -> None:
        if self.process is not None:
            raise CdbError("Controller already started")
        creationflags = (getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) |
                         getattr(subprocess, "CREATE_NO_WINDOW", 0))
        argv = [str(self.cdb), "-lines", "-y", str(self.pdb.parent), str(self.executable), *self.arguments]
        self.process = subprocess.Popen(
            argv, cwd=self.cwd, env=normalized_environment(updates=self.environment), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=creationflags,
        )
        self._reader = threading.Thread(target=self._read_output, name="xport-cdb-reader", daemon=True)
        self._reader.start()
        self._send(f'.reload /f {self.module}.exe')
        for name in ("xport_lockstep_boundary_break", "xport_lockstep_vblank_break",
                     "xport_lockstep_transition_break"):
            symbol = f"{self.module}!{name}"
            self._send(
                f'bu {symbol} ".printf \\"XPORT|BOUNDARY|{self.token}|%x\\\\n\\", @$tpid"'
            )
        if self.memory_ready:
            ready_symbol = f"{self.module}!xport_lockstep_memory_ready"
            self._send(
                f'bu {ready_symbol} ".printf \\"XPORT|MEMORY_READY|{self.token}|%x\\\\n\\", @$tpid"'
            )
        self._send(
            f'sxe -c ".printf \\"XPORT|FAULT|{self.token}|%x|%p\\\\n\\", @$tpid, @eip" av'
        )
        self._install_abort_breakpoints()
        self._send(
            f'sxe -c ".printf \\"XPORT|EXIT|{self.token}|%x\\\\n\\", @$tpid" epr'
        )
        self._send(f'.printf "XPORT|READY|{self.token}|%x\\n", @$tpid')
        marker = self._wait("READY", self.timeout)
        self._remember_pid(marker[2])
        self.continue_execution()

    def wait_memory_ready(self, timeout: float | None = None) -> dict:
        if not self.memory_ready or not self._running:
            raise CdbError("Memory-ready breakpoint is not active")
        marker = self._wait("MEMORY_READY", timeout or self.timeout)
        self._remember_pid(marker[2])
        self._running = False
        return {"status": "memory_ready", "process_id": self._target_pid}

    def memory_exceptions(self, image_sha256: str) -> dict:
        if self._running:
            raise CdbError("Target must be stopped before reading memory exceptions")
        names = {0: "dram", 1: "scratchpad"}
        kinds = {0: "guest_stack", 1: "firmware_runtime", 2: "trusted_device_state"}
        ranges = []
        for owner in ("xport", "game"):
            count_symbol = f"{self.module}!{owner}_memory_exception_count"
            array_symbol = f"{self.module}!{owner}_memory_exceptions"
            self._send(f'.printf "XPORT|EXCOUNT|{self.token}|{owner}|%x\\n", poi({count_symbol})')
            count_marker = self._wait("EXCOUNT", self.timeout)
            if len(count_marker) != 4 or count_marker[2] != owner:
                raise CdbProtocolError(f"Malformed memory exception count: {count_marker}")
            count = int(count_marker[3], 16)
            if count > 256:
                raise CdbProtocolError("Memory exception count is unreasonable")
            for index in range(count):
                address = f"{array_symbol}+0x{index * 16:x}"
                self._send(
                    f'.printf "XPORT|EXROW|{self.token}|{owner}|{index}|%x|%x|%x|%x\\n", '
                    f'poi({address}), poi({address}+4), poi({address}+8), poi({address}+0xc)'
                )
                marker = self._wait("EXROW", self.timeout)
                if len(marker) != 8 or marker[2] != owner or int(marker[3]) != index:
                    raise CdbProtocolError(f"Malformed memory exception row: {marker}")
                region_value, offset, size, kind_value = (int(value, 16) for value in marker[4:])
                if region_value not in names or kind_value not in kinds:
                    raise CdbProtocolError("Unknown compiled memory exception value")
                ranges.append({
                    "region": names[region_value], "offset": offset, "size": size,
                    "kind": kinds[kind_value], "owner": owner,
                    "name": f"{owner}_{index}",
                })
        ranges.sort(key=lambda row: (row["region"], row["offset"]))
        value = {"schema": SCHEMA, "contract": EXCEPTION_CONTRACT,
                 "image_sha256": image_sha256, "ranges": ranges}
        return validate_exceptions(value)

    def _read_output(self) -> None:
        assert self.process and self.process.stdout
        pending = bytearray()
        while True:
            try:
                chunk = os.read(self.process.stdout.fileno(), 4096)
            except OSError:
                return
            if not chunk:
                return
            with self._output_lock:
                self._output.extend(chunk)
                if len(self._output) > 2 * 1024 * 1024:
                    del self._output[:-1024 * 1024]
            pending.extend(chunk)
            while b"\n" in pending:
                raw, _, remainder = pending.partition(b"\n")
                pending = bytearray(remainder)
                text = raw.decode("utf-8", errors="replace")
                match = self._MARKER.search(text)
                if match:
                    self._markers.put(match.group(1).rstrip("\r"))

    def _send(self, command: str) -> None:
        if not self.process or not self.process.stdin or self.process.poll() is not None:
            raise CdbProtocolError("CDB is not running")
        self.process.stdin.write((command + "\n").encode("ascii", errors="strict"))
        self.process.stdin.flush()

    def _wait(self, kind: str, timeout: float) -> list[str]:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CdbTimeout(f"Timed out waiting for CDB {kind}\n{self.output[-8000:]}", process_id=self._target_pid)
            try:
                marker = self._markers.get(timeout=min(remaining, 0.1))
            except queue.Empty:
                if self.process and self.process.poll() is not None:
                    raise CdbProtocolError(f"CDB exited while waiting for {kind}: {self.process.returncode}")
                continue
            parts = marker.split("|")
            if len(parts) >= 2 and parts[0] == kind and parts[1] == self.token:
                return parts

    def _remember_pid(self, value: str) -> None:
        observed = int(value.split()[0], 16)
        if self._target_pid is None:
            self._target_pid = observed
        elif observed != self._target_pid:
            raise CdbProtocolError(f"Target PID changed from {self._target_pid} to {observed}")

    def continue_execution(self) -> None:
        if self._closed:
            raise CdbError("Controller is closed")
        if self._running:
            return
        self._send("g")
        self._running = True

    def set_breakpoint(self, address: int, *, one_shot: bool = False) -> None:
        if self._running:
            raise CdbError("Target must be stopped before setting a breakpoint")
        suffix = " /1" if one_shot else ""
        self._send(f"bp{suffix} 0x{address:x}")

    def stop_at_game_entry(self, symbol: str) -> None:
        if self._running or not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', symbol):
            raise CdbError('Invalid stopped game entry request')
        self._send(f'bu /1 {self.module}!{symbol} ".printf \\"XPORT|GAME_ENTRY|{self.token}|%x\\\\n\\", @$tpid"')
        self.continue_execution()
        marker = self._wait('GAME_ENTRY', self.timeout)
        self._running = False
        self._remember_pid(marker[2])

    def set_ot_submission_breakpoint(self, *, draw_ot_only=False) -> None:
        if self._running:
            raise CdbError("Target must be stopped before installing the OT observer")
        entries = (("DrawOTag", 0),) if draw_ot_only else (("DrawOTag", 0), ("gpu_draw_env_native", 28))
        for symbol, packet_offset in entries:
            self._send(
                f'bu {self.module}!{symbol} ".printf \\"XPORT|OT|{self.token}|%x|%x|%x|%y|%y|%y|%y\\\\n\\", '
                f'@$tpid, poi(@esp+4)+{packet_offset:x}-poi({self.module}!xport_lockstep_dram), '
                f'poi({self.module}!frame_entry_index), poi(@esp), poi(@ebp+4), '
                f'poi(poi(@ebp)+4), poi(poi(poi(@ebp))+4)"'
            )

    def _install_abort_breakpoints(self) -> None:
        for runtime in (self.module, "ucrtbased", "ucrtbase"):
            self._send(
                f'bu {runtime}!abort ".printf \\"XPORT|FAULT|{self.token}|%x|%p\\\\n\\", @$tpid, @eip"'
            )

    def probe_function_entry(self, symbol: str, output_address: int, *,
                             max_hits: int = 128, timeout: float = 30.0,
                             second_storage: str = "guest",
                             first_storage: str = "scalar",
                             caller_stack_offsets: tuple[int, ...] = (),
                             match_occurrence: int = 1) -> dict:
        if self._running:
            raise CdbError("Target must be stopped before probing a function entry")
        if (not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", symbol) or
                not 0 <= output_address <= 0xFFFFFFFF or not 1 <= max_hits <= 1024 or
                not 0 < timeout <= 120 or second_storage not in {"guest", "host", "none"} or
                first_storage not in {"scalar", "guest"} or
                not 1 <= match_occurrence <= max_hits or
                len(caller_stack_offsets) > 8 or any(
                    type(offset) is not int or offset % 4 or not -256 <= offset <= 256
                    for offset in caller_stack_offsets)):
            raise ValueError("Native function entry probe is invalid")
        self._send("bc *")
        self._install_abort_breakpoints()
        host_values = ("|%x|%x|%x|%x" if second_storage == "host" else "")
        host_reads = (", dwo(poi(@esp+8)), dwo(poi(@esp+8)+4), "
                      "dwo(poi(@esp+8)+8), dwo(poi(@esp+8)+0xc)"
                      if second_storage == "host" else "")
        caller_formats = "|%x" * len(caller_stack_offsets)
        first_expression = ('poi(@esp+4)' if first_storage == 'scalar' else
                            f'poi(@esp+4)-poi({self.module}!xport_lockstep_dram)+0x80000000')
        caller_reads = "".join(
            f", dwo(@ebp{'+' if offset >= 0 else '-'}0x{abs(offset):x})"
            for offset in caller_stack_offsets)
        self._send(
            f'bm {self.module}!{symbol} ".printf \\"XPORT|ENTRY|{self.token}|%x|%p|%p|%p|%p|%p|%p'
            f'{host_values}{caller_formats}|%y\\\\n\\", @$tpid, @eip, poi(@esp), {first_expression}, '
            f'poi(@esp+8), poi(@esp+0xc), poi(@esp+0x10){host_reads}{caller_reads}, poi(@esp)"'
        )
        matching_hits = 0
        for hit_number in range(1, max_hits + 1):
            self.continue_execution()
            marker = self._wait("ENTRY", timeout)
            self._running = False
            if len(marker) != (14 if second_storage == "host" else 10) + len(caller_stack_offsets):
                raise CdbProtocolError(f"Malformed native entry marker: {marker}")
            self._remember_pid(marker[2])
            pc, caller, first, second, third, fourth = (
                int(value.split()[0], 16) for value in marker[3:9])
            if first == output_address:
                matching_hits += 1
                if matching_hits != match_occurrence:
                    continue
                result = {"symbol": symbol, "pc": f"0x{pc:08X}",
                          "caller_pc": f"0x{caller:08X}",
                          "caller_symbol": marker[-1].strip(),
                          "output_address": f"0x{first:08X}",
                          "arguments": [f"0x{value:08X}" for value in
                                        (first, second, third, fourth)],
                          "hits": hit_number, "match_occurrence": match_occurrence,
                          "second_storage": second_storage, "first_storage": first_storage}
                if second_storage != "none":
                    result["right_address"] = f"0x{second:08X}"
                if second_storage == "host":
                    words = (int(value.split()[0], 16) for value in marker[9:13])
                    result["right_data"] = b"".join(
                        word.to_bytes(4, "little") for word in words).hex()
                stack_start = 13 if second_storage == "host" else 9
                if caller_stack_offsets:
                    result["caller_stack"] = [
                        {"offset": offset, "value": f"0x{int(value.split()[0], 16):08X}"}
                        for offset, value in zip(caller_stack_offsets,
                                                 marker[stack_start:-1])]
                return result
        raise CdbError(f"Native function entry did not receive the requested output address: "
                       f"last arguments {first:08X}, {second:08X}, {third:08X}, {fourth:08X}; "
                       f"caller {caller:08X}")

    def set_causal_entry_breakpoint(self, symbol: str, *, quaternion_inputs: bool = False,
                                   all_symbol_matches: bool = False) -> None:
        if self._running or not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', symbol):
            raise CdbError('Invalid causal entry request')
        if quaternion_inputs and symbol != 'sub_8001005C':
            raise CdbError('Quaternion capture requires the reviewed quaternion conversion ABI')
        first = (f'poi(@esp+4)-poi({self.module}!xport_lockstep_dram)+0x80000000'
                 if quaternion_inputs else 'poi(@esp+4)')
        formats = '|%x|%x|%x|%x' if quaternion_inputs else ''
        reads = (', poi(poi(@esp+8)), poi(poi(@esp+8)+4), '
                 'poi(poi(@esp+8)+8), poi(poi(@esp+8)+0xc)' if quaternion_inputs else '')
        command = 'bm' if all_symbol_matches else 'bu'
        self._send(
            f'{command} {self.module}!{symbol} ".printf \\"XPORT|CAUSAL_ENTRY|{self.token}|%x|%x|%x|%x|%x{formats}\\\\n\\", '
            f'@$tpid, {first}, poi(@esp+8), poi(@esp+0xc), poi(@esp+0x10){reads}"')

    def set_data_watchpoint(self, region: str, offset: int, width: int = 1) -> None:
        if self._running:
            raise CdbError("Target must be stopped before setting a data watchpoint")
        if self._refinement_watchpoint:
            raise CdbError("Only one refinement watchpoint is supported per fresh replay")
        limits = {"dram": 0x200000, "scratchpad": 0x400, "vram": 0x100000}
        symbols = {"dram": "xport_lockstep_dram", "scratchpad": "xport_lockstep_scratchpad",
                   "vram": "xport_lockstep_vram"}
        if (region not in limits or width not in {1, 2, 4} or offset < 0 or
                offset % width or offset + width > limits[region]):
            raise CdbError("Native watchpoint is outside lockstep memory")
        address = f"poi({self.module}!{symbols[region]})+0x{offset:x}"
        readers = {1: "by", 2: "wo", 4: "dwo"}
        reader = readers[width]
        self._send(f"r @$t0={reader}({address})")
        self._send(
            f'ba w{width} /1 {address} ".printf \\"XPORT|WATCH|{self.token}|%x|%p|{region}|{offset:x}|{width:x}|%x|%x|%p|%p|%y|%y|%y|%y\\n\\", '
            f'@$tpid, @eip, @$t0, {reader}({address}), poi(@ebp+4), poi(poi(@ebp)+4), '
            f'poi(@ebp+4), poi(poi(@ebp)+4), poi(poi(poi(@ebp))+4), '
            f'poi(poi(poi(poi(@ebp)))+4)"'
        )
        self._refinement_watchpoint = True

    def capture_writer_locals(self) -> str:
        if self._running:
            raise CdbError('Writer locals require a stopped target')
        start = len(self.output)
        self._send('.frame 0')
        self._send('dv /t')
        self._send('.frame 2')
        self._send('dv /t')
        self._send(f'dt -r2 {self.module}!gte')
        self._send('.frame 0')
        self._send(f'.echo XPORT|WRITER_LOCALS|{self.token}')
        self._wait('WRITER_LOCALS', self.timeout)
        return self.output[start:]

    def writer_ring(self, limit: int = 16) -> dict:
        if self._running:
            raise CdbError("Target must be stopped before reading the writer ring")
        if not 1 <= limit <= 128:
            raise ValueError("Writer ring read limit is invalid")
        module = self.module
        self._send(
            f'.printf "XPORT|WRMETA|{self.token}|%x|%x\\n", '
            f'poi({module}!xport_lockstep_write_ring_available), '
            f'poi({module}!xport_lockstep_write_ring_armed)'
        )
        meta = self._wait("WRMETA", self.timeout)
        if len(meta) != 4 or int(meta[2], 16) not in (0, 1) or int(meta[3], 16) not in (0, 1):
            raise CdbProtocolError(f"Malformed writer ring availability: {meta}")
        available, armed = (int(value, 16) for value in meta[2:])
        if not available or not armed:
            return {"available": bool(available), "armed": bool(armed), "entries": []}
        base = f"{module}!xport_lockstep_write_ring"
        self._send(
            f'.printf "XPORT|WRHEAD|{self.token}|%x|%I64x|%I64x|%x\\n", '
            f'poi({module}!xport_lockstep_write_event_size), '
            f'qwo({base}+0x1c00), qwo({base}+0x1c08), poi({base}+0x1c10)'
        )
        head = self._wait("WRHEAD", self.timeout)
        if len(head) != 6:
            raise CdbProtocolError(f"Malformed writer ring header: {head}")
        size, next_index, dropped, count = (int(value, 16) for value in head[2:])
        if size != 56 or count > 128 or next_index < count or dropped > next_index:
            raise CdbProtocolError("Unsupported or inconsistent native writer ring layout")
        entries = []
        first = next_index - min(count, limit)
        for sequence in range(first, next_index):
            slot = sequence % 128
            address = f"{base}+0x{slot * size:x}"
            self._send(
                f'.printf "XPORT|WRROW|{self.token}|{slot}|%I64x|%x|%x|%x|%x|%x|%x|%x|%x|%p|%p|%x\\n", '
                f'qwo({address}), poi({address}+0x8), poi({address}+0xc), '
                f'poi({address}+0x10), poi({address}+0x14), poi({address}+0x18), '
                f'poi({address}+0x1c), poi({address}+0x20), poi({address}+0x24), '
                f'poi({address}+0x28), poi({address}+0x2c), poi({address}+0x34)'
            )
            row = self._wait("WRROW", self.timeout)
            if len(row) != 15 or int(row[2]) != slot:
                raise CdbProtocolError(f"Malformed writer ring row: {row}")
            values = [int(value, 16) for value in row[3:]]
            event = {
                "sequence": values[0], "operation": values[1], "region": values[2],
                "offset": values[3], "width": values[4], "before": values[5],
                "after": values[6], "byte_mask": values[7], "flags": values[8],
                "line": values[11],
            }
            if event["operation"] not in (2, 3, 4) or event["region"] not in (1, 2):
                raise CdbProtocolError("Writer ring contains an unsupported event")
            if event["flags"] & 4:
                self._send(
                    f'.printf "XPORT|WRSRC|{self.token}|{slot}|%ma|%ma\\n", '
                    f'poi({address}+0x28), poi({address}+0x2c)'
                )
                source = self._wait("WRSRC", self.timeout)
                if (len(source) != 5 or int(source[2]) != slot or
                        not source[3] or not source[4]):
                    raise CdbProtocolError(f"Malformed writer ring source: {source}")
                event["source"] = {"file": source[3], "function": source[4],
                                   "line": event["line"]}
            entries.append(event)
        return {"available": True, "armed": True, "count": count,
                "dropped": dropped, "truncated": count > limit, "entries": entries}

    def arm_writer_ring(self, region: str, offset: int, length: int = 1) -> bool:
        if self._running:
            raise CdbError("Target must be stopped before arming the writer ring")
        regions = {"dram": (1, 0x200000), "scratchpad": (2, 0x400)}
        if (region not in regions or length not in (1, 2, 4) or offset < 0 or
                offset + length > regions[region][1]):
            raise ValueError("Writer ring target is outside lockstep memory")
        module = self.module
        self._send(
            f'.printf "XPORT|WRAVAILABLE|{self.token}|%x\\n", '
            f'poi({module}!xport_lockstep_write_ring_available)'
        )
        available = self._wait("WRAVAILABLE", self.timeout)
        if len(available) != 3 or int(available[2], 16) not in (0, 1):
            raise CdbProtocolError(f"Malformed writer ring availability: {available}")
        if int(available[2], 16) == 0:
            return False
        self._send(f"ed {module}!xport_lockstep_write_request_region 0x{regions[region][0]:x}")
        self._send(f"ed {module}!xport_lockstep_write_request_offset 0x{offset:x}")
        self._send(f"ed {module}!xport_lockstep_write_request_length 0x{length:x}")
        self._send(
            f'.printf "XPORT|WRARM|{self.token}|%x|%x|%x\\n", '
            f'poi({module}!xport_lockstep_write_request_region), '
            f'poi({module}!xport_lockstep_write_request_offset), '
            f'poi({module}!xport_lockstep_write_request_length)'
        )
        marker = self._wait("WRARM", self.timeout)
        if (len(marker) != 5 or
                tuple(int(value, 16) for value in marker[2:]) !=
                (regions[region][0], offset, length)):
            raise CdbProtocolError(f"Writer ring request did not stick: {marker}")
        return True

    def diagnostic_values(self, expressions: list[str]) -> dict[str, int]:
        if self._running:
            raise CdbError("Target must be stopped before reading diagnostic values")
        if len(expressions) > 128 or any(not re.fullmatch(
                r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*|\[[0-9]{1,2}\])*", value)
                for value in expressions):
            raise CdbError("Invalid diagnostic field expression")
        result = {}
        for value in expressions:
            self._send(f'.printf "XPORT|VALUE|{self.token}|%I64x\\n", '
                       f'@@c++({self.module}!{value})')
            marker = self._wait("VALUE", self.timeout)
            if len(marker) != 3:
                raise CdbProtocolError(f"Malformed diagnostic value: {marker}")
            result[value] = int(marker[2], 16)
        return result

    def registers(self) -> dict[str, int]:
        if self._running:
            raise CdbError("Target must be stopped before reading registers")
        self._send(
            f'.printf "XPORT|REGS|{self.token}|%x|%x|%x|%x|%p\\n", '
            "@eax, @edx, @esp, @ebp, @eip"
        )
        marker = self._wait("REGS", self.timeout)
        if len(marker) != 7:
            raise CdbProtocolError(f"Malformed register marker: {marker}")
        names = ("eax", "edx", "esp", "ebp", "eip")
        return dict(zip(names, (int(value, 16) for value in marker[2:])))

    def capture_checkpoint(self, frame: int, *, restored: bool = False) -> None:
        if self._running:
            raise CdbError("Target must be stopped before capturing a checkpoint")
        self._send(
            f'bu /1 {self.module}!xport_lockstep_checkpoint_committed_break '
            f'".printf \\"XPORT|CAPTURED|{self.token}|%x|%x\\\\n\\", '
            f'poi({self.module}!xport_lockstep_checkpoint_valid), '
            f'poi({self.module}!frame_entry_index)"'
        )
        if not restored:
            self._send(f"ed {self.module}!xport_lockstep_checkpoint_requested 1")
        self.continue_execution()
        marker = self._wait("CAPTURED", self.timeout)
        self._running = False
        if len(marker) != 4 or int(marker[2], 16) != 1 or int(marker[3], 16) != frame + 1:
            raise CdbProtocolError("Native checkpoint did not commit the matched frame")

    def restore_checkpoint(self) -> None:
        if self._running:
            raise CdbError("Target must be stopped before restoring a checkpoint")
        self._send(
            f'.printf "XPORT|CHECKPOINT|{self.token}|%x|%p\\n", '
            f'poi({self.module}!xport_lockstep_checkpoint_valid), '
            f'{self.module}!xport_lockstep_checkpoint_jump'
        )
        marker = self._wait("CHECKPOINT", self.timeout)
        if len(marker) != 4 or int(marker[2], 16) != 1:
            raise CdbProtocolError("Native checkpoint is unavailable")
        address = int(marker[3], 16)
        if not address:
            raise CdbProtocolError("Native checkpoint jump symbol is unavailable")
        self._send("bc *")
        self._install_abort_breakpoints()
        for name in ("xport_lockstep_boundary_break", "xport_lockstep_vblank_break",
                     "xport_lockstep_transition_break"):
            symbol = f"{self.module}!{name}"
            self._send(
                f'bu {symbol} ".printf \\"XPORT|BOUNDARY|{self.token}|%x\\\\n\\", @$tpid"'
            )
        fault_instruction = getattr(self, "_last_fault_instruction", None)
        if fault_instruction is not None:
            self._send(
                f'ba e1 0x{fault_instruction:x} ".printf \\"XPORT|FAULT|{self.token}|%x|%p\\\\n\\", @$tpid, @eip"')
        self._send(f"r eip=0x{address:x}")
        self._event_sequence = 0
        self._boundary_sequence = 0
        self._mode = "DETAILED"
        self._epoch = 0
        self._next_invocation = 1
        self._stack.clear()
        self._ordinals.clear()
        self._return_breakpoints.clear()
        self._refinement_watchpoint = False
        self.continue_execution()

    def gte_snapshot(self) -> dict[str, list[int] | int]:
        if self._running:
            raise CdbError("Target must be stopped before reading the GTE snapshot")
        base = f"poi({self.module}!xport_lockstep_gte_state)"
        matrix_offsets = (0, 2, 4, 6, 8, 10, 12, 14, 16)
        fields = []
        for matrix_base in (0, 0x20, 0x40):
            fields.extend(f"wo({base}+0x{matrix_base + offset:x})" for offset in matrix_offsets)
        fields.extend(f"poi({base}+0x{offset:x})" for offset in (
            0x60, 0x64, 0x68, 0x6c, 0x70, 0x74, 0x78, 0x7c, 0x80, 0x84,
            0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c, 0xa0,
        ))
        formats = "|".join("%x" for _ in fields)
        self._send(f'.printf "XPORT|GTE|{self.token}|{formats}\\n", ' + ", ".join(fields))
        marker = self._wait("GTE", self.timeout)
        if len(marker) != 2 + len(fields):
            raise CdbProtocolError(f"Malformed GTE snapshot marker: {marker}")
        values = [int(value, 16) for value in marker[2:]]
        return {
            "rotation": values[0:9], "light": values[9:18], "color": values[18:27],
            "translation": values[27:30], "back_color": values[30:33],
            "h": values[33], "ofx": values[34], "ofy": values[35], "flag": values[36],
            "sxy": values[37:40], "sz": values[40:44],
        }

    def capture_call_stack(self) -> str:
        if self._running:
            raise CdbError("Target must be stopped before capturing its call stack")
        start = len(self.output)
        self._send("k")
        self._send(f'.printf "XPORT|CALL_STACK|{self.token}\\n"')
        self._wait("CALL_STACK", self.timeout)
        return self.output[start:]

    def stack(self) -> list[dict]:
        return [
            {
                "invocation": frame.invocation, "parent_invocation": frame.parent_invocation,
                "image": frame.image, "function": frame.function,
                "address": f"0x{frame.address:08x}",
                "source": {"file": frame.file, "line": frame.line},
                "host_return_address": f"0x{frame.return_address:08x}",
            }
            for frame in self._stack
        ]

    def source_location(self) -> dict | None:
        if not self._stack:
            return None
        frame = self._stack[-1]
        return {"symbol": frame.function, "file": frame.file, "line": frame.line}

    def _break_target(self) -> None:
        if self._target_pid is None:
            raise CdbProtocolError("Native PID is unavailable for debugger break")
        # Match the supported x86 target without requiring a console
        powershell = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "SysWOW64/WindowsPowerShell/v1.0/powershell.exe"
        script = '''
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class XportDebuggerBreak {
    [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint rights, bool inherit, uint pid);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool DebugBreakProcess(IntPtr process);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr process);
}
'@
$targetHandle = [XportDebuggerBreak]::OpenProcess(0x1F0FFF, $false, TARGET_PID)
if ($targetHandle -eq [IntPtr]::Zero) { exit 2 }
try { if (-not [XportDebuggerBreak]::DebugBreakProcess($targetHandle)) { exit 3 } }
finally { [void][XportDebuggerBreak]::CloseHandle($targetHandle) }
'''.replace("TARGET_PID", str(self._target_pid))
        result = subprocess.run(
            [str(powershell), "-NoLogo", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", script],
            capture_output=True, text=True, timeout=15,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if result.returncode:
            raise CdbProtocolError(f"Native debugger break failed: {result.returncode}: {result.stderr[-1024:]}")

    def interrupt(self, timeout: float | None = None) -> tuple[int, int]:
        if not self.process or self.process.poll() is not None:
            raise CdbProtocolError("CDB is not running")
        if not self._running:
            self._send(f'.printf "XPORT|INTERRUPTED|{self.token}|%x|%p\\n", @$tpid, @eip')
        else:
            self._break_target()
            self._send("~0s")
            self._send(f'.printf "XPORT|INTERRUPTED|{self.token}|%x|%p\\n", @$tpid, @eip')
        marker = self._wait("INTERRUPTED", timeout or self.timeout)
        self._remember_pid(marker[2])
        self._running = False
        return int(marker[2], 16), int(marker[3], 16)

    def _read_hit(self) -> tuple[list[str], list[dict]]:
        pointer = "poi(@esp+4)"
        self._send(
            f'.printf "XPORT|HIT|{self.token}|%x|%x|%x|%I64x|%ma|%ma|%x|%ma|%ma|%x|%x|%x|%p|%x|%x|%x|%x|%x|%x|%x|%x|%x|%x\\n", '
            f'@$tpid, poi({pointer}), poi({pointer}+4), qwo({pointer}+8), poi({pointer}+0x10), poi({pointer}+0x14), '
            f'poi({pointer}+0x18), poi({pointer}+0x1c), poi({pointer}+0x20), poi({pointer}+0x24), '
            f'poi({pointer}+0x2c), poi({pointer}+0x30), poi({pointer}+0x34), poi({pointer}+0x38), '
            f'poi({pointer}+0x3c), poi({pointer}+0x40), poi({pointer}+0x44), poi({pointer}+0x48), '
            f'poi({pointer}+0x4c), poi({pointer}+0x50), poi({pointer}+0x28), '
            f'poi({self.module}!xport_lockstep_mode), poi({self.module}!xport_lockstep_epoch)'
        )
        marker = self._wait("HIT", self.timeout)
        self._remember_pid(marker[2])
        if len(marker) != 25:
            raise CdbProtocolError(f"Malformed boundary marker: {marker}")
        count = int(marker[12], 16)
        arguments_pointer = int(marker[22], 16)
        if count > 16 or (count and not arguments_pointer):
            raise CdbProtocolError("Invalid native argument descriptor")
        arguments: list[dict] = []
        for index in range(count):
            address = arguments_pointer + index * 0x20
            self._send(
                f'.printf "XPORT|ARG|{self.token}|{index}|%x|%I64x|%x|%x|%x\\n", '
                f'poi(0x{address:x}), qwo(0x{address + 8:x}), poi(0x{address + 0x10:x}), '
                f'poi(0x{address + 0x14:x}), poi(0x{address + 0x18:x})'
            )
            argument = self._wait("ARG", self.timeout)
            if len(argument) != 8 or int(argument[2]) != index:
                raise CdbProtocolError(f"Malformed argument marker: {argument}")
            arguments.append({
                "kind": int(argument[3], 16), "scalar": int(argument[4], 16),
                "region": int(argument[5], 16), "offset": int(argument[6], 16),
                "bytes": int(argument[7], 16),
            })
        return marker, arguments

    def _install_return_breakpoint(self, address: int) -> None:
        if address in self._return_breakpoints:
            return
        self._send(
            f'bp /1 0x{address:x} ".printf \\"XPORT|RETURN|{self.token}|%x|%p\\\\n\\", @$tpid, @eip"'
        )
        self._return_breakpoints.add(address)

    def _pre_event(self) -> dict | None:
        marker, arguments = self._read_hit()
        schema = int(marker[3], 16)
        if schema != 1:
            raise CdbProtocolError(f"Unsupported native boundary schema {schema}")
        if marker[6] != BOUNDARY_CONTRACT:
            raise CdbProtocolError(f"Unsupported native boundary contract {marker[6]}")
        sequence = int(marker[5], 16)
        if sequence != self._boundary_sequence:
            raise CdbProtocolError(f"Native boundary sequence {sequence} != {self._boundary_sequence}")
        self._boundary_sequence += 1
        mode_value = int(marker[23], 16)
        if mode_value not in (0, 1):
            raise CdbProtocolError(f"Unsupported native lockstep mode {mode_value}")
        self._mode = "DETAILED" if mode_value == 0 else "PREFIX_GUARD"
        self._epoch = int(marker[24], 16)
        kind = int(marker[4], 16)
        if kind == 4:
            if arguments:
                raise CdbProtocolError("VBLANK boundary carries call arguments")
            return self._vblank_event(marker)
        if kind == 5:
            if arguments:
                raise CdbProtocolError("MODE_TRANSITION boundary carries call arguments")
            self._stack.clear()
            self._ordinals.clear()
            self._next_invocation = 1
            event = {
                "schema": SCHEMA, "contract": EVENT_CONTRACT, "side": "native",
                "kind": "MODE_TRANSITION", "mode": self._mode, "epoch": self._epoch,
                "event_sequence": self._event_sequence, "frame": int(marker[15], 16),
                "image": marker[7], "function": None, "parent_invocation": 0,
                "invocation": 0, "depth": 0, "call_ordinal": 0, "callsite": None,
                "target": None, "return_pc": None,
                "source": {"previous_mode": "PREFIX_GUARD", "next_mode": "DETAILED",
                           "target_frame": int(marker[15], 16)},
            }
            self._event_sequence += 1
            return validate_event(event)
        if kind != 1:
            raise CdbProtocolError(f"Unsupported native boundary kind {kind}")
        image, function, file = marker[7], marker[9], marker[10]
        if any("|" in value or "\n" in value or not value for value in (image, function, file)):
            raise CdbProtocolError("Unsafe or empty native identity string")
        address = int(marker[8], 16)
        if self.boundary_addresses is not None and address not in self.boundary_addresses:
            return None
        line = int(marker[11], 16)
        return_kind = int(marker[13], 16)
        return_address = int(marker[14], 16)
        parent = self._stack[-1].invocation if self._stack else 0
        ordinal = self._ordinals.get(parent, 0)
        self._ordinals[parent] = ordinal + 1
        invocation = Invocation(
            invocation=self._next_invocation, parent_invocation=parent, depth=len(self._stack),
            call_ordinal=ordinal, image=image, function=function, address=address, file=file,
            line=line, return_kind=return_kind, return_address=return_address,
            frame=int(marker[15], 16), arguments=arguments,
        )
        self._next_invocation += 1
        self._stack.append(invocation)
        self._install_return_breakpoint(return_address)
        event = self._event(invocation, "PRE_CALL")
        event["arguments"] = arguments
        return event

    def _vblank_event(self, marker: list[str]) -> dict:
        names = {0: "disconnected", 1: "digital", 2: "analog"}
        states = []
        for port in range(2):
            controller_type = int(marker[16 + port], 16)
            if controller_type not in names:
                raise CdbProtocolError(f"Unsupported native controller type {controller_type}")
            states.append({
                "type": names[controller_type],
                "buttons": int(marker[18 + port], 16),
                "analog": int(marker[20 + port], 16),
            })
        event = {
            "schema": SCHEMA, "contract": EVENT_CONTRACT, "side": "native", "kind": "VBLANK",
            "mode": self._mode, "epoch": self._epoch,
            "event_sequence": self._event_sequence, "frame": int(marker[15], 16),
            "image": marker[7], "function": None, "parent_invocation": 0, "invocation": 0,
            "depth": 0, "call_ordinal": 0, "callsite": None, "target": None,
            "return_pc": None, "controller_state": states,
        }
        self._event_sequence += 1
        return validate_event(event)

    def _post_event(self, marker: list[str]) -> dict | None:
        self._remember_pid(marker[2])
        address = int(marker[3].split()[0], 16)
        if not self._stack:
            return None
        if self._stack[-1].return_address != address:
            raise CdbProtocolError(
                f"Return 0x{address:x} does not match active invocation return 0x{self._stack[-1].return_address:x}"
            )
        invocation = self._stack.pop()
        self._return_breakpoints.discard(address)
        if any(frame.return_address == address for frame in self._stack):
            self._install_return_breakpoint(address)
        self._send(
            f'.printf "XPORT|REGS|{self.token}|%x|%x|%x|%x\\n", @eax, @edx, @esp, @ebp'
        )
        registers = self._wait("REGS", self.timeout)
        if len(registers) != 6:
            raise CdbProtocolError(f"Malformed register marker: {registers}")
        eax, edx, esp, ebp = (int(value, 16) for value in registers[2:])
        event = self._event(invocation, "POST_CALL")
        event["return_value"] = None if invocation.return_kind in (0, 5) else eax
        event["registers"] = {"eax": eax, "edx": edx, "esp": esp, "ebp": ebp}
        return event

    def _event(self, invocation: Invocation, kind: str) -> dict:
        event = {
            "schema": SCHEMA, "contract": EVENT_CONTRACT, "side": "native", "kind": kind,
            "mode": self._mode, "epoch": self._epoch,
            "event_sequence": self._event_sequence, "frame": invocation.frame, "image": invocation.image,
            "function": f"0x{invocation.address:08x}",
            "parent_invocation": invocation.parent_invocation,
            "invocation": invocation.invocation, "depth": invocation.depth,
            "call_ordinal": invocation.call_ordinal, "callsite": None,
            "target": f"0x{invocation.address:08x}",
            "return_pc": None,
            "source": {
                "symbol": invocation.function, "file": invocation.file, "line": invocation.line,
                "host_return_address": f"0x{invocation.return_address:08x}",
            },
        }
        self._event_sequence += 1
        return validate_event(event)

    def _terminal_event(self, kind: str, *, instruction: int | None = None) -> dict:
        if kind == "FAULT":
            self._last_fault_instruction = instruction
        frame = 0
        if kind == "FAULT" and self.memory_ready:
            self._send(
                f'.printf "XPORT|TERMINAL_STATE|{self.token}|%x|%x|%x\\n", '
                f'poi({self.module}!xport_lockstep_mode), poi({self.module}!xport_lockstep_epoch), '
                f'poi({self.module}!boundary_frame)')
            state = self._wait("TERMINAL_STATE", self.timeout)
            if len(state) != 5 or int(state[2], 16) not in (0, 1):
                raise CdbProtocolError("Malformed native terminal state")
            self._mode = "DETAILED" if int(state[2], 16) == 0 else "PREFIX_GUARD"
            self._epoch = int(state[3], 16)
            frame = int(state[4], 16)
        event = {
            "schema": SCHEMA, "contract": EVENT_CONTRACT, "side": "native", "kind": kind,
            "mode": self._mode, "epoch": self._epoch,
            "event_sequence": self._event_sequence, "frame": frame, "image": self.executable.name,
            "function": None, "parent_invocation": self._stack[-1].invocation if self._stack else 0,
            "invocation": 0, "depth": len(self._stack), "call_ordinal": 0,
            "callsite": None, "target": None, "return_pc": None,
            "source": {"instruction": None if instruction is None else f"0x{instruction:08x}"},
        }
        self._event_sequence += 1
        return validate_event(event)

    def next_event(self, timeout: float | None = None) -> dict:
        if not self.process:
            raise CdbError("Controller has not started")
        if not self._running:
            raise CdbError("Target is stopped; call continue_execution first")
        deadline = time.monotonic() + (timeout or self.timeout)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                pid, instruction = self.interrupt(self.timeout)
                self._send("k")
                self._send(f'.printf "XPORT|STACK_CAPTURED|{self.token}\\n"')
                self._wait("STACK_CAPTURED", self.timeout)
                raise CdbTimeout("Native target made no boundary progress", process_id=pid,
                                 instruction=instruction)
            try:
                marker = self._markers.get(timeout=min(remaining, 0.1))
            except queue.Empty:
                if self.process.poll() is not None:
                    self._running = False
                    raise CdbProtocolError(f"CDB exited unexpectedly: {self.process.returncode}")
                continue
            parts = marker.split("|")
            if len(parts) < 2 or parts[1] != self.token:
                continue
            if parts[0] == "OT":
                if len(parts) != 9:
                    raise CdbProtocolError(f"Malformed OT submission marker: {parts}")
                self._running = False
                self._remember_pid(parts[2])
                root, frame = (int(value.split()[0], 16) for value in parts[3:5])
                if root & 3 or root >= 0x200000:
                    raise CdbProtocolError("Native OT root is outside mapped DRAM")
                return {"kind": "OT_SUBMIT", "side": "native", "frame": frame,
                        "root": root, "callers": [value.strip() for value in parts[5:]]}
            if parts[0] == "BOUNDARY":
                self._remember_pid(parts[2])
                self._running = False
                event = self._pre_event()
                if event is not None:
                    return event
                self.continue_execution()
            if parts[0] == "RETURN":
                self._running = False
                event = self._post_event(parts)
                if event is not None:
                    return event
                self.continue_execution()
            if parts[0] == 'CAUSAL_ENTRY':
                self._running = False
                self._remember_pid(parts[2])
                if len(parts) not in (7, 11):
                    raise CdbProtocolError(f'Malformed causal entry marker: {parts}')
                result = {'stop': 'causal_entry', 'side': 'native',
                          'arguments': [f'0x{int(value.split()[0], 16):08x}' for value in parts[3:7]]}
                if len(parts) == 11:
                    result['quaternion'] = b''.join(int(value.split()[0], 16).to_bytes(4, 'little')
                                                  for value in parts[7:11]).hex()
                return result
            if parts[0] == "WATCH":
                self._running = False
                self._remember_pid(parts[2])
                self._refinement_watchpoint = False
                if len(parts) != 15:
                    raise CdbProtocolError(f"Malformed watchpoint marker: {parts}")
                return {
                    "stop": "watchpoint", "side": "native",
                    "pc": f"0x{int(parts[3].split()[0], 16):08x}",
                    "region": parts[4], "offset": int(parts[5], 16), "width": int(parts[6], 16),
                    "before": int(parts[7].split()[0], 16).to_bytes(
                        int(parts[6], 16), "little").hex(),
                    "after": int(parts[8].split()[0], 16).to_bytes(
                        int(parts[6], 16), "little").hex(),
                    "caller_pc": f"0x{int(parts[9].split()[0], 16):08x}",
                    "caller2_pc": f"0x{int(parts[10].split()[0], 16):08x}",
                    "caller_symbol": parts[11].strip(),
                    "caller2_symbol": parts[12].strip(),
                    "caller3_symbol": parts[13].strip(),
                    "caller4_symbol": parts[14].strip(),
                }
            if parts[0] == "FAULT":
                self._running = False
                self._remember_pid(parts[2])
                return self._terminal_event("FAULT", instruction=int(parts[3].split()[0], 16))
            if parts[0] == "EXIT":
                self._running = False
                self._remember_pid(parts[2])
                return self._terminal_event("PROCESS_EXIT")

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        process = self.process
        if not process:
            return
        try:
            if process.poll() is None:
                try:
                    if self._running:
                        self.interrupt(timeout=3.0)
                    self._send(".kill")
                    self._send("q")
                    process.wait(timeout=3)
                except (OSError, subprocess.TimeoutExpired, CdbError):
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
        finally:
            try:
                if process.stdin:
                    process.stdin.close()
                if process.stdout:
                    process.stdout.close()
            finally:
                if self._reader:
                    self._reader.join(timeout=1)

    def __enter__(self) -> "CdbController":
        self.start()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()
