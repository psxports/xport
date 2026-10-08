"""Index targeted execution events and locate byte-accurate last writers"""

import argparse
import hashlib
import json
from pathlib import Path
import sqlite3

from trace_execution_events import DeviceEvent, StoreEvent, read_execution_file


SCHEMA = "xport-execution-index-v1"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build(trace, database):
    trace = Path(trace).resolve()
    database = Path(database).resolve()
    if database.exists():
        raise FileExistsError(database)
    manifest, events, ended = read_execution_file(trace)
    if not ended:
        raise ValueError("Execution trace lacks END")
    connection = sqlite3.connect(database)
    try:
        connection.executescript("""
        PRAGMA journal_mode=DELETE;
        CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
        CREATE TABLE stores(event_seq INTEGER PRIMARY KEY,frame INTEGER NOT NULL,guest_clock INTEGER NOT NULL,
          invocation INTEGER NOT NULL,pc INTEGER NOT NULL,instruction INTEGER NOT NULL,region_id INTEGER NOT NULL,
          physical_offset INTEGER NOT NULL,size INTEGER NOT NULL,byte_mask INTEGER NOT NULL,changed_mask INTEGER NOT NULL,
          virtual_address INTEGER NOT NULL,before BLOB NOT NULL,after BLOB NOT NULL);
        CREATE TABLE byte_writes(region_id INTEGER NOT NULL,physical_offset INTEGER NOT NULL,event_seq INTEGER NOT NULL,
          before INTEGER NOT NULL,after INTEGER NOT NULL,changed INTEGER NOT NULL,
          PRIMARY KEY(region_id,physical_offset,event_seq),FOREIGN KEY(event_seq) REFERENCES stores(event_seq));
        CREATE INDEX byte_last ON byte_writes(region_id,physical_offset,event_seq DESC);
        CREATE TABLE device_writes(event_seq INTEGER PRIMARY KEY,operation_id INTEGER NOT NULL,
          parent_operation_id INTEGER NOT NULL,frame INTEGER NOT NULL,guest_clock INTEGER NOT NULL,
          device INTEGER NOT NULL,primitive INTEGER NOT NULL,direction INTEGER NOT NULL,evidence INTEGER NOT NULL,
          source_region INTEGER NOT NULL,source_offset INTEGER NOT NULL,destination_region INTEGER NOT NULL,
          destination_offset INTEGER NOT NULL,byte_count INTEGER NOT NULL,byte_mask INTEGER NOT NULL,
          channel INTEGER NOT NULL,command INTEGER NOT NULL);
        CREATE TABLE device_byte_writes(region_id INTEGER NOT NULL,physical_offset INTEGER NOT NULL,event_seq INTEGER NOT NULL,
          PRIMARY KEY(region_id,physical_offset,event_seq),FOREIGN KEY(event_seq) REFERENCES device_writes(event_seq));
        CREATE INDEX device_byte_last ON device_byte_writes(region_id,physical_offset,event_seq DESC);
        """)
        identity = digest(trace)
        for key, value in {"schema": SCHEMA, "trace": str(trace), "trace_sha256": identity,
                           "manifest": json.dumps(manifest, sort_keys=True)}.items():
            connection.execute("INSERT INTO metadata VALUES(?,?)", (key, value))
        stores = 0
        bytes_written = 0
        device_writes = 0
        device_bytes_written = 0
        for event in events:
            if isinstance(event, DeviceEvent) and event.phase == 4:
                connection.execute("INSERT INTO device_writes VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                                   (event.event_seq, event.operation_id, event.parent_operation_id, event.frame,
                                    event.guest_clock, event.device, event.primitive, event.direction, event.evidence,
                                    event.source_region, event.source_offset, event.destination_region,
                                    event.destination_offset, event.byte_count, event.byte_mask, event.channel,
                                    event.command))
                device_writes += 1
                for offset in range(event.destination_offset, event.destination_offset + event.byte_count):
                    connection.execute("INSERT INTO device_byte_writes VALUES(?,?,?)",
                                       (event.destination_region, offset, event.event_seq))
                    device_bytes_written += 1
                continue
            if not isinstance(event, StoreEvent):
                continue
            connection.execute("INSERT INTO stores VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                               (event.event_seq, event.frame, event.guest_clock, event.invocation,
                                event.pc, event.instruction, event.region_id, event.physical_offset,
                                event.size, event.byte_mask, event.changed_mask, event.virtual_address,
                                event.before, event.after))
            stores += 1
            for index in range(event.size):
                if not event.byte_mask & (1 << index):
                    continue
                changed = int(event.before[index] != event.after[index])
                connection.execute("INSERT INTO byte_writes VALUES(?,?,?,?,?,?)",
                                   (event.region_id, event.physical_offset + index, event.event_seq,
                                    event.before[index], event.after[index], changed))
                bytes_written += 1
        connection.commit()
    finally:
        connection.close()
    return {"schema": 1, "contract": SCHEMA, "trace": str(trace), "trace_sha256": digest(trace),
            "database": str(database), "events": len(events), "stores": stores,
            "byte_writes": bytes_written, "device_writes": device_writes,
            "device_byte_writes": device_bytes_written}


