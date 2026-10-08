# Lockstep convergence

`trace_workflow converge NAME` requires an `xport-lockstep-record-v1` record and a migrated
project. It never routes to the prior trace worker.

For an agent, `converge` is always a durable goal with full-record `MATCH` as its terminal
condition. Create that goal before starting the command, keep the owned supervisor under
observation, and continue through divergences and recoverable restarts. Do not treat a
successful background launch as task completion. Stop only at `MATCH`, an explicit user stop,
or a condition that actually requires user input. Once full-record `MATCH` is verified, call
`update_goal(status=complete)` so the converge goal is no longer active.

`MATCH` proves only the recorded interval beginning at the restored initial save state. It
does not prove BIOS, startup, initialization or any game code that executed before that state.
Never use bytes seeded from the save state as runtime evidence for their earlier producer. If
the reported defect can originate before the recording boundary, audit that producer and
obtain separate cold-start evidence or record from a state before the producer. Report the
terminal result as a recorded-interval match and state every required behavior that remains
outside its proof scope.

While an attempt is running, observe it by waiting for a durable state revision, supervisor
exit or attention artifact instead of repeatedly polling and reporting unchanged state. A
bounded periodic liveness check may verify that the supervisor and participants still exist
and make progress, but identical snapshots are not progress and must not consume the agent's
working context. Return to active diagnosis immediately when the event wait reports a state
change, process exit or required attention.

Reject every repair hack. A converge repair must follow complete original evidence for a
required audited function. Never add frame or time windows, arbitrary address/value exceptions,
input-dependent compensation, observed-case branches, difference masks, event suppression or
reproducer-specific behavior. Never edit an unaudited helper or global compensator. If causal
ownership is incomplete, keep the supervisor waiting and obtain causal evidence; do not submit
a speculative proposal.

For an `extra_store`, a paired-entry causal packet may assign ownership to a native function
only when the original and native entry inputs match byte-for-byte and every unmatched native
write resolves uniquely to that function in the captured native call stack. The repair still
requires a complete audit of the corresponding original MIPS function and its direct helpers.

A boundary divergence may prove a defect in a project-owned firmware or device adapter rather
than in a translated MIPS function. Submit that repair through `adapter_audits`; never edit the
adapter outside the supervisor. Each adapter audit must bind the packet failure signature, the
exact complete C function text and its SHA-256, and the complete `diagnostic-result.json` for the
current last-matching boundary. The supervisor accepts edits only inside that bound function.
This path does not waive any MIPS audit required by the packet and does not permit timing,
input, address or value compensation.
Use `scope: "shared"` and an `@xport/src/...` path for a common xport device adapter. Use
`kind: "declaration"` only when the repair changes an exact complete type declaration required
by the audited adapter functions; bind its complete text and SHA-256 to the same diagnostic.
The supervisor applies project and shared adapter edits atomically and rejects an unbound global
or partial declaration.

A repair may declare bounded `memory_exceptions` only for proven `firmware_runtime` or
`trusted_device_state` storage. The supervisor binds each range to the active attention packet,
the repair source identity and a reason, persists it in `repair-memory-exceptions.json`, and
applies it on the next replay. The range must not overlap the active divergence, game output,
ordering tables, another repair range or a compiled exception. The complete repair-exception
set is part of the implementation identity and attention evidence. Use an empty list when the
source repair does not require a new exception.

Every lockstep DuckStation launch writes the standard project-owned endpoint receipt with
PID, creation time, executable, data root and reserved port. Before a retry, the supervisor
stops an existing endpoint only when that complete identity verifies; an unverified listener
fails closed. This lets an interrupted converge recover its orphan without touching manual
play or another project.

An access-denied failure while an attempt is still launching is recoverable. After the owned
orphan has been verified and stopped, restarting the supervisor must retry the same target even
when neither source nor implementation identity changed; it must not preserve that launch
failure as a terminal result.

For each attempt the supervisor performs these steps:

