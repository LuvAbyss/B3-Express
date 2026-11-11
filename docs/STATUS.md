# Bayonetta 3 PC port status

**Installed (2026-11-09, at the user's request):** runtime `1ffcc581...` built from `86b0e90` (widescreen stages 2c and 3: black bars beside movies, the HUD at the screen's edges with `hud_area`), game code compiled with the new `code-tablecalls.txt` (`f323390c...`), saves unchanged (hashed before and after, `diagnostics/install-20261109/`). The same files ran the fight route in the isolated copy at 21:9 for 120 s: layouts hooked, exit 0, saves unchanged. The installed settings keep `aspect_ratio=0`: the user picks 21:9 or 16:10 in the launcher's Options. Rollback to the 2026-11-08 candidate (`3eb7ebfd...`, game code `4e8d0411...`, stamp, patches and settings) in `diagnostics/install-20261109/preserved-installed/`. The next release is numbered 0.2.5.

**Release 0.2.5 (2026-11-09, packaged, the user publishes it):** mods (RomFS and code patches, the Mods page), the detail options, widescreen stages 1 to 3 with the HUD position option, and the issue 10 fix, runtime `1ffcc581...` built from `86b0e90`. Zip `release/B3-Express-0.2.5-windows-x64.zip`, `f44b79fb...`, 84.0 MB, 1,027 files. New-player test on the unzipped package through the new window's logic (`test-fresh.py --web`): Install from an extracted dump in 176 s, no error, next step Prepare shaders, the game drew 600 frames and exited 0. Notes in `release/RELEASE-NOTES-0.2.5.md`.

**Issue 10 fixed in the source (2026-11-08, `05594f4`, not released):** Install stopped at "cannot resolve tool paths" on a player's Windows 11 laptop. The compile step resolves the build folder compiled into the runtime (`D:\Project-B3PC\...`), and when Windows refused to open that path (a `D:` that is an empty card reader or DVD drive, a locked BitLocker volume, a folder the user can't read) the runtime gave up. It now resolves such a path like a missing one, and the error names the path. Reproduced with an unreadable build folder and checked fixed, from an ASCII and a Vietnamese folder name. Reaches players with the next release.

