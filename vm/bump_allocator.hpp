namespace factor {

struct bump_allocator {
  // offset of 'here' and 'end' is hardcoded in compiler backends
  cell here;
  cell start;
  cell end;
  cell size;

  bump_allocator(cell size, cell start)
      : here(start), start(start), end(start + size), size(size) {}

  bool contains_p(object* obj) {
    return (cell)obj >= start && (cell)obj < end;
  }

#if defined(FACTOR_WASM)
  // Check if address is within allocated portion (below 'here')
  // This is stricter than contains_p and should be used when we need
  // to ensure the address points to an actual allocated object.
  bool contains_allocated_p(object* obj) {
    return (cell)obj >= start && (cell)obj < here;
  }
#endif

  object* allot(cell data_size) {
    cell h = here;
    here = h + align(data_size, data_alignment);
    return (object*)h;
  }

  cell occupied_space() { return here - start; }

  cell free_space() { return end - here; }

  void flush() {
    here = start;
#if defined(FACTOR_DEBUG) || defined(FACTOR_WASM)
    // In case of bugs, there may be bogus references pointing to the
    // memory space after the gc has run. Filling it with a pattern
    // makes accesses to such shadow data fail hard.
    // For WASM: also clear to prevent "invalid header" warnings during
    // full GC iteration over aging space with stale semispace data.
    memset_cell((void*)start, 0xbaadbaad, size);
#endif
  }
};

}
