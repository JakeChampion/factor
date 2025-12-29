#include "master.hpp"

namespace factor {

#if defined(FACTOR_WASM)

void factor_vm::c_to_factor(cell quot) {
  // Reduce logging overhead - only log with FACTOR_WASM_TRACE
  if (std::getenv("FACTOR_WASM_TRACE")) {
    FILE* f = fopen("init-factor.log", "a");
    if (f) { fprintf(f, "c_to_factor entered quot=0x%lx\n", (unsigned long)quot); fclose(f); }
    std::cout << "[wasm] c_to_factor entering" << std::endl;
  }

  // Mirror the native c-to-factor stub which swaps in the spare context
  // while running Factor code, then restores the previous one on return.
  context* saved_ctx = ctx;
  if (!ctx) {
    if (!spare_ctx) {
      fatal_error("c_to_factor: no context available (both ctx and spare_ctx are null)", 0);
    }
    ctx = spare_ctx;
  }

  // Use a simple try/finally pattern to ensure context is restored
  // even if interpret_quotation raises an error
  interpret_quotation(quot);
  ctx = saved_ctx;
}

void factor_vm::unwind_native_frames(cell quot, cell to) {
  // In WASM interpreter mode, we need to:
  // 1. Reset the callstack to the target position
  // 2. Execute the error handler quotation
  // The 'to' parameter is the callstack top to unwind to

  // Only log with FACTOR_WASM_TRACE to reduce overhead
  if (std::getenv("FACTOR_WASM_TRACE")) {
    FILE* f = fopen("init-factor.log", "a");
    if (f) {
      fprintf(f, "[UNWIND] calling error handler, to=0x%lx, datastack_depth=%lu\n",
              (unsigned long)to, (unsigned long)ctx->depth());
      fclose(f);
    }
    std::cout << "[wasm] unwind_native_frames quot=0x" << std::hex << quot
              << " to=0x" << to << std::dec << std::endl;
  }

  // Reset callstack to target
  ctx->callstack_top = (cell)to;

  // Clear the faulting flag since we're handling the error
  faulting_p = false;

  // Execute the error handler quotation
  // The error object is already on the data stack (pushed in general_error)
  interpret_quotation(quot);
}

cell factor_vm::get_fpu_state() {
  return 0;
}

void factor_vm::set_fpu_state(cell state) { (void)state; }

#else

void factor_vm::c_to_factor(cell quot) {
  // First time this is called, wrap the c-to-factor sub-primitive inside
  // of a callback stub, which saves and restores non-volatile registers
  // per platform ABI conventions, so that the Factor compiler can treat
  // all registers as volatile
  if (!c_to_factor_func) {
    tagged<word> c_to_factor_word(special_objects[C_TO_FACTOR_WORD]);
    code_block* c_to_factor_block = callbacks->add(c_to_factor_word.value(), 0);
    cell func = c_to_factor_block->entry_point();
    CODE_TO_FUNCTION_POINTER_CALLBACK(this, func);
    c_to_factor_func = (c_to_factor_func_type) func;
  }
  c_to_factor_func(quot);
}

void factor_vm::unwind_native_frames(cell quot, cell to) {
  tagged<word> entry_point_word(special_objects[UNWIND_NATIVE_FRAMES_WORD]);
  cell func = entry_point_word->entry_point;
  CODE_TO_FUNCTION_POINTER(func);
  ((unwind_native_frames_func_type) func)(quot, to);
}

cell factor_vm::get_fpu_state() {
  tagged<word> entry_point_word(special_objects[GET_FPU_STATE_WORD]);
  cell func = entry_point_word->entry_point;
  CODE_TO_FUNCTION_POINTER(func);
  return ((get_fpu_state_func_type) func)();
}

void factor_vm::set_fpu_state(cell state) {
  tagged<word> entry_point_word(special_objects[SET_FPU_STATE_WORD]);
  cell func = entry_point_word->entry_point;
  CODE_TO_FUNCTION_POINTER(func);
  ((set_fpu_state_func_type) func)(state);
}

#endif // FACTOR_WASM

}
