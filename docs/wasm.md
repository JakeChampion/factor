# WebAssembly (WASM) Port of Factor

This document describes the WebAssembly port of the Factor programming language, including architecture, build instructions, and implementation details.

## Overview

The WASM port provides an **interpreter-only** implementation of Factor that runs in WebAssembly environments via WASI (WebAssembly System Interface). This port trades JIT compilation performance for portability and sandboxed execution.

### Key Characteristics

- **Interpreter-based execution**: No JIT compilation (WASM doesn't support dynamic code generation)
- **Trampoline architecture**: Avoids stack overflow in WASM's limited stack
- **WASI platform layer**: Minimal system interface for file I/O and timing
- **Single-threaded**: No threading support (WASM/WASI limitation)
- **No FFI**: Foreign function interface disabled (no dlopen in WASM)

## Building

### Prerequisites

1. **WASI SDK** (required):
   ```bash
   # macOS (Homebrew)
   brew install wasi-sdk wasi-libc

   # Or download from https://github.com/WebAssembly/wasi-sdk/releases
   # Extract to /opt/wasi-sdk
   ```

2. **wasmtime** (for running):
   ```bash
   brew install wasmtime
   ```

### Build Commands

```bash
# Build the WASM VM
make wasi-wasm32

# This produces: factor.wasm
```

### Running

```bash
# Run with wasmtime, mounting current directory as /work
wasmtime run --dir /work \
  factor.wasm -- \
  -i=factor.image \
  -resource-path=/work
```

## Architecture

### Execution Model

The WASM port uses a **work queue trampoline** instead of recursive interpretation:

1. **Work Queue**: Queue of items to execute (quotations, words, values)
2. **Trampoline Loop**: Iteratively processes work items without deep recursion
3. **Stack Management**: Data/retain/call stacks managed explicitly

This prevents stack overflow in WASM's limited call stack (typically 64KB-1MB).

### Memory Layout

```
Linear Memory (WASM)
├─ Data Heap
│  ├─ Nursery (young generation)
│  ├─ Aging (semi-space)
│  └─ Tenured (old generation)
├─ Code Heap (unused - interpreter only)
├─ Data Stack (Factor data stack)
├─ Retain Stack (Factor retain stack)
└─ Call Stack (Factor call frames)
```

### Key Components

#### 1. Interpreter (`vm/interpreter.cpp`)

The core interpreter handles:
- Word execution
- Quotation evaluation
- Primitive operations
- Curried/composed quotation dispatch
- Hash table operations
- Method dispatch

**Note**: This file is large (6,480 lines) and should be refactored into modules in the future.

#### 2. Platform Layer

- **`vm/os-wasm.cpp`**: WASI platform implementation
- **`vm/cpu-wasm.hpp`**: WASM32 CPU definitions
- **`vm/mvm-none.cpp`**: Multi-VM disabled (no threading)

#### 3. Garbage Collector

GC works with WASM constraints:
- No memory protection (guard pages unavailable)
- Graceful handling of stale pointers during compaction
- Pre-escalation checks to avoid retry loops
- Defensive invalid header handling

#### 4. Bootstrap

- **`basis/bootstrap/stage1-wasm-tiny.factor`**: Minimal stage1 bootstrap
- **`basis/bootstrap/stage2-wasm.factor`**: Skips JIT components
- **`basis/bootstrap/assembler/wasm32.factor`**: Stub assembler backend

## Environment Variables

### Debug Flags

- `FACTOR_WASM_TRACE`: Enable detailed execution tracing
- `FACTOR_WASM_DEBUG`: Enable debug logging
- `FACTOR_WASM_NOOP_GC`: Disable GC for debugging (dangerous!)
- `FACTOR_WASM_SKIP_GC_STARTUP`: Leave GC disabled during init

### Usage Example

```bash
FACTOR_WASM_TRACE=1 wasmtime run --dir /work factor.wasm -- -i=factor.image
```

## Implementation Details

### Performance Optimizations

1. **Layout Caching**: Cache tuple layouts to avoid repeated string comparisons
   - `g_curried_layout`, `g_composed_layout`, etc.
   - Cleared after GC compaction

2. **Fast Type Checks**: Compare cached layout pointers instead of string names

3. **Minimal Allocations**: Use `std::string_view` where possible to avoid copies

### Limitations

| Feature | Status | Reason |
|---------|--------|--------|
| JIT Compilation | ❌ Disabled | WASM doesn't support dynamic code generation |
| Threading | ❌ Disabled | WASI has no threading support |
| FFI (dlopen) | ❌ Disabled | WASM has no dynamic linking |
| Sampling Profiler | ❌ Disabled | No signal support in WASI |
| Memory Protection | ❌ Disabled | WASM has no guard pages |
| Image Saving | ⚠️ Limited | File I/O through WASI |

### Known Issues

None currently identified. Previous issues have been resolved:

- ~~Stack Leak During Bootstrap~~ - **FIXED**: Stack manipulation handlers (DUP, 2DUP, OVER, 2OVER, PICK, etc.) now handled directly in trampoline instead of recursive dispatch

## Development

### Adding Primitives

To add a new primitive for WASM:

1. Add handler in `vm/interpreter.cpp`:
   ```cpp
   case PRIMITIVE_YOUR_OPERATION: {
     // Implementation
     break;
   }
   ```

2. Update primitive count in `vm/primitives.hpp` if needed

3. Test with: `FACTOR_WASM_TRACE=1 wasmtime ...`

### Debugging

1. **Enable Tracing**: `FACTOR_WASM_TRACE=1`
2. **Check Call Trace**: Interpreter maintains `wasm_call_trace` vector
3. **Dump Stack**: `dump_stack()` helper shows top N stack items
4. **GC Logging**: `wasm_debug_enabled()` gates GC diagnostics

### Testing

Currently no automated tests exist for WASM target. Manual testing:

```bash
# Build image
make wasi-wasm32

# Run basic test
echo '5 5 + .' | wasmtime run --dir /work factor.wasm -- -i=factor.image

# Expected output: 10
```

## Troubleshooting

### Build Fails: "wasi-sdk not found"

- Install WASI SDK (see Prerequisites)
- Or set `WASI_SDK_PATH=/path/to/wasi-sdk`

### Runtime: "Cannot find factor.image"

- Ensure `factor.image` exists in working directory
- Mount directory with `--dir /work`
- Use absolute path: `-i=/work/factor.image`

### Stack Overflow

- Increase stack size in `vm/Config.wasi`:
  ```make
  LDFLAGS += -Wl,-z,stack-size=16777216  # 16MB
  ```

### GC Crashes

- Try disabling GC: `FACTOR_WASM_NOOP_GC=1` (debugging only!)
- Check for stale pointers with `FACTOR_WASM_DEBUG=1`

## Future Work

### Planned Improvements

- [x] ~~Fix stack leak during bootstrap~~ - **COMPLETED**
- [ ] Split `vm/interpreter.cpp` into modular files (see below)
- [ ] Add automated tests for WASM target
- [ ] Optimize string allocations in interpreter
- [ ] Implement WASM-specific profiling
- [ ] Support wasm32-emscripten target (browser)
- [ ] Optimize method dispatch (function pointer table?)

#### Interpreter Modularization Plan

The `vm/interpreter.cpp` file (6,480 lines) should be split into focused modules for maintainability:

**Proposed Structure**:
```
vm/wasm/
├── interpreter.cpp          # Main trampoline loop (500 lines)
├── interpreter_dispatch.cpp # Word handler dispatch (1000 lines)
├── interpreter_primitives.cpp # Primitive handlers (2000 lines)
├── interpreter_control.cpp  # Control flow (if/loop/while) (800 lines)
├── interpreter_combinators.cpp # Combinators (dip/bi/2bi/etc) (1200 lines)
├── interpreter_hashtable.cpp # Hash table operations (500 lines)
└── interpreter_utils.cpp    # Helper functions (480 lines)
```

**Rationale**:
- Keeps related functionality together
- Each file is manageable size (<2000 lines)
- Clear responsibility boundaries
- Easier to navigate and review

**Implementation Notes**:
- Use inline functions for hot paths to avoid performance regression
- Keep handler dispatch table in main interpreter.cpp
- Consider using function pointers for primitive dispatch
- Maintain single compilation unit via `#include` if needed for optimization

### Research Areas

- **Wasm GC Integration**: Use WASM GC proposal for managed objects
- **SIMD Support**: Leverage WASM SIMD for array operations
- **Streaming Compilation**: Compile Factor to WASM bytecode offline
- **Browser Backend**: Add HTML canvas/DOM bindings for UI

## References

- [WASI Specification](https://github.com/WebAssembly/WASI)
- [WebAssembly Reference](https://webassembly.github.io/spec/core/)
- [wasmtime Documentation](https://docs.wasmtime.dev/)
- [Factor Language](https://factorcode.org/)

## Contributing

When contributing to the WASM port:

1. Test changes with `FACTOR_WASM_TRACE=1` and `FACTOR_WASM_DEBUG=1`
2. Ensure builds work with default WASI SDK paths
3. Document any new environment variables or flags
4. Gate debug code with `wasm_debug_enabled()` or `#ifdef FACTOR_DEBUG`
5. Add comments explaining WASM-specific workarounds

---

**Status**: Experimental (works for basic Factor code, needs testing/hardening)

**Maintainer**: Factor contributors

**Last Updated**: 2025-12-27