**Kraken fight, verse 4: a black slab and smearing debris (the user, 2026-11-10; slab fixed, debris to check).** Compared with a capture from a Switch video, the dark rubble mound in front of the Kraken draws as a flat black shape with straight edges, and fills most of the screen when the camera goes inside it. The concrete debris after a tentacle slam smears when the camera moves, then vanishes where it should fade out. The session's log has no warning about either. The fight is too deep in the chapter for a scripted run, so there's a new capture key: **F6** saves the next frame's host images to `captures/frame_<date>_<time>-fNNNNNN*.ppm` beside the exe, and logs every draw of the two frames up to it (shader hashes, draw kind and count, target and depth formats, blend, depth and stencil state, cull) next to the existing per-draw texture lines. `SNX_NVN_CAPTURE_AT=<frame>` does the same in scripted runs. Checked in the isolated copy: a capture at frame 4800 of the fight route logged all 101 draws of each frame (the frame's own count agrees) and saved its images (about 200 MB). Installed into `output/` (runtime `06767f28`, game code unchanged) for the user to capture both bugs. A 100 s fight-route smoke run of the installed files exited 0, saves unchanged. The build before is in `diagnostics/install-20261110b/`. **Found and fixed the same day.** The black goo around the Kraken is the game's look (the user confirmed it); the flat slab with straight edges under it is not. Reproduced offline: the user's save resumes at this fight, so a scripted run from a copy of it reaches it in 30 s. Skipping draws by shader on odd frames only, with captures of an even and an odd frame of one run, put the slab in the goo's material (vertex stage `4962677f`): a box drawn from inside (front faces culled) that the pixel stage carves to the goo's shape by discarding where an alpha, made from two noise textures and the scene's linear depth, is below the game's 64x64 screen-door pattern. `dither=3` (the user's setting, `gpu/vk/dither.c`) turns every such discard into blending by that alpha, which is right for a fade but not for a cut-out: the box's faces near the ground stayed and drew as the slab. With `dither=0` the slab doesn't show; with `dither=3` it did in 5 of 9 shots of the route. Fix: in blend mode a stage whose compared alpha comes from a sampled texture (other than the pattern) keeps the game's discard; fades from vertex and constant values still blend. After it, two `dither=3` runs of the route show the goo with no slab, and the goo's pixel stage hash is the unpatched one. Also fixed on the way: the renderer applied no depth bounds test (always off); `nvnCommandBufferSetDepthBounds` is now kept in the draw's dynamic state and applied (about ten draws a frame use it, none of them the goo). New debugging switches: `SNX_NVN_SKIP_SHADER=hash,...` (vertex or pixel SPIR-V hash; pixel hashes change with resolution, vertex ones don't) with `SNX_NVN_SKIP_ODD=1`, `SNX_NVN_NEUTRAL=hash/tN/v,hash/cN/0` (a texture slot reads zeros or ones, a constant buffer zeros), `SNX_NVN_CAPTURE_AT` takes a list, `SNX_NVN_CAPTURE_LAYER=k`; captures log each draw's guest textures, vertex attributes, depth bounds, depth clamp and offsets, and the NaN counts of every image. Fight route 120 s with `dither=0` and `dither=3`: exit 0, saves unchanged, median 17 ms. The debris that smears and vanishes instead of fading wasn't reproduced; it fades through the same discard, so the fix may change it. Installed for the user's replay. **Debris, 2026-11-11 (not fixed).** The user's captures show the goo and the slab right, but the debris after a slam draws as dotted halftone patches on the ground that float and vanish. Cause: the slab fix above keeps the game's raw discard for every fade whose alpha comes from a sampled texture, and on the fight route that is 27 pixel stages, not only the goo (`c70b81b3`); only 3 stages are still blended. The data flow of the alpha doesn't separate the goo from the others (all pull in the lighting chain), and the cull mode doesn't either: a twin of each such stage with the game's discard, used for draws that cull faces, brought the slab back (the goo's material also draws back-face culled). Both attempts reverted, nothing installed. Open: separate cut-outs from fades by what they do (a depth-write pass, or a screen-space depth read in the alpha) or add an INI switch.

**Smear, 2026-11-11 night: found and fixed, not installed.** The user's own play from the old save (in the isolated copy, production saves unchanged) narrowed it down: the smear stays with `SNX_NVN_CB=0` and with `SNX_NVN_VRAM=0`, and goes away with `dither=0`. The user also pinned the moment: the debris after a slam is right until it should fade, then it disappears, and panning the camera then smears. Mechanism: `dither=3` (dither.c) takes the screen-door discard out of every fading pixel stage and blends by its alpha instead, but a pipeline only blends when it has one colour target. The game draws this debris (and other opaque fades) in a depth prepass (no colour target, LEQUAL, depth write) and then a colour pass with the depth test EQUAL. In the prepass the stage had lost its discard and could not blend, so a fading object wrote its whole shape into the depth. In the colour pass the object blended away to nothing and the ground behind it failed EQUAL, so those pixels kept the previous frame's colour: the smear while panning, and the debris looking as if it disappeared. Fix: a stage made to blend gets a twin module with the even pattern's discard (`g_mod[].cut`), used by every pipeline that can't blend by the fade (no single unblended colour target) and by colour passes drawn with the depth test EQUAL (`NvnPipeDesc.fade_cut`, set in cmdlist.c), so both passes cut the object at the same pixels. Forward-drawn fades still blend. Kraken route 130 s with the camera turning and the user's settings (`dither=3`, `anisotropy=8`): exit 0, production saves unchanged, no slab or other breakage in 91 screenshots. Runtime `140dbc98`, not installed. The user's play of it is the real check.

**Smear, 2026-11-11 night: not the merge.** The user's play of the word-merge build still shows the smear, and so does the same build with `SNX_NVN_CB=0` (video `screenshot-for-debug/smearing captures/2026-11-11 20-16-09.mp4`). The word merge stays (byte merging could tear a word), but it is not this bug. A closer look at capture `f006772`'s scene image: the bands are ground and rubble pieces each with one side pulled far off in the same direction, one tooth per piece, which reads as wrong per-piece transforms or vertices (bone or instance matrices) rather than a single bad vertex. Next test for the user: `SNX_NVN_VRAM=0` (no video memory copies of the pools at all). If the smear goes away, a pool copy is stale when a draw reads it.

**Installed, 2026-11-11 evening (word merge).** The word-merge build (8309e6f, runtime `482d85b6`, game code unchanged) is in output/Bayo3_120/Bayo3_120.exe for the user to replay the ch14 Kraken and check the smear. The even-pattern build before it is kept as Bayo3_120.exe.before-word-merge. The same exe ran the 120 s Kraken route (k46): exit 0, saves unchanged, median 17 ms.

**Installed, 2026-11-11 (third form).** The even-pattern build (4694349) is in output/Bayo3_120/Bayo3_120.exe for the user to replay ch14 verse 4 with a moving camera. The first form stays as Bayo3_120.exe.before-debris-fix.

**Smear, 2026-11-11 (open, next session).** The user finished ch14 (the verse 7 crash is fixed, 8513d06). What they call smearing is NOT the dotted halftone patches (those are the game's own screen-door transparency, now drawn finer for 3 stages). It is long jagged sawtooth bands on the ground and rubble, stretched along the view direction, in the user's video `screenshot-for-debug/2026-11-11 11-22-54.mp4` (00:15, 00:18, 00:36, annotated pictures in `screenshot-for-debug/smearing captures/`) and in captures `diagnostics/desktop-launch-20261028/isolated/Bayo3_120/captures/frame_20261111_1539*-f0064xx..f006772` (clearest: `frame_20261111_154005-f006772`, whose draws are in `k37/game.log` lines 153763..160787). The scripted harness runs (even with anisotropy=8, film_grain=0 and the older save `k20/previous-test-save`) never show it, the user's own play does. That frame has 1347 draws; 13 are instanced alpha-blended effects without depth write or culling: vs `87c2c6d8` fs `093c24c9` (10 draws, 400 instances, 39 indices) and vs `5f656b3b` fs `f38a0534` (3 draws, 35 instances), their per-instance streams are s1 to s5 (stride 4 and 8). Not proven to be the cause. Test for the user: `$env:SNX_NVN_SKIP_SHADER='87c2c6d8,5f656b3b'; .\Bayo3_120.exe` and look for the smear. If it stays, bisect the big groups (vs `e3fa9afe`, 164 draws into an 800x800 viewport, vs `60a613c6`, `494f4283`). Ruled out: stale depth or render-target reads (no fallback messages in the logs), the F6 freeze (the 1638 ms frame in the video was the capture itself). Harness notes: scratchpad `kraken11.sh` (old save, right stick pulsed `sweep_script.txt`), the run needs the older save because the production one now ends the chapter.

**Smear, 2026-11-11 evening: a likely mechanism, fixed, not installed.** The F6 capture `f006772` has the bands already in the scene's colour image (81), before any post effect, as rows of thin triangles pulled far off in one screen direction: vertex positions gone wrong, not a blend or a temporal smear (the game has no TAA, its post chain is bloom and tonemap). Storage buffers the GPU writes (c7, bef8240, 2026-11-03) stay in video memory, and when the game writes the same 64 KiB block from the CPU, the renderer diffs the block against its snapshot and patched in only the bytes that changed (`own_patch`), merging back the same way (`own_merge`). A word written by both the GPU and then the game kept the GPU's bytes wherever the game's new bytes matched the snapshot, which for floats is often the exponent byte: a torn float, so a vertex scaled far away, so a spike. The user's log shows the game writing into such blocks about 1800 times every 10 s. Fix: both merges work on whole aligned 32-bit words (a word the game changed goes in whole, a word the GPU changed goes back only where the guest's whole word is untouched). Kraken route 120 s from the older save (`k46`): exit 0, production saves unchanged, median 17 ms and p99 22 ms as before, patched bytes about 400 KiB per 10 s (was 260). The harness never showed the smear, so only the user's play can confirm it. Tests for the user, in order: the installed build with `$env:SNX_NVN_CB='0'` (GPU-written storage in the pools, as before c7: if the smear goes away, the mechanism is this one), then the fixed build. The shader-skip test above stays as a fallback.

**Installed, 2026-11-11.** The debris fix (b1f4df1) is in output/Bayo3_120/Bayo3_120.exe for the user to replay ch14 verse 4. The previous exe is kept as Bayo3_120.exe.before-debris-fix.

**Debris fix, 2026-11-11 (third form).** The user's captures still showed dotted ground patches around the Kraken goo after the first form. Eight pixel stages shape a cut-out and a fade in one alpha (blended they show as a flat slab, with the game's discard the fade shows as dots). Now five of them, the goo (`goo[]` in `dither_stage`, nvn_res.c), keep the game's discard and the three drawn without culling that leave the ground patches (`patches[]`, hashes of their translated SPIR-V) get the even pattern (dither=2's, at the screen's own pixels), too fine to see as dots. Tried and dropped: blending those 8 through smoothstep(0.3, 0.7, alpha), which brought the slab back in late frames. On the ch14 verse 4 route with the camera turning (scratchpad `kraken10.sh`, right stick pulsed) the dot score of the late frames went from 0.0053 to 0.0044 and no slab showed in the arena frames (the black-area score is dominated by the dark void around the arena, so it was not used). Not played by the user yet. `SNX_DITHER_LOG=1` logs the changed stages. A translator change changes the hashes.

**Chapter select nodes flicker (the user, 2026-11-08):** seen in the launcher on 11-06 with build pt2. On the installed 0.2.5 candidate the user saw no flicker, started exactly as the launcher does and with the video memory copies off, and the test harness's screenshots (the user's settings, save, 2560x1440, fullscreen) show none either. Not reproduced since, so not traced further. On the way, the harness was found to run the fast renderer without `SNX_NVN_RELEASE_ONLY=1` (the launcher sets it): in that debug mode the nodes did go black every other frame, a constant buffer bound from a snapshot of guest memory missing the 32 bytes the game copies into it on the GPU. Fixed for that mode (`rp_cb_written`); normal play doesn't use it. Screenshots finish the GPU's work, so a race between the game's next frame and the GPU can't show in them: check such bugs by eye.

**Widescreen stage 2 (2026-11-08, not installed, `diagnostics/widescreen-20261107/README.md`):** ten more hooks ported from Witch Time (gauges' clip rectangles, capture effects, elements over 3D points, the aim), and a new recompiler feature, hook points: before each instruction listed in `launcher/code-insnhooks.txt` the game code calls into the runtime (through `brk` with an immediate no game instruction can make, so the ABI is unchanged). It carries Witch Time's 34 single-instruction changes: screen-sized sprites at the real UI size, markers kept on screen. In the isolated copy at 21:9 the health and magic gauges now fill to the end of the bar (about 80 % before), dialogs' blurred band and the character select's backdrop reach the screen's edges. 16:10 and 32:9 run clean. Open: at 32:9 a dialog's band stays 16:9 wide, the chapter select's bottom bar and panel end inside the screen (stage 3).

**Gallery slowdown fixed (2026-11-10, not installed, needs the game code compiled again):** the gallery (The World of Bayonetta 3) ran at 7-12 FPS (median 91 ms a frame in the isolated copy, menus before it 17 ms). It was not the video path: NVDEC jobs take about 1.4 ms and VIC 2.5 ms, with no queue (new 5 s `host1x:` lines in the log). The game's job and draw threads on core 2 waited about 2.4 s of every 5 s for that core, held by Wwise's `AK::EventManager` (priority 43), which spent 77% of its time in the recompiler's soft-float helpers, called from one Wwise DSP function (guest `92f37f8`, a meter: vector `FMUL` by element, `FMLA`, squares). Its float state is FZ clear with UFC and IXC set: the audio carries denormals, and the inline fast path (`ir_fp_fast`) sent every operation touching one to the helpers lane by lane. On the console denormals cost nothing. Fix in `ir_fp.c`: with FZ clear, rounding to nearest and UFC and IXC already set, finite operands and results (denormals included) take the host's IEEE instructions, which give the same bits, and the only flags they could add are those two. Checked against the helpers on 43 million random float and double add, sub, mul, div, sqrt and fma, mostly denormal or tiny: no value differs, no other flag. In the isolated copy with game code `799aab41`: gallery median 17 ms, p99 20 ms, 1% low 50 FPS (was 91 ms, 7.4 FPS), same picture; fight route 120 s equal or better (p99 19-23 ms against 19-27). All exit 0, saves unchanged. New diagnostics: `SNX_CORE_STATS=1` logs who waited for which core, `SNX_PROF_CALLERS=1` (with the profile option) counts helper time for the game function that called them, thread dumps show each thread's FPCR and FPSR. The LLVM differential tests fail the same way without this change (`link: not an x86-64 COFF object`).

**Widescreen stage 2c, movies (2026-11-09, not installed):** a movie is already drawn in the interface's 16:9 block, but the game composites its 3D scene into the whole window first, and at 21:9 that scene showed in the strips beside the movie (yellow beside `ev2005`). The renderer now clears those strips to black before the movie's draw (`rp_movie_bars` in `nvn_res.c`): a draw whose fragment stage samples a 16:9 R8 picture from guest memory at least 1280 wide (the Y plane VIC writes) into the window, when the window isn't 16:9. The skill tree's and tutorials' small movies (260x144, 384x216) aren't touched. The gallery's backdrop is a movie too, so its strips are now black where the purple scene behind it showed before. In the isolated copy on the route to `ev2005`: 21:9 pillarboxed, 16:10 letterboxed, 16:9 never clears, and the 21:9 fight route (240 s) never triggers it. All exit 0, saves unchanged. Runtime only, the game code stays.

**Widescreen stage 3, the HUD at the screen's edges (2026-11-09, not installed):** the layouts' element function (`main+0xc0ef04`, added to `launcher/code-tablecalls.txt`, so the game code needs one recompile) is hooked as Witch Time's `h_ui_element`: what is 1280 across in a layout gets the screen's width, heading rules reach the left edge, a few layouts by name (Game Over, Play History, the chapter select's banner, the credits), and with `hud_area=0` (new, the default; 1 keeps them in the middle 16:9) the HUD's health, gauges and score move out to the screen's edges. The launcher's Options page has a HUD position row. Game code `f323390c...`. In the isolated copy on the fight route: 21:9 with `hud_area` 0 and 1 (health and the weapon gauges about 250 px further out with 0, gauges still filled to the end cap), 16:10 and 32:9, and the character select's panel and bottom bar now reach the left edge at 21:9. A title run at 16:9 installs no hooks. All exit 0, saves unchanged. Open: at 32:9 the character select's right edge still shows a lighter strip.

**2026-11-02, phase 6 (smooth 60 FPS and a friendly launcher).** Bayonetta 3 v1.2.0 runs on Windows through ScarletNX: the game's ARM64 code is recompiled to a native DLL, and an NVN-level renderer on Vulkan replaces the Maxwell path. Build a2 is installed in `output/Bayo3_120` (`3b78e390...`, 2026-11-03 at the user's request, rollback to c7 in `diagnostics/scene-hitch-20261103/preserved-installed-a2/`): c7 plus button prompts, shaders in order, the pipeline cache and pool imports off the frame, and the batched ASTC decoder (area-change hitch 264 to 101 ms in the replay), not yet played by the user. c7 was: f5 plus cutscenes at 60, GPU timing by pass naming each large job, a shader module dump, input recording and a test switch for software denormal flushing (`SNX_SOFT_FTZ=0`), with the module recompiled. Release 0.1.0 is c2. Release 0.1.1 (2026-11-03, published on GitHub) is c7 built from the committed source (`2c85ecac...`): it fixes the FPS drop when loading into or out of a verse. Release 0.1.2 (2026-11-04, published on GitHub) is a2 built from `c5774e0` (`ac31d727...`) with the launcher fix for issue 1: zip `7790fbfe...`, 82.7 MB, new-player test passed (Install in 214 s, the game started and exited 0), notes in `release/RELEASE-NOTES-0.1.2.md`. Release 0.1.3 (2026-11-04, published on GitHub without its zip, so replaced by 0.1.4) is 0.1.2 plus Object fade, Transparent by default, built from `784f4fb` (`917c4e4b...`): zip `57b865df...`, 82.7 MB, new-player test passed (Install in 218 s, the game started and exited 0 with object fade 3), notes in `release/RELEASE-NOTES-0.1.3.md`. Release 0.1.4 (2026-11-04, packaged, not published yet) is 0.1.3 plus the fast renderer by default, built from `1944bd4` (`f7d4d4fd...`): zip `6ca30599...`, 82.7 MB, new-player test passed (Install in 218 s, the game started and exited 0), and the packaged game started by itself with no switches set drew with the fast renderer and object fade 3; notes in `release/RELEASE-NOTES-0.1.4.md`. Release 0.1.5 (2026-11-04, packaged, not published yet) is 0.1.4 plus compute shaders in shader preparation, the area-entry ASTC decode spread over frames, the interval-aware 1% low and the launcher's Save settings button, built from `f14f3ed` (`1c13bbaf...`): zip `87f1eaaf...`, 82.7 MB, new-player test passed (Install in 203 s, the game started and exited 0), notes in `release/RELEASE-NOTES-0.1.5.md`. Release 0.1.6 (2026-11-05, packaged, not published yet) is 0.1.5 plus the 32-bit depth fallback for AMD (issue 3) and no pool copies on integrated GPUs (issue 2), built from `754db7e` (`24a5f7b8...`): zip `50e63161...`, 82.7 MB, new-player test passed (Install in 198 s, the game started and exited 0), notes in `release/RELEASE-NOTES-0.1.6.md`. Not installed in `output/`. Release 0.1.7 (2026-11-06, packaged, not published yet) is 0.1.6 plus the present thread (frames on even slots) and the compute barrier fix for issue 2, built from `fbf7bf6` (`30e053b5...`): zip `ac9793e5...`, 82.7 MB, new-player test passed (Install in 193 s, the game started and exited 0), notes in `release/RELEASE-NOTES-0.1.7.md`. Release 0.2.0 (2026-11-06, packaged, not published yet) is the new launcher, started by `B3-Express.exe`, on the same game runtime as 0.1.7 (`30e053b5...`): zip `36a01ea6...`, 84.0 MB, new-player test through the new window passed (Install in 198 s, next step Prepare, the game started and exited 0), notes in `release/RELEASE-NOTES-0.2.0.md`. **New launcher window (2026-11-06, for 0.2.0):** the launcher is now a WebView2 page (`launcher/ui/index.html`, through pywebview, `launcher/web_launcher.py`) over the same logic: one button that does the next step (Install, Prepare shaders, Play) with progress and time left, the last session's average FPS, 1% low and frame time, Options, save backups, a bug report zip and an update check; the old window opens where WebView2 is missing. New-player test through it (`test-fresh.py --web`, 0.1.7 package with the new launcher): Install from an extracted dump in 202 s, next step Prepare, the game ran and exited 0. The user ran the window: Play, Options, backups and the report work. BC7 was measured and parked on branch `bc7-pack` (quality gain on text and effects only, no stutter gain here, 14 GB). Next, 0.2.0 (the user's plan): BC7 with one in-game shader and texture preparation screen, the gallery's FPS (it waits on the movie it plays, `gallery_directing.webm`), and a launcher redesign. **Issue 2 not fixed by 0.1.6 (reporter, 2026-11-05): cause found the same day.** Compute programs with a `BAR` inside flow ran the barrier in one case of the flow loop's `switch`, non-uniform control flow, which NVIDIA tolerates and Intel hangs on (device lost at the first frame with compute). Fix in `spirv_translate.c` (`bar_phases`): the workgroup meets at one barrier outside the flow loop. On NVIDIA it also fixes rotated colours in the dartboard room (the user confirmed the fix's are the game's). 5616 corpus programs pass `spirv-val`, replay on the RTX 3080 clean. Build b1 `bcaee9a6...`, not installed, uncommitted, untested on Intel; test zip `release/B3-Express-0.1.6-intel-barrier-test.zip` for the reporter. `diagnostics/barrier-20261105/README.md`. **Frame pacing (2026-11-05 night):** on the user's new recording `recordings/20261105-201500` (replays in step) and the same route on Witch Time, the gap is steadiness, not hitches (Witch Time p99 about 17 ms, ours 21-29; longest frames alike), so the BC7 texture work was dropped. Most 20-33 ms frames are uneven present times: the pacer holds the game at the frame's start, but the frame shows after varying work (a slow frame is followed by a short one). **Present thread built 2026-11-06 (build pt2 `4aa1d57c`, uncommitted), INSTALLED in `output/Bayo3_120` the same day at the user's request (rollback to 0.1.5 `1c13bbaf` in `diagnostics/pacing-20261105/preserved-installed-pt2/`, saves unchanged):** frames go to the display on even 16.67 ms slots from a thread of their own, with a queue lock apart from `vk_lock`. On the recording, frames on the display under 14 ms 207 to 13, over 19 ms 326 to 177, long-then-short pairs 130 to 4, p99 24.2 to 23.3 ms. The overruns left are real work. Next, the user's choice: one in-game preparation step before the title (every shader and a BC7 texture pack, one progress bar), from a player's request. `diagnostics/pacing-20261105/README.md`. 0.1.5's runtime is INSTALLED in `output/Bayo3_120` (2026-11-04, at the user's request, Object fade back to Transparent, saves unchanged), rollback to a2 in `diagnostics/release-0.1/preserved-installed-0.1.5/`. Its zip (`bfb72d76...`, 82.6 MB) passed the new-player test: Install from an extracted dump in 188 s, the game started and exited 0. On c1, the same without the timing, the user played chapter 12 at 2560x1440: **in-engine cutscenes 54.8 FPS (29.9 on f5), gameplay 54.3 FPS, combat 48.8 FPS, peak 2.9 GB of video memory for the game (about 7.4 GB before phase 6), no crash.** Open: 8-12 s at about 17 FPS after a map load. The c3 load test (2026-11-03, `diagnostics/passtime-20261102/README.md`) names it: a chain of three game compute shaders, 20-25 ms together, and one full-screen pass of about 11 ms even at 3 draws. Their size doesn't explain that cost, so our translation of them is the lead. Those shaders are captured (2026-11-03, `diagnostics/passtime-20261102/spv-c5/`, from a replay of the user's recorded session). Read against Witch Time's on 2026-11-03 (same game code, ours about twice the SPIR-V): every float op is wrapped in a software denormal flush, because the RTX 3080 offers no flush mode, and Witch Time doesn't do it. That alone looks too cheap for the cost; the other suspect is how we run branchy shaders (one block per loop pass, with a wait for lagging lanes at barriers). The user's A/B (2026-11-03) ruled the flush out (no change). The cause is memory: storage buffers a game compute job writes were bound to the pools, guest memory in system RAM, so every access crossed PCIe. Build c7 (2026-11-03, INSTALLED 15:35 at the user's request, rollback to c6 in `diagnostics/vram-cb-20261103/preserved-installed-c7/`, `diagnostics/vram-cb-20261103/README.md`) keeps them in video memory, landing in guest memory only when needed and merged so the game's own writes stay (Witch Time's design, credited). In a replay of the street it lifts the 1% low from about 30 to 46-48 FPS (median 23-24 to 17 ms), with no waits and no rendering faults seen; **the user's verse exit on c7 (2026-11-03): the drop is gone** (compute at most 0.6 ms a frame against up to 24 ms on c6, no sampled frame over 20 ms of GPU against 43, median 17 ms throughout; the user: "better better"). Left: the full-screen pass `1e842019` (about 7-9 ms, half its textures from the pools) and ASTC load hitches. Build c5 (2026-11-03, installed) also records a play session's input and replays it, so a load test recorded once can be rerun on every build: checked on the scripted fight route, which replays to the same place (`diagnostics/input-replay-20261103/README.md`). PlayStation and Xbox button prompts are built (2026-11-03, build p1, not installed, see "Button prompts" below). Goal: butter smooth at 60, judged by hitches and frame pacing, not by FPS above 60. This is an undergraduate's graduate project, started 2026-10-19.

