! Copyright (C) 2004, 2009 Slava Pestov.
! See https://factorcode.org/license.txt for BSD license.
USING: assocs bootstrap.image.private hash-sets hashtables init
io io.files kernel kernel.private make memory namespaces parser
parser.notes sequences system vocabs vocabs.loader ;

"Bootstrap stage 1 (wasm tiny)..." print flush

"resource:basis/bootstrap/primitives.factor" run-file

load-help? off
{ "resource:core" } vocab-roots set

! Create a boot quotation for the target by collecting all top-level
! forms into a quotation, surrounded by some boilerplate.
[
    [
        ! WASM WORKAROUND: Skip hashtable rehashing due to no-math-method error
        ! The instances/filter operation triggers generic math dispatch issues
        ! This is safe because WASM uses the same hashing as the host
        ! TODO: Fix generic.math dispatch for WASM and re-enable rehashing
        boot
    ] %

    "math.integers" require
    "math.ratios" require
    "math.floats" require
    "memory" require

    "io.streams.c" require
    "io.streams.byte-array" require ! for utf16 on Windows
    "vocabs.loader" require

    "syntax" require

    "locals" require
    "locals.fry" require
    "locals.macros" require

    "resource:basis/bootstrap/layouts.factor" parse-file %

    [
        f parser-quiet? set-global

        ! WASM: Skip init-resource-path to avoid vector creation issue
        ! Set resource-path directly instead
        "." "resource-path" set-global

        "resource:basis/bootstrap/stage2-wasm-tiny.factor"
        dup file-exists? [
            run-file
        ] [
            "Cannot find " write write "." print
            "Please move " write image-path write " into the same directory as the Factor sources," print
            "and try again." print
            1 (exit)
        ] if
    ] %
] [ ] make
OBJ-STARTUP-QUOT
bootstrap.image.private:special-objects get set-at
