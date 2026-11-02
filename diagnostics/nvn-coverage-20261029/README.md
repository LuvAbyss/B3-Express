# NVN API coverage: Bayonetta 3 against B2recomp (2026-10-29)

Static comparison only: no build, no game run. Names only, no game data copied.

- `nso_names.py` decompresses an NSO's three LZ4 segments in memory and lists `nvn*` names.
  B3's `main` + `subsdk0`: **536** unique NVN names (`b3-nvn-names.txt`). `sdk` (the NVN driver): 36.
- B2recomp `runtime/nvn_gl.c` + `nvn_stub.c` dispatch tables: **502** `{name, handler}` pairs
  (`b2recomp-nvn-handlers.txt`), 19 of them deliberate no-ops (`n_nop`, `n_true`).
- **499 of B3's 536 (93%) have a B2recomp handler.** 18 of those are no-ops in B2 (fast-clear registration,
  ZCull save/restore, tiled cache, debug callbacks), listed in `b3-where-b2recomp-is-noop.txt`.
- **37 have none** (`b3-not-in-b2recomp.txt`):
  - deferred and separate bindings (newer NVN): `Bind{Textures,Images,UniformBuffers,StorageBuffers}Deferred`,
    `BindSeparateTexture(s)`, `BindSeparateSampler(s)` and their deferred forms (10)
  - transform feedback: Begin/End/Pause/Resume, Bind buffer(s), DrawTransformFeedback (7)
  - checkpoints and debug settings: `InsertCheckpoint*`, `DeviceSetAutomaticCheckpoints*`,
    `DeviceSetCheckpoint*` (8, likely no-ops)
  - subroutines and subtiles: `SetProgramSubroutines`, `ProgramSetSubroutineLinkage`, `ProgramGetSubtileSize`,
    `OverrideSubtileSize` (4)
  - NvRm interop: `SyncInitializeFromNvRmSyncNVX`, `SyncGetNvRmSyncNVX`, `TextureGetNvRmSurfaceNVX` (3)
  - `MemoryPoolMapVirtual` (sparse/virtual pools), `TextureBuilderGetViewOffset`, `SetZCullZF32CompressionEnable`
  - `DeviceGetProcAddress`, `DeviceGetInteger`: B2 provides these through its bootstrap path, not the table

Caveat: a name in the binary means the game's loader can request it, not that the game calls it (NVN's C loader
typically fetches every entry point). Which of the 37 are actually called needs a run that logs the NVN entry
points reached. Matching names also do not mean matching behaviour: B2's handlers were written for Bayonetta 2's use.

## Dynamic trace: what the game actually calls (run `range-nvn-trace-1`)

`SNX_NVN_TRACE=1` (source `hle/kernel/debug.c` `kernel_nvn_trace_init`, off by default) hooks `nvnBootstrapLoader`
in the dispatch table, then every pointer `nvnDeviceGetProcAddress` returns, counting calls and logging each
function's first call. Build `trace-build/Bayo3_120.exe`. Route: boot, title, menus, Chapter 12 street with movement
inputs, 4,800 frames, exit 0, production saves unchanged.

- **188 distinct NVN functions called** (`b3-called.txt`). Last new first call at 91 s (gameplay).
- **186 of 188 have a B2recomp handler.** Not in B2's table: `nvnDeviceGetProcAddress` (B2 serves it in its bootstrap
  path) and `nvnDeviceGetInteger` (a device query). 9 called functions are no-ops in B2 (fast-clear registration,
  ZCull save/restore, colour reduction, tiled cache, scratch memory, `BufferInvalidateMappedRange`).
- **None of the 37 functions B2 lacks** (deferred/separate binds, transform feedback, checkpoints, subroutines,
  `MapVirtual`) was called on this route. Other chapters, cutscenes and effects may still use some.
- Counts at 47 s (`b3-call-counts-47s.txt`): `nvnSyncWait` 61.9 M calls (the game polling GPU fences, ~1.3 M/s),
  then texture handles, vertex state, uniform-buffer binds; `DrawElements` 143 k.
- Shader path: `ProgramInitialize`, `ProgramSetShaders`, `BindProgram`. B2 translates NVN programs itself
  (`mx_translate.c`), so B3's shaders would go through that translator too.
