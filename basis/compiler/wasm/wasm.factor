! Copyright (C) 2025 The Factor Project.
! Stub entry point for a future WASM/WASI backend.
USING: kernel system ;
IN: compiler.wasm

ERROR: wasm-backend-unimplemented word ;

: wasm-backend ( tree word -- * )
    ! Placeholder: stop early with a clear error until codegen/runtime work lands.
    nip wasm-backend-unimplemented ;
