#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Formats the C and C++ sources with clang-format 23 and the Python sources with ruff (through uvx, pinned below).
#
#   scripts/format.sh            format the repository
#   scripts/format.sh --check    list what is not formatted, exit 1 if anything is
#
# Formatted: the C and C++ files under src/, tools/ and tests/, and the Python files up to two folders deep plus the
# Claude Code hooks. Left alone: third-party code (.cache/cpm).
set -euo pipefail

CLANG_FORMAT_MAJOR=23
RUFF=ruff@0.17.0

repo=$(cd "$(dirname "$0")/.." && pwd)
check=0
for arg in "$@"; do
    case $arg in
        --check) check=1 ;;
        *) echo "usage: $0 [--check]" >&2; exit 2 ;;
    esac
done

if ! command -v clang-format >/dev/null; then
    echo "clang-format $CLANG_FORMAT_MAJOR is required and is not on PATH" >&2
    exit 1
fi
version=$(clang-format --version | sed -n 's/.*version \([0-9]*\)\..*/\1/p')
if [ "$version" != "$CLANG_FORMAT_MAJOR" ]; then
    echo "clang-format $CLANG_FORMAT_MAJOR is required (found ${version:-none}): another version formats differently" >&2
    exit 1
fi

cd "$repo"
mapfile -t c_files < <(find src tools tests -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) | sort)
mapfile -t py_files < <( (find . -maxdepth 2 -name '*.py' -not -path './third_party/*' -not -path './.cache/*' \
    -not -path './build*'; find .claude/hooks -name '*.py') | sort)

# cksum is POSIX (sha1sum is missing on FreeBSD); it only has to tell whether a pass changed anything
digest() { cat "${c_files[@]}" | cksum; }

status=0
if [ $check = 1 ]; then
    for f in "${c_files[@]}"; do
        clang-format --style="file:$repo/.clang-format" --dry-run -Werror "$f" 2>/dev/null || { echo "$f"; status=1; }
    done
    if [ ${#py_files[@]} -gt 0 ]; then
        uvx -q "$RUFF" format --no-cache --config "$repo/pyproject.toml" --check "${py_files[@]}" || status=1
    fi
    [ $status = 0 ] || echo "Not formatted: run scripts/format.sh" >&2
else
    # clang-format is not idempotent here (trailing comments after a closing brace realign on a second pass), so
    # repeat until nothing changes: the result is then stable, and --check agrees with it. With no files, cat and
    # clang-format would read stdin and ruff would format the whole folder: skip them
    if [ ${#c_files[@]} -gt 0 ]; then
        for pass in 1 2 3 4 5; do
            before=$(digest)
            clang-format --style="file:$repo/.clang-format" -i "${c_files[@]}"
            [ "$(digest)" = "$before" ] && break
            if [ $pass = 5 ]; then
                echo "clang-format did not settle after 5 passes" >&2
                exit 1
            fi
        done
    fi
    if [ ${#py_files[@]} -gt 0 ]; then
        uvx -q "$RUFF" format --no-cache --config "$repo/pyproject.toml" -q "${py_files[@]}"
    fi
fi
exit $status
