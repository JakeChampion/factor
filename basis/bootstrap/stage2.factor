! Copyright (C) 2004, 2008 Slava Pestov.
! See https://factorcode.org/license.txt for BSD license.
USING: command-line compiler.units continuations definitions io
io.pathnames kernel math math.parser memory namespaces parser
parser.notes sequences sets splitting system
vocabs vocabs.loader ;
IN: bootstrap.stage2

SYMBOL: core-bootstrap-time

SYMBOL: bootstrap-time

CONSTANT: stage2-log-path "stage2.log"

: log-stage2 ( string -- )
    [
        stage2-log-path utf8 <file-appender> dup [
            [ print flush ] with-output-stream*
        ] [ close-stream ] bi
    ] with-scope ;

: strip-encodings ( -- )
    os unix? [
        [
            P" resource:core/io/encodings/utf16/utf16.factor" forget
            "io.encodings.utf16" loaded-child-vocab-names [ forget-vocab ] each
        ] with-compilation-unit
    ] when ;

: default-image-name ( -- string )
    vm-path file-name os windows? [ "." split1-last drop ] when
    ".image" append resource-path ;

: load-component ( name -- )
    dup "* Loading the " write write " component" print
    dup [ "* stage2 loading " prepend log-stage2 ] when
    "bootstrap." prepend require ;

: load-components ( -- )
    "include" "exclude" [ get-global split-words harvest ] bi@ diff
    [ dup "* stage2 loading " write print load-component ] each ;

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

: save/restore-error ( quot -- )
    error get-global
    original-error get-global
    error-continuation get-global
    [ call ] 3dip
    error-continuation set-global
    original-error set-global
    error set-global ; inline

CONSTANT: default-components
    "math compiler threads io tools ui ui.tools unicode help handbook"

[
    "stage2: entry" log-stage2
    ! We time bootstrap
    nano-count

    ! parser.notes sets this to t in the global namespace.
    ! We have to change it back in finish-bootstrap.factor
    f parser-quiet? set-global

    default-image-name "output-image" set-global

    default-components "include" set-global
    "" "exclude" set-global

    strip-encodings

    "stage2: after strip-encodings" log-stage2

    (command-line) parse-command-line

    "stage2: after parse-command-line" log-stage2

    ! Set dll paths
    os windows? [ "windows" require ] when

    "staging" get [
        "stage2: deployment mode" print
        "stage2: deployment mode" log-stage2
    ] [
        "debugger" require
        "listener" require
        "stage2: listener/debugger loaded" log-stage2
    ] if

    load-components
    "stage2: after load-components" log-stage2

    nano-count over - core-bootstrap-time set-global

    "stage2: before run-bootstrap-init" log-stage2
    run-bootstrap-init
    "stage2: after run-bootstrap-init" log-stage2
    "* stage2: startup quot = " write OBJ-STARTUP-QUOT special-objects get . flush
    "stage2: before c_to_factor_toplevel" log-stage2
    [
        "stage2: entering startup quot" log-stage2
        c-to-factor
        "stage2: returned from startup quot" log-stage2
    ] [ "stage2: error in startup quot" log-stage2 ] recover

    nano-count swap - bootstrap-time set-global
    print-report
    "stage2: after print-report" log-stage2

    "staging" get [
        "resource:basis/bootstrap/finish-staging.factor" run-file
    ] [
        "resource:basis/bootstrap/finish-bootstrap.factor" run-file
        "stage2: after finish-bootstrap" log-stage2
    ] if

    f error set-global
    f original-error set-global
    f error-continuation set-global
    "output-image" get save-image-and-exit
] [
    drop
    [
        load-help? off
        [ "resource:basis/bootstrap/bootstrap-error.factor" parse-file ] save/restore-error
        call
    ] with-scope
] recover
