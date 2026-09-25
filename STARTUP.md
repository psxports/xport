# New project startup

`startup FULL_NAME SHORT_NAME` initializes the current Codex project directory. Both names are user input; never infer or normalize `SHORT_NAME`. The legacy phrase `Prepare project FULL_NAME (SHORT_NAME) according to STARTUP.md` is equivalent.

## Agent protocol

1. Resolve the current project directory and shared `[XPORT_ROOT]`. If `xport-project.json` exists, stop with `STARTUP_ALREADY_INITIALIZED` before creating a goal or changing files.
2. Create one durable goal for startup, then launch exactly one supervisor:

   ```powershell
   python -B [XPORT_ROOT]/tools/startup.py start --project PROJECT_ROOT --name "FULL_NAME" --short-name SHORT_NAME
   ```

3. Treat the returned JSON as authoritative. Do not reproduce its deterministic steps with individual scripts or inspect full logs unless it returns `failed`.
4. On `attention`, perform only the requested bounded judgment. Write the reusable response file named below and submit it once. On interruption, run `status`; it resumes an unfinished deterministic step under the single-worker lock.
5. Complete the goal only when the supervisor returns `complete`. Original images, accepted exports and unrelated user work remain immutable.

Compact observation and submission:

```powershell
python -B [XPORT_ROOT]/tools/startup.py status --project PROJECT_ROOT --after-revision REVISION
python -B [XPORT_ROOT]/tools/startup.py submit --project PROJECT_ROOT
```

`status/startup.json` is the durable monotonic state, `status/startup.log` holds full command output, and `status/startup-response.json` is the sole reusable response file. An unchanged `status --after-revision` returns `changed: false`; do not call a model again. A response is accepted only when both `revision` and `attention_sha256` match the current attention packet.

Response shape:

```json
{
  "revision": 4,
  "attention_sha256": "hash from startup.json",
  "decision": {}
}
```

## Supervisor-owned gates

The supervisor performs and records these gates in order:

1. Refuse an initialized or conflicting root; create the C/VS2022 scaffold and unique runtime ports
2. Run project diagnosis and Debug native build
3. Inventory immutable inputs and hashes; request image metadata and licensed tool paths only when missing
4. Convert Red Book audio when configured
5. Plan, export, independently canonicalize and fail-closed accept IDA/MIPS evidence
6. Run Ghidra PsyQ recognition; pause for review of accepted and pending classifications
7. Build SQLite/progress and independently validate similarity evidence
8. Issue one bounded dependency-aware MIPS/IDA translation packet; pause for reviewed C changes
9. Run `code_refresh` and publish the compact handoff

The model owns only the ambiguous attention kinds `image_review`, `tool_paths`, `psyq_review`, and `frontier_translation`. It must preserve image-qualified identities, audit MIPS bytes against pseudocode, keep unsupported facts explicit, and never treat compilation, SKIP classification or wrapper presence as runtime equivalence.

Detailed evidence rules and manual recovery live in `tools/docs/PROJECT_SETUP.md` and `tools/docs/DECOMPILATION.md`; read them only when an attention packet or failure routes there. Runtime `record`, `stop`, and `converge` are later workflows and are not part of startup.