Hardware: desktop Ryzen 7 5700X3D, RTX 3080, driver 621.01. Laptop i5-12450H, RTX 4050 6 GB. Numbers are from the desktop unless noted. Fuller notes: `RESUME-ON-DESKTOP.md` and the READMEs under `diagnostics/`.

## Release 0.1.0 (2026-11-02)

Published by the user on GitHub, repo public, tag `release`. The user judges the build playable from start to finish. Measured here so far: see "What runs".

`B3-Express-0.1.0-windows-x64.zip`, 82.6 MB, built by `diagnostics/release-0.1/make-release.py`, runtime c2. No game files or compiled game code. Guide: `docs/INSTALL.md`, in the zip as `INSTALL.txt`. Tested as a new player on this PC: unzipped to an empty folder, the package's own Python ran Install from an extracted dump in 202 s, the module came out the same as the installed one (`85df1689...`), and the game started and exited 0. Not tested on another PC, or with a dump from another tool.

## What builds

| step | result |
|---|---|
| runtime | c2, `Bayo3_120.exe` 54.4 MB (`5389fc20...`), built from `source/` with the bundled clang (`ninja_build.py scarletnx`) |
| game code | 4 modules, 107,997 functions, 7.35 M instructions, 83 blocks, 250 MB DLL, 1,125,557 entries. 179 s on 14 jobs, peak 3.5 GB of RAM. The same bytes on every build, for one CPU. Installed module `85df1689...`, compiled with the table call list |
| optimisation | O1. It gave +10% over O0, O2 nothing over O1 |
| shader preparation | 7,764 stages from the game, 5,315 unique, 171 s on 14 workers, cache 266 MB |
| launcher tests | logic, page build and real-window tests pass, `launcher/test_launcher.py`, `diagnostics/launcher-install-20261101/` |

