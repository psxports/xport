# Native XA and STR transport

## Consumer contract

- Compile `src/psx_xa.c` with `src/psx_spu.c`
- Mount the reviewed raw MODE2/2352 CUE with `cd_mount_cue`
- Use `CdInit`, `CdReset(1)`, `CdControl`/`CdControlBPSX`, `CdMix` and SPU CD volume controls
- `ReadN`/`ReadS` start one sequential physical reader; the 44100 Hz `spu_render` clock advances it at 75 or 150 sectors per second according to mode bit 7
- XA-enable bit 6 selects audio demultiplexing; filter bit 3 selects file/channel; with filtering disabled, the first accepted audio identity owns playback until seek/reset/filter change
- STR consumers use `StSetStream` and `StUnSetRing`; `stream_pump` pops queued non-audio sectors without advancing the disc cursor
- `StSetEmulate` retains its independent supplied-memory path
- Synchronous file reads stop sequential playback before changing its cursor
- Muting host output does not disable decoding or advance the reader from polling frequency

## Initialization and commands

- `CdInit` ensures native SPU/backend readiness without resetting an already initialized SPU; `CdInit` and `CdReset(1)` apply CD volume `0x3FFF` and control `0xC001`
- Master volume is changed to `0x3FFF` only when both current fixed master volumes are zero; volume sweeps remain unsupported
- Device fields are the source of truth for MMIO reads and mixing; MMIO addresses are not passed to RAM-only `psx_addr`
- `Nop` reports the actual sequential-read bit `0x20`; `GetlocL` returns the last physical header/subheader, or fails before a valid header
- Seek/reset/unmount invalidate pending data, decoder history and last-header state
- Queue backpressure retains one pending sector credit; it does not overflow an indefinitely growing clock accumulator or overwrite unread sectors
- Commands, disc fetch state and queue operations share the audio lock; the producer does not invoke game or STR callbacks on the audio worker

## Codec scope

- Decode 4-bit and 8-bit XA, mono/stereo, at 37800 Hz and 18900 Hz
- The 37800 Hz path uses the documented seven-phase, 29-coefficient FIR and a deterministic six-step reset phase
- Predictor arithmetic uses a combined prediction with `+32` followed by floor division by 64
- The 18900 Hz path uses an independently generated 63-tap Blackman-windowed sinc filter with rational 7/3 phase; it is a native audio resampler, not a reconstruction of the hardware half-rate interpolator
- Reject malformed subheaders, unsupported emphasis and invalid capacity without committing partial decoder state
- No codec or resampler implementation is copied from DuckStation, Mednafen or Avocado
- Existing Red Book `CdlATV` normalization is retained for the shared mixer
- Native arithmetic, initial phase and resampling do not establish hardware equivalence; full CPU/DMA interrupt semantics remain separate

## Provenance and exercised scope

- Sector format, predictors and 37800 Hz FIR facts: <https://psx-spx.consoledev.net/cdromformat/>
- Command and status facts: <https://psx-spx.consoledev.net/cdromdrive/>
- CdInit/CdReset audio-routing effects were read from complete immutable PsyQ MIPS bodies in the consuming project's boot and MOVPLAY images
- Demolition Racer Release native evidence: `status/draft-c/native-media-runtime-evidence.json`; actual movie image and CD PCM, menu CD PCM, race CD PCM and 600 physics calls passed in hidden/muted native runs
- The separate half-rate fixture selects the disc's INFLOGO movie path as test input; production playlist order is unchanged
- Eight-bit, mono, every title/track, long-duration looping and Red Book regressions are not covered by this game's smoke evidence
