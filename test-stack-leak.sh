#!/bin/bash
# Test to identify stack leak by monitoring depth changes

export FACTOR_WORD_COUNTER=1

# Run with stack leak detection
timeout 10 wasmtime run -W max-wasm-stack=4000000000 --dir ./ -S inherit-env \
  factor.wasm -i=boot.wasi-wasm32.image -datastack=4 -run=none 2>&1 | \
  grep -E "NEW MAX datastack|datastack overflow|fatal_error" | \
  tail -50
