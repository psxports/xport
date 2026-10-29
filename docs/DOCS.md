# Agent contract maintenance

Audience: agents only. AGENTS.md is the mandatory router, not a reference dump. Each workflow contract specifies inputs, authority/goal, ordered actions, failure/stop conditions, output/progress and terminal checks. Technical references are read by affected section only.

## Ownership and brevity

Canonical owners are in CONTRACTS.json. Build rules belong to BUILD_POLICY, context/bindings/Git staging to CONTEXT, host executable/authentication discovery to HOST_TOOLS. Stage files own their execution lifecycle. Link to a rule owner instead of repeating its paragraphs or appending project-specific exceptions.

Keep mandatory entrypoints within the byte budgets checked by docs_check. Budgets constrain entry overhead, never justify dropping semantic requirements. Move a required technical contract to an addressable reference section and route it explicitly. Prefer short ASCII operational text; no narrative, historical rollout instructions, vague 'usually/as appropriate' gates or unresolved alternatives without a selection rule.

Shared runtime references contain current ABI, ownership, behavior, constraints/errors and scoped check commands. Project provenance/rollout status is retained in RUNTIME_EVIDENCE.json and the owning project. Historical notes are evidence, not present API behavior or a prerequisite for unrelated projects. Never erase a real unsupported feature or verification limitation while shortening text.

README files remain immutable under AGENTS. Write X invocations in inline code or fenced command blocks so they are distinguishable from coordinate/prose uses. Update linked commands/contracts when changing tool behavior; remove superseded instructions, aliases and dedicated tests after dependency review. Lack of a Python import does not prove an executable is unused: inspect launchers, stage routing, CLI references and current consumers. Do not preserve dead code just for old project wrappers, and do not delete a working compiler/SDK/toolchain as cleanup.

## Checks and scoped reading

Run `python -B [XPORT_ROOT]/tools/docs_check.py` after documentation/command changes. It checks local links/anchors, referenced X commands/scripts, known retired APIs/tools, repeated long normative paragraphs, stage entry budgets and declared contract owners. It does not prove natural-language consistency or runtime correctness; manually compare changed contracts to implementation and active user requirements.

For a technical section use `python -B [XPORT_ROOT]/tools/docs_check.py --section docs/FILE.md#anchor`. The helper returns the complete section with its source range, default maximum 12000 characters. An oversized section fails instead of truncating rules: choose a subheading or explicitly increase --max-chars. Never apply this bounded documentation reading as a replacement for complete MIPS evidence.

For Python/static-tool changes run focused positive and negative tests covering changed behavior. Native game builds are governed by BUILD_POLICY and are not implied by this maintenance task. Remove only owned temporary outputs at completion and report actual checks/limitations.
