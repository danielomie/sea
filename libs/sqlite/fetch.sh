#!/usr/bin/env bash
# Copyright (c) 2026 Daniel
# SPDX-License-Identifier: BSD-3-Clause

# Download the SQLite amalgamation (sqlite3.c + sqlite3.h) into vendor/.
# The version and the SHA-256 of the release archive are pinned here (the
# archive's sqlite3.c matches the SHA3-256 in SQLite's release log).
set -euo pipefail

VERSION=3530400          # 3.53.4 (2026-07-24)
YEAR=2026
SHA=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
cd "$(dirname "$0")"

ZIP="sqlite-amalgamation-$VERSION.zip"
URL="https://www.sqlite.org/$YEAR/$ZIP"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "fetching SQLite $VERSION..."
if command -v curl >/dev/null; then curl -fsSL -o "$TMP/$ZIP" "$URL"
else wget -q -O "$TMP/$ZIP" "$URL"; fi

echo "$SHA  $TMP/$ZIP" | sha256sum -c --quiet - || { echo "fetch.sh: checksum mismatch" >&2; exit 1; }

mkdir -p vendor
unzip -o -q -j "$TMP/$ZIP" "sqlite-amalgamation-$VERSION/sqlite3.c" "sqlite-amalgamation-$VERSION/sqlite3.h" -d vendor
echo "$VERSION" > vendor/VERSION
echo "SQLite $VERSION in $(pwd)/vendor"
