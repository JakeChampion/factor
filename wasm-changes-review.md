# WASM Port Changes Review

This document reviews all changes between HEAD and master branch for the Factor WASM port.

## Summary

The WASM port adds a software interpreter to Factor since WebAssembly doesn't support JIT compilation. The main changes are:

1. New interpreter in `vm/interpreter.cpp` (6500+ lines)
2. WASM-specific OS layer in `vm/os-wasm.cpp` and `vm/os-wasm.hpp`
3. Various guards and stubs for WASM in existing VM files
4. Build configuration for wasi-sdk

## Critical Files

### vm/interpreter.cpp (NEW - 6527 lines)

This is the heart of the WASM port - a complete software interpreter for Factor.

#### Architecture
- Uses a **trampoline-based** interpreter to avoid deep C stack recursion (WASM has limited stack)
- Has two dispatch paths:
  1. `trampoline_dispatch_handler` - correct trampoline-aware version
  2. `dispatch_by_handler_id` - older recursive version (BUGGY for control flow!)

#### CRITICAL BUG FOUND: Dual Dispatch Paths

The file has TWO versions of many handlers (bi, dip, keep, etc.):

1. **In `dispatch_by_handler_id` (lines ~2410-4180)**: Uses `interpret_quotation()` which just QUEUES work, then immediately modifies the stack. This is WRONG for trampoline mode!

   Example - buggy `dip` at line 2796:
   ```cpp
   case HANDLER_DIP: {
       cell quot = ctx->pop();
       cell x = ctx->pop();
       interpret_quotation(quot);  // Just queues work!
       ctx->push(x);               // Pushes x BEFORE quot executes!
       return true;
   }
   ```

2. **In `trampoline_dispatch_handler` (lines ~5045-5565)**: Uses work items correctly:
   ```cpp
   case HANDLER_DIP:
       cell quot = ctx->pop();
       cell x = ctx->pop();
       push_restore_1(x);      // Will push x AFTER quot
       push_callable_work(quot);
       return true;
   ```

#### The Problem

`trampoline_dispatch_handler` has a `default` case that falls through to `dispatch_by_handler_id`:
```cpp
default: {
    return dispatch_by_handler_id(handler_id);
}
```

If a handler is in `dispatch_by_handler_id` but NOT in `trampoline_dispatch_handler`, it uses the buggy version.

Handlers that ARE correctly implemented in both:
- HANDLER_CALL, HANDLER_DIP, HANDLER_2DIP, HANDLER_3DIP
- HANDLER_KEEP, HANDLER_2KEEP, HANDLER_3KEEP
- HANDLER_BI, HANDLER_BI_STAR, HANDLER_BI_AT
- HANDLER_TRI, HANDLER_TRI_STAR, HANDLER_TRI_AT

Handlers that may be missing from trampoline version:
- Many control flow handlers in `dispatch_by_handler_id` that use `interpret_quotation`

### vm/contexts.hpp

Changes:
1. Added `depth()` method to calculate stack depth
2. Added underflow detection in `pop()` for WASM
3. Added overflow detection in `push()` for WASM

These changes look correct.

### vm/contexts.cpp

Added logging to `primitive_set_context_object` for debugging.

### vm/tuples.cpp

Changes to `primitive_tuple_boa`:
1. Added extensive logging
2. Fixed stack pointer calculation (was incorrect before):
   ```cpp
   // CRITICAL: datastack points AT top element, not past it!
   cell* src = (cell*)(ctx->datastack - size + sizeof(cell));
   ```

This fix looks correct.

### vm/arrays.cpp

Added logging to `primitive_array` for debugging.

### Other VM files

Most other VM files have:
- `#if defined(FACTOR_WASM)` guards to disable JIT-related code
- Stub implementations for functions not available on WASM
- Logging for debugging

## Potential Issues

### 1. Control Flow Handler Bugs (CRITICAL)

The dual dispatch path architecture means some control flow words may be using the wrong (buggy) implementation. Need to verify ALL control flow handlers are in `trampoline_dispatch_handler`.

### 2. Stack Corruption Pattern

The observed bug shows:
- After `primitive_array`, depth=5
- After `HANDLER_BI`, it queues work correctly
- But after `primitive_tuple_boa`, only depth=1
- Missing: the value that should have been pushed by PUSH_VALUE work item

This suggests either:
1. PUSH_VALUE work item isn't being processed
2. Something is popping extra items
3. Work items are being processed out of order

### 3. PUSH_VALUE Work Item

The HANDLER_BI in trampoline version pushes:
```cpp
push_callable_work(q);      // Will run last
WorkItem push_x;
push_x.type = WorkType::PUSH_VALUE;
push_x.single.value = x;
trampoline_push(push_x);    // Will run second
push_callable_work(p);      // Will run first
```

The PUSH_VALUE should push x onto the datastack after p runs but before q runs.

Need to verify:
1. Is PUSH_VALUE being processed correctly?
2. Is the trampoline stack order correct (LIFO)?

## Files Needing Further Review

1. `vm/interpreter.cpp` - Verify all control flow handlers
2. `vm/gc.cpp` - Check if GC is correctly handling the interpreter state
3. `vm/slot_visitor.hpp` - Large changes, may affect GC tracing

## Recommendations

1. **Remove or guard `dispatch_by_handler_id` control flow handlers** - They should ONLY be used in recursive interpreter mode
2. **Add assertions** to detect when buggy handlers are called in trampoline mode
3. **Add unit tests** for each control flow word to verify stack effects
