#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "atomic-gcc.hpp"

namespace factor {

// WASM uses UTF-8 strings everywhere.
typedef char vm_char;
typedef char symbol_char;

typedef int THREADHANDLE;

#define VM_C_API extern "C"

#define STRING_LITERAL(string) string

#define SSCANF sscanf
#define STRCMP strcmp
#define STRNCMP strncmp
#define STRDUP strdup

#define FTELL ftello
#define FSEEK fseeko

#define OPEN_READ(path) fopen(path, "rb")
#define OPEN_WRITE(path) fopen(path, "wb")

#define THREADSAFE_STRERROR(errnum, buf, buflen) strerror_r(errnum, buf, buflen)

#define print_native_string(string) print_string(string)

#define FACTOR_OS_STRING "wasm"
#define ZSTD_LIB "libzstd.wasm"

#define CODE_TO_FUNCTION_POINTER(code) (void)0
#define CODE_TO_FUNCTION_POINTER_CALLBACK(vm, code) (void)0
#define FUNCTION_CODE_POINTER(ptr) ptr
#define FUNCTION_TOC_POINTER(ptr) ptr

THREADHANDLE start_thread(void* (*start_routine)(void*), void* args);
inline static THREADHANDLE thread_id() { return 0; }

void early_init();
const char* vm_executable_path();
const char* default_image_path();

int getpagesize();
bool set_memory_locked(cell base, cell size, bool locked);

uint64_t nano_count();
void sleep_nanos(uint64_t nsec);

void* native_dlopen(const char* path);
void* native_dlsym(void* handle, const char* symbol);
void native_dlclose(void* handle);

inline static void breakpoint() { __builtin_trap(); }

#define AS_UTF8(ptr) ptr

}
