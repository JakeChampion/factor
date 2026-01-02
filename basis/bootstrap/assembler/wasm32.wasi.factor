! WASI/WASM32 assembler backend - loads wasm32 assembler
! This file exists because architecture "wasi-wasm32" maps to file
! "wasm32.wasi.factor" via the asm-file naming convention.
USING: kernel parser sequences ;
IN: bootstrap.assembler.wasm32

<< "resource:basis/bootstrap/assembler/wasm32.factor" parse-file suffix! >> call
