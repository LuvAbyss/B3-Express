# Graphics storage-image landing: where 35 s of 154 s goes (2026-10-29)

Diagnostics only. Nothing here changes behaviour or is deployed. Runs on the isolated save, production saves unchanged.

## Measurements (street route, 4,800 frames, both runs exit 0)

`range-overlap-diag-1` (`Bayo3_120.exe`, render-overlap.c): every one of 9,000 graphics storage-image landings
came from the land-all path (`vk_gfx_image_land`, 39.7 s, 19.3 GB). None came from a range need, so the page-alias
false-positive hypothesis is **falsified**.

`range-signal-diag-1` (`sig/Bayo3_120.exe`, render-signal.c + gpu-signal.c + engines-signal.c), by signal source:

| Source | Landings | ms | MB |
| --- | ---: | ---: | ---: |
| WAIT_FOR_IDLE (engines.c 0x44) | 4,500 | 12,351 | 12,899 |
| INVALIDATE_TEXTURE_DATA_CACHE (engines.c 0x3DD) | 4,500 | 23,162 | 6,449 |
| job end, syncpoint, release write, other | 0 | 0 | 0 |

Each landing is one image (~2.9 MB at WFI, ~1.4 MB at the invalidate): a `vk_finish()` inside `surf_flush`, then CPU
detile and `vk_write_changed` merge. Batching several copies behind one wait cannot help: there is one per call.

## Why the barrier landing exists, and what cannot be done

`gpu_signal_point()` -> `vkr_before_write(0, 0)` -> `vk_gfx_image_land()` lands every pending image so later work
sees the bytes. Buffer fetches (`buf_get`, `gfx_gpu_src`, `atlas_fill`, `cb_get`) read guest memory without the
image hook. Simply not landing at the barriers would let them read stale bytes, and landing on demand inside a
draw's setup would break the rule in source/CLAUDE.md (no submit or GPU wait in draw setup).

## Next: deferred landing (design, not built)

At the two barriers, record each pending image's copy into its own readback allocation and submit **without waiting**,
keeping the submission's timeline value. Complete (timeline wait, then the existing detile + merge) only when bytes
are needed: a range need (including buffer fetches, alongside their `vk_compute_mem_need`), a re-record of the same
surface (before its new snapshot), job end, syncpoint or release (existing land-all). A timeline wait on an
already-submitted value does not reset frame resources, unlike `vk_finish`. Acceptance: matched ABBA FPS, unchanged
frame hashes/screenshots on the route, and the existing tests plus a landing-order test.
