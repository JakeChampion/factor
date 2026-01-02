! WASM32 CPU skeleton.
! Provides minimal hooks so the compiler/backend can be wired without missing vocab.
USING: compiler.codegen.relocation compiler.constants kernel math system ;
IN: cpu.wasm32

SINGLETON: wasm32

HOOK: ds-reg cpu ( -- reg )
HOOK: rs-reg cpu ( -- reg )

! GC map support: wasm frames have no callee-saved registers; spill slots map
! directly to linear stack slots. Root 0 is the first spill slot at
! frame_top + FRAME_RETURN_ADDRESS (which is 0 for wasm).
M: wasm32 gc-root-offset n>> cell * ;

! Safepoint marker: emit relocation only (backend-specific code emission is
! handled in the wasm assembler).
M: wasm32 %safepoint ( gc-map -- )
    drop
    0 rc-absolute rel-safepoint ;
