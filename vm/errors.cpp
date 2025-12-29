#include "master.hpp"

namespace factor {

static bool wasm_trace_enabled_local() {
#if defined(FACTOR_WASM)
  return std::getenv("FACTOR_WASM_TRACE") != nullptr;
#else
  return false;
#endif
}

bool factor_vm::fatal_erroring_p;

static inline void fa_diddly_atal_error() {
  printf("fatal_error in fatal_error!\n");
  breakpoint();
#if defined(FACTOR_WASM)
  abort();
#else
  ::_exit(86);
#endif
}

void fatal_error(const char* msg, cell tagged) {
  if (factor_vm::fatal_erroring_p)
    fa_diddly_atal_error();

  factor_vm::fatal_erroring_p = true;

  std::cout << "fatal_error: " << msg;
  std::cout << ": " << (void*)tagged;
  std::cout << std::endl << std::endl;
  factor_vm* vm = current_vm();
  if (vm->data) {
    vm->dump_memory_layout(std::cout);
  }
  abort();
}

void critical_error(const char* msg, cell tagged) {
  std::cout << "You have triggered a bug in Factor. Please report.\n";
  std::cout << "critical_error: " << msg;
  std::cout << ": " << std::hex << tagged << std::dec;
  std::cout << std::endl;
  current_vm()->factorbug();
}

// Allocates memory
void factor_vm::general_error(vm_error_type error, cell arg1_, cell arg2_) {

  data_root<object> arg1(arg1_, this);
  data_root<object> arg2(arg2_, this);

#if defined(FACTOR_WASM)
  // Track error counts - only report non-IO errors (IO errors are expected during vocab loading)
  static int io_errors = 0;
  static int other_errors = 0;

  // Log ALL errors to file for debugging
  {
    FILE* f = fopen("init-factor.log", "a");
    if (f) {
      static const char* error_names[] = {
        "EXPIRED", "IO", "UNUSED", "TYPE", "DIVIDE_BY_ZERO", "SIGNAL",
        "ARRAY_SIZE", "OUT_OF_FIXNUM_RANGE", "FFI", "UNDEFINED_SYMBOL",
        "DATASTACK_UNDERFLOW", "DATASTACK_OVERFLOW", "RETAINSTACK_UNDERFLOW",
        "RETAINSTACK_OVERFLOW", "CALLSTACK_UNDERFLOW", "CALLSTACK_OVERFLOW",
        "MEMORY", "FP_TRAP", "INTERRUPT", "CALLBACK_SPACE_OVERFLOW"
      };
      const char* name = (error < 20) ? error_names[error] : "UNKNOWN";
      fprintf(f, "[ERROR] type=%d (%s) arg1=0x%lx arg2=0x%lx\n",
              error, name, (unsigned long)arg1.value(), (unsigned long)arg2.value());
      // If it's a TYPE error, print what type was expected
      if (error == ERROR_TYPE) {
        fixnum expected_type = untag_fixnum(arg1.value());
        fprintf(f, "[ERROR]   expected_type=%ld got_value_tag=%ld\n",
                (long)expected_type, (long)TAG(arg2.value()));
      }
      fclose(f);
    }
  }

  if (error == ERROR_IO) {
    io_errors++;
  } else {
    other_errors++;
    // Print all non-IO errors - these are potentially real bugs
    std::cerr << "[wasm] NON-IO ERROR #" << other_errors << " type=" << error << " arg1=";
    print_obj(std::cerr, arg1.value());
    std::cerr << " arg2=";
    print_obj(std::cerr, arg2.value());
    std::cerr << std::endl;
  }
#endif

  faulting_p = true;

  // If we had an underflow or overflow, data or retain stack
  // pointers might be out of bounds, so fix them before allocating
  // anything
  ctx->fix_stacks();

  // If error was thrown during heap scan, we re-enable the GC
  gc_off = false;

  // If the error handler is set, we rewind any C stack frames and
  // pass the error to user-space.
  if (!current_gc && to_boolean(special_objects[ERROR_HANDLER_QUOT])) {
#ifdef FACTOR_DEBUG
    // Doing a GC here triggers all kinds of funny errors
    primitive_compact_gc();
#endif

    // Now its safe to allocate and GC
    cell error_object =
        allot_array_4(tag_fixnum(KERNEL_ERROR), tag_fixnum(error),
                      arg1.value(), arg2.value());
    ctx->push(error_object);

    // Clear the data roots since arg1 and arg2's destructors won't be
    // called. This is necessary on all platforms including WASM to prevent
    // stale pointers from causing GC issues.
    data_roots.clear();

    // The unwind-native-frames subprimitive will clear faulting_p
    // if it was successfully reached.
    unwind_native_frames(special_objects[ERROR_HANDLER_QUOT],
                         ctx->callstack_top);
  } // Error was thrown in early startup before error handler is set, so just
    // crash.
  else {
    std::cout << "You have triggered a bug in Factor. Please report.\n";
    std::cout << "error: " << error << std::endl;
    std::cout << "arg 1: ";
    print_obj(std::cout, arg1.value());
    std::cout << std::endl;
    std::cout << "arg 2: ";
    print_obj(std::cout, arg2.value());
    std::cout << std::endl;
    factorbug();
    abort();
  }
}

// Allocates memory
void factor_vm::type_error(cell type, cell tagged) {
  general_error(ERROR_TYPE, tag_fixnum(type), tagged);
}

void factor_vm::set_memory_protection_error(cell fault_addr, cell fault_pc) {
  // Called from the OS-specific top halves of the signal handlers to
  // make sure it's safe to dispatch to memory_signal_handler_impl.
  if (fatal_erroring_p)
    fa_diddly_atal_error();
  if (faulting_p && !code->safepoint_p(fault_addr))
    fatal_error("Double fault", fault_addr);
  else if (fep_p)
    fatal_error("Memory protection fault during low-level debugger", fault_addr);
  else if (factor::atomic::load(&current_gc_p))
    fatal_error("Memory protection fault during gc", fault_addr);
  signal_fault_addr = fault_addr;
  signal_fault_pc = fault_pc;
}

// Allocates memory
void factor_vm::divide_by_zero_error() {
  general_error(ERROR_DIVIDE_BY_ZERO, false_object, false_object);
}

// Allocates memory
void memory_signal_handler_impl() {
  factor_vm* vm = current_vm();
  if (vm->code->safepoint_p(vm->signal_fault_addr)) {
    vm->handle_safepoint(vm->signal_fault_pc);
  }
  else {
    vm_error_type type = vm->ctx->address_to_error(vm->signal_fault_addr);
    cell number = vm->from_unsigned_cell(vm->signal_fault_addr);
    vm->general_error(type, number, false_object);
  }
  if (!vm->signal_resumable) {
    // In theory we should only get here if the callstack overflowed during a
    // safepoint
    vm->general_error(ERROR_CALLSTACK_OVERFLOW, false_object, false_object);
  }
}

// Allocates memory
void synchronous_signal_handler_impl() {
  factor_vm* vm = current_vm();
  vm->general_error(ERROR_SIGNAL,
                    vm->from_unsigned_cell(vm->signal_number),
                    false_object);
}

// Allocates memory
void fp_signal_handler_impl() {
  factor_vm* vm = current_vm();

  // Clear pending exceptions to avoid getting stuck in a loop
  vm->set_fpu_state(vm->get_fpu_state());

  vm->general_error(ERROR_FP_TRAP,
                    tag_fixnum(vm->signal_fpu_status),
                    false_object);
}
}
