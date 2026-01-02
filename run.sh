#!/usr/bin/env bash

FACTOR_WORD_COUNTER=1 wasmtime run -W max-wasm-stack=100000000 --dir ./ -S inherit-env factor.wasm -i=boot.unix-x86.32.image -run=none 2>&1