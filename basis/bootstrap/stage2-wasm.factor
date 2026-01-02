! Copyright (C) 2025 Factor contributors.
! See https://factorcode.org/license.txt for BSD license.
! Minimal stage2 bootstrap for WASM - skips heavy components
USING: command-line compiler.units continuations definitions io
io.pathnames kernel math math.parser memory namespaces parser
parser.notes sequences sets splitting system
vocabs vocabs.loader ;
IN: bootstrap.stage2

SYMBOL: core-bootstrap-time

SYMBOL: bootstrap-time

: default-image-name ( -- string )
    vm-path file-name os windows? [ "." split1-last drop ] when
    ".image" append resource-path ;

: load-component ( name -- )
    dup "* Loading the " write write " component" print flush
    "bootstrap." prepend require ;

: load-components ( -- )
    "include" "exclude" [ get-global split-words harvest ] bi@ diff
    [ load-component ] each ;

: print-time ( us -- )
    1,000,000,000 /i
    60 /mod swap
    number>string write
    " minutes and " write number>string write " seconds." print ;

: print-report ( -- )
    "Core bootstrap completed in " write core-bootstrap-time get print-time
    "User bootstrap completed in " write bootstrap-time get print-time

    "Bootstrapping is complete." print
    "Now, you can run Factor:" print
    vm-path write " -i=" write "output-image" get print flush ;

! WASM-minimal components: skip compiler, threads, ui, tools, help
! Only load essential I/O and math
CONSTANT: wasm-components ""

[
    "WASM minimal bootstrap starting..." print flush
    nano-count

    f parser-quiet? set-global

    default-image-name "output-image" set-global

    ! Use minimal component set for WASM - empty means core only
    wasm-components "include" set-global
    "" "exclude" set-global

    (command-line) parse-command-line

    "WASM stage2: command line parsed" print flush

    ! Don't load any extra components for now
    ! load-components

    nano-count over - core-bootstrap-time set-global

    "WASM stage2: running bootstrap init..." print flush
    run-bootstrap-init

    nano-count swap - bootstrap-time set-global
    print-report

    "WASM stage2: running finish-bootstrap-wasm..." print flush
    "resource:basis/bootstrap/finish-bootstrap-wasm.factor" run-file

    "WASM stage2: bootstrap complete!" print flush
    
    ! Exit successfully - can't save image in WASM yet
    0 exit
] [
    drop
    "WASM stage2: fatal error during bootstrap" print flush
    1 exit
] recover
