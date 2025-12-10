#include "master.hpp"

namespace factor {

factor_vm* global_vm;

void init_mvm() { global_vm = NULL; }

void register_vm_with_thread(factor_vm* vm) {
  FACTOR_ASSERT(!global_vm);
  global_vm = vm;
}

factor_vm* current_vm_p() { return global_vm; }

VM_C_API THREADHANDLE start_standalone_factor_in_new_thread(int argc,
                                                            vm_char** argv) {
  (void)argc;
  (void)argv;
  fatal_error("Threads are not supported on this platform", 0);
  return 0;
}

}
