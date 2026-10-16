# Original function coverage

Normative extension of `../AGENTS.md` for the chat command:

```text
function_converge NAME
stop
```

`function_converge NAME` starts one interactive DuckStation run from disc without a save state and records the first execution of every registered function in the configured `analysis.default_image`. `NAME` identifies `status/function-coverage/NAME`; it must contain 1 through 64 ASCII letters, digits, `.`, `_` or `-`, must be one path component and must not equal `.` or `..`.

Resolve `PROJECT_ROOT` from the current working directory's nearest ancestor containing `xport-project.json`, or from a project path explicitly supplied by the user. If the working directory is a shared parent or the xport toolset and no project path was supplied, ask for the project path before starting. Do not select a sibling project using `NAME`, a game title, previous coverage receipts or available artifacts. Use that resolved `PROJECT_ROOT` for `X` and all runtime checks.

Execute the start command as:

```text
X function_coverage start --image IMAGE --name NAME
```

The launcher uses DuckStation's default CPU-driven timing. `-standard-timing` remains an explicit emulator alias for this default; do not append it to the xport `function_coverage start` command. The cooperative `-lockstep-timing` flag belongs to record/replay launchers and is not used for function discovery. Function coverage uses CPU-driven device time and ordinary disc/BIOS boot without `duckstation.wait_points`, `sio_delay`, save states or project-specific executable injection. GDB registers targets and drains results; it does not configure device time.

Resolve `IMAGE` from `xport-project.json` key `analysis.default_image`. Do not assume `MAIN.EXE`. Wait for JSON status `running` before asking the user to play. DuckStation must be visible on `WinSta0\Default`. Do not launch it hidden and do not move it between desktops.

The exact user command `stop` following `function_converge NAME` means:

```text
X function_coverage stop
```

Do not route this `stop` to `record stop` or terminate DuckStation directly. Stop pauses the owned emulator, drains the in-memory list, atomically writes `status/function-coverage/NAME/functions.bin`, updates `status/runtime/function-coverage.json`, and then closes the owned emulator. Success requires status `complete`, a nonempty file whose size is divisible by four, and `unique_functions == filesize / 4`.

`functions.bin` is a headerless sequence of unique little-endian `uint32` function addresses in first-entry order. DuckStation deduplicates addresses in memory. Never sort, deduplicate or otherwise post-process the file. Coverage proves execution only; it does not prove correctness, unreachability, call relationships or implementation status.

Start writes `status/function-coverage/NAME/receipt.json`; stop atomically finalizes both that run-local receipt and `status/runtime/function-coverage.json`. The completed run-local receipt is the provenance input for the later `coverage NAME` command. For a legacy directory created before run-local receipts existed, `coverage` binds the list to the currently configured `analysis.default_image`, verifies every address against that image and records `legacy_default_image_binding: true`; it does not launch another recording.

Only one function-coverage session may be active. Do not start it while a user-directed `record` or `converge` operation is active. A missing or stale active receipt is an error; do not fabricate or recover a partial list from logs.

Use the low-level `function_coverage start --image IMAGE --name NAME [--role ROLE] [--hidden]` interface only for an explicitly requested diagnostic. The chat command always uses a visible window and the configured default image.

## Explicit status-limited recording

Only when the user requests a partial recording, add `--status TODO` (or `WIP`, `DONE`, `SKIP`) to the start command. The default still registers every image function. The filter reads the configured analysis database in read-only mode, binds statuses to the exact image/address/original SHA-256 and rejects stale or missing identities. It does not modify accepted exports or implementation statuses. The registered-target limit is 8192; selected counts outside 1..8192 fail before launch. Function coverage has independent capacity; the call-tracker boundary-target limit is unchanged.

The receipt records `target_filter.status`, the database path/hash and `full_image_coverage: false`. Such a recording proves execution only within the selected status snapshot and cannot establish full-image or full-game coverage. Stop preserves the selected run's first-entry order and receipt metadata.