## What runs

| scene | how it was seen | result |
|---|---|---|
| title, menus, chapter select | scripted, played | run |
| prologue | scripted, older renderer | 17,077 frames in 901 s, reached Chapter Select, exit 0 |
| street (`f1street`) | scripted | 2560x1440: 41-44 FPS native, 59.4 with FSR 1 from 1600x900 |
| scripted fight (`route-remnant9.txt`) | scripted, 12,000 frames | 59.1-59.7 FPS windows at 2560x1440, GPU 9.3-9.4 ms |
| Jeanne cutscene | scripted | 30 FPS, the game's rate |
| Remnant 10 | played on f5 | 51.0 FPS in combat, see below |
| Remnant 10, laptop | played on 5h2 | 54.0 FPS in combat, 0.43% of frames over 33.4 ms |
| Remnant 11 | played on phase 5 | froze in intense scenes then, not played since the freeze work |
| chapter 12 | played on f5 and c1 | whole chapter, 54-56 FPS in play, 48.8 at 1,300+ draws, no crash |
| chapters 10, 11, Records of Time | older tests | load and play, not measured on f5 |
| other chapters | | not run on the current build |

## Performance history

| date | work | result |
|---|---|---|
| 2026-10-19 | start | street about 17.6 FPS |
| 2026-10-29 | chunked submits | +12% |
| 2026-10-30 | NVN renderer, phases 1-4 | one frame replays correctly, then whole scenes |
| 2026-10-31 | video memory copies of the GPU-cached pools | street 40.1 to 58.5 FPS on O0, 43.7 to 59.8 on O1 |
| 2026-10-31 | FSR 1 | street 40 to 59.4 FPS, GPU 21.5 to 12.3 ms |
| 2026-11-01 | ASTC as BC3, sparse copies | fight peak 8,819 to 3,753 MiB, no FPS cost seen |
| 2026-11-01 | pipeline libraries, shader preparation | first fight on empty caches: waits 27 s to 0.51 s, worst frame about 4,400 to 412 ms |
| 2026-11-01 | launcher Install compiles the game code | extracted folder to Play ready in 196 s |

## Video memory (2026-11-01)

Each step is on by default and has its own off switch.

| run | scene | build | peak MiB | last FPS windows | GPU ms |
|---|---|---|---|---|---|
| m1b | fight | RGBA8, whole copies | 8,819 | 59.2, 59.5, 59.5 | 9.41 |
| m1a | fight | BC3 | 6,053 | 58.9, 59.5, 59.5 | 9.47 |
| m2a | fight | BC3, sparse | **3,753** | 58.7, 59.5, 59.5 | 9.41 |
| m2c | street | BC3, sparse | 3,607 | 44.1, 41.6, 41.7 | 12.93 |

`nvidia-smi` peaks include about 1.4 GB of desktop. Stale check: 9,364 blocks read back, 0 stale.

- **ASTC as BC3** (`SNX_VK_ASTC_BC=0` off): `astc.comp` decodes on the GPU, `bc3.comp` encodes each 4x4 block. Lossy, with a mean difference of about 1/255 in still frames.
- **Sparse copies** (`SNX_NVN_VRAM_SPARSE=0` off): each pool copy is a sparse buffer, bound per 64 KiB block when first copied.
- **Left:** 34% of combat reads still come from system memory, over 99% of them blocks the GPU writes. Video memory as the truth for those blocks is the riskiest change so far, parked. 64 MiB blocks were tried and rejected.

## Freezes (2026-11-01)

| fight | pipelines made | waits total | longest wait | worst gameplay frame | frames over 250 ms |
|---|---|---|---|---|---|
| first run before | 669-714 | 29.5-32.7 s | 2,288 ms | 4,430-5,110 ms | 20-25 |
| first run, libraries | 674 | 27.1 s | 2,241 ms | 4,330 ms | 23 |
| first run, libraries, background preparation | 682 | 24.1 s | 2,254 ms | 4,192 ms | 23 |
| **first run after the pass** | 717 | **0.51 s** | **42 ms** | **412 ms** | **6** |
| second run | 687 | 0.16 s | 44 ms | 260 ms | 2 |

- **Pipeline libraries** (`SNX_VK_GPL=0` off): four libraries per pipeline, links at most 0.2 ms.
- **Shader preparation** (`SNX_NVN_PREP=0` off): stages compile ahead when the game gives them, below normal priority, pausing in heavy frames.
- **Preparation pass:** the launcher's first Play runs it, about 3 minutes, and shows a percentage.
- **Tried:** moving a waiting draw to the front of the queue. Queues were short here, so the cost is the compile itself. It was never measured off.

## The user's play: Remnant 10 on f5 (2026-11-01)

