# Game code compile and AMD shader preparation speed (2026-11-16)

Ryzen 7 5700X3D (8 cores, 16 threads), RX 6700 XT. Tools here: `compile.py` (the launcher's compile command in a
scratch folder, timed), `e2e.py` (the launcher's own `build_code` twice on a scratch game folder), `prep.py` (a new
AMD player's preparation pass through `bench.py`, AMD's `VkCache` set aside and put back).

## Game code compile

| run | wall |
|---|---|
| installed runtime, 14 jobs | 207.1 s |
| 8 jobs | 274.3 s |
| 16 jobs | 191.6-191.7 s |
| `--cache`, empty, 14 / 16 jobs | 200.6 / 191.6 s |
| `--cache`, full, 14 / 16 jobs | 14.1 / 13.6 s |
| launcher `build_code`, new player / after an update | 188.3 / 13.0 s |

Every DLL hashes `f13c034c...`, cached or not. 82 blocks, 331 MB of cache. The analysis takes about 1 s; the rest is
LLVM on 82 blocks of about 90,000 guest instructions (2,600 s of block time). One block alone (code_054, 56 s in a
14-job run) is 15.6 s of passes, half of it InstCombine, and 14.7 s of codegen, half of it instruction selection,
single-threaded: with 14 jobs each thread runs at about half speed. The IR is about 15 instructions a guest
instruction, mostly getelementptr, load and store on the CPU state.

Not done:
- Ordering blocks: instructions per function predict a block's time poorly (correlation 0.39). Simulated on the
  measured times, that order gives 204 s against 203 s; the best possible order 188 s.
- A lighter pass pipeline or a leaner IR emitter: changes the game code, needs fight benchmarks.

## AMD shader preparation (new player)

| run | pass |
|---|---|
| 14 workers | 103.8, 108.3 s |
| 8 workers | 113.5 s |
| 16 workers | 108.8 s |
| 14 workers, libraries made without the pipeline cache (test switch, removed) | 109.4 s |

190-300 ms a stage, of which 0.7 ms translating and 10-17 ms waiting for `vk_lock` (the progress line now says so).
The CPU is 55-69 % busy. The time is AMD's compiler, which doesn't scale past about 8 threads here; nothing on the
runtime's side moved it.
