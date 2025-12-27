namespace factor {

struct aging_space : bump_allocator {
  object_start_map starts;

  aging_space(cell size, cell start)
      : bump_allocator(size, start), starts(size, start) {}

  object* allot(cell dsize) {
    if (here + dsize > end)
      return NULL;

    object* obj = bump_allocator::allot(dsize);
    starts.record_object_start_offset(obj);
    return obj;
  }

  cell next_object_after(cell scan) {
    cell data_size = ((object*)scan)->size();
    #if defined(FACTOR_WASM)
    // Validate object size to avoid corruption propagation
    if (data_size == 0 || data_size > this->size) {
      static int bad_obj_count = 0;
      if (bad_obj_count < 5) {
        std::cout << "[wasm] aging next_object_after bad object at 0x"
                  << std::hex << scan << " size=0x" << data_size
                  << " header=0x" << ((object*)scan)->header << std::dec
                  << std::endl;
        bad_obj_count++;
      }
      return 0; // Stop iteration
    }
    #endif
    if (scan + data_size < here)
      return scan + data_size;
    return 0;
  }

  cell first_object() {
    if (start != here)
      return start;
    return 0;
  }
};

}
