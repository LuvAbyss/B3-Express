# BC7 texture pack (2026-11-06)

The user's plan for 0.2.0: one in-game preparation step before the title that compiles every shader and builds a BC7
pack of the game's ASTC textures, with one progress bar. Design taken from Witch Time (GPL-3.0-or-later,
`E:\download\WitchTime-v0.4.5-windows-x86_64`: `tools/wtbuild/src/texpack.rs`, `runtime/tex_pack.c`,
`runtime/tex_decode.c`, `docs/STATUS.md` "Converted textures"): a texture is keyed by a hash of its ASTC bytes, its size,
format and dimensions, so nothing of the game's files changes and a texture a mod changes is simply not found.

## Step 1: the key check (done)

A pack made from the archives only helps if the renderer uploads the same bytes. Two halves:

- **The archives.** A prototype scanner (`scan.cpp` here, C++, built with ooz's kraken.cpp, lzna.cpp and bitknit.cpp): pkz entries decompressed with powzix's
  ooz (Kraken, GPL-3.0-or-later, `github.com/powzix/ooz` at `05038060`), nested DAT archives walked, WTB/XT1 records
  read, each ASTC texture's size from the block linear layout (Witch Time's `tex_layout` rules) and hashed as Witch
  Time's `tex_pack_hash`. Each family of archives runs in a process of its own. The user ran it (the safety classifier
  doesn't let the session run code built from a download): **16,828 distinct ASTC textures, 4.89 GiB**, Witch Time's
  census exactly. It took about 20 s.
- **The renderer.** `SNX_TEX_HASHLOG=file` (`gpu/vk/texture.c` `tex_hashlog`) logs each ASTC texture's first upload
  keyed the same way. Build h1 (`Bayo3_120-h1.exe`, `cddcda68`), the user's recording replayed (`range-h1-hashes`):
  1,860 first uploads, 1,041 distinct textures.

**All 1,041 are in the scan, with the same size.** The design holds for this game.

One family fails: ooz crashes (access violation) on `event_000.pkz` `event/ev1004.evn` (Kraken, 30.9 to 47.4 MB). The
census matches without it, so `event` holds no texture the others don't. The builder must still survive it: it runs
in a process of its own, or that entry is skipped.

## Step 2: the encoder (done)

Our own, `src/nx/gpu/texture/bc7.c` (the BC7 tables in `bc7_tables.h` come from Witch Time's decoder). Every block
tries mode 6 (one RGBA line, 4-bit indices); one that mode 6 doesn't fit within a squared error of 256 also tries mode 1
(two RGB subsets, on the partition whose subsets lie closest to two lines) when opaque, or mode 5 (separate colour and
alpha lines, each of the four rotations) when not. A line: principal axis, then three rounds of quantizing (p-bits
tried), nearest index from the projection, least squares. Witch Time's own runtime encoder (mode 6, min/max) was
too weak for the pack: its comment calls it good enough for the rare case it serves.

Bench (`bench.c` here, built in the scratchpad with `astc.c`, `bc7.c` and Witch Time's BC7 decoder and BC3 encoder as
references): the user's recording dumped with `SNX_ASTC_DUMP` (`range-h1-astcdump`, 16,465 levels, 489 MB of ASTC,
2,406 Mpx), PSNR over RGBA against our ASTC decode, the worst 1 % of each texture's blocks averaged over textures:

