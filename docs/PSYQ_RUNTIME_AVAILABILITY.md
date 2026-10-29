# Newly recognized PsyQ names: shared runtime availability

Scope: four projects and five images in `validation-v3`; only confirmed selected names absent from the preserved Ghidra results. Source inventory date: 2026-10-24. [Machine-readable evidence](../psyq/validation/runtime-availability-v1.json) contains all 148 names, image/address occurrences, strongest matching SDK libraries, implementation locations/body hashes and the shared-source snapshot. This report does not alter classifications or coverage.

## Counts

The 232 newly confirmed symbol occurrences collapse to 148 distinct SDK names. The other 469 analyzer-only addresses are weak or ambiguous and are not counted as identified SDK functions here.

- **23 names (15.5%)** have actual shared runtime function bodies: 32 symbol occurrences
- **1 name (0.7%)**, `_firstfile`, maps to a restricted `firstfile` implementation: 3 occurrences; functional directory enumeration is absent
- **124 names (83.8%)** have no separately named shared implementation or established alias in this audit: 197 occurrences

These counts attest source availability, not complete PsyQ semantics or compatibility with every recovered SDK version. Do not automatically replace a guest function with a native function of the same name. In particular, native `MemCardSync` is implemented but its `MemCardSyncPSX` bridge aborts. Native `DrawSync` has sampler-bound behavior and a synchronous fallback.

| Project | New confirmed addresses | Implementation exists | Restricted firstfile | No mapped implementation |
| --- | ---: | ---: | ---: | ---: |
| Test Drive 5 | 86 | 8 | 1 | 77 |
| Gex 3 | 73 | 18 | 1 | 54 |
| Tekken 3 | 72 | 5 | 1 | 66 |
| FightingForce | 1 | 1 | 0 | 0 |

## Existing implementations

| SDK name | Shared definition |
| --- | --- |
| `ClearImage` | [`ClearImage`](../src/psx_gpu.c) |
| `ClearOTagR` | [`ClearOTagR`](../src/psx_gpu.c) |
| `DrawOTag` | [`DrawOTag`](../src/psx_gpu.c) |
| `DrawPrim` | [`DrawPrim`](../src/psx_gpu.c) |
| `DrawSync` | [`DrawSync`](../src/psx_gpu.c) |
| `DrawSyncCallback` | [`DrawSyncCallback`](../src/psx_gpu.c) |
| `InitGeom` | [`InitGeom`](../src/psx.c) |
| `MemCardSync` | [`MemCardSync`](../src/psx.c) |
| `PadGetState` | [`PadGetState`](../src/psx.c) |
| `PadInfoAct` | [`PadInfoAct`](../src/psx.c) |
| `PadInfoMode` | [`PadInfoMode`](../src/psx.c) |
| `SetDrawEnv` | [`SetDrawEnv`](../src/psx_gpu.c) |
| `SetGraphDebug` | [`SetGraphDebug`](../src/psx_gpu.c) |
| `SetRCnt` | [`SetRCnt`](../src/psx.c) |
| `SpuGetAllKeysStatus` | [`SpuGetAllKeysStatus`](../src/psx_spu.c) |
| `_ctl` | [`_ctl`](../src/psx_gpu.c) |
| `_getctl` | [`_getctl`](../src/psx_gpu.c) |
| `_spu_FsetRXXa` | [`_spu_FsetRXXa`](../src/psx_spu.c) |
| `_status` | [`_status`](../src/psx_gpu.c) |
| `memset` | [`memset`](../src/platform/win/xcrt.c) |
| `setIntrDMA` | [`setIntrDMA`](../src/psx.c) |
| `setIntrVSync` | [`setIntrVSync`](../src/psx.c) |
| `trapIntrVSync` | [`trapIntrVSync`](../src/psx.c) |

## Names without mapped implementations

Most are SDK driver internals. Existing native GPU, CD/filesystem, pad, interrupt and audio services can replace complete higher-level behavior without recreating each original helper. Absence in this list does not authorize 124 new runtime functions. Establish the original owner, callers, SDK version and an existing host API boundary first. Project-local translations are excluded from the shared-runtime count.

