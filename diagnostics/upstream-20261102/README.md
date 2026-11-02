# Latest ScarletNX on Bayonetta 3 (2026-11-02)

Upstream `269ce0d`, 1,872 commits past our base `5c97949`, NVN as its only GPU path. Built with our toolchain in `D:\Project-B3PC\snx-upstream-test`, outside the repo. One change: the `script` setting's limit, 4096 to 16384, to accept the route. Default settings otherwise, `resolution=1.6`, `vsync=1`, module O1 with our hints. Same PC, same scripted fight, a copy of the isolated save. Logs in `logs/` here.

| scripted fight, from frame 3,200-3,600 on | this port, c2 | upstream, warm run |
|---|---|---|
| mean FPS | **58.5** | 25.2 (5 s windows, 20.0 to 56.2) |
| 12,000 frames reached in 420 s | yes, in about 227 s | no, 11,016 |
| longest frame | under 162 ms from frame 3,600 on | 427 ms, the longest its log names in the whole run |
| game's video memory | **2,264 MiB** | about 4.9 GB |

The first run, on empty caches, was the same: 22-26 FPS, longest frame 1,572 ms, 6.5 s of pipeline waits. The picture and the route looked right, and there was no crash.

Its GPU use after the first minute averaged 47%, 126 W, at 25 FPS, which suggests the limit isn't the GPU, but that wasn't measured further. **This is one scripted scene and its logs were not studied, so it says nothing about why, or about their other games.** Its NVN-only path is days old and may not be tuned for this game yet, and the team may know settings that help.

## Build notes for upstream on Windows here

- OpenSSL needs the MSYS Perl (`PERL_EXECUTABLE` = `deps/posix-tools/usr/bin/perl.exe`).
- FFmpeg's `make install` left `include/libavutil` empty: the install line was cut off, likely by its length, and Ninja went on. Copying the headers from `_deps/ffmpeg-patched/libavutil` and `ffmpeg-build-patched/libavutil` fixed it.
- The game's data folder must hold real files: junctions and hard links are refused (`tree.c`, `FS_TREE_INVALID`, error 4).

## Decision

No merge of their renderer now. Hooks and pchtxt can be brought over later on their own, if they pay.
