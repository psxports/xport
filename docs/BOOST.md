# `boost`: fast draft C

Normative agent contract for `boost NAME` or bare `boost`. This command produces unverified draft C. It is exempt from `DECOMPILATION.md` proof gates and never claims equivalence.

## Input

- `boost NAME`: use `status/function-coverage/NAME/functions.bin` without starting `function_coverage`, `coverage` or `converge`
- Bare `boost`: use only the explicit image-qualified addresses or project-owned list supplied by the user; otherwise fail `BOOST_FUNCTION_SET_REQUIRED`
- Bind addresses to `analysis.default_image` and current original hashes
- Exclude only reviewed PsyQ/SKIP functions
- `N` is the resulting unique selected count; never add dependencies to it

## Execution

1. Do not create a goal
2. The active model performs every translation itself; do not create worker agents or delegate functions to other chats
3. Preserve usable existing C; do not restart translations
4. Sort remaining functions by descending listing bytes and process them in that order
5. Before translation, save one signature registry at `status/boost/NAME/signatures.json`: image, address, C symbol, return type, parameters, stack arguments and uncertainty. Resolve every prototype against this registry
6. Reserve `game_batchN.c` modules before use and keep each within the line limit
7. Save completed functions directly to project source as they finish
8. Stop after every selected function has been processed once

Use `manual` instead of `NAME` for a bare set.

## Translation

- Use complete pseudocode as the structural basis
- Use MIPS only to restore lost or ambiguous logic; no complete MIPS audit
- Preserve semantic stack arguments, memory access widths, arithmetic, branches, side-effect order and call order
- Do not reproduce guest stack frames, register saves, spills or register-shaped C solely for later verification
- Mark uncertain types, expressions, layouts, branches and targets with brief `TODO` comments
- Do not traverse dependencies outside the selected set
- Missing BIOS/SDK calls and adapters may use project-local fail-fast stubs that identify the symbol and terminate; do not implement them
- Never modify shared xport source, tools or documentation
- Keep each module at most 2000 physical lines; reserve another disjoint `game_batchN.c` when needed
- Do not register new modules in the native project during this pass

Every saved draft needs its exact image-qualified `FUNCTION_MARKER` and a ledger entry containing the original SHA, source, `status: "TODO"`, `draft_translation: true`, `evidence: []` and an unverified-draft note. Never set WIP, DONE, SKIP, static evidence or runtime evidence.

Preserve existing WIP/DONE bodies byte-for-byte. Continue an existing `draft_translation` body. A `link_only_stub` does not count as C; if its address is selected, remove that generated stub and save the draft as TODO with `draft_translation: true`.

## Forbidden

No goal, build, formatting, source-index refresh, test, comparison, fixture, proof, audit receipt, coverage/converge mutation, recording, emulation, replay, debugger, shared-xport edit or equivalence claim. Integration and build are separate work.

## Output

`T` is the number of selected functions with a complete body saved in project source, including preserved WIP/DONE and boost drafts. Prototypes, partial bodies, TODO-only entries, link stubs and chat-only code do not count. Proof and testing never gate `T`. Unresolved functions may leave `T < N`.

Every progress message and the final response contains only:

```text
T/N
```
