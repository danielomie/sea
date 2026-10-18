#!/bin/bash
# Copyright (c) 2026 Daniel
# SPDX-License-Identifier: BSD-3-Clause

# Build Sea and run the unit tests on aarch64, in an arm64 Docker container.
#
#   bash tests/arm64/run.sh [make targets...]     (default: test test-dwe test-seac)
#
# On an x86-64 host this needs qemu registered for arm64 binaries, once per
# boot:   docker run --privileged --rm tonistiigi/binfmt --install arm64
# (undo with --uninstall qemu-aarch64). Everything runs emulated, so expect
# it to be several times slower (per-test timeout: TEST_TIMEOUT, default 300 s);
# test-jit takes the longest.
set -euo pipefail
cd "$(dirname "$0")/../.."
targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=(test test-dwe test-seac)

docker build --platform linux/arm64 -q -t sea-arm64 tests/arm64 >/dev/null
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# the working tree as it is (tracked + new files), without build outputs
git ls-files -co --exclude-standard -z | tar --null -T - -cf "$tmp/src.tar"

docker run --rm --init --platform linux/arm64 -e TEST_TIMEOUT="${TEST_TIMEOUT:-300}" \
    -v "$tmp:/src:ro" sea-arm64 bash -c "
    set -e
    tar xf /src/src.tar
    uname -m
    make -s -j\$(nproc)
    # libs with a fetch.sh download their engine (duckdb, sqlite): not needed by the unit tests
    for l in libs/*/Makefile; do d=\$(dirname \$l); [ -f \$d/fetch.sh ] && continue; make -s -C \$d >/dev/null; done
    for t in ${targets[*]}; do echo \"== \$t\"; make -s \$t; done
"