1. Verify the record and source identity
2. Build Debug x86 with `LOCKSTEP_DEBUG`
3. Start native mapped DRAM and scratchpad, then start paused DuckStation
4. Seed both mappings from the restored original state exactly once
5. Scan the complete record with VBlank-only `PREFIX_GUARD`
6. Compare input, DRAM and scratchpad at every FrameDone
7. On a mismatch in frame `N`, restore the paired checkpoint from `N-1`
8. Switch both sides to a new detailed epoch only for frame `N`
9. Compare PRE_CALL, POST_CALL, arguments, returns and GTE state in that frame
10. Replay the last interval with watchpoints and build one attention packet
11. Apply one source-scoped model repair and resume the VBlank-only prefix scan from checkpoint `N-1`
12. After the checkpoint suffix reaches the record end, discard every checkpoint and run one complete VBlank-only prefix scan from the initial state
13. Publish MATCH only when that complete final prefix scan consumes the record without divergence

Checkpoint `N-1` is the only valid origin for detailed frame `N`. Do not move the origin to `N-2` to provide an implicit warmup frame. Every device event needed by frame `N`, including intra-frame SPU key and envelope-read timing, must be preserved by the checkpoint or reproduced from its exact shared event timeline.

Before every diagnostic probe, rebuild or hash-validate the required lockstep Debug executable and PDB. Immediately before every Native CDB launch, require both files to match the build receipt. Fail and rebuild if an ordinary Debug build replaced either artifact; never launch or reuse mismatched symbols.

An ordered project handoff is a sequence suffix contract. A project may declare one contract
or a finite list of disjoint routes for different valid save-state phases. The supervisor arms
every route before continuing a restored initial state. The first observed breakpoint selects
exactly one route and its suffix because a save state may already be past one-time startup
breakpoints. The supervisor disarms every unselected route. Every subsequently observed
breakpoint must be the next element of the selected suffix, and the final breakpoint and
expected register constraints remain mandatory before Native starts. An expected register
constraint is one value or an explicit list of accepted values; the captured value is always
transferred to Native unchanged. Route breakpoint sets must be disjoint so selection cannot be
ambiguous. Each route names its native continuation and the ordered captured registers passed
to it.
The final handoff breakpoint may be reached directly from the initial save state before the
first recorded `FrameDone`. The supervisor must then continue to the next occurrence of that
breakpoint after applying the first recorded input. It must leave that breakpoint disarmed
until the first `FrameDone` event so continuing cannot stop repeatedly at the same PC. A
handoff never seeds memory or starts Native with a zero-length input prefix.
At the selected handoff boundary, transfer all 32 original GTE control registers into the
native GTE model before entering the continuation. DRAM seeding does not transfer persistent
coprocessor state, and reconstructing only matrices or projection distance is insufficient.

Every VBlank counted as confirmed must contain a completed `match` memory comparison.
`deferred` is not a memory match and must not advance progress, a checkpoint or Full Match.
On the first `deferred` result for a generation, keep Original stopped and advance Native only
until Native closes that generation. Queue and validate every Native semantic event in original
order. Then keep Native stopped and pair each queued event with the next Original event. Compare
memory at every paired boundary. Accept each remaining `deferred` result only for the same
generation and keep it unconfirmed. Treat the first `match` that resolves the generation as
`ranged_match`, then require a full `match` at the next VBlank before advancing confirmed
progress. A mode transition, new generation or process exit must not replace, clear or accept
an unresolved generation or required full comparison. A project may treat an audited, bounded
file-load operation as atomic. Register only the exact destination ranges written by that load
and the minimum audited loader state required to publish its completion. Every VBlank inside
that operation must still match its event and input schedule. The Native side must wait until
the Original side reaches the published ready state.
The ready state must be written after the last externally visible side effect derived from
the load. A marker written before dependent initialization, device commands or callbacks is
not a valid completion boundary.
Resolution may exclude the published canonical timer range for that one boundary. The next
VBlank must perform a full comparison of DRAM and scratchpad. Any difference at that boundary
is an ordinary divergence. Never suspend comparison of complete DRAM or scratchpad and never
add unrelated ranges to hide intermediate writes.

A VBlank mismatch identifies the only frame eligible for detailed comparison. The supervisor
must restore the paired checkpoint from the preceding matched FrameDone and enable PRE_CALL and
POST_CALL boundaries only inside that bad frame. It must never start a detailed replay from the
initial state or continue detailed comparison beyond the bad frame. If the checkpoint replay
reaches the bad VBlank without reproducing the difference, fail with an explicit checkpoint
diagnostic error. Do not start another prefix or detailed replay to hide that failure.