2560x1440, `fsr=0`, 334 s, no crash. Against the matched run on phase5i, combat taken as windows of 900+ draws, 16 windows each.

| | phase5i | f5 |
|---|---|---|
| video memory | about 7.4 GB, another tool | **peak 2,491 MiB** |
| combat FPS | 49.8 | 51.0 |
| combat frames over 33.4 ms | 107, 1.11% | 84, 0.88% |
| mean draws | 1,237 | 1,268 |
| freezes in play over 1 s | 3 | 1 |
| longest freeze of 200-999 ms | 289 ms | 233 ms |

- **The 2.4 s freeze just after the fight is one pipeline, the same in both runs:** 54 draws, a 3-vertex draw waiting about 2.44 s. Every stage was prepared, so the draw most likely asks for a variant preparation doesn't predict. Which one isn't known yet.
- **Combat is CPU limited:** before the first window 5-8 ms, windows 9-12, after the last 3-4, about 19-21 ms a frame, with GPU time at 13-20 ms.
- That run didn't start from the launcher's Play. Preparation was quick only because NVIDIA's disk cache held the shaders from the isolated runs.

## Launcher (2026-11-01)

`Launch-Bayonetta3.cmd`. Pages: Play, Graphics, Controller, Install, Sessions.

- **Install takes an extracted dump**, a folder with `exefs` and `romfs` at 1.2.0. Players need no keys, by the user's decision. The version is checked before anything is copied, then the launcher compiles the game code on that PC.
- **Measured in the real window:** a wrong folder and a same-size wrong version refused, ready for Play in 196 s, the source folder left alone. Closing the window at 10% left no compiler running and the game not marked ready, and the next build finished.
- **First player issue, fixed 2026-11-04 (not released yet):** on a non-English Windows, Install did nothing, with no message and no log (0.1.0 and 0.1.1). The bundled Python 3.15 reads program output as UTF-8, and `tasklist` answers in the PC's code page (German "ausgeführt" has byte 0x81), so the "is the game running" check crashed before any status was set, and `pythonw` dropped the error. That check now reads bytes, `nvidia-smi` is read the same way, and any unexpected launcher error now shows a message box and goes to `logs/launcher-errors.log`. Reproduced with German `tasklist` output on the 0.1.1 launcher (empty status, error dropped), fixed launcher goes on to the version check; `launcher/test_launcher.py` covers it.
- **Save settings button (2026-11-04, not released yet):** Graphics and Controller end with "Save settings", greyed until a setting changes, and a line saying "Unsaved changes." or "Settings saved.". Play still saves first. Closing the launcher with unsaved changes asks whether to save (Cancel keeps it open). A first save before the game ever wrote its settings file no longer fails on the backup copy. Checked in the real window against a scratch copy of the INI: nothing written before Save, the INI updated by each page's button, Cancel and No on close behave; `launcher/test_launcher.py` passes.
- **Hints:** code the recompiler missed runs in the interpreter and is listed in `.hints`. Every build now includes the 351 known addresses. **Tried, no gain:** alternating runs, 559 and 535 frames over 33.4 ms without them, 554 and 536 with them, so the installed module stays.

## Witch Time, for comparison

Witch Time and B2recomp are references, not rivals. Notes: `diagnostics/witch-time-study-20261101/README.md`.

Side-by-side of 2026-11-01: the user played Remnant 10 on both, on one evening, same PC. 2560x1440, 3D at scale 1.6, mailbox on both. Combat is windows of 900+ draws. Hand-played, so the two fights differ. Logs: `diagnostics/freezes-20261101/play-f5-20261101/`, `diagnostics/witch-time-study-20261101/play-remnant10-v045-20261101/`.

| | Witch Time v0.4.5 | this port, f5 |
|---|---|---|
| combat FPS | **59.2** | 51.0 |
| mean draws in combat | 1,208 | 1,268 |
| windows of combat frames below 58 FPS | 3 of 31 (lowest 50.6) | most |
| longest combat frame | **31 ms** | 233 ms |
| longest frame in play | 136 ms | 2,454 ms |
| pipelines made | 564 | 1,717 |
| shader preparation | 8,024 variants, 58 s | 5,315 stages, 171 s |
| frame path | render thread of its own, 11.5 ms a frame, 3.8 of it draw preparation | game and recording in one thread, about 19-21 ms |
| GPU | 37-45% and 135-140 W, as the user read them | 13-20 ms a frame by our log, not read by hand |
| video memory | about 5 GB in RTSS, an older reading, likely the whole card | 2.5 GB, the game alone, not the same counter |

- **The main difference is structure.** Replaying the frame costs about the same on both, 11.5 against 9-12 ms. Witch Time does it on its own thread, while ours does it in line with the game. That is an observation, not yet a measure of what a render thread gives here.
- **The GPU gap is unmeasured but looks large:** 40% of a 16.7 ms frame would be about 7 ms, against our 13-20 ms. Reads from system memory are the first suspect, and we have no same-tool reading of ours yet. A render thread alone won't reach 60 in the windows over 16.7 ms.
- **Stated plainly:** it's ahead on smoothness, and memory is where this port may lead, but that needs one counter on both.

**Superseded on GPU time:** the 7 ms estimate above was wrong, see the chapter 12 section.

## Chapter 12, whole chapter (2026-11-02)

The user played it on both, with `nvidia-smi` recording each. Hand-played, so not the same fights. Our figures leave out windows that cross into a cutscene. Notes and scripts: `diagnostics/chapter12-20261102/README.md`.

| | Witch Time v0.4.5 | this port, f5 |
|---|---|---|
| gameplay FPS | **59.1** | 55.6 |
| combat at 1,300+ draws | **58.9** | 48.8 |
| time under 50 FPS | **0.3%** | 20% |
| windows where 1% of frames passed 33.4 ms | **1 of 350** | 8 of 97 |
| longest frame in play | **132 ms** | 2,428 ms |
| pipeline waits in play | **1.1 s**, longest 435 ms | 5.6 s, longest 2,386 ms |
| loading screens held for pipelines | 115 s in all | none |
| GPU busy a frame at 1,300+ draws, one counter | **10.3 ms** | 16.4 ms |
| GPU busy a frame under 500 draws | 7.9 ms | 8.0 ms |
| game side at 1,300+ draws | 5.7 ms | 6.9 ms |
| peak video memory, `nvidia-smi` | 5,834 MiB | **4,881 MiB** |

- **Baseline for map loads, set by the user:** a seamless feel, not 60 FPS. Target, from Witch Time on the same loads: no 5 s window under about 45 FPS, none with p99 over 33.4 ms, longest frame about 126 ms.
- **Transitions:** Witch Time waits at loading screens until all queued shaders and pipelines are done, 115 s here. Ours does that work in play. This is likely most of why its transitions feel seamless.
- **Heavy drops at 380-700 draws are GPU time:** 16-25 ms, with the game waiting on the GPU. ASTC to BC3 on the GPU is a suspect, not proved.
- **Combat:** 20.4 ms in one thread, and GPU 15.9 ms, so a render thread alone leaves it on the edge of 60.
- **The game's own code isn't the gap.** `before the first window` includes the pacer's hold.
- **Video memory:** on one counter, about 2.9 GB against 3.9 GB, using one idle figure for both, so this port does lead here.

## Cutscenes at 60 (2026-11-02)

Built, tested on scripted scenes and played, **installed at the user's request with `cutscene_fps=60` as c1, now in c2**. Rollback to c1: `diagnostics/cutscene60-20261102/preserved-installed-c2/`, to f5: `preserved-installed-c1/`. Notes: `diagnostics/cutscene60-20261102/README.md`. The design is Witch Time's, written as our own code: four game functions replaced, a recompiler list so their calls go through the dispatch table.

| scene | at 30 | at 60 |
|---|---|---|
| title loop, in-engine | 39.96 s | 40.05 s at 52-59 FPS |
| movie | 79.66 s | 79.87 s |

The user played chapter 12 on c1: in-engine scenes 29.9 to 54.8 FPS, gameplay unchanged once the route is allowed for, no crash. Its lower pipeline waits come from caches filled by the run before. Rides aren't tested, and two scenes ran 2-3 s longer, unexplained. The user heard slight audio desync when a cutscene fell to 28-35 FPS. c2 pays a late scene off faster, by calculation from about 150 to about 50 ms behind at a steady 30 FPS, not yet heard. Skips work, by the user's play. **New first target: stretches of GPU 19-29 ms at 300-860 draws, 89% of c1's frames over 33.4 ms, on both builds. The user reports them on returning from the verse maps, 15-16 FPS for 5-7 s. ASTC causes the single spikes of up to 348 ms at those loads, not the 8-9 s at 16-18 FPS that follow, where the GPU reads 32 ms and fades over about a minute. Witch Time has the same heavy phase, 44.7-50 FPS for 45-90 s after each load, GPU 14-17 ms, with no holds at these loads, so the phase is the game's. The 8-12 s at about 17 FPS (GPU about 55 ms, 92-94% use) are ours alone. The long phase nearly matches: 41-44 against 45-46 FPS. Named by GPU timing, c2: in the worst seconds one game compute job (613xN) takes about 20 ms and one group of passes 13-16 ms, against 0 and 3 ms when settled. The game has dynamic resolution, but it stayed at the usual size, so it isn't the cause. Which shader, and why it costs this much here, isn't known yet. See `diagnostics/passtime-20261102/README.md`.**

