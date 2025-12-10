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
