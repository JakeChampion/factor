namespace factor {

struct tenured_space : free_list_allocator<object> {
  object_start_map starts;

  tenured_space(cell size, cell start)
      : free_list_allocator<object>(size, start), starts(size, start) {}

  object* allot(cell dsize) {
    object* obj = free_list_allocator<object>::allot(dsize);
    if (obj) {
      starts.record_object_start_offset(obj);
      return obj;
    }
    return NULL;
  }

  cell next_allocated_object_after(cell scan) {
    while (scan != this->end && ((object*)scan)->free_p()) {
      free_heap_block* free_block = (free_heap_block*)scan;
      cell blk_size = free_block->size();
      #if defined(FACTOR_WASM)
      // Validate block size to avoid infinite loop on corruption
      if (blk_size == 0 || blk_size > this->size) {
        static int bad_free_count = 0;
        if (bad_free_count < 5) {
          std::cout << "[wasm] tenured next_allocated_object_after bad free block at 0x"
                    << std::hex << scan << " size=0x" << blk_size << std::dec
                    << std::endl;
          bad_free_count++;
        }
        return 0; // Stop iteration
      }
      #endif
      scan = (cell)free_block + blk_size;
    }
    return scan == this->end ? 0 : scan;
  }

  cell first_object() {
    return next_allocated_object_after(this->start);
  }

  cell next_object_after(cell scan) {
    cell data_size = ((object*)scan)->size();
    #if defined(FACTOR_WASM)
    // Validate object size to avoid corruption propagation
    if (data_size == 0 || data_size > this->size) {
      static int bad_obj_count = 0;
      if (bad_obj_count < 5) {
        std::cout << "[wasm] tenured next_object_after bad object at 0x"
                  << std::hex << scan << " size=0x" << data_size
                  << " header=0x" << ((object*)scan)->header << std::dec
                  << std::endl;
        bad_obj_count++;
      }
      return 0; // Stop iteration
    }
    cell next = next_allocated_object_after(scan + data_size);
    // Debug: track when we jump far
    if (wasm_debug_enabled()) {
      static int jump_track = 0;
      if (jump_track < 10 && next > scan + 0x10000) { // more than 64KB jump
        std::cout << "[wasm] tenured big jump: 0x" << std::hex << scan
                  << " + 0x" << data_size << " -> 0x" << next
                  << " (header=0x" << ((object*)scan)->header << ")" << std::dec << std::endl;
        jump_track++;
      }
    }
    return next;
    #else
    return next_allocated_object_after(scan + data_size);
    #endif
  }

  void sweep() {
    free_list_allocator<object>::sweep();
    starts.update_for_sweep(&this->state);
  }
};

}