## Button prompts (2026-11-03)

The game's prompts can show PlayStation or Xbox buttons instead of the Switch's, with no mod. Ported from Witch Time
(credited in `source/docs/CREDITS.md`): every prompt comes from one atlas texture (`core_sys_btn`, 256x512 ASTC). When
our texture cache first makes it (found by its hash), `source/src/nx/gpu/vk/prompts.c` paints the ten Switch cells over
it in guest memory: face buttons, L/R/ZL/ZR, Plus and Minus. It paints again when the choice changes, and the cache
reloads it like any texture the game wrote.

- Setting: `button_prompts` in `[Controllers]`: 0 Auto (the type of the pad used last, Switch with no pad), 1 Switch,
  2 PlayStation, 3 Xbox. It is also on the launcher's Controller page (not yet in the in-game overlay menu). Face buttons follow
  `gamepad_swap_ab`: by position by default (the console's A, on the right, shows Circle or Xbox B), by letter when swapped.
- Build p1 (`fd057876...`, `diagnostics/prompts-20261103/`), c7 plus prompts. Not installed.
- Tested in the isolated copy on the scripted route: PlayStation and Xbox sets seen on Chapter Select's footer and the
  combo list (L2 / LT in place of ZL), Auto with no pad keeps Switch. Painted cells decode right in our CPU ASTC decoder.
- Not tested: Auto with a real pad, changing the setting while the game runs, a keyboard set (not ported), the game's
  BC3 atlas variant (not ported, only logged as not found).

## Shaders in order (2026-11-03)

Witch Time's change, written here from its notes: a shader whose jumps all go forward runs its blocks once, in order,
instead of in the `switch (pc)` loop (`spirv_translate.c`, `SNX_SHADER_LINEAR=0` turns it off). On the game's 5,616
recorded shaders, those left in the loop: vertex 198 to 7, compute 62 to 15, fragment 2,818 to 1,297, all translating.
Build s2 (`ba207167...`, `diagnostics/shader-order-20261103/`), not installed. On the RTX 3080: no rendering faults
seen, steady p99 23-24 to 20-22 ms on the scripted route, but GPU time in the replayed session is the same within
noise. NVIDIA wasn't paying for the loop, so our combat GPU gap is elsewhere. Witch Time's gains were on AMD, not tested
here. New shader code misses the driver's pipeline cache once after an update.

## Scene-change hitch (2026-11-03)

The 200-300 ms frame at each area change was our GPU decoding 230-310 Mpx of first-time ASTC in one frame (~276 ms),
which the game's NVN driver waits for. The decoder now batches a texture's levels (one barrier before its encodes, one
before its copies) instead of draining the GPU three times a level: on real blocks 431 -> 174 ms for 609 Mpx, byte for
byte the same as before. In the replay: GPU 289 -> 148 ms and 263 -> 111 ms at the two area changes, their frames
264 -> 117 ms and 214 -> under 100 ms; batched across textures too (build a2, `3b78e390...`): GPU 121 and 95 ms,
the first frame 101 ms (a2 INSTALLED 2026-11-03 at the user's request, rollback to c7 in `diagnostics/scene-hitch-20261103/preserved-installed-a2/`). Also moved off the frame: the pipeline cache save (494 MB, every 5 s on the vsync
loop) and the host import of memory pools (up to 313 ms at boot). Build a1 (`7a8d599a...`), not installed;
`diagnostics/scene-hitch-20261103/README.md`.

## Fast renderer by default (2026-11-04)

The user's a2 play test felt worse because it ran on the old renderer: started without the launcher
(`Run-Bayonetta3.cmd` or the exe), so `SNX_NVN_FRONTEND=2` and `SNX_NVN_RELEASE_ONLY=1` were missing. The runtime now
sets both itself when they aren't set (INI `fast_renderer=1`, `main.c` `fast_renderer_default`, before `nvn_init`).
Checked in the isolated copy: nothing set gives the fast renderer (204 `nvn-live` lines), `SNX_NVN_FRONTEND=0` the
old one (none); both exit 0 with saves unchanged. Not installed: `output/` holds a2 for the user's play test.

## Object fade (2026-11-04)

The game fades objects with a 64 x 64 screen-door pattern (429 of 3,755 pixel programs). Build d1 (`953aef25...`, installed 2026-11-04,
then a2 put back the same day at the user's request for a play test; d1's INI and last log kept in `diagnostics/dither-20261104/preserved-installed-d1/`) adds `dither=0..3` (launcher Graphics "Object fade"): the game's, off, even (the pattern at
the image's own pixels) and transparent (blended by the alpha it compared). All patched programs pass spirv-val; six
fight runs at 2560x1440 ran clean with no frame time change, and transparent removes the checkerboard on a real fade. A/B on the fight route (2026-11-04, two runs a mode): slow frames 517/502 game, 505/492 off, 511/496
transparent of 5999, so no cost. Transparent is the default since then, at the user's choice (`dither=3`).
Details: `diagnostics/dither-20261104/README.md`.

## 1% low (2026-11-04)

The 1% low and "frames over 33.4 ms" now count only frames the game paces at 60 (swap interval 1). Its own 30 FPS
stretches, about 910 of the fight route's 6000 frames, were 283 of its 287 "33 ms" frames. A truly cold first play
(NVIDIA's shader cache emptied too, which earlier "cold" runs didn't do) on the launcher path made 8 compute pipelines
on the draw thread, 150-185 ms each: shader preparation skipped compute stages. It now prepares them (5,389 stages,
160 s): 8 to 1, and the five 200-290 ms frames they caused are gone. About 10 graphics compiles (0.6 s) remain,
keyed in ways preparation can't foresee. Waiting once for a compiling pipeline was tried and dropped (no gain).
Area entry: the 228-234 Mpx of new ASTC textures decoded in one frame made the game wait 103-157 ms for the GPU. Big
new textures are now drawn from their third level first and their top two levels decoded over the next presents (16
Mpx each, `SNX_ASTC_SPREAD_MPX`): the area-entry frames went from 92 + 157 ms to 129 + 54 ms, the 129 almost all a pipeline wait, and time lost to slow frames after
30 s from 1,010 to 708 ms (build s2 `9a46b5ae...`, not installed).
Warm runs unchanged. Build l10 (`7c183773...`), not installed, committed as `bf7496c`; `diagnostics/lows-20261104/README.md`.

## Black 3D on AMD, issue 3 (2026-11-05)

Issue 3 (RX 560, and a 6700 XT in a comment): the UI draws, the 3D stays black. The NVN path mapped the game's depth formats 0x33 and 0x35 straight to `X8_D24_UNORM_PACK32` and `D24_UNORM_S8_UINT`, which AMD can't use as depth attachments, so every pipeline with depth was refused ("depth target format 0x35") and every 3D draw dropped. The Maxwell path already fell back to 32-bit float depth (`format.c`). Now the NVN path does too (`depth24_fmt`, `nvn_res.c`), and `SNX_NVN_NO_D24=1` forces it on any GPU. Build d1 (`f8f84cce...`, `diagnostics/amd-depth-20261105/`): the warm fight route with the fallback forced on the RTX 3080 drew the scene with depth and shadows right, 263 pipelines made, 0 failed, no crash, save unchanged. Not installed. **Not tested on AMD:** waits on a reporter's run. Test zip for the issue: `release/B3-Express-0.1.5-amd-depth-test.zip` (`40cecaf1...`, 17.9 MB, d1 as `Bayo3_120.exe` plus a README), for the user to attach.

## Device lost on Intel Iris Xe, issue 2 (2026-11-05)

Issue 2: the device is lost 15-25 s after start, on 0.1.1 and 0.1.4, then "binding video memory to a pool copy failed", which is only the next call after the loss. Not reproducible here (no Intel GPU). The lead: the sparse video memory copies of pools (VRAM step 2, in every release). On an integrated GPU they read the same RAM as the pools, so they only cost memory, and sparse binds are what such drivers test least. Integrated GPUs now skip them (`nvn_res.c`, `vk_nvn_pool_mirror`, the log says "no video memory copies of pools"), `SNX_NVN_VRAM=1` copies anyway. The startup warning "past this GPU's per-stage limits" was false on GPUs without texture arrays (it counted the 512 array descriptors the layouts leave out), now fixed (`pipeline.c`). Build d2 (`7569c7e1...`, both fixes): fight route with `SNX_NVN_VRAM=0` (the integrated path) drew right, 265 pipelines, 0 failed, no crash, 1% low lower as expected on a discrete GPU; default run still copies pools on the RTX 3080 (4 copies), 0 failed. Save unchanged. **Not tested on Intel:** a cause other than the copies (the indexed texture fallback, the Sunshine or RTSS layers) is still possible.

## Detail options as Witch Time's (2026-11-07)

Three new options, ported from Witch Time with credit (`diagnostics/detail-20261107/README.md`): `texture_lod_bias` (sharper distant textures), `model_lod=1` (every model at its finest level, a hook on the game's level selector `main+0xbd563c`, added to `code-tablecalls.txt`) and `film_grain=0` (the grain's strength reads as 0 in the four tonemap stages). The launcher's Options page has all three. On the fight route in the isolated copy, with the same inputs on and off: all three took effect (model_lod changed levels from 14 s; two tonemap stages matched Witch Time's hashes), fine detail measured +11 % to +49 % on the same frames, frame times the same within noise, exit 0, saves unchanged. Build d1 `d9fa42e1...`, not installed.

