# Preserved: unfinished readback-batch edit (2026-10-28 16:26-17:07)

These five files were found in `source/src/nx/gpu/vk/` on 2026-10-29. No note documents them. They implement part
of a batched image-readback idea (`vk_readback_begin`/`end` stubs in `cmd.c`, callers in `surface.c`), but `vk.h` had
been reduced from 29,568 bytes to these 845, which removed every other declaration, so the tree no longer compiled
`render.c` and the other Vulkan files. None of the tested or validated builds compiled these files (they reuse the
deployed archive's objects), so no measurement was affected.

The five files were restored from `E:\download\B3-PC-Port-Development.zip` (2026-10-28 14:52). Each matched the
SHA-256 recorded in `migration/development-manifest.json`. These copies are kept for reference only. Do not restore
them: they would break the build again. The measured case for readback batching is in
`../image-land-overlap-20261029/README.md`.
