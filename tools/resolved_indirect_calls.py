"""Import reviewed, image-bound indirect edges without inferring pointer targets"""

import hashlib
import json
from pathlib import Path


def _read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _project_file(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError("Missing or external indirect-call evidence: " + str(relative))
    return path


def populate(db, root, config, exported, graph):
    """Keep reviewed alternatives distinct from exhaustive runtime resolution"""
    db.execute("""CREATE TABLE edge_provenance(
        edge INTEGER PRIMARY KEY, evidence TEXT NOT NULL,
        FOREIGN KEY(edge) REFERENCES edges(id))""")
    relative = config.get("analysis", {}).get("resolved_indirect_calls")
    if not relative:
        return {"edges": 0, "sites": 0, "manifest": None}
    manifest_path = _project_file(root, relative)
    manifest = _read(manifest_path)
    if manifest.get("schema") != "xport-reviewed-indirect-calls-v1":
        raise ValueError("Unsupported reviewed indirect-call manifest")
    functions = {(image, item["address"]): item
                 for image, items in exported.items() for item in items}
    images = {}
    for image, source, sha, base, offset in db.execute(
            "SELECT id,source,sha256,base,file_offset FROM images"):
        raw = Path(source).read_bytes()
        if hashlib.sha256(raw).hexdigest() != sha:
            raise ValueError("Changed original image: " + image)
        images[image] = (raw, sha, base, offset)
    seen = set()
    sites = set()
    for edge in manifest["edges"]:
        image = edge["image"]
        source, site, target = edge["source"], edge["site"], edge["target"]
        identity = (image, source, site, target)
        if identity in seen:
            raise ValueError("Duplicate reviewed indirect edge: " + repr(identity))
        seen.add(identity)
        function = functions[(image, source)]
        functions[(image, target)]
        raw, image_sha, base, offset = images[image]
        if edge["image_sha256"] != image_sha or edge["source_sha256"] != function["sha256"]:
            raise ValueError("Stale reviewed indirect edge: " + repr(identity))
        instruction = next((i for i in function["raw_instructions"] if i["address"] == site), None)
        word = int(instruction["word"], 16) if instruction else 0
        if not instruction or word >> 26 != 0 or word & 63 != 9:
            raise ValueError("Reviewed callsite is not JALR: " + repr(identity))
        if not db.execute("SELECT 1 FROM indirect_transfers WHERE image=? AND function=? AND site=?",
                          (image, source, site)).fetchone():
            raise ValueError("Reviewed callsite missing from original indirect transfers")
        proof = edge["proof"]
        evidence = _project_file(root, proof["path"])
        if hashlib.sha256(evidence.read_bytes()).hexdigest() != proof["sha256"]:
            raise ValueError("Changed indirect-call review: " + proof["path"])
        if proof.get("complete_target_set") is not False:
            raise ValueError("Unsupported or unproved exhaustive indirect-call resolution")
        if proof.get("kind") == "reviewed_vtable_slot":
            constructor = functions[(image, proof["constructor"])]
            if constructor["sha256"] != proof["constructor_sha256"]:
                raise ValueError("Stale constructor proof")
            start = offset + proof["table"] + proof["slot_offset"] - base
            if start < offset or start + 8 > len(raw):
                raise ValueError("Vtable slot is outside original image")
            pair = raw[start:start + 8]
            adjustment = int.from_bytes(pair[:2], "little", signed=True)
            if adjustment != proof["receiver_adjustment"] or int.from_bytes(pair[4:], "little") != target:
                raise ValueError("Original vtable slot disagrees with reviewed edge")
            if pair.hex() != proof["slot_bytes"]:
                raise ValueError("Changed original vtable bytes")
        elif proof.get("kind") == "reviewed_callback_argument":
            _callback_argument(functions, image, function, site, word, target, proof)
        else:
            raise ValueError("Unsupported indirect-call proof kind")
        cursor = db.execute("""INSERT INTO edges(
            source_image,source_function,site,target,kind,resolution)
            VALUES(?,?,?,?,?,?)""", (image, source, site, target, "indirect_reviewed", "same_image"))
        db.execute("INSERT INTO edge_candidates VALUES(?,?,?)", (cursor.lastrowid, image, target))
        db.execute("INSERT INTO edge_provenance VALUES(?,?)", (cursor.lastrowid, json.dumps(edge)))
        graph.append(dict(source="%s:%08X" % (image, source), site=site, target=target,
                          kind="indirect_reviewed", resolution="same_image",
                          candidates=["%s:%08X" % (image, target)], proof=edge["proof"]))
        sites.add((image, source, site))
    for image, source, site in sites:
        db.execute("""UPDATE indirect_transfers SET status='partially_resolved_static'
            WHERE image=? AND function=? AND site=?""", (image, source, site))
    return {"edges": len(seen), "sites": len(sites), "manifest": relative,
            "manifest_sha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
            "exhaustive": False}


def _callback_argument(functions, image, function, site, jalr, target, proof):
    """Validate local constant argument setup; control-flow binding requires the review"""
    caller = functions[(image, proof["caller"])]
    if caller["sha256"] != proof["caller_sha256"]:
        raise ValueError("Stale callback caller proof")
    words = {i["address"]: int(i["word"], 16) for i in caller["raw_instructions"]}
    call_site = proof["call_site"]
    call = words.get(call_site, 0)
    destination = ((call_site + 4) & 0xF0000000) | ((call & 0x3FFFFFF) << 2)
    if call >> 26 != 3 or destination != function["address"]:
        raise ValueError("Callback caller does not call reviewed source")
    register = proof["argument_register"]
    setup = proof["argument_setup"]
    if register not in range(4, 8) or len(setup) != 2:
        raise ValueError("Unsupported callback argument setup")
    for instruction in setup:
        if words.get(instruction["site"]) != int(instruction["word"], 16):
            raise ValueError("Changed callback argument instruction")
    hi, lo = [int(i["word"], 16) for i in setup]
    if (hi >> 26 != 15 or (hi >> 21) & 31 or (hi >> 16) & 31 != register
            or lo >> 26 != 9 or (lo >> 21) & 31 != register or (lo >> 16) & 31 != register
            or not setup[0]["site"] < call_site or setup[1]["site"] != call_site + 4):
        raise ValueError("Unsupported callback LUI/ADDIU delay-slot setup")
    low = lo & 0xFFFF
    value = (((hi & 0xFFFF) << 16) + (low - 0x10000 if low & 0x8000 else low)) & 0xFFFFFFFF
    if value != target:
        raise ValueError("Callback argument disagrees with target")
    binding = proof["callback_binding"]
    source_words = {i["address"]: int(i["word"], 16) for i in function["raw_instructions"]}
    move = int(binding["word"], 16)
    if (source_words.get(binding["site"]) != move or binding["site"] >= site
            or move >> 26 or move & 63 not in (33, 37)
            or (move >> 21) & 31 != register or (move >> 16) & 31
            or (move >> 11) & 31 != (jalr >> 21) & 31 or (move >> 6) & 31):
        raise ValueError("Callback binding is not argument-to-JALR-register move")
