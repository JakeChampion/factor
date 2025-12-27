! Copyright (C) 2025 Factor contributors.
! See https://factorcode.org/license.txt for BSD license.
! WASM-specific finish-bootstrap - minimal setup for interpreted execution
USING: init io kernel namespaces sequences system vocabs ;
IN: bootstrap.finish-bootstrap-wasm

! WASM doesn't need:
! - Compiler optimization settings (no JIT)
! - Signal handlers (WASI doesn't have signals)
! - Threading setup (single-threaded)
! - Native library loading (no FFI)

: finish-wasm-bootstrap ( -- )
    "WASM finish-bootstrap: starting" print flush
    
    ! Set up basic I/O defaults
    ! (most I/O should already work from core)
    
    ! Clear any stale error state
    f error set-global
    f original-error set-global
    f error-continuation set-global
    
    "WASM finish-bootstrap: complete" print flush ;

! Run it
finish-wasm-bootstrap
