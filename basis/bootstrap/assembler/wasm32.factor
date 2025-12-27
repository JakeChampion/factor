! WASM32 assembler backend skeleton.
! Real instruction selection/encoding is being built; this file now contains
! a minimal emitter layer to encode wasm instructions into a byte-array buffer.
USING: bootstrap.image.private combinators compiler.units
compiler.codegen.relocation compiler.constants
kernel kernel.private layouts math make namespaces sequences ;
IN: bootstrap.assembler.wasm32

! 32-bit cells for wasm32
4 \ cell set

ERROR: wasm32-assembler-unimplemented word ;

! Fixnum bounds for 32-bit cells (untagged)
CONSTANT: wasm-fixnum-min -134217728
CONSTANT: wasm-fixnum-max  134217727

! Relocation helpers: reuse existing relocation machinery with absolute class.
: wasm-relocate-entry ( obj -- ) [ add-literal ] dip rc-absolute rt-entry-point add-relocation ;
: wasm-relocate-literal ( obj -- ) [ add-literal ] dip rc-absolute rt-literal add-relocation ;

! ------------------------------------------------------------------------
! Minimal wasm encoder
! ------------------------------------------------------------------------

! Opcodes we need first (numerical values per WebAssembly binary format)
CONSTANT: wasm-i32.const 0x41
CONSTANT: wasm-i32.add   0x6A
CONSTANT: wasm-i32.sub   0x6B
CONSTANT: wasm-i32.mul   0x6C
CONSTANT: wasm-i32.shl   0x74
CONSTANT: wasm-i32.shr_s 0x75
CONSTANT: wasm-i32.or    0x72
CONSTANT: wasm-i32.eq    0x46
CONSTANT: wasm-i32.lt_s  0x48
CONSTANT: wasm-i32.gt_s  0x4A
CONSTANT: wasm-i32.le_s  0x4C
CONSTANT: wasm-i32.ge_s  0x4E
CONSTANT: wasm-local.get 0x20
CONSTANT: wasm-local.set 0x21
CONSTANT: wasm-return    0x0F
CONSTANT: wasm-call      0x10
CONSTANT: wasm-br        0x0C
CONSTANT: wasm-br_if     0x0D
CONSTANT: wasm-end       0x0B
CONSTANT: wasm-unreachable 0x00
CONSTANT: wasm-nop       0x01
CONSTANT: wasm-if        0x04
CONSTANT: wasm-else      0x05
CONSTANT: wasm-blocktype-empty 0x40
CONSTANT: wasm-i32.load  0x28
CONSTANT: wasm-i32.store 0x36

! Basic byte emission helpers
: u8, ( n -- ) , ;

: leb128-u32, ( n -- )
    [
        dup 0x80 u>=
    ] [
        dup 0x7F bitand 0x80 bitor u8,
        -7 shift
    ] while
    u8, ;

: wasm-emit ( opcode -- ) u8, ;

: wasm-emit-i32.const ( n -- )
    wasm-i32.const wasm-emit
    leb128-u32, ;

: wasm-emit-br ( depth -- )
    wasm-br wasm-emit
    leb128-u32, ;

: wasm-emit-local.get ( idx -- )
    wasm-local.get wasm-emit
    leb128-u32, ;

: wasm-emit-local.set ( idx -- )
    wasm-local.set wasm-emit
    leb128-u32, ;

: wasm-emit-call ( funcidx -- )
    wasm-call wasm-emit
    leb128-u32, ;

: wasm-emit-binop ( opcode -- ) wasm-emit ;

: wasm-emit-i32.load ( align offset -- )
    wasm-i32.load wasm-emit
    leb128-u32, leb128-u32, ;

: wasm-emit-i32.store ( align offset -- )
    wasm-i32.store wasm-emit
    leb128-u32, leb128-u32, ;

! ------------------------------------------------------------------------
! Stack frame parameters (needed early for helpers)
: stack-frame-size ( -- n )
    ! locals: vm, ctx, first spill slot
    3 bootstrap-cells ;                 ! 3 * 4 bytes = 12 bytes for now
: frame-vm-slot ( -- n ) 0 ;
: frame-ctx-slot ( -- n ) 1 ;
: frame-spill-base ( -- n ) 2 ;

! ------------------------------------------------------------------------
! Call/return helpers

: wasm-emit-return ( -- ) wasm-return wasm-emit ;

: wasm-emit-br-if ( depth -- )
    wasm-br_if wasm-emit
    leb128-u32, ;

! Emit a call to a helper identified by a relocation slot.
: wasm-emit-helper-call ( slot -- )
    ! Add relocation for helper entry point and emit placeholder index 0.
    dup wasm-relocate-entry drop
    0 wasm-emit-call ;