| | PSNR | worst 1 % | opaque / with alpha | Mpx/s, one thread |
|---|---|---|---|---|
| BC3, min/max fit (today's kind) | 36.15 dB | 31.52 dB | 37.14 / 34.81 dB | 154 |
| **BC7, ours** | **45.12 dB** | **40.65 dB** | **47.73 / 42.59 dB** | **5.13** |

The way there, on every 20th level: mode 6 alone 38.9 dB at 7.9 Mpx/s; with mode 1 46.4 dB opaque but 1.3 Mpx/s;
mode 5 lifted alpha from 38.9 to 42.8 dB; projecting for the index and skipping the other modes under an error of 256
(0.3 dB less) brought it to 4.0, one mode 1 partition instead of two (0.04 dB less) to 4.5 Mpx/s.

With the CPU ASTC decode (19.5 Mpx/s a thread), about 4 Mpx/s a thread. The whole game is about 26 Gtexels at this
dump's ratio of texels to ASTC bytes, so about 9 minutes on the user's 8 cores (16 threads). Witch Time's Intel
encoder (very fast) measured 12 dB over BC3 on its own set, 50.2 dB; ours is 9 dB over BC3 on this one.

## Step 3: the helper and the runtime (working)

- **The pack format** (`src/nx/gpu/texture/texpack.h`, `texpack.c`): Witch Time's design under our own names
  (`index.b7i` and a `<family>.b7p` shard per family of archives), LZ4 blocks by our own compressor and decoder
  (round trip checked on random, repeated and real BC7 data, and a cut stream is refused), the reader.
- **The helper** (`source/tools/texpack/texpack.cpp`, built in the scratchpad with ooz for now): the scanner's walk,
  each level deswizzled (Witch Time's `bl_copy`), decoded by our `astc.c`, encoded by `bc7.c`, LZ4'd; a pool of
  threads; each family in a process of its own, a counting pass first, `.part` renamed when complete, so it resumes;
  progress lines for the game. The user ran it: **16,828 textures, 14 GB, about 10 minutes** on the 5700X3D (16
  threads), every family with textures done. The first count counted a texture shared by two families twice (24.8 of
  20.5 Gtexels): fixed since, counts now leave out what earlier counts found (`<family>.keys`), not rerun yet.
- **The runtime** (`gpu/vk/texture.c` `pack_*`, build b1 `c6596c05`): before a new ASTC texture is created, its
  bytes are looked up; found, it is a BC7 image filled from the pack in one copy (its memory stays ASTC: `Surf.src`).
  The game rewriting a pack texture's memory looks it up again, else encodes the new bytes here. `SNX_TEXPACK=dir`,
  default `shader_cache/texpack`; `=0` off. A `texpack:` log line every 600 frames.

On the recording (`range-b1-pack` against `range-b1-nopack`, screenshots every 600 frames): **1,823 textures from the
pack, 0 missing, 0 encoded here, no ASTC decoded during play**, no fault. The pictures match: the practice room
(hair, outfit, sky, UI) and the street at full resolution (cars, road, embroidery), the differences being animation
only. Reading from the pack costs about 3 ms a texture on the render thread (171 MB in 467 ms in the busiest 10 s).

**Timing** (`range-b1-pack-t` against `range-b1-nopack-t`, no screenshots, 6,000 frames on the display): the pack
costs frames as it stands. Frames over 19 ms 205 against 179; the render thread spent **2,941 ms** reading the pack
(1,853 textures, 1,758 MB: textures dropped after 10 s unused come back and are read again; worst 10 s 838 ms)
against **66 ms** for the ASTC path's first uploads (worst 10 s 18 ms), whose decoding is on the GPU. Video memory
the same (BC7 656 MiB, ASTC as BC3 639 MiB). On this RTX 3080 the pack's gain is the picture (9 dB over BC3), not
time, and it must not cost time: its reads go off the render thread, ahead of the texture's first use (the game
registers nearly every texture 30 or more frames before it draws it, `astc-ahead`), with shard files kept open and
the decompressed blocks kept a while for the textures that come back.

**Reading ahead** (`gpu/vk/texahead.c`): the game's texture registrations (`vk_nvn_tex_registered`, now with the
size) go to a worker thread, which hashes the guest bytes for each size the pack holds that width and height in,
and reads a texture found into a cache (768 MB); the render thread takes its blocks from there. Shards stay open
per thread. The game registers far more than it draws (8,585 registrations in the first 10 s), so eviction matters:

| build | frames over 19 ms | under 14 | render thread on pack textures | worst 10 s | at creation: cache / disk | lowest 10 s 1 % low |
|---|---|---|---|---|---|---|
| ASTC path (two runs) | 179, 169 | 11, 7 | 66 ms | 18 ms | | 32.3, 26.3 FPS |
| b1, no read ahead | 205 | 8 | 2,941 ms | 838 ms | 0 / 1,853 | 24.4 |
| b2, read ahead, least recently used goes | 161 | 4 | 991 ms | 484 ms | 922 / 744 | 22.2 |
| b3, read ahead not yet taken goes first | 185 | 17 | 2,166 ms | 851 ms | 787 / 1,042 | 22.2 |
| **b4, taken goes first, then untaken after 20 s; no read without room** | **162** | **5** | 933 ms | 294 ms | **1,479 / 348** | 16.7 |

b3 had it backwards: a texture read ahead and not yet taken is the one about to be drawn. b4 has fewer slow frames
over the run than the ASTC path, but its worst stretch, the entry to an area (387 textures in 10 s, 169 ms on the
render thread), has a p99 of 60 ms against 38: the ASTC path draws a big new texture from level 2 and decodes its top
levels over the next presents (`astc_spread`), the pack's path uploads every level at once. Left: the same spreading
for pack textures, the 348 reads still at creation, and the hash the render thread takes of each new texture.

## Next

3. Reads ahead on a worker thread, from the game's texture registration; then timing again; ooz as a pinned package and the helper in
   the build; tests.
4. The preparation screen before the title: shaders and the pack, one progress bar, resumable.
5. Launcher: disk space (about 16 GB), an option.
