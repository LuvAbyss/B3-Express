# NVN route, phase 1: the seam (2026-10-29)

Plan: `../nvn-route-plan-20261029/PLAN.md`. Not deployed; off by default.

## What was built

- `source/src/nx/hle/nvn/nvn.c` / `nvn.h` (component `hle`): owns the redirection of the game's NVN entry points.
  `nvnBootstrapLoader` (found with the new `kernel_symbol_addr`, `hle/kernel/debug.c`) and every pointer
  `nvnDeviceGetProcAddress` returns get a `g_disp` entry that runs `nvn_entry`, which finds the function's `NvnFn`
  record and calls its handler. The handler table (`g_handlers`) is empty, so every handler is `nvn_guest`, the game's
  own NVN driver. `SNX_NVN_FRONTEND=1` routes; `SNX_NVN_TRACE=1` also logs first calls, counts and nvnSyncWait
  callers (the trace from `../nvn-coverage-20261029`, moved here from `debug.c`). `runtime/main.c` calls `nvn_init`.
- `source/tests/nvn_seam.c` (`nvn_seam`): off leaves `g_disp` alone; bootstrap -> GetProcAddress -> routed call with
  the driver's result; routed once under any name; no function or one outside the code left alone; an unrouted
  address in `nvn_entry` goes to the interpreter; locks balanced. It fails when `nvn_guest` stops calling the driver
  (mutation check). Eight targeted tests pass.
- Full source build `Bayo3_120.exe` (`8df827a1...`), includes the chunked submits (default 50).
- `source-changes.patch`: the phase's source diff.

## Exit criterion (runs `../desktop-launch-20261028/range-seam-*`, `runs-*.json`)

| run | env | FPS last two intervals | draws/interval | dispatches |
| --- | --- | --- | ---: | ---: |
| off-1 | none | 21.94 22.03 | 542,605 | 23,020 |
| on-1 | FRONTEND | 20.35 21.09 | 542,947 | 22,990 |
| on-2 | FRONTEND | 21.24 22.02 | 543,056 | 23,136 |
| off-2 | none | 22.46 21.95 | 542,912 | 23,188 |
| trace-1 | FRONTEND + TRACE | 21.06 21.15 | 542,593 | 22,899 |

All exit 0, 4,800 frames, production saves unchanged, same workload state, periodic screenshots match per index.
**All 188 functions were called through the module** (trace-1: 188 first calls, 151 M calls by 158 s). Behaviour
unchanged: phase 1 done.

Pass-through costs ~4% (mean 22.10 off vs 21.18 routed, ranges overlapping slightly): a hash lookup and an atomic
count on ~1 M NVN calls/s, most of them nvnSyncWait polls. Irrelevant while off by default; for phase 2, count only
when tracing and give the hot functions (nvnSyncWait first) host handlers.

## Next: phase 2 (shadow)

Register handlers that still call the driver and also build host records of memory pools, buffers, textures and
builders, samplers, programs, command buffers, syncs and windows, checking B3's (SDK 13.3.2) sizes, enums and call
patterns against B2recomp's assumptions (SDK 3.6.0). Start with the object-creation functions in `b3-called.txt`.