: wasm-emit-helper-call+return ( slot -- )
    wasm-emit-helper-call
    wasm-emit-return ;

! ------------------------------------------------------------------------
! Literals and GC maps (placeholders)

: wasm-emit-literal ( idx -- )
    ! Emit a literal reference with relocation; placeholder value 0.
    dup wasm-relocate-literal drop
    0 wasm-emit-i32.const ;

: >wasm-gc-map ( obj -- gc-map/f )
    ! During bootstrap, GC maps aren't available yet.
    ! Just return f to skip GC map recording.
    drop f ;

: wasm-emit-gc-map ( gc-map/f -- )
    ! Record GC map for the current safepoint so emit-gc-maps can serialize it later.
    ! During bootstrap, this is a no-op.
    drop ;

: wasm-default-gc-map ( -- gc-map )
    f ;

: wasm-emit-safepoint ( -- )
    0 wasm-emit-i32.const
    rc-absolute rel-safepoint
    wasm-nop wasm-emit ;

! ------------------------------------------------------------------------
! Tagged fixnum helpers (placeholder implementations)
! Tagged fixnum: low 2 bits are tag; shift right by 2 for untagged value.
! These emit untagged i32 ops; overflow handling still TODO.

: wasm-emit-untag ( -- )
    ! assumes top of stack is a tagged fixnum; shr by 2
    2 wasm-emit-i32.const
    wasm-i32.shr_s wasm-emit ;

: wasm-emit-retag ( -- )
    ! assumes top of stack is untagged fixnum; shl by 2
    2 wasm-emit-i32.const
    wasm-i32.shl wasm-emit ;

: wasm-emit-fixnum-add ( -- )
    ! Stack: ... a b (tagged)
    wasm-emit-untag
    wasm-emit-untag
    wasm-i32.add wasm-emit
    wasm-emit-retag
    ! TODO: overflow detection and fallback
    ;

: wasm-emit-fixnum-sub ( -- )
    wasm-emit-untag
    wasm-emit-untag
    wasm-i32.sub wasm-emit
    wasm-emit-retag
    ;

: wasm-emit-fixnum-mul ( -- )
    wasm-emit-untag
    wasm-emit-untag
    wasm-i32.mul wasm-emit
    wasm-emit-retag
    ;

: wasm-emit-fixnum-shl ( -- )
    wasm-emit-untag
    wasm-emit-untag
    wasm-i32.shl wasm-emit
    wasm-emit-retag
    ;
: wasm-emit-fixnum-shr ( -- )
    wasm-emit-untag
    wasm-emit-untag
    wasm-i32.shr_s wasm-emit
    wasm-emit-retag
    ;

: wasm-emit-fixnum-lt ( -- ) wasm-i32.lt_s wasm-emit ;
: wasm-emit-fixnum-gt ( -- ) wasm-i32.gt_s wasm-emit ;
: wasm-emit-fixnum-le ( -- ) wasm-i32.le_s wasm-emit ;
: wasm-emit-fixnum-ge ( -- ) wasm-i32.ge_s wasm-emit ;

! Overflow helpers: compare against fixnum bounds (tag-aware) and branch to
! a slow path if needed. Placeholder: just emit raw op for now.
: wasm-emit-fixnum-overflow-guard ( -- )
    ! Assumes untagged result is on top of stack.
    frame-spill-base wasm-emit-local.set       ! spill result to local2
    frame-spill-base wasm-emit-local.get
    wasm-fixnum-min wasm-emit-i32.const
    wasm-i32.lt_s wasm-emit                    ! underflow?
    frame-spill-base wasm-emit-local.get
    wasm-fixnum-max wasm-emit-i32.const
    wasm-i32.gt_s wasm-emit                    ! overflow?
    wasm-i32.or wasm-emit                      ! combined condition
    wasm-if wasm-emit wasm-blocktype-empty wasm-emit
    wasm-unreachable wasm-emit                 ! TODO: call runtime helper
    wasm-end wasm-emit ;

: wasm-emit-fixnum-add-checked ( -- )
    wasm-emit-fixnum-add
    ! TODO: overflow guard and slow path call
    ;

: wasm-emit-fixnum-sub-checked ( -- )
    wasm-emit-fixnum-sub
    ! TODO: overflow guard and slow path call
    ;

: wasm-emit-fixnum-mul-checked ( -- )
    wasm-emit-fixnum-mul
    ! TODO: overflow guard and slow path call
    ;

