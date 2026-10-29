# Masked guest word reads

`r_masked_u32(address, byte_mask)` returns the full aligned little-endian backing word, with one read observation carrying the requested active byte lanes. Use it for literal LWL/LWR reconstruction: callers perform the original shift/merge. The observer receives the full word in both value fields, as for ordinary reads, and the explicit lane mask.

- Require a four-byte-aligned address and a nonzero mask in `1..15`
- Invalid arguments return zero with `XPORT_MEMORY_ERROR_ARGUMENT` and no observation
- RAM aliases/mirrors, scratchpad and read-only BIOS use the existing mapper
- Partial MMIO reads fail with `XPORT_MEMORY_ERROR_ARGUMENT` before invoking a device: the existing read callback has no lane-mask argument
- Mask15 delegates to ordinary word MMIO, including its failure behavior
- Existing scalar reads/writes, callbacks, consumer bindings and snapshots are unchanged
- The API does not implement LWL/LWR register merging or physical bus timing

From `tools`, run `python -B -m unittest discover -s tests -p test_masked_memory.py -v`. Select x64 with `XPORT_TEST_ARCH=x64`. Passing these tests does not establish other games' gameplay compatibility.

FF integration evidence: `FightingForce-xport/status/implementation-completion/texture-loader-full-review.json`. Its two unaligned palette-copy cases compare ordered addresses, values and byte masks against original LWL/LWR execution without mask exclusions.
