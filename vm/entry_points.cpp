#include "master.hpp"

namespace factor {

#if defined(FACTOR_WASM)

void factor_vm::c_to_factor(cell quot) {
  (void)quot;
  fatal_error("c_to_factor is not supported on wasm (no callbacks/JIT)", 0);
}

void factor_vm::unwind_native_frames(cell quot, cell to) {
  (void)quot;
  (void)to;
  fatal_error("unwind_native_frames is not supported on wasm", 0);
}

cell factor_vm::get_fpu_state() {
  fatal_error("get_fpu_state is not supported on wasm", 0);
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