After a repair, resume VBlank-only comparison from the retained checkpoint immediately before
the repaired frame. Repeating this suffix scan is provisional. Once the suffix reaches the end,
discard all live and durable checkpoints and perform one complete VBlank-only scan from the
record's initial state. Every frame in that final scan must complete a full DRAM and scratchpad
comparison. A full-record detailed acceptance replay is forbidden.

The latest matched frame is exported as one `last-match.xpck` containing DuckStation's
save-state buffer and the native portable state. The container binds the record ID,
DuckStation executable hash, native checkpoint ABI, frame and section hashes. A restarted
supervisor validates and imports both sections, then checks DRAM/scratchpad immediately
before execution. It also rejects a resumed divergence earlier than a frame already
confirmed by a full replay and retries from the initial state without asking the model
to diagnose that false result. DuckStation also restores the controller binding applied at
the checkpoint FrameDone, which is outside its emulated save-state buffer. A matching checkpoint suffix remains provisional until a full-record scan from
the initial state completes after a source repair.

After the final recorded FrameDone, record exhaustion becomes a matching terminal event only when both sides reached the next identical event sequence. Missing input before the declared frame count remains a replay fault.

The event streams must match before memory is compared. A missing call, extra call, wrong
target, different caller or different frame is a control-flow divergence and includes both
invocation paths. The packet also includes the MIPS call address and word, delay slot,
listing, pseudocode, current C and bounded writer evidence.

DRAM comparison excludes reviewed guest stack, firmware-runtime and trusted-device-state
ranges. Scratchpad is compared except for reviewed stack or device-state ranges. Ordering
tables and game-owned callback results remain ordinary memory. VRAM and SPU RAM are excluded
from the first implementation because CPU access is mediated by trusted GPU, DMA and PsyQ
wrappers.

Python never receives full memory. `python_memory_bytes` must remain zero. Telemetry counts
each launch by reason: `converge_attempt` or `diagnostic_refinement`. Repeated temporary
traces outside this loop are a pipeline defect.

An unchanged failure signature is returned without a second model call. There is no repair
attempt cap unless a project explicitly configures one. After complete MATCH, `finalize_match`
must synchronously rebuild Release x86 without `LOCKSTEP_DEBUG`. The supervisor must publish
COMPLETE only after that build succeeds and records its executable hash. A failed Release build
fails convergence instead of leaving the Debug lockstep executable as the user-facing binary.

The in-process progress window receives compact updates directly from the supervisor. It shows
the furthest fully matched frame and the current replay frame on two 480-pixel bars. The
window is not topmost and closes only when the supervisor marks the run inactive. While the
supervisor is active, a close or minimize request must restore the same owned window instead
of hiding it.
While a model audits a paused divergence, update the same window's title with the active
frame and audit step:

```text
python -B ../xport/tools/lockstep_activity.py --receipt status/lockstep-converge/RUN/supervisor.json --frame FRAME --activity "MIPS audit FUNCTION: STEP"
```

The helper verifies the supervisor PID, process creation time, Default desktop and unique
owned window before changing the title. It does not restart converge or write a progress
file. Update the title when moving to another frame or audit step; a failed ownership check
must not be bypassed by targeting an arbitrary window.

For a paused memory divergence, `diagnostic-request.json` can request a bounded paired
memory sample at the exact divergent semantic boundary:

```json
{"kind":"paired_memory","until_divergence":true,"memory_reads":[{"address":"0x800B6C00","size":4}]}
```

The supervisor binds the target frame and event identity from its current result, reuses
the retained checkpoint when available, and writes `diagnostic-result.json`. The probe
rejects earlier divergence, invalid guest ranges, more than 16 ranges, individual reads
over 256 bytes or more than 4096 bytes total. It compares only requested bytes; full RAM
comparison remains inside DuckStation.
For paired probes, the replay-local `event_sequence` is not part of the binding: the
original callsite and exact DRAM/scratchpad difference must reproduce before bytes are
sampled. A different earlier divergence is returned without sampling.

Control-flow refinement includes original GTE control registers and the live native GTE snapshot. This lets the attention packet prove missing matrix or lighting setup without reading either process memory through Python.