## Widescreen 16:10 and 21:9 (started 2026-11-07)

**Stage 1 works (2026-11-07, ported from Witch Time):** `aspect_ratio=` and the launcher's Aspect ratio row (16:9, 16:10, 21:9, 32:9, experimental). The game's own picture takes the shape (2100x900 at 21:9, 1440x900 at 16:10, 3200x900 at 32:9), the camera's projection follows (2.3333 and 1.6 measured), the interface stays a 16:9 block in the middle, and the scene targets are drawn wider on x for full sharpness. Checked on the title and the fight route in the isolated copy, every run clean. Interface details (menus' bars, markers over enemies, gauges' clips, movies) are stages 2 and 3. Build w1 `b530a720...`, not installed. `diagnostics/widescreen-20261107/README.md`.

The user chose real widescreen. Not working yet: a 16:10 or 21:9 window still gets the 16:9 picture with bars. First findings in `diagnostics/widescreen-20261107/README.md`: `main` builds 16/9 in code at 8 places and the HUD's 1280x720 canvas at about 25. The game ignores a wider reported display (experiment x1, reverted). A 21:9 return from the one 16/9 getter (`main+0x706b70`, dynamic-resolution sizing) changed nothing at the title (experiment p1, a code-patch mod). Next: find the camera's projection, then widen the screen-sized targets on x, and keep the HUD 16:9 and centered. **Second pass (same day):** the camera's projection is found in the uniform data (16:9, about 46 degrees at the title), and it doesn't follow the game's screen-size getters (`main+0xd02d70`/`d80`, experiment p2). The scene size is hard-coded in the graphics setup at `main+0x69c698` (1600x896). Changing it there, wider (p3) or flatter (p4, 1600x688), crashes 1.2 s in: a setup call refuses other sizes. Next: the GDB stub to find which, instead of a compile per guess.

## Mods (2026-11-07)

**Code-patch mods (build m2 `bb333640...`, same day):** `.ips` (IPS and IPS32) and `.pchtxt` (IPSwitch) files in `mods/<name>/exefs/`, named by or naming the module's build ID as yuzu and Ryujinx expect, applied in `mods.txt` order. One parser (`source/src/nx/loader/patch.c`, C90) is used twice: the recompiler applies the patches before compiling, so patched instructions are what gets compiled, and the runtime applies them again to the modules in memory before setting page permissions. The compiler records a digest of every patch record in `Bayo3_120.patches`, and the runtime refuses to start, with a message in the log, when the patches on don't match it. The launcher copies the code mods of the mods on into the compile, records them in `Bayo3_120.codemods`, and shows "Code mods changed" with a Compile button when they differ. It also imports code-only mods and Atmosphère's `exefs_patches/<name>/`, and treats a zip with `contents/<id>/romfs` and `exefs_patches` under one `atmosphere/` as one mod. Tests: new `code_patches` (IPS, IPS32, pchtxt with offset_shift, enabled and disabled blocks, strings, bad files, build-ID matching), `runtime_nro` with a patch (refused without a matching digest, applied with one, refused again after turning it off), `loader_native`, `driver_workflow`, the filesystem tests and `test_launcher.py` pass. **End to end:** the launcher's own `build_code` compiled the game with an identity patch (4 bytes of `main` rewritten with their own values) in 166 s, logging "1 patch files, 4 bytes patched (4 in code)". The isolated copy ran that code for 45 s at the title, the runtime applied the same patch ("main-identity.pchtxt patches main"), exit 0, saves unchanged. With the mod turned off and the code not rebuilt, it refused to start in 2 s with the message. Isolated copy put back after.

**Launcher Mods page (same day):** add a mod from a folder or a zip (yuzu/Ryujinx, Atmosphère or bare `romfs` layouts, a zip of several mods refused with a message, nothing outside its folder unpacked), on and off, order, remove (not while the game runs), and per mod the game files it replaces and adds, files a later mod covers, and files that differ from the game's names only in capitals. The page warns when the installed game program is older than m1. The bug report names the mods on. Tested: `test_launcher.py` (import, remove, order, overlaps, capitals, refusals), the window's API methods on a temp game folder, and the page in headless Edge with a stand-in API (no script errors, a move saved the new order). Not tested: the real window's file dialogs, which the user runs. **First game run with a mod (isolated copy, m1, 45 s at the title):** an identity mod replacing `Text_000.pkz` and `chapter_000.pkz` with copies and adding one file logged "identity: 2 files replaced, 1 added", the missing mod in `mods.txt` was warned and left out, the title drew normally, exit 0, saves unchanged. The same run with no mods on reached the same frame count (780 against 766 at 45 s, the isolated module is O0). Isolated copy put back after (`01bc7481`, no `mods/`). A real texture or model mod is not tried yet. Players get it once a release ships m1's runtime: 0.2.0 still has 0.1.7's.

The game can now load RomFS mods. A mod is a folder in `Bayo3_120_data/mods/` laid out as yuzu's and Ryujinx's are (`mods/<name>/romfs/...`), so existing Switch RomFS mods drop in as they are. It is turned on by naming it on a line of `mods/mods.txt`. Where two mods have the same file, the later line wins. At start the runtime lays the mods over the index of `romfs/` and builds the RomFS image from that overlay (`fs.c`: `mods_find`, `mnode_merge`, `romfs_build_modded`). The installed `romfs/` never changes, and a mod taken off the list is gone at the next start. Files on are mapped and readonly like `romfs/`. A missing mod, a path instead of a name and a file where the game has a folder are warned about in the log and left out. The log says per mod how many files it replaced and added. ExeFS patches (IPS, cheats) are not handled. Tests: new `filesystem_mods` (two mods in order, one off, a clash, bad lines in `mods.txt`) and the existing `filesystem_tree`, `filesystem_service`, `filesystem_raw` pass. Build m1 `af360cd1...` in `diagnostics/mods-20261107/`, not installed, uncommitted. Four test programs unrelated to this (`tex_upload_rows`, `vulkan_release`, `land_target`, `vi_service`) already fail to build on main.

## Game and source identity

- Title ID `01004A4010FEA000`, display version 1.2.0. Main build ID `B47CC79B88B7091EA514B2B725C50AE01C6F00DF`, main SHA-256 `499983f6...`.
- Four NSO modules: rtld, main, subsdk0, sdk. All 12 decompressed segments match their embedded SHA-256.
- ScarletNX source: commit `5c97949bf6cb69738c7d6ac110f07533376a3bd9`, ABI stamp 8.
- Release model: a launcher, the runtime and local compile tools. No game files, saves or compiled game code ship with it.

## History before phase 5 (2026-10-19 to 2026-10-28)

From the old root README, whose full text is `diagnostics/old-docs/README-root-to-20261028.md`. Laptop, RTX 4050, old renderer. Single runs in this period differ by up to 10%, so small gains here aren't evidence.