def validate(connection):
    metadata = dict(connection.execute("SELECT key,value FROM metadata"))
    if metadata.get("schema") != SCHEMA:
        raise ValueError("Unsupported execution index")
    if digest(metadata["trace"]) != metadata["trace_sha256"]:
        raise ValueError("Execution trace identity changed")
    return metadata


def last_writers(database, region_id, physical_offset, size, until_seq=None):
    connection = sqlite3.connect(database)
    connection.row_factory = sqlite3.Row
    try:
        validate(connection)
        result = []
        for offset in range(physical_offset, physical_offset + size):
            limit = " AND bw.event_seq<=?" if until_seq is not None else ""
            device_limit = " AND dbw.event_seq<=?" if until_seq is not None else ""
            arguments = (region_id, offset, until_seq) if until_seq is not None else (region_id, offset)
            columns = ("bw.region_id,bw.physical_offset,bw.event_seq,bw.before,bw.after,bw.changed,"
                       "s.frame,s.guest_clock,s.invocation,s.pc,s.instruction,s.virtual_address,"
                       "s.size,s.byte_mask,s.changed_mask")
            last = connection.execute(
                "SELECT " + columns + " FROM byte_writes bw JOIN stores s USING(event_seq) "
                "WHERE bw.region_id=? AND bw.physical_offset=?" + limit +
                " ORDER BY bw.event_seq DESC LIMIT 1", arguments).fetchone()
            changed = connection.execute(
                "SELECT " + columns + " FROM byte_writes bw JOIN stores s USING(event_seq) "
                "WHERE bw.region_id=? AND bw.physical_offset=?" + limit +
                " AND bw.changed=1 ORDER BY bw.event_seq DESC LIMIT 1", arguments).fetchone()
            device_columns = ("dbw.region_id,dbw.physical_offset,dbw.event_seq,dw.operation_id,dw.parent_operation_id,"
                              "dw.frame,dw.guest_clock,dw.device,dw.primitive,dw.direction,dw.evidence,"
                              "dw.source_region,dw.source_offset,dw.destination_offset,dw.byte_count,dw.byte_mask,"
                              "dw.channel,dw.command")
            device = connection.execute(
                "SELECT " + device_columns + " FROM device_byte_writes dbw JOIN device_writes dw USING(event_seq) "
                "WHERE dbw.region_id=? AND dbw.physical_offset=?" + device_limit +
                " ORDER BY dbw.event_seq DESC LIMIT 1", arguments).fetchone()
            cpu_write = dict(last) if last else None
            device_write = dict(device) if device else None
            if device_write and (not cpu_write or device_write["event_seq"] > cpu_write["event_seq"]):
                selected = dict(device_write, writer_kind="device")
            elif cpu_write:
                selected = dict(cpu_write, writer_kind="cpu")
            else:
                selected = None
            result.append({"physical_offset": offset,
                           "last_write": selected,
                           "last_value_change": dict(changed) if changed else None})
        return result
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    index = commands.add_parser("build")
    index.add_argument("--trace", type=Path, required=True)
    index.add_argument("--database", type=Path, required=True)
    query = commands.add_parser("last-writers")
    query.add_argument("--database", type=Path, required=True)
    query.add_argument("--region-id", type=int, required=True)
    query.add_argument("--physical-offset", type=lambda value: int(value, 0), required=True)
    query.add_argument("--size", type=int, required=True)
    query.add_argument("--until-seq", type=int)
    args = parser.parse_args()
    result = (build(args.trace, args.database) if args.command == "build" else
              last_writers(args.database, args.region_id, args.physical_offset, args.size, args.until_seq))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
