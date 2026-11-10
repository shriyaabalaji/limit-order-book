#!/usr/bin/env bash
# Compile the C++ order book to WebAssembly for the browser playground.
# Requires Emscripten (brew install emscripten). Output: web/dist/lob.js + lob.wasm
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p dist
em++ -O2 -std=c++20 -I ../include bindings.cpp \
  -lembind \
  -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=createLob \
  -s ENVIRONMENT=web -s ALLOW_MEMORY_GROWTH=1 \
  -o dist/lob.js
echo "built web/dist/lob.js + lob.wasm"