! ------------------------------------------------------------------------
! Additional stack frame / register placeholders
: shift-arg ( -- ) wasm32-assembler-unimplemented ;
: div-arg ( -- ) wasm32-assembler-unimplemented ;
: mod-arg ( -- ) wasm32-assembler-unimplemented ;
: temp0 ( -- ) wasm32-assembler-unimplemented ;
: temp1 ( -- ) wasm32-assembler-unimplemented ;
: temp2 ( -- ) wasm32-assembler-unimplemented ;
: temp3 ( -- ) wasm32-assembler-unimplemented ;
: pic-tail-reg ( -- ) wasm32-assembler-unimplemented ;
: return-reg ( -- ) wasm32-assembler-unimplemented ;
: stack-reg ( -- ) wasm32-assembler-unimplemented ;
: frame-reg ( -- ) wasm32-assembler-unimplemented ;
: vm-reg ( -- ) wasm32-assembler-unimplemented ;
: ctx-reg ( -- ) wasm32-assembler-unimplemented ;
: nv-regs ( -- seq ) { } ;              ! wasm has no GPR set; locals only
: volatile-regs ( -- seq ) { } ;
: nv-reg ( -- ) wasm32-assembler-unimplemented ;
: ds-reg ( -- ) wasm32-assembler-unimplemented ;
: rs-reg ( -- ) wasm32-assembler-unimplemented ;
: fixnum>slot@ ( -- ) wasm32-assembler-unimplemented ;
: rex-length ( -- n ) 0 ;
: red-zone-size ( -- n ) 0 ;

: ctx-ds-offset ( -- n ) context-datastack-offset ;
: ctx-rs-offset ( -- n ) context-retainstack-offset ;

: wasm-emit-load-ds ( -- )
    ! load ctx->datastack into stack top
    frame-ctx-slot wasm-emit-local.get
    2 ctx-ds-offset wasm-emit-i32.load ; ! align=4 bytes

: wasm-emit-store-ds ( -- )
    ! store top into ctx->datastack
    frame-ctx-slot wasm-emit-local.get
    2 ctx-ds-offset wasm-emit-i32.store ; ! value must be below address on stack

: wasm-emit-load-rs ( -- )
    frame-ctx-slot wasm-emit-local.get
    2 ctx-rs-offset wasm-emit-i32.load ;

: wasm-emit-store-rs ( -- )
    frame-ctx-slot wasm-emit-local.get
    2 ctx-rs-offset wasm-emit-i32.store ;

: wasm-emit-ds-push ( -- )
    ! Stack: ... value
    frame-spill-base wasm-emit-local.set       ! spill value to local2
    frame-ctx-slot wasm-emit-local.get         ! ctx
    2 ctx-ds-offset wasm-emit-i32.load         ! ds
    frame-spill-base wasm-emit-local.get       ! ds value
    2 0 wasm-emit-i32.store                    ! *ds = value
    frame-ctx-slot wasm-emit-local.get         ! ctx
    2 ctx-ds-offset wasm-emit-i32.load         ! ds
    cell wasm-emit-i32.const
    wasm-i32.add wasm-emit                     ! ds+cell
    frame-ctx-slot wasm-emit-local.get         ! ctx (addr)
    2 ctx-ds-offset wasm-emit-i32.store ;      ! ctx->ds = ds+cell

: wasm-emit-ds-pop ( -- )
    ! Stack: ... -> push popped value
    frame-ctx-slot wasm-emit-local.get         ! ctx
    2 ctx-ds-offset wasm-emit-i32.load         ! ds
    cell wasm-emit-i32.const
    wasm-i32.sub wasm-emit                     ! new ds
    frame-spill-base wasm-emit-local.set       ! save new ds in local2
    frame-ctx-slot wasm-emit-local.get         ! ctx
    frame-spill-base wasm-emit-local.get       ! ctx newds
    2 ctx-ds-offset wasm-emit-i32.store        ! ctx->ds = new ds
    frame-spill-base wasm-emit-local.get       ! new ds
    2 0 wasm-emit-i32.load ;                   ! load value from ds

: wasm-emit-rs-push ( -- )
    frame-spill-base wasm-emit-local.set       ! spill value to local2
    frame-ctx-slot wasm-emit-local.get         ! ctx
    2 ctx-rs-offset wasm-emit-i32.load         ! rs
    frame-spill-base wasm-emit-local.get       ! rs value
    2 0 wasm-emit-i32.store                    ! *rs = value
    frame-ctx-slot wasm-emit-local.get         ! ctx
    2 ctx-rs-offset wasm-emit-i32.load         ! rs
    cell wasm-emit-i32.const
    wasm-i32.add wasm-emit                     ! rs+cell
    frame-ctx-slot wasm-emit-local.get
    2 ctx-rs-offset wasm-emit-i32.store ;