| date | what happened |
|---|---|
| 10-19 | First -O0 module: 107,992 functions, 7.35 M instructions, 150 s. Starts, then a black scene with prompts |
| 10-19 | Scene rendering fixed: a shader predicate bug (`O_SHFL` in `spirv_translate.c`) and dropped attachment-less storage passes (`render.c`). 4,040+ skipped draws to 0. Shader corpus 61 of 61 |
| 10-22 | Citron compared: all 86 HID layout constants match. Scripted input reaches the game's SDK |
| 10-23 | An interpreted main module reproduced the same scene, so the recompiled code was unlikely to be at fault |
| 10-24 | Title fixed: a cold start reported a stored user, so the game replaced the title with Chapter Select. Menus and new game then work |
| 10-25 | Prologue reached Chapter Select, 17,077 frames in 901 s. One stall after Skip, seen once |
| 10-26 | 2D-array compute surface stores and storage bindings on mip chains fixed. 211 skipped dispatches to 0 |
| 10-27 | Masked shader-result merge installed, 303,936 correctness cases. Chapter 10 and 11 saves load. Method barrier removal tried, slower, rejected |
| 10-27 | New Game with a save opens a warning with No selected. Not a bug. Prologue path confirmed by scripted runs |
| 10-28 | Landing by target removed whole-queue buffer waits (36,794 calls, 46.7 s of a 360 s run). Not installed then |
| 10-28 | The game wasn't GPU or CPU bound: it recorded GPU work and blocked until the queue drained. This led to the NVN renderer of 10-29 |

Open from that period, unseen since but not proven gone: intermittent native access violations at startup, the stall after Skip. Audio device commands 1 and 7 and account command 160 remain undeclared. The chapter select hand was missing then, and hasn't been checked on the new renderer.

## Left

- **Crash at the end of chapter 14, verse 7 (the user's 77-minute play test of the installed 0.2.5 candidate, 2026-11-08), parked for later.** The game plays four movies back to back there (`ev7023` to `ev7026.webm`). As the fourth opened its decoders (nvdec, vic), the game's video library wrote into a decoder command stream whose buffer pointer was null: `NvRmStreamPushSetClass` (sdk `e4b0930`) through the push helper at `e4b0570`, which stores through `[stream+0x10]`. The cause isn't known yet: our host1x/nvmap emulation (`gpu/video/host1x.c`, `nvdrv.c`) or the game failing to get memory for the buffer. Not seen in any earlier log. Not reproduced: there's no input recording of the session. **Looked into again 2026-11-10 (still not reproduced, and the log is gone: a later run overwrote it before it was copied).** The stream's write helper (sdk `e4b0570`) stores at `[cmdbuf+0x10] + [cmdbuf+0x120]`, where the cmdbuf is one of two inside the stream's 0x12d98-byte heap block (`stream+0x48`). In a live stream that pointer is never null: `NvRmStreamInit` fails as a whole if either 0x8000-byte command buffer can't be allocated or mapped, and `NvRmMemMap` returns the CPU address the library recorded at allocation, not anything nvdrv answers. The only code that leaves it null is `NvRmStreamFree`, which zeroes the whole block before freeing it. So the crash is a push on a stream that had already been freed: one movie's decoder (TVMR in subsdk0) was torn down while its thread still pushed, or the next movie used a stream of the last one. Our side is probably timing (host1x jobs run later on the channel's worker), not a wrong answer, but that's not proven. Done meanwhile: the game keeps the run before as `Bayo3_120.prev.log` (and the bug report includes it), nvdrv now logs each NVDEC and VIC close (and a close while an ioctl still runs on that fd), and the Windows error box is topmost and minimizes a fullscreen window first. Checked with two runs of the movie route in the isolated copy (exit 0, saves unchanged, the first run's log kept). The box change is untested, since no crash happened. Next time, the user replays that spot with input recording on. **Also found:** after the crash, the window stayed frozen for about 2 minutes before the game closed. The crash was on the decoder's thread, the main thread didn't come to show the error screen (probably waiting on that decoder), and the fallback Windows box (`win_fatal_box`, `sdl3.c`) opened behind the fullscreen window. The same session had one 7 s freeze at a cutscene start (2626 s): about 13 pipelines not in the cache, 500-900 ms per fragment library. The pipeline cache was saved afterwards. The rest of the session: median 17 ms, 1% low 40-45 FPS. **Reproduced by the user 2026-11-10 and fixed in nvdrv (not yet play-tested).** The game froze on entering verse 7 and crashed as `ev7026` opened its decoders, this time in TVMR (subsdk0 `e5fd0`): it allocated a 0xb90-byte buffer, `NvRmMemMap` returned success with a null pointer, and the memset after it wrote to 0. The mechanism behind both crashes: `NvRmMemHandleFree` (sdk `425680`) sends the nvmap FREE ioctl first and only afterwards takes the handle out of the library's own table. Our nvmap handed a freed handle number straight back out, last freed first. When movie 3's teardown overlapped movie 4's start (the log shows the fds of one movie still open while the next opens its own), a CREATE on another thread got the number still in the table, and the lookup in `NvRmMemMap` (sdk `4259f0`) found the dying entry, whose CPU address is 0. The earlier `NvRmStreamPushSetClass` crash is the same thing: a command buffer mapped through a colliding handle. Fix: nvmap uses every handle number in its 8192-entry table before reusing one, and then reuses the one freed longest ago. `nvmap_from_id` now indexes the table directly, since the id is the handle. `snx-test-nvdrv-service` now checks a freed number isn't handed out again at once (577 checks pass), and the nvdec tests pass. Installed into `output/` the same day for that replay (runtime `cd34f81a`, game code `799aab41`, which also carries the gallery fix), with the build before kept in `diagnostics/install-20261110/preserved-installed/` and the crash log beside it. The saves are unchanged. A 120 s fight-route smoke run of the installed files exited 0 (median 17 ms). Still to do: the user replays chapter 14 verse 7 with this build.
- Button prompts: the user's check with a real controller on Auto, and a change while playing (p1, not installed).

In order of work:

1. Name the compute shader (613xN) and the pass group behind the drops after loads, and what they read. A texture pack made at install would fix load spikes only.
1a. Hold loading screens until queued shader preparation and pipelines are done.
2. Log real frame percentiles, ASTC per window, and the key of any wait over 100 ms.
3. A/B of ASTC as BC3 where textures stream, in chapter 12.
4. A measure of whether a render thread pays.
5. GPU time in combat, 16.4 against 10.3 ms. Reads from system memory are the first lead, which brings back VRAM step 3.
6. A packaged `.exe` for the launcher. The zip release is done.
7. Texture hitches, 100-270 ms at loads and camera cuts.
8. Cutscenes at 60: built, needs the user's play and a launcher setting.
9. FSR 2/3, DLSS and HDR only if time allows.

Also open: merging upstream, now 1,872 commits ahead of our base, with NVN as its only GPU path and a hook system (`docs/HOOKS.md` there). Hooks match exported symbols, and the game's main exports 25, none of the cutscene functions. Tested unchanged on the scripted fight: 25.2 FPS against our 58.5, about 4.9 against 2.3 GB of video memory, no crash, `diagnostics/upstream-20261102/README.md`. One scene, not studied further. Decision: no renderer merge now. **Hooks by address for upstream (2026-11-05, the user: "let's contribute ScarletNX"):** built in a worktree of upstream `56dbd88`, `D:\Project-B3PC\snx-addr-hooks`, branch `hook-by-address`, uncommitted. `[<nsobid>:<offset>] hook NAME(...)` lines in `src/nx/patches/*.hook` (Lizzie's invited format): the key stands for a symbol in the table and route list, so the recompiler routes the direct calls and NCE stubs the function. Our cutscenes at 60 ported as `src/nx/patches/bayonetta3.c` (Witch Time credited), off with `hooks_off=bayonetta3_cutscenes`. 50 hook tests pass. Bayonetta 3 recompiled with it (`D:\Project-B3PC\snx-addr-run`, 15 GB): all four hooks installed and routed, title and menus 36-41 FPS against 30.0 with the family off (upstream's renderer caps it below 60 here), picture fine. The user chose to send the mechanism only: the Bayonetta 3 patch and both runs' logs are kept in `diagnostics/scarletnx-hooks-20261105/`. Committed on the branch as `747a99d` (not pushed); full ctest 556 of 559, the three failures (`https` without curl, `fatal_screen`, `runtime_architecture`) the same on unmodified upstream. Pushed to the user's fork (LuvAbyss/ScarletNX, branch hook-by-address); PR text in `diagnostics/scarletnx-hooks-20261105/PR.md`, the user opens the PR. The project has a git repo since 2026-11-02, by allowlist in `.gitignore`: no game files, keys, saves, logs or captures. ScarletNX has agreed to the project on that condition. The launcher doesn't install `control.nacp`. A future ABI change shows ready and then stops at Play.

## Not tested

- BC3 up close on faces and metal, and movies.
- FSR 1 on Thule, Gomorrah, the kraken and full-screen movies.
- `mute_unfocused` and the cutscene audio fix, by ear.
- f5 on the laptop, or any AMD GPU. A CPU of 12 threads for the compile.
- Other chapters on f5. Tessellation and geometry stages aren't prepared (compute is since l10).
- A dump extracted by another tool.
