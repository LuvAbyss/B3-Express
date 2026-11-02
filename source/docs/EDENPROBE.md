<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# edenprobe

[edenprobe](https://github.com/scarletnx/edenprobe) is a homebrew program of hardware probes: suites of cases, each
asking the machine one question and judging the answer against the console's. A run leaves `report.jsonl`, a line per
case (`pass`, `fail` or `skip`, with a detail) and a summary. `scripts/edenprobe.py` recompiles it, runs it headless and
holds the report against a baseline, the cases that passed before: a case of the baseline that no longer passes fails.

## Running it

```sh
python3 scripts/edenprobe.py run --runtime build/scarletnx --nro edenprobe.nro --out /tmp/edenprobe \
    --mode "quick notime fatal cpu graphics fs video display kernel audio" \
    [--baseline tests/edenprobe/linux-x86_64.txt]
```

`--out`, empty or an earlier run's, is emptied, then gets the recompiled game and its XDG folders (`work/`, so
nothing of yours is read or written), `report.jsonl`, the failures' pictures (`images/`), the recompile's and each
launch's log, `summary.md` and `baseline.txt`: this run's passing cases. A launch that ends before the summary, crashed
or killed after `--timeout` seconds (600) without a new line in the report, is followed by another, which goes on after
the case it ended inside, reported as `crashed`. The baseline is lavapipe's: a GPU passes other cases, so on one the
run says what differs rather than what regressed.

## The mode words

Those of the example, which CI runs; edenprobe's
[mode file](https://github.com/scarletnx/edenprobe/blob/main/docs/ModeFile.md) has them all. The judge's suites
(selfcheck, support, reference) run whatever is named:

| word | what it adds |
|---|---|
| `quick` | the run with no one in front of it: every suite named, the report, exit |
| `notime` | each drawing suite once, no timing passes, no times in the report |
| `fatal` | the cases that may end the program: SM4, CNTVCT_EL0 and DAIF reads, geometry shaders, BC4 arrays |
| `cpu` | the processor's instructions |
| `graphics` | the drawing suites: clears, blending, depth, textures and their formats, queries, blits |
| `fs` | the memory card: a 64 MiB file and 256 small ones, written once and read back |
| `video` | the video image compositor, and the VP9 decoder on libvpx's test vectors |
| `display` | a layer's buffer history |
| `kernel` | the scheduler: priority inheritance, preemption, migration |
| `audio` | the audio renderer: resampler, circular buffer, limiter; headless, nothing is played |

Left out: `net` (servers the runner does not have), `pad` and `listen` (a person), `stress` (256 MiB lists), `hang`
(cases that never return on a machine that gets them wrong).

## In CI

The `edenprobe` job of `.github/workflows/build.yml` takes the Linux x86-64 install and runs the release
`EDENPROBE_VERSION` of edenprobe, pinned by `EDENPROBE_SHA256`, on lavapipe with the words of `EDENPROBE_MODE`, against
`tests/edenprobe/linux-x86_64.txt`. The result goes in the run's summary and in the pull request's comment of builds;
the artifact `edenprobe-<commit>-<attempt>` holds the report, pictures, logs and `baseline.txt`.

A regression there fails the Build run, and a run that failed publishes nothing: neither the nightly nor a version.

Then the job runs edenprobe's main, its `nightly` release (checked by its `.sha256`), with
`scripts/edenprobe.py run --advisory edenprobe/report.jsonl`: beside the pinned release's report it never fails. A case
the release has not got that does not pass, a case the release passes that does not pass here, a run that ends early, or
a nightly that cannot be fetched is a warning on the run, and the summary has a section of its own. A case new in main
becomes binding when a release that has it is pinned and the baseline written again.

## The baseline

`tests/edenprobe/<platform>.txt` names the edenprobe build and the mode words it was written with, then one passing
case a line, `suite: name`. Another build or other words fail the job until it is written again; a case that starts to
pass is listed, to be added. Write it from CI's own runs, never a desktop GPU's: download the artifact of two or three
runs (run the job again for more) and keep what each passed, so a case that passes now and then stays out:

```sh
python3 scripts/edenprobe.py baseline run1/baseline.txt run2/baseline.txt -o tests/edenprobe/linux-x86_64.txt
```
