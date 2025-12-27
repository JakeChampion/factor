#include "master.hpp"
#include <sys/stat.h>

namespace factor {

static const int wasm_page_size = 64 * 1024;

static std::string word_name_string(word* w) {
  string* name = untag<string>(w->name);
  cell len = untag_fixnum(name->length);
  return std::string(reinterpret_cast<const char*>(name->data()),
                     static_cast<size_t>(len));
}

static std::string tuple_class_name(tuple* t) {
  tuple_layout* layout = untag<tuple_layout>(t->layout);
  word* klass = untag<word>(layout->klass);
  return word_name_string(klass);
}

static cell tuple_slot(tuple* t, fixnum slot_index) {
  return t->data()[slot_index];
}

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

const char* vm_executable_path() { return safe_strdup("/work/factor.wasm"); }

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
  cell raw = ctx->pop();
  
#if defined(__wasi__) || defined(__EMSCRIPTEN__)
  cell tag = raw & 0xF;
  bool trace = std::getenv("FACTOR_WASM_TRACE") != nullptr;
  if (trace) {
    std::cout << "[wasm] existsp raw=0x" << std::hex << raw << std::dec
              << " tag=" << tag << std::endl;
  }
  
  // Handle different input types
  char* path = nullptr;
  char pathbuf[4096];  // Stack-allocated to avoid race conditions
  
  if (tag == BYTE_ARRAY_TYPE) {
    // Expected: byte_array containing null-terminated path
    byte_array* ba = untag<byte_array>(raw);
    path = ba->data<char>();
  } else if (tag == STRING_TYPE) {
    // Factor string - need to convert to C string
    string* str = untag<string>(raw);
    cell len = untag_fixnum(str->length);
    uint8_t* data = str->data();
    cell copy_len = len < sizeof(pathbuf)-1 ? len : sizeof(pathbuf)-1;
    for (cell i = 0; i < copy_len; i++) {
      pathbuf[i] = (char)data[i];
    }
    pathbuf[copy_len] = '\0';
    path = pathbuf;
  } else if (tag == TUPLE_TYPE) {
    tuple* t = untag<tuple>(raw);
    // Heuristic: many path representations end up as pathname tuples whose
    // string slot is at index 2. Grab it if present even if the tuple class
    // name is unknown.
    cell str_cell = tuple_slot(t, 0); // pathname has a single string slot
    if (TAG(str_cell) == STRING_TYPE) {
      string* str = untag<string>(str_cell);
      cell len = untag_fixnum(str->length);
      uint8_t* data = str->data();
      cell copy_len = len < sizeof(pathbuf)-1 ? len : sizeof(pathbuf)-1;
      for (cell i = 0; i < copy_len; i++) {
        pathbuf[i] = (char)data[i];
      }
      pathbuf[copy_len] = '\0';
      path = pathbuf;
    }
  } else if (tag == ALIEN_TYPE) {
    path = alien_offset(raw);
  } else {
    // Unsupported type - log details and return false
    if (trace) {
      std::cout << "[wasm] existsp unsupported type tag=" << tag;
      if (tag == WORD_TYPE) {
        word* w = untag<word>(raw);
        std::cout << " word=" << word_name_string(w);
      } else if (tag == TUPLE_TYPE) {
        tuple* t = untag<tuple>(raw);
        std::cout << " tuple=" << tuple_class_name(t);
      }
      std::cout << " obj=";
      print_obj(std::cout, raw);
      std::cout << std::endl;
    }
    ctx->push(tag_boolean(false));
    return;
  }
  
  // Check for null path before proceeding
  if (path == nullptr) {
    ctx->push(tag_boolean(false));
    return;
  }
  
  // Expand resource: prefix to the workspace root so we can locate Factor
  // sources when running under wasmtime.
  char resolved[4096];  // Stack-allocated to avoid race conditions
  if (strncmp(path, "resource:", 9) == 0) {
    snprintf(resolved, sizeof(resolved), "/work/%s", path + 9);
    path = resolved;
  }

  // Use stat to check if file exists
  struct stat sb;
  bool exists = (stat(path, &sb) >= 0);
  // Fallback: if /work is not mounted, try relative path without the prefix.
  if (!exists && strncmp(path, "/work/", 6) == 0) {
    const char* rel = path + 6;
    exists = (stat(rel, &sb) >= 0);
    if (trace) {
      std::cout << "[wasm] existsp fallback path='" << rel
                << "' exists=" << exists << std::endl;
    }
  }
  
  if (trace) {
    std::cout << "[wasm] existsp path='" << path << "' exists=" << exists << std::endl;
  }

  ctx->push(tag_boolean(exists));
#else
  (void)raw;
  ctx->push(tag_boolean(false));
#endif
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