| SDK library | Count | Names |
| --- | ---: | --- |
| LIBAPI.LIB | 1 | `_Pad1` |
| LIBC2.LIB | 1 | `_putchar` |
| LIBCD.LIB | 18 | `CD_cachefile`, `CD_flush`, `CD_getsector2`, `CD_initvol`, `CD_newmedia`, `CD_searchdir`, `CdReset2`, `_cmp`, `callback`, `cb_data`, `cb_pause`, `cb_read`, `cd_read`, `cd_read_retry`, `def_cbread`, `def_cbready`, `def_cbsync`, `getintr` |
| LIBDS.LIB | 22 | `CQ_add_result`, `CQ_delete_command`, `CQ_error_flush`, `CQ_last_queue`, `CQ_ready_system`, `DS_cw`, `DS_read_cbdata`, `DS_read_cbready`, `DS_ready_system`, `DS_reset_members`, `DS_sync_for_system`, `DS_sync_for_user`, `DS_sync_for_void`, `DS_sync_system`, `DsRead`, `DsReadSync`, `ER_cbstart`, `GD_cbready`, `StCdInterrupt2`, `ds_read`, `parcpy`, `rescpy` |
| LIBETC.LIB | 5 | `memclr`, `setIntr`, `trapIntr`, `trapIntrDMA`, `v_wait` |
| LIBGPU.LIB | 23 | `GPU_dummy`, `SetDrawEnv2`, `_addque`, `_addque2`, `_clr`, `_cwb`, `_cwc`, `_drs`, `_dws`, `_exeque`, `_otc`, `_param`, `_reset`, `_sync`, `_version`, `checkRECT`, `get_alarm`, `get_ce`, `get_cs`, `get_mode`, `get_ofs`, `get_tw`, `set_alarm` |
| LIBGS.LIB | 2 | `gte_read_lc`, `valiable_init` |
| LIBMCRD.LIB | 6 | `_callback_handler`, `_clr_card_event`, `_get_card_event`, `_get_card_event_x`, `_make_file_discrpt`, `_rslt_to_code` |
| LIBPAD.LIB | 38 | `PadEnableCom`, `_padClrIntSio0`, `_padCmdParaMode`, `_padInitSioMode`, `_padLoadActInfo`, `_padRecvAtLoadInfo`, `_padSetActAlign`, `_padSioRW`, `_padSioRW2`, `chkCurrLimit`, `clrInfo`, `cmdActInfo`, `cmdCombList`, `cmdGetType`, `cmdNext`, `cmdPadInfo`, `dirChkEngCmd`, `dirClrCmdNo`, `dirRecvAuto`, `dirSendAuto`, `execIntState`, `getTxByte`, `intFuncAck`, `intFuncCmd`, `intFuncData`, `intFuncReqa`, `intFuncSize`, `intPad`, `isVsync`, `nextPort`, `port2PadInfo`, `ptr2Port`, `recvAtActAlign`, `recvAtLoadAct`, `recvAtMainMode`, `sendAtActAlign`, `sendAtLoadAct`, `sendAtMainMode` |
| LIBSND.LIB | 2 | `_SsSeqCalledTbyT_1per2`, `_SsTrapIntrVSync` |
| LIBSPU.LIB | 6 | `_SpuIsInAllocateArea_`, `_spu_FsetDelayR`, `_spu_FsetDelayW`, `_spu_FsetPCR`, `_spu_Fw1ts`, `_spu_FwriteByIO` |

## Verification and limits

Definitions were parsed from shared top-level C/C++ sources and headers, including static/inline bodies; unmatched names were checked throughout nonvendored shared runtime/platform sources and headers. `memset` is implemented in Windows `xcrt.c` and provided by the host C runtime on other platforms. The only leading-underscore alias used is explicitly restricted `_firstfile` Р Р†РІР‚В РІР‚в„ў `firstfile`. No fuzzy name matching was treated as an implementation. Comment/variable occurrences such as `callback` are not function definitions.

The source snapshot records every inspected runtime file hash. Other agents may subsequently change the shared sources; recheck those hashes before reusing the numbers. This was a source availability audit, with manual inspection of matched bodies and identified restricted bridges; no runtime implementation was changed and no behavior-equivalence tests were claimed.
