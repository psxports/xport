"""Align bounded memory writer observations without assuming equal write counts"""

from __future__ import annotations


def correlate_writers(original: list[dict], native: list[dict]) -> dict:
    native_index = 0
    matched = []
    for original_index, writer in enumerate(original):
        expected = writer.get("after")
        found = next((index for index in range(native_index, len(native))
                      if native[index].get("after") == expected), None)
        if found is None:
            return {"schema": 1, "confidence": "observed_timeline",
                    "matched_original_writes": len(matched),
                    "first_unmatched_original": {"index": original_index, "writer": writer},
                    "native_remainder": native[native_index:],
                    "limit": "Intermediate writes may be optimized, reordered or overwritten"}
        matched.append({"original_index": original_index, "native_index": found})
        native_index = found + 1
    return {"schema": 1, "confidence": "observed_timeline",
            "matched_original_writes": len(matched), "matches": matched,
            "native_remainder": native[native_index:],
            "limit": "Intermediate writes may be optimized, reordered or overwritten"}
