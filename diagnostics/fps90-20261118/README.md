# 90 FPS, step 1: clock floor and pacing (2026-11-18; installed 2026-11-18, `diagnostics/install-20261118c/`; not committed)

The user asked for 90 FPS first, then higher ("90 fps first then we work our way up"). The plan is in
`diagnostics/fps120-20261117/README.md`. This is its step 1. Step 2, the game's frame counters, isn't started.

## What changed

- `frame_rate` (`[Performance]`, 60 to 120, default 60, at startup): `runtime/options.c`.
- `g_vi_hz` (`hle/services/vi.c`): the rate the frontend loop ticks vi's vsync at, the game's own pace in
  `present_period_ns` (`runtime/present.c`, which the renderer's present schedule also follows) and `main.c`'s loop.
  60 unless `scene60_init` hooked the clock, so an unknown game never runs fast.
- `hle/scene60.c`:
  - Above 60 the hooks go on even with `cutscene_fps=30`. As 30, two fixed steps a frame, scenes would run fast.
  - The clock at `main+0xc1f600`, read from the disassembly: a measured frame's step is the average of the frames
    measured since the last step, in steps of 16.7 ms (`+0xec`), less what the step before took ahead (`+0xe4`),
    kept in 1.0 .. 2.0. A step under 1.0 becomes 1.0 and the rest is carried. Above 60 the floor becomes
    60 / frame_rate. The game's result is recomputed first and only that path is changed: a step the game took from
    its call at `main+0xba3300` (that path leaves the carry alone) is kept.
  - Fixed-step scenes: their even step is scaled by 60 / frame_rate.
- Tests: `tests/runtime_present.c` checks the game's pace at 90 Hz (38 checks pass).

## RivaTuner capped every run at 60

The first 90 run came out at exactly 600 frames per 10 s, every present held 13-22 ms. The user's RivaTuner profile
`Bayo3_120.exe.cfg` had `Limit=60`, and RTSS hooks the isolated copy's exe by its name too. The user set it to 90.
Earlier "uncapped" benches (`fps_limit=0`, a 17 ms median) were probably this cap, not the game's vsync.

## Runs

Fight route (`route-remnant9.txt`) in the isolated copy, `cutscene_fps=60`, `resolution=1`, scripted presses and
shots scaled by rate / 60 so they land at the same real time (`fps_run.py` in the session scratchpad).
The isolated copy's exe and `.ini` were put back after each run. Production saves unchanged in every run.

| Label | frame_rate | RTSS | Frames, time | On the display | Held by the pacer (play) | Exit |
|---|---|---|---|---|---|---|
| `fps90-at60` | 60 | 60 | 6000 in 113.7 s | median 16.75 ms | 7.7-8.4 ms | 0 |
| `fps90-at90` | 90 | 60 | 9000 in 159.3 s | median 16.75 ms: capped | 0-1.3 ms | 0 |
| `fps90-at90b` | 90 | 90 | 9000 in 111.7 s | median 11.25 ms, p99 11.25-17.25 ms, 1-5 of 600 over 19 ms | 1.5-6.7 ms | 0 |

- `pairs-60-90.png`: the shot every 5 s of `fps90-at60` (left) and `fps90-at90b` (right). The same scenes at the same
  times through menus, the fight's load, the death screen and the restart: the game keeps real time at 90.
- The log shows the hook working: `play's step is under 1.0 (0.68) at 90 Hz` when play starts.
- Frames over 33.4 ms in play: 1-3 of 600 per 10 s at 90, as at 60.

## Cutscenes: the title's in-engine loop

No presses, the title's own scene loop, `cutscene_fps=60`. `scene60.c` now logs, every 600 fixed-step frames, the
scene's time (the sum of its steps) against real time.

| Label | frame_rate | Frames on the display | Scene time in real time, each 600 frames |
|---|---|---|---|
| `fps90-title60b` | 60 | median 16.75 ms | 10.000-10.038 s in 10.000-10.041 s |
| `fps90-title90b` | 90 | median 11.25 ms, p99 11.25 ms, 1-2 of 600 over 19 ms | 6.688-6.732 s in 6.695-6.732 s |

So cutscenes run at 90 and keep real time, within 0.2%. The loop shows a different demon each run, so the pictures
(`title-60-90.png`) can't be matched frame to frame. Not checked: sound and lip sync by ear, a story cutscene, a
movie, a skip. The logic steps alternate kinds, so a scene now runs 90 a second (60 at `frame_rate=60`, as on the
console's two per 30 Hz frame): anything a scene counts in steps runs 1.5x, part of step 2.

## Not done

- Step 2: what the game counts in frames (input windows, timers, hit-stops, Witch Time's 62 sites) runs 1.5x
  faster at 90. Not looked at.
- Not played by hand. Rides (`FLAG_RIDE`, steps that must stay exact) not run at 90.
- Not run on AMD, or on a weaker CPU.
- The launcher has no control for `frame_rate`: set it in `Bayo3_120.ini`.

## The user's first play at 90 (2026-11-18 20:3x, `play1/Bayo3_120.log`, 400 s, `resolution=1.6`)

"90 is achieved but stable isn't." The log (frames on the display per 600):

| Stretch | Late frames (over 19 ms) | Render thread recording | GPU | The game's wait for the GPU | What it is |
|---|---|---|---|---|---|
| 0-187 s, title, menus, light play | 0-4 | 4-6 ms | 5-6 ms | 0.2-2.4 ms | fine |
| 187-280 s, heavy play (900-1,450 draws) | 3-14 | 7.0-8.7 ms | 6.6-8.7 ms | 3.2-5.0 ms | the render thread's recording |
| 301-316 s | 8-39 | 3.5 ms | 3.7 ms | 0.1 ms | a video (NVDEC and VIC busy): its own 30/60 on a 90 display |
| 323-360 s, heavy play | 0-9 | 6.9-7.9 ms | 7-7.7 ms | 2-4.7 ms | as 187-280 |

- The GPU's time follows the recording almost exactly (8.71 against 8.7 ms): the GPU runs each window as it is
  recorded, so the frame's sync is reached when recording ends. The game's draw thread waits for the frame before's
  sync (`main+0xb986d4`, an `nvnSyncWait` with timeout 0, spun), so at 90 the render thread's 7-8.7 ms plus the
  game's own 4-5 ms no longer fit in 11.1 ms.
- Hitches 56-164 ms (6 in 190-232 s): windows 52-161 ms, of them draw setup 31-84 ms, pipeline waits 1-6 ms.
  Pipelines made in those windows: 68-245 per 7 s.

## The double tap's window (main+0x55a41c), 2026-11-18

`hle/scene60.c` hooks the input decoder `main+0x55b058` (its only call is the update's, `main+0x55a40c`, so the
address went into `launcher/code-tablecalls.txt`; the launcher now counts that list in the code-mods fingerprint, so
a changed list compiles the game code again). After it returns, the window `[player + 0x3bc]` gets back the game's
decrement unless a whole 60 Hz frame of game time is due. Off at 60.

Scripted (`tap_run.py`, scratchpad): fight route to play, forward held, ZR twice GAP apart, 50 ms presses, game code
compiled with the new list in the isolated copy.

| Gap | 60 | 90 |
|---|---|---|
| 120 ms | transforms | transforms |
| 200 ms | transforms | not in 3 runs |
| 300 ms | no | no |

The log at 90 (a debug line, removed): the window runs 15 to 0 in 218-242 ms (at 60: 250 ms), so it counts game time.
But the second tap reached the decoder 230 and 265 ms after the first, past it: the script times presses in
presents, and the frames around the dodge ran long. Not settled: a test with taps timed in ms is needed, or the
user's hand.

## Where the render thread's recording goes (2026-11-18, `rec90-b`)

New log lines in `gpu/vk/nvn_res.c`, every 7 s beside the pace line: `nvn-live: recording, a frame on average` (lists,
draw setup, the draws' own part, the other kinds) and `nvn-live: a frame's draws on average` (inside `rp_draw_body`).
Fight route at 90, `resolution=1.6`, a heavy window (97.6 s, 694 draws a frame):

| Part | ms a frame | us a draw |
|---|---|---|
| lists, all | 4.59 | |
| draw setup (`draw_pipeline` in `cmdlist.c`) | 4.22 | 6.1 |
| of it, before the replay: stage keys, variants, pipeline lookup | 1.27 | 1.8 |
| descriptor sets (`rp_stage_set`: a new set, every binding written, staging for the host's buffers, every stage of every draw) | 1.01 | 1.5 |
| commands (binds, the draw; dynamic state is already sent only when it changes) | 1.06 | 1.5 |
| textures made ready (`rp_tex_ready`) | 0.43 | 0.6 |
| pipeline fetch, vertex and index buffers | 0.13 | 0.2 |

Recording is linear in draws: the user's heavy play had 1,000-1,200 draws a frame (7-8.7 ms). A frame at 90 takes
about the longer of the game's own work and the recording, plus the GPU's tail, so 90 needs recording at about 7 ms or
less with margin at 1,200 draws: about 5.5 us a draw or less, a cut of 10-30%. Candidates, by size: reuse a stage's
descriptor set when its module and bindings match the draw before (side effects to keep: `own_land_range`,
`rp_read_buf`'s copies, staging per frame); skip the keys and variant lookups when no state they read changed since
the draw before.

## Descriptor set reuse: tried, taken out (2026-11-18)

`rec90-c`: of a set's 1.0-1.3 ms a frame, the driver allocating and writing it is 0.5-0.7 ms. A stage's set was then
reused when its writes matched the draw before's (`rec90-d` .. `rec90-i`; the diff is in the session scratchpad,
`nvn_res-setreuse.diff`). Two traps on the way, worth keeping:

- Staging is GPU memory the CPU only writes: hashing a 1 KB block in it cost about 70 us (reads cross PCIe), and
  frames went to 100-280 ms. Compare what a staging block is made from, never its bytes.
- Moving the set's allocation later moved `vk_cmd()` (which starts the frame) after staging was taken from it.

Result: only 5-13% of sets matched the draw before (each draw binds its own range of a constant buffer for its object),
and building the keys (0.6-0.8 ms a frame) cost more than the reuse saved: recording went up. Taken out; the timing
lines stay (`rec90-j`: back to 4.6-5.7 ms of lists in the heavy windows). What would cut the sets: the per-object
constant buffer as a dynamic offset (`UNIFORM_BUFFER_DYNAMIC`), so one set serves many draws; a change to the set
layouts and the translator's bindings.

## The draw's keys in `cmdlist.c`: measured, nothing to take (2026-11-19)

Temporary timers in `draw_pipeline_body` (`rec90-k`, fight route at 90, `resolution=1.6`), heavy windows of 400-750
draws: of the keys' 0.6-1.2 ms a frame, the fragment texture key (`vk_nvn_tex_key`) is 0.42-0.85, the rest (bound
programs, attributes, variants, the pipeline description, the pipeline lookup) 0.03-0.10 each. 70-84% of draws have the
same keys and description as the draw before.

The texture key's cost is texture cache misses, not the keys: a lookup that hits `tex_get`'s cache takes about 100 ns,
one that misses about 2.4 us (landing the GPU's writes under the texture, `vk_lock`, `vk_tex_from_tic`), and the cache
starts empty every frame (`vk_nvn_frame`), so each texture's first lookup in a frame misses (`rec90-n`).

Tried and taken out:

- Keeping the draw before's texture key when the module, slots and pools match (`rec90-l`, `rec90-m`): it hit half the
  draws, but only lookups that were already cache hits. No change.
- The draw's own lookup (with its declaration, a different cache key) taking the texture the key's lookup found, so a
  texture lands and is found once a frame, not twice (`rec90-o`; diff in the session scratchpad,
  `nvn_res-sharedlookup.diff`): textures ready 0.17 to 0.13-0.15 ms, about 0.03 ms a frame. Noise.

What would cut it: a texture cache that lives across frames, its entries dropped when the descriptor or the memory under
it changes. That and the dynamic constant buffer offsets (above) are what is left for recording; both are larger
changes.

## Sets the draw before's serve (2026-11-19)

Measured first (`rec90-p`, `rec90-q`, temporary lines): with every constant buffer's offset dynamic, 52.5% of stage sets
would be the draw before's (3.9% now), most differing in one offset. By binding, what differs from the draw before:
the fragment stage's resolution scales (106, `SNX_VKB_RS`, staged each draw with the same values) on 24-31% of sets,
the storage buffer page map (108, `SNX_VKB_SMAP`, constant, staged each draw) on 9-17%, the game's constant buffer 4
(binding 4, an object's own range) on 26-30%, others under 8%.

Built (`rp_stage_set`, `nvn_res.c`): the page map staged once a frame, the scales built on the CPU and staged only when
they differ from the frame's last copy (compared with a CPU copy, never read back from staging), and a stage's set
the draw before's when every write is the same in the same frame (`vk_frame_id`). The set is allocated last, so the
function calls `vk_cmd()` first, the trap of the first reuse attempt.

`rec90-r`, fight route at 90, `resolution=1.6`: 62-298 sets a frame served by the draw before's; descriptor sets'
driver time 0.40 to 0.35 ms in the heaviest windows; lists 2.30-2.87 to 2.09-2.63 ms in the middle windows, 3.78-4.30
to 3.67-4.31 in the heaviest. Screenshots differ from `rec90-k` only by animation timing (the same as between two runs
of the old build).

Not done: binding 4 as a dynamic offset. It would serve about a quarter more sets, but the driver's whole part is
0.35-0.41 ms a heavy frame, so it can save about 0.1-0.15 ms, and it changes the set layouts every renderer path shares
(`pipeline.c`, `render.c`, `compute.c`) and so every pipeline: the players' pipeline caches would be rebuilt once.

## Textures' write checks, one scan a 4 MB block a window (2026-11-19)

Why a texture cache that lives across frames wouldn't help: what a miss costs is the texture's once-a-frame check
that the game hasn't written its memory, not the lookup. In `rec90-s` (a temporary timer around the check in
`tex_from_tic`, `texture.c`), the check was 0.2-0.5 ms of the 0.3-0.8 ms of "from TIC" in the heavy windows: one
`GetWriteWatch` call per texture (`vk_written_since` scans the texture's own range). A cache kept across frames
would have to make the same check.

Built: `vk_tex_written_since` (`buffer.c`), the textures' check. Inside an NVN window (`vk_ww_window`, opened and
closed by `vk_nvn_live_window`) it scans each 4 MB leaf of the write table once, and every texture in that leaf reads
its pages from the table. A window is one submit's lists, all submitted before it opens (`window_run`, `cmdlist.c`),
so what they read was written before the window's first scan. A write after it is found by the next window's scan.
Outside a window, and for buffers and the mirror, the check scans the caller's own range as before.

`rec90-t`, fight route at 90, `resolution=1.6`, same frames as `rec90-s`: the check 0.5 to 0.3 ms in the heaviest
windows (0.3 to 0.2 in the others); lists 4.1-4.3 to 3.7-3.8 ms and 3.8-4.0 to 3.3-3.5 in the heaviest. No crash,
no "write tracking failed". Screenshots differ from `rec90-s` by animation timing only, about as much as `rec90-r`
differs from `rec90-s` (two builds without this change).

## The fight holds 90; what recording still costs (2026-11-19)

The question: does the heavy fight hold 90 on current code (0.3.5's), and what is recording's next largest cost?
Runs on the fight route at 90, `resolution=1.6`, on a quiet PC (no other game, nothing above 3% of the GPU).

- **The display holds 90.** `rec90-t`, `rec90-v` (both 0.3.5's code) and `rec90-w` (the change below), every 7 s
  window from 35 s to the end: median 11.25 ms, p99 12.5-15.3 ms (one 18.0), 590-599 of 600 frames under 14 ms, 1-5
  over 19 ms. The screenshot frames (one every 450, about 100 ms each) make about 1.3 of those per 600. The pacer
  still holds 5.1-6.3 ms of each frame in the heaviest windows. Recording no longer decides whether a fight holds 90.
- **Run to run, recording varies by about 0.6 ms.** `rec90-t` and `rec90-v`, the same build: lists 3.28 against 3.90
  ms at 76 s, 3.54 against 3.95 at 110 s. A change worth less than that needs several runs to show.
- **Windows per frame: one.** `prof90-d` (temporary counters): every sampled frame was one window, and no texture
  missed the cache twice in a frame. A cache kept across windows saves nothing. Heavy frames miss 300-640 textures,
  all the frame's, since the cache empties each frame; keeping it across frames still needs the write check (the
  section above), and its views would outlive textures the cache evicts.
- **Where the time goes** (`prof90-d`, every thread sampled each 1 ms, 110 s): the NVIDIA driver (`nvoglv64.dll`)
  4,274 samples, about 0.4 ms a frame; in the runtime, `pb_decode` 1,002, `nvn_entry` 833, `gpu_thread` 740,
  `plat_ns` 724, `vk_nvn_live_window` 475, `rp_target_of` 452 (about 0.05 ms a frame), `check_list_body` 442,
  `rp_stage_set` 391, `tex_get` 316. Nearly all other host samples are threads waiting (ntdll).
- **Tried and taken out:** the render targets' address and size packed in their own array (4 KB in place of 256
  entries of about 700 bytes), so `rp_target_of` and `rp_target_vk` scan in cache. `rec90-w`: lists 3.33-3.92 ms in
  the heaviest windows against `rec90-v`'s 3.73-4.13 and `rec90-t`'s 3.28-4.00, inside the run-to-run spread, as the
  profile's 0.05 ms predicted. Not kept.

Not done: step 2 (what the game counts in frames runs 1.5x fast at 90), which is now the open part of 90 FPS; binding
4 as a dynamic offset (0.1-0.15 ms, rebuilds every pipeline cache); not played by hand; not on AMD.

## 1% low at 90: the present slot's slack (2026-11-19)

Fight route at 90, `resolution=1.6`, isolated copy, no screenshots (`shot_every=0`: each costs about 100 ms and would
count), each frame's time on the display from `SNX_PRESENT_TIMES`, the GPU from `nvidia-smi` every 100 ms. The 1% low is
the FPS of the mean of the slowest 1% of frames after the first 30 s (about 6,150 frames, so the slowest 61).

**What the slowest 1% are** (`low90-a`, `low90-b`, 0.3.5's code; the same frames in both runs):

- Two frames of about 68 ms and a few of 25-36 ms at the area's title card (66.6 s) and the result screen (85.6 s): the
  time is after the last window, on the game's side, while it loads. 135 ms of the 1,080 ms the 61 frames add up to.
- Six frames of 20-39 ms when new effects first draw (50.3, 63.0, 71.0, 107.8 s): the render thread translates their
  shader stages (up to 60 in a frame, 16 ms; 0.14 ms translating and 0.12 ms in `vkCreateShaderModule` each, SPIR-V
  128 KB for 3 KB of code) and then waits for each new pipeline in turn (5-8 ms). One frame (62.0 s) decodes 13 Mpx
  of ASTC.
- The rest: 87-96 frames of 12.5-16 ms in steady play. They weren't late for their present slot (0.25 ms after it,
  as all frames); the slot itself was. `pt_hand` (`renderer.c`) gives each frame a slot `PT_SLACK` earlier than one
  period after the last, so the slot creeps up on the frames until they come after it; a frame that does is shown
  when it comes, and the schedule moves with it, so that frame is long on the display. At 250 us that settled where
  frames reached the present a median 0.5 ms before their slot, and 1.5% of them after it.
- The GPU isn't the limit: 50% load on average at 1.87-1.89 GHz, 148 W, 48-53 C, 4.7 GB of VRAM.

**Changed:** `PT_SLACK` 250 to 50 us (`renderer.c`). The slot creeps a fifth as fast, so it sits behind more of the
frames.

| run | slack | 1% low | 0.1% low | frames 12.5-16 ms | over 19 ms | held before the present, median / mean |
|---|---|---|---|---|---|---|
| `low90-a`, `low90-b` | 250 us | 57.7, 57.2 | 23.7, 23.0 | 87, 89 | 12, 12 | 0.49-0.55 / 1.04-1.05 ms |
| `slack50-a`, `-b` (test build) | 50 us | 59.6, 58.9 | 24.0, 23.8 | 34, 39 | 11, 13 | 1.70-1.77 / 2.85-3.03 ms |
| `slack25-a`, `-b` (test build) | 25 us | 61.5, 60.5 | 24.9, 25.3 | 31, 45 | 10, 9 | 2.30-2.51 / 3.65-3.87 ms |
| `slack50-c`, `-d` (the change alone) | 50 us | 60.4, 58.8 | 24.1, 24.0 | 40, 44 | 11, 10 | 1.77-1.85 / 2.73-3.30 ms |

In the steady parts (35-45 s, 85-95 s) the 1% low went from 74-76 to 80-83 FPS. The cost is latency: frames wait
about 2 ms longer, on average, before the present. 25 us didn't cut the long frames further and held another 0.8 ms,
so 50. The temporary builds (`-a`, `-b` rows) carried timing lines that cost about 1 FPS of 1% low.

Not done: the shader stages and pipelines new effects need on their first draw (translation is under `vk_lock`, so
workers can't share it as it is; keeping prepared modules would take about 1 GB); the game's own loads at transitions;
not played by hand; not on AMD; no screenshots compared (the change moves only when frames are presented).