: wasm-emit-rs-pop ( -- )
    frame-ctx-slot wasm-emit-local.get
    2 ctx-rs-offset wasm-emit-i32.load
    cell wasm-emit-i32.const
    wasm-i32.sub wasm-emit                     ! new rs
    frame-spill-base wasm-emit-local.set
    frame-ctx-slot wasm-emit-local.get
    frame-spill-base wasm-emit-local.get
    2 ctx-rs-offset wasm-emit-i32.store
    frame-spill-base wasm-emit-local.get
    2 0 wasm-emit-i32.load ;

: jit-call ( name -- ) wasm32-assembler-unimplemented ;
: jit-call-1arg ( arg1s name -- ) drop wasm32-assembler-unimplemented ;
: jit-call-2arg ( arg1s arg2s name -- ) 2drop wasm32-assembler-unimplemented ;
: jit-call-3arg ( arg1s arg2s arg3s name -- )
    3drop wasm32-assembler-unimplemented ;

: jit-load-vm ( -- ) wasm32-assembler-unimplemented ;
: jit-load-context ( -- ) wasm32-assembler-unimplemented ;
: jit-save-context ( -- ) wasm32-assembler-unimplemented ;
: jit-restore-context ( -- ) wasm32-assembler-unimplemented ;

: wasm-call-word ( -- )
    f wasm-relocate-entry drop
    0 wasm-emit-call ;

: wasm-call-word+return ( -- )
    wasm-call-word
    wasm-emit-return ;

: wasm-jit-prolog ( -- ) ;
: wasm-jit-epilog ( -- ) ;
: wasm-jit-return ( -- ) wasm-emit-return ;

: wasm-jit-push-literal ( -- )
    wasm-emit-literal
    wasm-emit-ds-push ;

: wasm-jit-word-call ( -- )
    wasm-call-word ;

: wasm-jit-word-jump ( -- )
    wasm-call-word+return ;

: wasm-jit-primitive ( -- )
    wasm-call-word+return ;

: wasm-jit-safepoint ( -- )
    wasm-emit-safepoint
    wasm-default-gc-map wasm-emit-gc-map ;

: wasm-jit-if ( -- )
    ! condition on ds; compare to f and branch
    wasm-emit-ds-pop
    f wasm-emit-literal
    wasm-i32.eq wasm-emit          ! 1 when cond == f
    wasm-if wasm-emit wasm-blocktype-empty wasm-emit
        ! false branch (cond == f)
        wasm-call-word
    wasm-else wasm-emit
        ! true branch
        wasm-call-word
    wasm-end wasm-emit ;

: wasm-jit-dip ( -- )
    wasm-emit-ds-pop
    wasm-emit-rs-push
    wasm-call-word
    wasm-emit-rs-pop
    wasm-emit-ds-push ;

: wasm-jit-2dip ( -- )
    wasm-emit-ds-pop wasm-emit-rs-push
    wasm-emit-ds-pop wasm-emit-rs-push
    wasm-call-word
    wasm-emit-rs-pop wasm-emit-ds-push
    wasm-emit-rs-pop wasm-emit-ds-push ;

: wasm-jit-3dip ( -- )
    wasm-emit-ds-pop wasm-emit-rs-push
    wasm-emit-ds-pop wasm-emit-rs-push
    wasm-emit-ds-pop wasm-emit-rs-push
    wasm-call-word
    wasm-emit-rs-pop wasm-emit-ds-push
    wasm-emit-rs-pop wasm-emit-ds-push
    wasm-emit-rs-pop wasm-emit-ds-push ;

: init-wasm32-backend ( -- )
    [
        [ wasm-jit-prolog ] JIT-PROLOG jit-define
        [ wasm-jit-epilog ] JIT-EPILOG jit-define
        [ wasm-jit-return ] JIT-RETURN jit-define
        [ wasm-jit-push-literal ] JIT-PUSH-LITERAL jit-define
        [ wasm-jit-word-call ] JIT-WORD-CALL jit-define
        [ wasm-jit-word-jump ] JIT-WORD-JUMP jit-define
        [ wasm-jit-primitive ] JIT-PRIMITIVE jit-define
        [ wasm-jit-safepoint ] JIT-SAFEPOINT jit-define
        [ wasm-jit-if ] JIT-IF jit-define
        [ wasm-jit-dip ] JIT-DIP jit-define
        [ wasm-jit-2dip ] JIT-2DIP jit-define
        [ wasm-jit-3dip ] JIT-3DIP jit-define
    ] with-compilation-unit ;
