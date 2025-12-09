! Copyright (C) 2025 The Factor Project.
! Stub entry point for a future WASM/WASI backend.
USING: kernel sequences system ;
IN: compiler.wasm

ERROR: wasm-backend-unimplemented word ;

: wasm-target? ( -- ? )
    cpu [ name>> "wasm" head? ] [ f ] if* ;

: wasm-backend ( tree word -- * )
    ! Placeholder: stop early with a clear error until codegen/runtime work lands.
    nip wasm-backend-unimplemented ;
