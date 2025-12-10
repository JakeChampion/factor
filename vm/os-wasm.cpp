#include "master.hpp"

namespace factor {

static const int wasm_page_size = 64 * 1024;

int getpagesize() { return wasm_page_size; }

bool set_memory_locked(cell base, cell size, bool locked) {
  (void)base;
  (void)size;
  (void)locked;
  // No guard pages on wasm; pretend it succeeded.
  return true;
}

THREADHANDLE start_thread(void* (*start_routine)(void*), void* args) {
  (void)start_routine;
  (void)args;
  fatal_error("threads are not supported on wasm", 0);
  return 0;
}

uint64_t nano_count() {
#if defined(__wasi__) || defined(__EMSCRIPTEN__)
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
    fatal_error("clock_gettime failed", 0);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#else
  return 0;
#endif
}

void sleep_nanos(uint64_t nsec) {
#if defined(__wasi__) || defined(__EMSCRIPTEN__)
  timespec ts;
  ts.tv_sec = nsec / 1000000000ull;
  ts.tv_nsec = nsec % 1000000000ull;
  nanosleep(&ts, NULL);
#else
  (void)nsec;
#endif
}

void* native_dlopen(const char* path) {
  (void)path;
  fatal_error("dlopen is not available on wasm", 0);
  return NULL;
}

void* native_dlsym(void* handle, const char* symbol) {
  (void)handle;
  (void)symbol;
  fatal_error("dlsym is not available on wasm", 0);
  return NULL;
}

void native_dlclose(void* handle) {
  (void)handle;
  fatal_error("dlclose is not available on wasm", 0);
}

void early_init() {}

const char* vm_executable_path() { return safe_strdup("factor.wasm"); }

const char* default_image_path() { return safe_strdup("factor.image"); }

void factor_vm::c_to_factor_toplevel(cell quot) { c_to_factor(quot); }

void factor_vm::init_signals() {}

void factor_vm::start_sampling_profiler_timer() {}

void factor_vm::end_sampling_profiler_timer() {}

void dispatch_signal(void* uap, void(handler)()) {
  (void)uap;
  (void)handler;
}

void unix_init_signals() {}

void factor_vm::init_ffi() {}

void factor_vm::ffi_dlopen(dll* dll) {
  (void)dll;
  general_error(ERROR_FFI, false_object, false_object);
}

cell factor_vm::ffi_dlsym(dll* dll, symbol_char* symbol) {
  (void)dll;
  (void)symbol;
  general_error(ERROR_FFI, false_object, false_object);
  return 0;
}

void factor_vm::ffi_dlclose(dll* dll) { (void)dll; }

void factor_vm::primitive_existsp() {
  char* path = (char*)(untag_check<byte_array>(ctx->pop()) + 1);
  (void)path;
  ctx->push(tag_boolean(false));
}

bool move_file(const vm_char* path1, const vm_char* path2) {
  (void)path1;
  (void)path2;
  return false;
}

void lock_console() {}
void unlock_console() {}
void ignore_ctrl_c() {}
void handle_ctrl_c() {}
void open_console() {}
void factor_vm::primitive_disable_ctrl_break() {}
void factor_vm::primitive_enable_ctrl_break() {}
void factor_vm::primitive_code_blocks() {
  ctx->push(tag<array>(allot_array(0, false_object)));
}
void abort() { ::abort(); }
void close_console() {}

segment::segment(cell size_, bool executable_p) {
  (void)executable_p;
  size = align(size_, wasm_page_size);
  cell alloc_size = size + 2 * wasm_page_size;
  char* base = (char*)malloc(alloc_size);
  if (!base)
    fatal_error("Out of memory allocating segment", alloc_size);
  start = (cell)(base + wasm_page_size);
  end = start + size;
}

segment::~segment() {
  char* base = (char*)(start - wasm_page_size);
  free(base);
}

}
