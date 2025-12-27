namespace factor {

extern bool factor_print_p;

// WASM debug output - controlled by environment variable FACTOR_WASM_DEBUG
// Set to 1, true, or yes to enable verbose WASM debugging
// Defaults to OFF for performance
#ifdef FACTOR_WASM
inline bool wasm_debug_enabled() {
  static int cached = -1;
  if (cached < 0) {
    const char* env = std::getenv("FACTOR_WASM_DEBUG");
    cached = (env && (env[0] == '1' || env[0] == 't' || env[0] == 'y')) ? 1 : 0;
  }
  return cached == 1;
}
#define WASM_DEBUG(x) do { if (wasm_debug_enabled()) { std::cout << "[wasm] " << x << std::endl; } } while(0)
#define WASM_DEBUG_VERBOSE(x) do { if (wasm_debug_enabled()) { std::cout << "[wasm] " << x << std::endl; } } while(0)
#else
#define WASM_DEBUG(x) ((void)0)
#define WASM_DEBUG_VERBOSE(x) ((void)0)
inline bool wasm_debug_enabled() { return false; }
#endif

#ifdef FACTOR_DEBUG

// To chop the directory path of the __FILE__ macro.
inline const char* abbrev_path(const char* path) {
  const char* p1 = strrchr(path, '\\');
  const char* p2 = strrchr(path, '/');
  return (p1 > p2 ? p1 : p2) + 1;
}

#define FACTOR_PRINT(x)                                          \
  do {                                                           \
    if (factor_print_p) {                                        \
      std::cerr                                                  \
          << std::setw(16) << std::left << abbrev_path(__FILE__) \
          << " " << std::setw(4) << std::right << __LINE__       \
          << " " << std::setw(20) << std::left << __FUNCTION__   \
          << " " << x                                            \
          << std::endl;                                          \
    }                                                            \
  } while (0)
#define FACTOR_PRINT_MARK FACTOR_PRINT("")

#else
#define FACTOR_PRINT(fmt, ...) ((void)0)
#define FACTOR_PRINT_MARK ((void)0)
#endif

}
