namespace factor {

static const cell free_list_count = 32;
static const cell allocation_page_size = 1024;

struct free_heap_block {
  cell header;

  bool free_p() const { return (header & 1) == 1; }

  cell size() const {
    cell size = header & ~7;
#if defined(FACTOR_WASM)
    // On WASM, return minimum alignment if size is zero to avoid infinite loops
    if (size == 0) {
      return data_alignment;
    }
#else
    FACTOR_ASSERT(size > 0);
#endif
    return size;
  }

  void make_free(cell size) {
    FACTOR_ASSERT(size > 0);
    header = size | 1;
  }
};

struct block_size_compare {
  bool operator()(free_heap_block* a, free_heap_block* b) const {
    return a->size() < b->size();
  }
};

struct allocator_room {
  cell size;
  cell occupied_space;
  cell total_free;
  cell contiguous_free;
  cell free_block_count;
};

template <typename Block> struct free_list_allocator {
  // Region of memory managed by this free list allocator.
  cell start;
  cell end;
  cell size;

  // Stores the free blocks
  std::vector<free_heap_block*> small_blocks[free_list_count];
  std::multiset<free_heap_block*, block_size_compare> large_blocks;
  cell free_block_count;
  cell free_space;

  mark_bits state;

  // Initializing & freeing
  free_list_allocator(cell size, cell start);
  void initial_free_list(cell occupied);
  void clear_free_list();
  void add_to_free_list(free_heap_block* block);
  void free(Block* block);

  // Allocating
  free_heap_block* find_free_block(cell size);
  free_heap_block* split_free_block(free_heap_block* block, cell size);
  Block* allot(cell size);

  // Data
  bool contains_p(Block* block);
  bool can_allot_p(cell size);
  cell occupied_space();
  cell largest_free_block();
  allocator_room as_allocator_room();

  // Iteration
  void sweep();
  template <typename Iterator> void sweep(Iterator& iter);
  template <typename Iterator, typename Fixup>
  void compact(Iterator& iter, Fixup fixup, const Block** finger);
  template <typename Iterator, typename Fixup>
  void iterate(Iterator& iter, Fixup fixup);
};

template <typename Block>
void free_list_allocator<Block>::clear_free_list() {
  for (cell i = 0; i < free_list_count; i++)
    small_blocks[i].clear();
  large_blocks.clear();
  free_block_count = 0;
  free_space = 0;
}

template <typename Block>
void free_list_allocator<Block>::add_to_free_list(free_heap_block* block) {
  cell size = block->size();

  free_block_count++;
  free_space += size;

  if (size < free_list_count * data_alignment)
    small_blocks[size / data_alignment].push_back(block);
  else
    large_blocks.insert(block);
}

template <typename Block>
void free_list_allocator<Block>::initial_free_list(cell occupied) {
  clear_free_list();
  if (occupied != end - start) {
    free_heap_block* last_block = (free_heap_block*)(start + occupied);
    cell free_size = end - (cell)last_block;
#if defined(FACTOR_WASM)
    if (wasm_debug_enabled()) {
      std::cout << "[wasm] initial_free_list: start=0x" << std::hex << start
                << " end=0x" << end << " occupied=0x" << occupied
                << " free_block=0x" << (cell)last_block << " free_size=0x" << free_size
                << std::dec << std::endl;
    }
#endif
    last_block->make_free(free_size);
#if defined(FACTOR_WASM)
    if (wasm_debug_enabled()) {
      std::cout << "[wasm] initial_free_list: header after make_free=0x" << std::hex
                << last_block->header << " free_p=" << last_block->free_p()
                << " size=0x" << last_block->size() << std::dec << std::endl;
    }
#endif
    add_to_free_list(last_block);
  }
}

template <typename Block>
free_list_allocator<Block>::free_list_allocator(cell size, cell start)
    : start(start),
      end(start + size),
      size(size),
      state(mark_bits(size, start)) {
  initial_free_list(0);
}

template <typename Block>
bool free_list_allocator<Block>::contains_p(Block* block) {
  return ((cell)block - start) < size;
}

template <typename Block>
bool free_list_allocator<Block>::can_allot_p(cell size) {
  return largest_free_block() >= std::max(size, allocation_page_size);
}

template <typename Block>
free_heap_block* free_list_allocator<Block>::split_free_block(
    free_heap_block* block,
    cell size) {
  if (block->size() != size) {
    // split the block in two
    free_heap_block* split = (free_heap_block*)((cell)block + size);
    split->make_free(block->size() - size);
    block->make_free(size);
    add_to_free_list(split);
  }

  return block;
}

template <typename Block>
free_heap_block* free_list_allocator<Block>::find_free_block(cell size) {
  // Check small free lists
  cell bucket = size / data_alignment;
  if (bucket < free_list_count) {
    std::vector<free_heap_block*>& blocks = small_blocks[bucket];
    if (blocks.size() == 0) {
      // Round up to a multiple of 'size'
      cell large_block_size = ((allocation_page_size + size - 1) / size) * size;

      // Allocate a block this big
      free_heap_block* large_block = find_free_block(large_block_size);
      if (!large_block)
        return NULL;

      large_block = split_free_block(large_block, large_block_size);

      // Split it up into pieces and add each piece back to the free list
      for (cell offset = 0; offset < large_block_size; offset += size) {
        free_heap_block* small_block = large_block;
        large_block = (free_heap_block*)((cell)large_block + size);
        small_block->make_free(size);
        add_to_free_list(small_block);
      }
    }

    free_heap_block* block = blocks.back();
    blocks.pop_back();

    free_block_count--;
    free_space -= block->size();

    return block;
  } else {
    // Check large free list
    free_heap_block key;
    key.make_free(size);
    auto iter = large_blocks.lower_bound(&key);
    auto end = large_blocks.end();

    if (iter != end) {
      free_heap_block* block = *iter;
      large_blocks.erase(iter);

      free_block_count--;
      free_space -= block->size();

      return block;
    }

    return NULL;
  }
}


template <typename Block>
Block* free_list_allocator<Block>::allot(cell size) {
  size = align(size, data_alignment);

  free_heap_block* block = find_free_block(size);
  if (block) {
    block = split_free_block(block, size);
    return (Block*)block;
  }
  return NULL;
}

template <typename Block>
void free_list_allocator<Block>::free(Block* block) {
  free_heap_block* free_block = (free_heap_block*)block;
  free_block->make_free(block->size());
  add_to_free_list(free_block);
}

template <typename Block>
cell free_list_allocator<Block>::occupied_space() {
  return size - free_space;
}

template <typename Block>
cell free_list_allocator<Block>::largest_free_block() {
  if (large_blocks.size()) {
    auto last = large_blocks.rbegin();
    return (*last)->size();
  } else {
    for (int i = free_list_count - 1; i >= 0; i--) {
      if (small_blocks[i].size())
        return small_blocks[i].back()->size();
    }
    return 0;
  }
}

template <typename Block>
template <typename Iterator>
void free_list_allocator<Block>::sweep(Iterator& iter) {
  clear_free_list();

  cell start = this->start;
  cell end = this->end;

  while (start != end) {
    // find next unmarked block
    start = state.next_unmarked_block_after(start);

    if (start != end) {
      // find size
      cell size = state.unmarked_block_size(start);
      FACTOR_ASSERT(size > 0);

      free_heap_block* free_block = (free_heap_block*)start;
      free_block->make_free(size);
      add_to_free_list(free_block);
      iter((Block*)start, size);

      start = start + size;
    }
  }
}

template <typename Block> void free_list_allocator<Block>::sweep() {
  auto null_sweep = [](Block* free_block, cell size) { (void)free_block; (void)size; };
  sweep(null_sweep);
}

// The forwarding map must be computed first by calling
// state.compute_forwarding().
template <typename Block>
template <typename Iterator, typename Fixup>
void free_list_allocator<Block>::compact(Iterator& iter, Fixup fixup,
                                         const Block** finger) {
  cell dest_addr = start;
  auto compact_block_func = [&](Block* block, cell size) {
    cell block_addr = (cell)block;
    if (!state.marked_p(block_addr))
      return;
    *finger = (Block*)(block_addr + size);
    if (dest_addr != (cell)block) {
      memmove((Block*)dest_addr, block, size);
    }
    iter(block, (Block*)dest_addr, size);
    dest_addr += size;
  };
  iterate(compact_block_func, fixup);

  // Now update the free list; there will be a single free block at
  // the end
  initial_free_list(dest_addr - start);
}

// During compaction we have to be careful and measure object sizes
// differently
template <typename Block>
template <typename Iterator, typename Fixup>
void free_list_allocator<Block>::iterate(Iterator& iter, Fixup fixup) {
  cell scan = this->start;
#if defined(FACTOR_WASM) && 0  // Disabled verbose iterate logging
  static int iterate_call_count = 0;
  if (iterate_call_count < 5) {
    std::cout << "[wasm] iterate: start=0x" << std::hex << this->start
              << " end=0x" << this->end << " size=0x" << this->size
              << std::dec << " (call #" << iterate_call_count << ")" << std::endl;
    iterate_call_count++;
  }
#endif
  while (scan != this->end) {
    Block* block = (Block*)scan;
    cell size = fixup.size(block);
#if defined(FACTOR_WASM)
    // Skip zero-sized or implausibly sized blocks to avoid infinite loops
    if (size == 0) {
      size = data_alignment;
    }
    if (size > this->size) {
      break;
    }
#endif
    if (!block->free_p())
      iter(block, size);
    scan += size;
  }
}

template <typename Block>
allocator_room free_list_allocator<Block>::as_allocator_room() {
  allocator_room room;
  room.size = size;
  room.occupied_space = occupied_space();
  room.total_free = free_space;
  room.contiguous_free = largest_free_block();
  room.free_block_count = free_block_count;
  return room;
}

}
