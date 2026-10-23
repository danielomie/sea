#!/usr/bin/env bash
# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

# Download llama.cpp (the inference engine: GGUF models on the CPU) and build
# its shared libraries into vendor/ (include/, lib/). The release and the
# SHA-256 of its source archive are pinned here. Needs cmake and a C++
# compiler; the build is tuned for this machine's CPU (GGML_NATIVE).
set -euo pipefail

VERSION=b11119
SHA=5c74908cda2ab8a09aa466c99ab51af9554ff80a023b9702a69902da86b5be69
cd "$(dirname "$0")"

command -v cmake >/dev/null || { echo "fetch.sh: cmake is needed to build llama.cpp" >&2; exit 1; }

TGZ="llama.cpp-$VERSION.tar.gz"
URL="https://github.com/ggml-org/llama.cpp/archive/refs/tags/$VERSION.tar.gz"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "fetching llama.cpp $VERSION..."
if command -v curl >/dev/null; then curl -fsSL -o "$TMP/$TGZ" "$URL"
else wget -q -O "$TMP/$TGZ" "$URL"; fi
echo "$SHA  $TMP/$TGZ" | sha256sum -c --quiet - || { echo "fetch.sh: checksum mismatch" >&2; exit 1; }
tar -xzf "$TMP/$TGZ" -C "$TMP"

echo "building llama.cpp $VERSION (a few minutes)..."
rm -rf vendor
cmake -S "$TMP/llama.cpp-$VERSION" -B "$TMP/build" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX="$(pwd)/vendor" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_RPATH='$ORIGIN' \
    -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_TOOLS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_APP=OFF \
    -DLLAMA_OPENSSL=OFF >"$TMP/cmake.log" 2>&1 || { tail -30 "$TMP/cmake.log" >&2; exit 1; }
cmake --build "$TMP/build" -j"$(nproc)" >"$TMP/build.log" 2>&1 || { tail -30 "$TMP/build.log" >&2; exit 1; }
cmake --install "$TMP/build" >/dev/null
echo "$VERSION" > vendor/VERSION
echo "llama.cpp $VERSION in $(pwd)/vendor"
