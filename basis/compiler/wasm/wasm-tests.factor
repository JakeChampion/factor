! Simple guard to ensure wasm backend dispatch fails with the stub error.
USING: compiler compiler.wasm continuations kernel locals namespaces
system tools.test ;
IN: compiler.wasm.tests

! Define a wasm CPU class locally in case the running image predates the core addition.
SINGLETON: wasm.32
UNION: wasm wasm.32 ;

: wasm-backend-test-word ( -- ) 1 drop ;

:: with-temp-cpu ( class quot -- )
    \ cpu get-global :> old
    class \ cpu set-global
    [ quot call ] [ old \ cpu set-global ] [ old \ cpu set-global ] cleanup ;

[
    [
        [ \ wasm-backend-test-word compile-word ]
        [ wasm-backend-unimplemented? ] must-fail-with
    ] wasm.32 swap with-temp-cpu
] unit-test
