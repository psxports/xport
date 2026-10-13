# Lockstep recording

The public recorder writes `xport-lockstep-record-v1`. Its complete payload is:

- Identity and SHA-256 of the initial save state
- Identity of every disc file, original executable, settings and patched DuckStation
- Video standard and effective field rate
- One effective state for both controller ports at every FrameDone

Agent task commands are:

```text
record SLOT NAME
stop
```

This `stop` applies only to the active `record SLOT NAME` operation. After `function_converge NAME`, route `stop` through [FUNCTION_COVERAGE.md](FUNCTION_COVERAGE.md); never call the recorder stop path for a function-coverage session.

`SLOT` is an integer from `1` through `10`. It identifies
`tools/duckstation/data/ROLE/savestates/SERIAL_SLOT.sav`, where `ROLE` and
`SERIAL` come from `xport-project.json`. The recorder rejects a missing slot, a
state with an unsupported header and a state whose serial differs from the
configured game serial.

The generated project launcher implements `record SLOT NAME` as:

```text
record SLOT NAME
# Play in the visible DuckStation window
```

On a `stop` task, run:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File ../xport/tools/record.ps1 -Project PROJECT -Action stop
```

`record.ps1 -Action start -State STATE_PATH -Name NAME` is a low-level interface
for tests or diagnostics that must name an exact save-state file. Do not use it
for a normal `record` task. `record.ps1 -Action start -Slot SLOT -Name NAME`
is the PowerShell equivalent of the generated `record SLOT NAME` launcher.

Only one interactive recording may have status `starting` or `recording`. A new
`record` task fails while such a session exists. `stop` resolves the single active
session and fails if none exists or if persisted state violates the singleton invariant.

Agents run `record.ps1 -Action start` or the project `record.bat` with GUI permission outside their sandbox. Both start
the recorder explicitly on `WinSta0\Default`. A numbered slot resolves to the recording role's game save state and is checked against the configured game serial before launch;
`trace_workflow record` accepts only bounded `--frames N` automation.

Recording preserves the configured CPU overclock enable flag, numerator and denominator. Never normalize these settings to the default CPU clock. Capture them in the settings snapshot and effective recording contract.

DuckStation polls its normal input sources. The recorder captures the effective state
after polling and before the next frame executes. The native replay applies the same state
from its own FrameDone hook. Intermediate game polls therefore observe the previous
FrameDone state on both sides.

Interactive recording never stops at FrameDone. DuckStation appends each compact two-port
state to an internal buffer while emulation continues. `record.ps1 -Action stop` interrupts
the emulator once, drains the buffer in bounded pages and commits `record.json`. The GDB
stop/continue cycle at every frame is reserved for converge and must not be used by record.

The record stores no periodic RAM, VRAM or SPU snapshots. Initial RAM and scratchpad come
from the save state and are seeded into shared mappings once. A changed source file, disc,
executable, settings file or save state invalidates the record before replay.

The first command returns only after capture is armed. The default-desktop worker owns the visible
DuckStation instance. The stop command waits for `record.json`, then the worker closes the
emulator. Recording launches DuckStation for reason `record_capture`. Existing names are
immutable; choose another name rather than overwriting evidence.

Use `--frames N` only for bounded automated tests. It records the same compact contract and
does not change the manual record format. DuckStation runs continuously and stops once after
frame `N`, then the recorder drains the same internal buffer.

The declared frame count is part of the terminal contract. Replay accepts input exhaustion only after all frames were consumed and both event streams reach the same terminal sequence; it never pads or repeats the last input.
