#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/pr-builds.sh --print against a fake gh that has a run's artifacts: the builds linked, the APK's halves and
edenprobe's report left out, the jobs that did not succeed named, and edenprobe's line, its result linked to the run,
never among them. Offline."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARTIFACTS = {
    'artifacts': [
        {'id': 2, 'name': 'ScarletNX-windows-x86_64-abc1234', 'size_in_bytes': 3 << 20, 'expired': False,
         'expires_at': '2026-11-01T00:00:00Z'},
        {'id': 1, 'name': 'ScarletNX-android-arm64-v8a-abc1234-stage', 'size_in_bytes': 1 << 20, 'expired': False,
         'expires_at': '2026-11-01T00:00:00Z'},
        {'id': 3, 'name': 'edenprobe-abc1234-1', 'size_in_bytes': 1 << 20, 'expired': False,
         'expires_at': '2026-11-01T00:00:00Z'},
    ]
}  # fmt: skip
RUN = 'https://github.com/owner/repo/actions/runs/7'
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


if not shutil.which('bash') or not shutil.which('jq'):
    print('pr-builds: skipped, it needs bash and jq')
    sys.exit(77)

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    gh = root / 'gh'
    gh.write_text(f'#!/bin/sh\n[ "$2" = "repos/owner/repo/actions/runs/7/artifacts?per_page=100" ] || exit 9\n'
                  f"echo '{json.dumps(ARTIFACTS)}'\n")  # fmt: skip
    gh.chmod(0o755)

    def comment(needs=None):
        env = dict(os.environ, PATH=f'{root}{os.pathsep}{os.environ["PATH"]}')
        env.pop('NEEDS', None)
        if needs is not None:
            env['NEEDS'] = json.dumps(needs)
        command = ['bash', str(ROOT / 'scripts' / 'pr-builds.sh'), 'owner/repo', '5', '7', 'abc1234def', '--print']
        done = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30, check=False)
        check('it prints the comment', done.returncode == 0, done.stderr)
        return done.stdout

    built = {'linux': {'result': 'success', 'outputs': {}}, 'windows': {'result': 'success', 'outputs': {}}}
    title = 'edenprobe a7f2beb: 906 passed, 20 failed, 3 skipped, 1 crashed'
    text = comment(built | {'edenprobe': {'result': 'success', 'outputs': {'result': title}}})
    check(
        "the builds linked, the halves of the APK and edenprobe's report left out",
        f'- [ScarletNX-windows-x86_64-abc1234]({RUN}/artifacts/2), 3 MiB' in text
        and '-stage' not in text
        and 'edenprobe-abc1234' not in text,
        text,
    )
    check('kept until the first to expire', 'Builds of abc1234, kept until 2026-11-01:' in text, text)
    check(
        "edenprobe's result linked to the run",
        f'\n\n[{title}]({RUN}): every case of the baseline passes\n' in text and 'Not built' not in text,
        text,
    )

    text = comment(
        built | {'windows': {'result': 'failure', 'outputs': {}}, 'edenprobe': {'result': 'failure', 'outputs': {}}}
    )
    check(
        'a failed edenprobe says so, without a result of its own',
        f'[edenprobe]({RUN}): failed, see its summary' in text,
    )
    check('a failed build named, edenprobe not among them', text.endswith('\n\nNot built: windows (failure)\n'), text)

    text = comment(built | {'edenprobe': {'result': 'skipped', 'outputs': {}}})
    check('a skipped edenprobe', text.endswith(f'\n\n[edenprobe]({RUN}): skipped\n') and 'Not built' not in text)

    text = comment()
    check('no NEEDS: the builds alone', 'edenprobe' not in text and 'Not built' not in text, text)

for failure in failures:
    print('FAIL:', failure)
print('pr-builds:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
