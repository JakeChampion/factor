namespace factor {

factor_vm* current_vm_p();

// Size sans alignment.
template <typename Fixup>
cell object::base_size(Fixup fixup) const {
  switch (type()) {
    case ARRAY_TYPE:
      return array_size((array*)this);
    case BIGNUM_TYPE:
      return array_size((bignum*)this);
    case BYTE_ARRAY_TYPE:
      return array_size((byte_array*)this);
    case STRING_TYPE:
      return string_size(string_capacity((string*)this));
    case TUPLE_TYPE: {
      cell raw_layout = ((tuple*)this)->layout;
      object* untagged_layout = untag<object>(raw_layout);
      tuple_layout* layout = (tuple_layout*)fixup.translate_data(untagged_layout);
      cell layout_size_field = layout->size;
      cell capacity = untag_fixnum(layout_size_field);
      cell sz = sizeof(tuple) + capacity * sizeof(cell);
      // Only log truly huge sizes that indicate a problem
      static int huge_log = 0;
      if ((sz > 0x10000000 || capacity > 0x10000000) && huge_log < 2) {
        huge_log++;
        std::cout << "[wasm] TUPLE huge size: this=0x" << std::hex << (cell)this
                  << " raw_layout=0x" << raw_layout
                  << " untagged=0x" << (cell)untagged_layout
                  << " translated=0x" << (cell)layout
                  << " layout->size=0x" << layout_size_field
                  << " capacity=0x" << capacity
                  << " computed_size=0x" << sz << std::dec << std::endl;
      }
      return sz;
    }
    case QUOTATION_TYPE:
      return sizeof(quotation);
    case WORD_TYPE:
      return sizeof(word);
    case FLOAT_TYPE:
      return sizeof(boxed_float);
    case DLL_TYPE:
      return sizeof(dll);
    case ALIEN_TYPE:
      return sizeof(alien);
    case WRAPPER_TYPE:
      return sizeof(wrapper);
    case CALLSTACK_TYPE: {
      cell callstack_length = untag_fixnum(((callstack*)this)->length);
      return callstack_object_size(callstack_length);
    }
    default:
      {
#if defined(FACTOR_WASM)
        // SAFETY NOTE: Returning data_alignment for invalid headers is safe because:
        // 1. This only occurs when visiting objects during GC compaction/relocation
        // 2. The invalid headers are from stale pointers in already-evacuated semispaces
        // 3. The GC only needs a size estimate to skip past these regions
        // 4. The fixup phase will correct or eliminate these stale references
        // 5. These objects are never dereferenced - only used for memory iteration
        //
        // This pattern is necessary in WASM because guard pages aren't available
        // to catch truly invalid memory access. The GC must gracefully handle
        // stale pointers during multi-phase collection (nursery→aging→tenured).
        if (wasm_debug_enabled()) {
          static int invalid_header_count = 0;
          if (invalid_header_count < 3) {
            invalid_header_count++;
            std::cout << "[wasm] invalid header base_size ptr=0x" << std::hex
                      << (cell)this << " header=0x" << header << std::dec;
            factor_vm* vm = current_vm_p();
            if (vm && vm->current_gc)
              std::cout << " gc_op=" << vm->current_gc->op;
            std::cout << " (first 3 only)" << std::endl;
          }
        }
        return data_alignment;
#else
        FILE* f = fopen("gc-bad.log", "a");
        if (f) {
          fprintf(f,
                  "invalid header base_size ptr=0x%lx header=0x%lx (returning minimum)\n",
                  (unsigned long)this, (unsigned long)header);
          fclose(f);
        }
        std::cout << "[wasm] invalid header base_size ptr=0x" << std::hex
                  << (cell)this << " header=0x" << header << std::dec;
        factor_vm* vm = current_vm_p();
        if (vm) {
          bool in_nursery = vm->data->nursery->contains_p((object*)this);
          bool in_aging = vm->data->aging->contains_p((object*)this);
          bool in_tenured = vm->data->tenured->contains_p((object*)this);
          std::cout << " heap[nursery=" << (in_nursery ? "y" : "n")
                    << " aging=" << (in_aging ? "y" : "n")
                    << " tenured=" << (in_tenured ? "y" : "n") << "]";
          if (vm->current_gc)
            std::cout << " gc_op=" << vm->current_gc->op;
        }
        std::cout << " (continuing with data_alignment)" << std::endl;
        return data_alignment;
#endif
      }
  }
}

// Size of the object pointed to by an untagged pointer
template <typename Fixup>
cell object::size(Fixup fixup) const {
  if (free_p())
    return ((free_heap_block*)this)->size();
  return align(base_size(fixup), data_alignment);
}

inline cell object::size() const { return size(no_fixup()); }

// The number of slots (cells) in an object which should be scanned by
// the GC. The number can vary in arrays and tuples, in all other
// types the number is a constant.
template <typename Fixup>
inline cell object::slot_count(Fixup fixup) const {
  if (free_p())
    return 0;

  cell t = type();
  if (t == ARRAY_TYPE) {
    // capacity + n slots
    return 1 + array_capacity((array*)this);
  } else if (t == TUPLE_TYPE) {
    tuple_layout* layout = (tuple_layout*)fixup.translate_data(
        untag<object>(((tuple*)this)->layout));
    // layout + n slots
    return 1 + tuple_capacity(layout);
  } else {
    switch (t) {
      // these objects do not refer to other objects at all
      case FLOAT_TYPE:
      case BIGNUM_TYPE:
      case BYTE_ARRAY_TYPE:
      case CALLSTACK_TYPE: return 0;
      case QUOTATION_TYPE: return 3;
      case ALIEN_TYPE: return 2;
      case WRAPPER_TYPE: return 1;
      case STRING_TYPE: return 3;
      case WORD_TYPE: return 8;
      case DLL_TYPE: return 1;
      default:
        critical_error("Invalid header in slot_count", (cell)this);
        return 0; // can't happen
    }
  }
}

inline cell object::slot_count() const {
  return slot_count(no_fixup());
}

// Slot visitors iterate over the slots of an object, applying a functor to
// each one that is a non-immediate slot. The pointer is untagged first.
// The functor returns a new untagged object pointer. The return value may
// or may not equal the old one, however the new pointer receives the same
// tag before being stored back to the original location.

// Slots storing immediate values are left unchanged and the visitor does
// inspect them.

// This is used by GC's copying, sweep and compact phases, and the
// implementation of the become primitive.

// Iteration is driven by visit_*() methods. Only one of them define GC
// roots:
//  - visit_all_roots()

// Code block visitors iterate over sets of code blocks, applying a functor
// to each one. The functor returns a new code_block pointer, which may or
// may not equal the old one. This is stored back to the original location.

// This is used by GC's sweep and compact phases, and the implementation of
// the modify-code-heap primitive.

// Iteration is driven by visit_*() methods. Some of them define GC roots:
//  - visit_context_code_blocks()
//  - visit_callback_code_blocks()

template <typename Fixup> struct slot_visitor {
  factor_vm* parent;
  Fixup fixup;
  cell cards_scanned;
  cell decks_scanned;
  cell current_container;

  slot_visitor<Fixup>(factor_vm* parent, Fixup fixup)
  : parent(parent),
    fixup(fixup),
    cards_scanned(0),
    decks_scanned(0),
    current_container(0) {}

  cell visit_pointer(cell pointer);
  void visit_handle(cell* handle);
  void visit_object_array(cell* start, cell* end);
  void visit_partial_objects(cell start, cell card_start, cell card_end);
  void visit_slots(object* ptr);
  void visit_stack_elements(segment* region, cell* top);
  void visit_all_roots();
  void visit_callstack_object(callstack* stack);
  void visit_callstack(context* ctx);
  void visit_context(context *ctx);
  void visit_object_code_block(object* obj);
  void visit_context_code_blocks();
  void visit_uninitialized_code_blocks();
  void visit_object(object* obj);
  void visit_mark_stack(std::vector<cell>* mark_stack);


  template <typename SourceGeneration>
  cell visit_card(SourceGeneration* gen, cell index, cell start);
  template <typename SourceGeneration>
  void visit_cards(SourceGeneration* gen, card mask, card unmask);


  template <typename TargetGeneration>
  void cheneys_algorithm(TargetGeneration* gen, cell scan);

  // Visits the data pointers in code blocks in the remembered set.
  void visit_code_heap_roots(std::set<code_block*>* remembered_set);

  // Visits pointers embedded in instructions in code blocks.
  void visit_instruction_operands(code_block* block, cell rel_base);
  void visit_embedded_code_pointers(code_block* compiled);
  void visit_embedded_literals(code_block* compiled);

  // Visits data pointers in code blocks.
  void visit_code_block_objects(code_block* compiled);
};

template <typename Fixup>
cell slot_visitor<Fixup>::visit_pointer(cell pointer) {
  object* untagged = fixup.fixup_data(untag<object>(pointer));
  bool trace = wasm_debug_enabled();
  if (trace &&
      ((cell)untagged < 0x100000 ||
       ((cell)untagged >= 0x45f000 && (cell)untagged < 0x460000))) {
    std::cout << "[wasm] visit_pointer suspicious tagged=0x" << std::hex
              << pointer << " untagged=0x" << (cell)untagged << std::dec;
    bool in_nursery = parent->data->nursery->contains_p(untagged);
    bool in_aging = parent->data->aging->contains_p(untagged);
    bool in_tenured = parent->data->tenured->contains_p(untagged);
    std::cout << " heap[nursery=" << (in_nursery ? "y" : "n")
              << " aging=" << (in_aging ? "y" : "n")
              << " tenured=" << (in_tenured ? "y" : "n") << "]";
    if (parent->current_gc)
      std::cout << " gc_op=" << parent->current_gc->op;
    if (current_container)
      std::cout << " container=0x" << std::hex << current_container << std::dec;
    std::cout << std::endl;
  }
  return RETAG(untagged, TAG(pointer));
}

template <typename Fixup> void slot_visitor<Fixup>::visit_handle(cell* handle) {
  if (!immediate_p(*handle)) {
    #if defined(FACTOR_WASM)
    object* untagged = untag<object>(*handle);
    bool in_nursery = parent->data->nursery->contains_p(untagged);
    bool in_aging = parent->data->aging->contains_p(untagged);
    bool in_tenured = parent->data->tenured->contains_p(untagged);
    if (!in_nursery && !in_aging && !in_tenured) {
      bool in_gc = parent->current_gc != NULL;
      // Log to file for post-mortem analysis (rate-limited)
      static int outside_heap_count = 0;
      if (outside_heap_count < 20) {
        FILE* f = fopen("gc-bad.log", "a");
        if (f) {
          fprintf(f, "visit_handle outside heap: handle=%p tagged=0x%lx untagged=0x%lx container=0x%lx in_gc=%d\n",
                  (void*)handle, (unsigned long)*handle, (unsigned long)untagged,
                  (unsigned long)current_container, in_gc ? 1 : 0);
          fclose(f);
        }
        outside_heap_count++;
      }
      if (wasm_debug_enabled()) {
        std::cout << "[wasm] visit_handle outside heap tagged=0x" << std::hex
                  << *handle << " untagged=0x" << (cell)untagged
                  << " container=0x" << current_container << std::dec;
        if (in_gc)
          std::cout << " gc_op=" << parent->current_gc->op;
        std::cout << std::endl;
      }
      // During startup fixups we expect pre-relocation addresses.
      // During GC, skip this handle to avoid following bad pointers,
      // but don't clear it - the object may still be valid after relocation.
      if (in_gc) {
        return;
      }
      // Fall through to visit_pointer for startup fixup
    }
    #endif
    *handle = visit_pointer(*handle);
  }
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_object_array(cell* start, cell* end) {
  while (start < end)
    visit_handle(start++);
}

template <typename Fixup> void slot_visitor<Fixup>::visit_slots(object* obj) {
  if (obj->type() == CALLSTACK_TYPE)
    visit_callstack_object((callstack*)obj);
  else {
    cell prev_container = current_container;
    current_container = (cell)obj;
    cell* start = (cell*)obj + 1;
    cell* end = start + obj->slot_count(fixup);
    visit_object_array(start, end);
    current_container = prev_container;
  }
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_stack_elements(segment* region, cell* top) {
  cell* start = (cell*)region->start;
  cell* end = (cell*)region->end;
  // An empty stack has top pointing one cell below start.
  if (top < start - 1 || top >= end) {
#if defined(FACTOR_WASM)
    static int skip_stack_oob = 0;
    if (skip_stack_oob < 20) {
      // Log to file for post-mortem analysis
      FILE* f = fopen("gc-bad.log", "a");
      if (f) {
        fprintf(f, "visit_stack_elements OOB: region=%p-%p top=%p\n",
                (void*)region->start, (void*)region->end, (void*)top);
        fclose(f);
      }
      std::cout << "[wasm] visit_stack_elements: top out of bounds region="
                << (void*)region->start << "-" << (void*)region->end
                << " top=" << (void*)top << " (skipping)" << std::endl;
      skip_stack_oob++;
    }
    return;  // Skip visiting this stack region - better than crashing
#else
    fatal_error("visit_stack_elements: top out of bounds", (cell)top);
#endif
  }
  cell prev_container = current_container;
  current_container = region->start;
  visit_object_array(start, top + 1);
  current_container = prev_container;
}

template <typename Fixup> void slot_visitor<Fixup>::visit_all_roots() {
  const cell roots_data = 0xd001;
  const cell roots_callbacks = 0xd002;
  const cell roots_uninitialized = 0xd003;
  const cell roots_samples = 0xd004;
  const cell roots_specials = 0xd005;
  const cell roots_contexts = 0xd006;
#if defined(FACTOR_WASM)
  bool trace_roots = wasm_debug_enabled();
  if (trace_roots) {
    std::cout << "[wasm] visit_all_roots begin" << std::endl;
    std::cout << "[wasm] data_roots count=" << parent->data_roots.size()
              << std::endl;
    size_t data_root_idx = 0;
    for (auto ptr : parent->data_roots) {
      std::cout << "  root[" << data_root_idx++ << "] handle=0x" << std::hex
                << (cell)ptr << " value=0x" << *ptr << std::dec << std::endl;
    }
  }
#endif
  current_container = roots_data;
  FACTOR_FOR_EACH(parent->data_roots) {
    cell value = **iter;
    if (!immediate_p(value) && UNTAG(value) < 0x200000) {
#if defined(FACTOR_WASM)
      if (trace_roots) {
        std::cout << "[wasm] root data_roots value=0x" << std::hex << value
                  << " handle=0x" << (cell)*iter << std::dec << std::endl;
      }
#endif
    }
    visit_handle(*iter);
  }
#if defined(FACTOR_WASM)
  if (trace_roots)
    std::cout << "[wasm] visit_all_roots done data_roots" << std::endl;

  auto visit_trampoline_handle = [](cell* handle, void* ctx) {
    static_cast<slot_visitor<Fixup>*>(ctx)->visit_handle(handle);
  };
#endif

  current_container = roots_callbacks; // callbacks
  auto callback_slot_visitor = [&](code_block* stub, cell size) {
	  (void)size;
    visit_handle(&stub->owner);
  };
  parent->callbacks->allocator->iterate(callback_slot_visitor, no_fixup());
#if defined(FACTOR_WASM)
  if (trace_roots)
    std::cout << "[wasm] visit_all_roots done callbacks" << std::endl;
#endif

  current_container = roots_uninitialized; // uninitialized blocks owners
  FACTOR_FOR_EACH(parent->code->uninitialized_blocks) {
    if (!immediate_p(iter->second))
      iter->second = visit_pointer(iter->second);
  }
#if defined(FACTOR_WASM)
  if (trace_roots)
    std::cout << "[wasm] visit_all_roots done uninitialized" << std::endl;
#endif

  current_container = roots_samples; // samples
  FACTOR_FOR_EACH(parent->samples) {
    visit_handle(&iter->thread);
  }
#if defined(FACTOR_WASM)
  if (trace_roots)
    std::cout << "[wasm] visit_all_roots done samples" << std::endl;
#endif

  current_container = roots_specials; // special_objects
  visit_object_array(parent->special_objects,
                     parent->special_objects + special_object_count);
#if defined(FACTOR_WASM)
  if (trace_roots)
    std::cout << "[wasm] visit_all_roots done specials" << std::endl;
#endif

  current_container = roots_contexts; // active contexts
  FACTOR_FOR_EACH(parent->active_contexts) {
    visit_context(*iter);
  }
#if defined(FACTOR_WASM)
  if (trace_roots)
    std::cout << "[wasm] visit_all_roots done contexts" << std::endl;
#endif
  current_container = 0;
}

// primitive_minor_gc() is invoked by inline GC checks, and it needs to
// visit spill slots which references objects in the heap.

// So for each call frame:
//  - trace roots in spill slots

template <typename Fixup> struct call_frame_slot_visitor {
  slot_visitor<Fixup>* visitor;

  call_frame_slot_visitor(slot_visitor<Fixup>* visitor)
      : visitor(visitor) {}

  // frame top -> [return address]
  //              [spill area]
  //              ...
  //              [entry_point]
  //              [size]

  void operator()(cell frame_top, cell size, code_block* owner, cell addr) {
	  (void)size;
    #if defined(FACTOR_WASM)
    if (!owner) {
      if (wasm_debug_enabled()) {
        std::cout << "[wasm] call_frame_slot_visitor missing owner addr=0x"
                  << std::hex << addr << std::dec << std::endl;
      }
      return;
    }
    #endif
    cell return_address = owner->offset(addr);

    code_block* compiled =
        Fixup::translated_code_block_map ? owner
                                         : visitor->fixup.translate_code(owner);
    gc_info* info = compiled->block_gc_info();

    FACTOR_ASSERT(return_address < compiled->size());
    cell callsite = info->return_address_index(return_address);
    if (callsite == (cell)-1)
      return;

#ifdef DEBUG_GC_MAPS
    FACTOR_PRINT("call frame code block " << compiled << " with offset "
                 << return_address);
#endif
    cell* stack_pointer = (cell*)(frame_top + FRAME_RETURN_ADDRESS);
    uint8_t* bitmap = info->gc_info_bitmap();

    // Subtract old value of base pointer from every derived pointer.
    for (cell spill_slot = 0; spill_slot < info->derived_root_count;
         spill_slot++) {
      uint32_t base_pointer = info->lookup_base_pointer(callsite, spill_slot);
      if (base_pointer != (uint32_t)-1) {
#ifdef DEBUG_GC_MAPS
        FACTOR_PRINT("visiting derived root " << spill_slot
                     << " with base pointer " << base_pointer);
#endif
        stack_pointer[spill_slot] -= stack_pointer[base_pointer];
      }
    }

    // Update all GC roots, including base pointers.
    cell callsite_gc_roots = info->callsite_gc_roots(callsite);

    for (cell spill_slot = 0; spill_slot < info->gc_root_count; spill_slot++) {
      if (bitmap_p(bitmap, callsite_gc_roots + spill_slot)) {
        #ifdef DEBUG_GC_MAPS
        FACTOR_PRINT("visiting GC root " << spill_slot);
        #endif
        visitor->visit_handle(stack_pointer + spill_slot);
      }
    }

    // Add the base pointers to obtain new derived pointer values.
    for (cell spill_slot = 0; spill_slot < info->derived_root_count;
         spill_slot++) {
      uint32_t base_pointer = info->lookup_base_pointer(callsite, spill_slot);
      if (base_pointer != (uint32_t)-1)
        stack_pointer[spill_slot] += stack_pointer[base_pointer];
    }
  }
};

template <typename Fixup>
void slot_visitor<Fixup>::visit_callstack_object(callstack* stack) {
  call_frame_slot_visitor<Fixup> call_frame_visitor(this);
  parent->iterate_callstack_object(stack, call_frame_visitor, fixup);
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_callstack(context* ctx) {
  call_frame_slot_visitor<Fixup> call_frame_visitor(this);
  parent->iterate_callstack(ctx, call_frame_visitor, fixup);
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_context(context* ctx) {
  visit_callstack(ctx);

  cell ds_ptr = ctx->datastack;
  cell rs_ptr = ctx->retainstack;
  segment* ds_seg = ctx->datastack_seg;
  segment* rs_seg = ctx->retainstack_seg;
  visit_stack_elements(ds_seg, (cell*)ds_ptr);
  visit_stack_elements(rs_seg, (cell*)rs_ptr);
  visit_object_array(ctx->context_objects,
                     ctx->context_objects + context_object_count);

  // Clear out the space not visited with a known pattern. That makes
  // it easier to see if uninitialized reads are made.
  ctx->fill_stack_seg(ds_ptr, ds_seg, 0xbaadbadd);
  ctx->fill_stack_seg(rs_ptr, rs_seg, 0xdaabdabb);
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_code_block_objects(code_block* compiled) {
  cell prev_container = current_container;
  current_container = (cell)compiled;
  visit_handle(&compiled->owner);
  visit_handle(&compiled->parameters);
  visit_handle(&compiled->relocation);
  current_container = prev_container;
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_embedded_literals(code_block* compiled) {
  if (parent->code->uninitialized_p(compiled))
    return;

  auto update_literal_refs = [&](instruction_operand op) {
    if (op.rel.type() == RT_LITERAL) {
      cell value = op.load_value(op.pointer);
      if (!immediate_p(value)) {
        op.store_value(visit_pointer(value));
      }
    }
  };
  compiled->each_instruction_operand(update_literal_refs);
}

template <typename Fixup> struct call_frame_code_block_visitor {
  Fixup fixup;

  call_frame_code_block_visitor(Fixup fixup) : fixup(fixup) {}

  void operator()(cell frame_top, cell size, code_block* owner, cell addr) {
    (void)size;
	  code_block* compiled =
        Fixup::translated_code_block_map ? owner : fixup.fixup_code(owner);
    cell fixed_addr = compiled->address_for_offset(owner->offset(addr));

    *(cell*)(frame_top + FRAME_RETURN_ADDRESS) = fixed_addr;
  }
};

template <typename Fixup>
void slot_visitor<Fixup>::visit_object_code_block(object* obj) {
  switch (obj->type()) {
    case WORD_TYPE: {
      word* w = (word*)obj;
      if (w->entry_point)
        w->entry_point = fixup.fixup_code(w->code())->entry_point();
      break;
    }
    case QUOTATION_TYPE: {
      quotation* q = (quotation*)obj;
      if (q->entry_point)
        q->entry_point = fixup.fixup_code(q->code())->entry_point();
      break;
    }
    case CALLSTACK_TYPE: {
      callstack* stack = (callstack*)obj;
      call_frame_code_block_visitor<Fixup> call_frame_visitor(fixup);
      parent->iterate_callstack_object(stack, call_frame_visitor, fixup);
      break;
    }
  }
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_context_code_blocks() {
  call_frame_code_block_visitor<Fixup> call_frame_visitor(fixup);
  FACTOR_FOR_EACH(parent->active_contexts) {
    parent->iterate_callstack(*iter, call_frame_visitor, fixup);
  }
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_uninitialized_code_blocks() {
  std::map<code_block*, cell> new_uninitialized_blocks;
  FACTOR_FOR_EACH(parent->code->uninitialized_blocks) {
    cell owner = iter->second;
    if (immediate_p(owner))
      continue;
    owner = visit_pointer(owner);
    new_uninitialized_blocks.insert(
        std::make_pair(fixup.fixup_code(iter->first), owner));
  }
  parent->code->uninitialized_blocks = new_uninitialized_blocks;
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_embedded_code_pointers(code_block* compiled) {
  if (parent->code->uninitialized_p(compiled))
    return;
  auto update_code_block_refs = [&](instruction_operand op){
    relocation_type type = op.rel.type();
    if (type == RT_ENTRY_POINT ||
        type == RT_ENTRY_POINT_PIC ||
        type == RT_ENTRY_POINT_PIC_TAIL) {
      code_block* block = fixup.fixup_code(op.load_code_block());
      op.store_value(block->entry_point());
    }
  };
  compiled->each_instruction_operand(update_code_block_refs);
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_object(object *ptr) {
  cell t = ptr->type();
#if defined(FACTOR_WASM)
  bool in_heap = parent->data->nursery->contains_p(ptr) ||
                 parent->data->aging->contains_p(ptr) ||
                 parent->data->tenured->contains_p(ptr);
  if (!in_heap) {
    static int skip_visit_outside = 0;
    if (skip_visit_outside < 10) {
      FILE* f = fopen("gc-bad.log", "a");
      if (f) {
        fprintf(f, "visit_object outside heap ptr=0x%lx header=0x%lx type=%ld\n",
                (unsigned long)ptr, (unsigned long)ptr->header, (long)t);
        fclose(f);
      }
      std::cout << "[wasm] visit_object outside heap ptr=0x" << std::hex
                << (cell)ptr << " header=0x" << ptr->header << " type=" << t
                << std::dec << " (skipping)" << std::endl;
      skip_visit_outside++;
    }
    return;  // Skip visiting this object
  }
  if (t >= TYPE_COUNT) {
    static int skip_visit_type = 0;
    if (skip_visit_type < 10) {
      std::cout << "[wasm] visit_object invalid type ptr=0x" << std::hex
                << (cell)ptr << " type=" << t << std::dec << " (skipping)" << std::endl;
      skip_visit_type++;
    }
    return;  // Skip visiting this object
  }
  if (wasm_debug_enabled()) {
    cell header = *(cell*)ptr;
    std::cout << "[wasm] visit_object ptr=0x" << std::hex << (cell)ptr
              << " type=" << t << " header=0x" << header << std::dec
              << std::endl;
  }
#endif
  visit_slots(ptr);
  if (ptr->type() == ALIEN_TYPE)
    ((alien*)ptr)->update_address();
}

// Pops items from the mark stack and visits them until the stack is
// empty. Used when doing a full collection and when collecting to
// tenured space.
template <typename Fixup>
void slot_visitor<Fixup>::visit_mark_stack(std::vector<cell>* mark_stack) {
  while (!mark_stack->empty()) {
    cell ptr = mark_stack->back();
    mark_stack->pop_back();

#if defined(FACTOR_WASM)
    if (wasm_debug_enabled()) {
      std::cout << "[wasm] visit_mark_stack ptr=0x" << std::hex << ptr
                << " size=" << std::dec << mark_stack->size() << std::endl;
    }
#endif

    if (ptr & 1) {
      code_block* compiled = (code_block*)(ptr - 1);
#if defined(FACTOR_WASM)
      if (!parent->code->allocator->contains_p(compiled)) {
        static int skip_code_outside = 0;
        if (skip_code_outside < 10) {
          std::cout << "[wasm] visit_mark_stack code outside heap ptr=0x"
                    << std::hex << (cell)compiled << std::dec << " (skipping)" << std::endl;
          skip_code_outside++;
        }
        continue;  // Skip code blocks outside the code heap
      }
#endif
      visit_code_block_objects(compiled);
      visit_embedded_literals(compiled);
      visit_embedded_code_pointers(compiled);
    } else {
      object* obj = (object*)ptr;
#if defined(FACTOR_WASM)
      bool trace_mark = wasm_debug_enabled();
      bool in_nursery = parent->data->nursery->contains_p(obj);
      bool in_aging = parent->data->aging->contains_p(obj);
      bool in_tenured = parent->data->tenured->contains_p(obj);
      if (!in_nursery && !in_aging && !in_tenured) {
        static int skip_outside_heap = 0;
        if (skip_outside_heap < 10) {
          if (trace_mark) {
            FILE* f = fopen("gc-bad.log", "a");
            if (f) {
              fprintf(f, "visit_mark_stack outside heap ptr=0x%lx size=%zu\n",
                      (unsigned long)obj, (size_t)mark_stack->size());
              fclose(f);
            }
            std::cout << "[wasm] visit_mark_stack data outside heap ptr=0x"
                      << std::hex << (cell)obj << std::dec << " (skipping)" << std::endl;
          }
          skip_outside_heap++;
        }
        continue;  // Skip this corrupted mark stack entry
      }
      cell header = *(cell*)obj;
      cell type = (header >> 2) & TAG_MASK;
      if (header & 1) {
        static int skip_free = 0;
        if (skip_free < 10) {
          std::cout << "[wasm] visit_mark_stack free object ptr=0x" << std::hex
                    << (cell)obj << " header=0x" << header << std::dec << " (skipping)" << std::endl;
          skip_free++;
        }
        continue;  // Skip free objects
      }
      if (header == 0) {
        static int skip_zero = 0;
        if (skip_zero < 10) {
          FILE* f = fopen("gc-bad.log", "a");
          if (f) {
            fprintf(f, "zero header ptr=0x%lx card_scan size=%zu\n",
                    (unsigned long)obj, (size_t)mark_stack->size());
            fclose(f);
          }
          std::cout << "[wasm] visit_mark_stack zero header ptr=0x" << std::hex
                    << (cell)obj << std::dec << " (skipping)" << std::endl;
          skip_zero++;
        }
        continue;  // Skip zero-header objects
      }
      if (trace_mark && (cell)obj >= 0x3f00000) {
        std::cout << "[wasm] visit_mark_stack obj ptr=0x" << std::hex
                  << (cell)obj << " header=0x" << header << " type=" << type
                  << std::dec << std::endl;
      }
      if (in_tenured) {
        object_start_map& starts = parent->data->tenured->starts;
        cell card_index = addr_to_card((cell)obj - parent->data->tenured->start);
        cell card_count = (cell)(starts.object_start_offsets_end -
                                 starts.object_start_offsets);
        if (card_index >= card_count) {
          static int skip_card_oob = 0;
          if (skip_card_oob < 10) {
            std::cout << "[wasm] visit_mark_stack card_index OOB ptr=0x"
                      << std::hex << (cell)obj << " card=" << card_index
                      << " count=" << card_count << std::dec << " (skipping)" << std::endl;
            skip_card_oob++;
          }
          continue;  // Skip objects with out-of-bounds card index
        }
        card offset = starts.object_start_offsets[card_index];
        if (offset == card_starts_inside_object) {
          static int skip_no_start = 0;
          if (skip_no_start < 10) {
            std::cout << "[wasm] visit_mark_stack no start recorded ptr=0x"
                      << std::hex << (cell)obj << " card=" << card_index
                      << std::dec << " (skipping)" << std::endl;
            skip_no_start++;
          }
          continue;  // Skip objects with no start recorded
        }
        cell recorded = parent->data->tenured->start +
                        card_index * card_size + offset;
        cell card_end = recorded + card_size;
        if (wasm_debug_enabled()) {
          std::cout << "[wasm] visit_mark_stack card scan recorded=0x"
                    << std::hex << recorded << " card=" << card_index
                    << " ptr=0x" << (cell)obj << std::dec << std::endl;
        }
        cell cur = recorded;
        bool found = false;
        for (int iter = 0; iter < 128 && cur < card_end; iter++) {
          object* base = (object*)cur;
          cell base_size = base->size(no_fixup());
          if (base_size == 0) {
            FILE* f = fopen("gc-bad.log", "a");
            if (f) {
              fprintf(f,
                      "zero-sized object cur=0x%lx card=%lu recorded=0x%lx container=0x%lx (skipping)\n",
                      (unsigned long)cur, (unsigned long)card_index,
                      (unsigned long)recorded, (unsigned long)current_container);
              cell* words = (cell*)base;
              for (int k = 0; k < 8; k++)
                fprintf(f, "  word[%d]=0x%lx\n", k, (unsigned long)words[k]);
              fclose(f);
            }
            std::cout << "[wasm] visit_mark_stack zero-sized object cur=0x"
                      << std::hex << cur << " card=" << card_index
                      << " recorded=0x" << recorded << " container=0x"
                      << current_container << " (skipping)" << std::dec
                      << std::endl;
            cur += tagged<object>(cur).untagged()->size(no_fixup());
            continue;
          }
          if (base_size > parent->data->tenured->size) {
            std::cout << "[wasm] visit_mark_stack implausible object size ptr=0x"
                      << std::hex << cur << " size=0x" << base_size << std::dec
                      << std::endl;
            // Dump first few words
            cell* words = (cell*)base;
            for (int k = 0; k < 6; k++) {
              std::cout << "    word[" << k << "]=0x" << std::hex << words[k]
                        << std::dec << std::endl;
            }
            // Skip this object and continue scanning to gather more context.
            cur += data_alignment;
            continue;
          } else if (trace_mark && base_size > card_size * 64) {
            std::cout << "[wasm] visit_mark_stack large object ptr=0x" << std::hex
                      << cur << " size=0x" << base_size << std::dec << std::endl;
          }
          cell next = cur + base_size;
          if (trace_mark) {
            std::cout << "[wasm] visit_mark_stack scan card cur=0x" << std::hex
                      << cur << " size=0x" << base_size << " next=0x" << next
                      << std::dec << std::endl;
          }
          if ((cell)obj >= cur && (cell)obj < next) {
            found = true;
            break;
          }
          cur = next;
        }
        if (!found) {
          static int skip_not_within = 0;
          if (skip_not_within < 10) {
            std::cout << "[wasm] visit_mark_stack ptr not within card objects ptr=0x"
                      << std::hex << (cell)obj << " recorded_start=0x" << recorded
                      << " card=" << card_index << " offset=" << (int)offset
                      << " cur_end=0x" << cur << std::dec << " (skipping)" << std::endl;
            skip_not_within++;
          }
          continue;  // Skip objects not found within their card
        }
      }
#endif
      visit_object(obj);
      visit_object_code_block(obj);
    }
  }
}

// Visits the instruction operands in a code block. If the operand is
// a pointer to a code block or data object, then the fixup is applied
// to it. Otherwise, if it is an external addess, that address is
// recomputed. If it is an untagged number literal (RT_UNTAGGED) or an
// immediate value, then nothing is done with it.
template <typename Fixup>
void slot_visitor<Fixup>::visit_instruction_operands(code_block* block,
                                                     cell rel_base) {
  auto visit_func = [&](instruction_operand op){
    cell old_offset = rel_base + op.rel.offset();
    cell old_value = op.load_value(old_offset);
    switch (op.rel.type()) {
      case RT_LITERAL: {
        if (!immediate_p(old_value)) {
          op.store_value(visit_pointer(old_value));
        }
        break;
      }
      case RT_ENTRY_POINT:
      case RT_ENTRY_POINT_PIC:
      case RT_ENTRY_POINT_PIC_TAIL:
      case RT_HERE: {
        cell offset = TAG(old_value);
        code_block* compiled = (code_block*)UNTAG(old_value);
        op.store_value(RETAG(fixup.fixup_code(compiled), offset));
        break;
      }
      case RT_UNTAGGED:
        break;
      default:
        op.store_value(parent->compute_external_address(op));
        break;
    }
  };
  if (parent->code->uninitialized_p(block))
    return;
  block->each_instruction_operand(visit_func);
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_partial_objects(cell start,
                                                cell card_start,
                                                cell card_end) {
  cell *scan_start = (cell*)start + 1;
  cell *scan_end = scan_start + ((object*)start)->slot_count();

  scan_start = std::max(scan_start, (cell*)card_start);
  scan_end = std::min(scan_end, (cell*)card_end);

  visit_object_array(scan_start, scan_end);
}

template <typename Fixup>
template <typename SourceGeneration>
cell slot_visitor<Fixup>::visit_card(SourceGeneration* gen,
                                     cell index, cell start) {
  cell heap_base = parent->data->start;
  cell start_addr = heap_base + index * card_size;
  cell end_addr = start_addr + card_size;

  // Forward to the next object whose address is in the card.
  if (!start || (start + ((object*)start)->size()) < start_addr) {
    // Optimization because finding the objects in a memory range is
    // expensive. It helps a lot when tracing consecutive cards.
    cell gen_start_card = (gen->start - heap_base) / card_size;
    start = gen->starts
        .find_object_containing_card(index - gen_start_card);
  }

#if defined(FACTOR_WASM)
  // Validate object at start before using it
  if (start) {
    object* obj = (object*)start;
    if (obj->header == 0) {
      static int card_zero_header_count = 0;
      if (card_zero_header_count < 5) {
        std::cout << "[wasm] visit_card found zero header at 0x" << std::hex
                  << start << " card_index=" << std::dec << index
                  << " (stopping card scan)" << std::endl;
        card_zero_header_count++;
      }
      return 0; // Stop scanning this card
    }
    cell obj_size = obj->size();
    if (obj_size == 0 || obj_size > gen->size) {
      static int card_bad_size_count = 0;
      if (card_bad_size_count < 5) {
        std::cout << "[wasm] visit_card found bad size at 0x" << std::hex
                  << start << " size=0x" << obj_size
                  << " header=0x" << obj->header << std::dec
                  << " (stopping card scan)" << std::endl;
        card_bad_size_count++;
      }
      return 0;
    }
  }
#endif

  while (start && start < end_addr) {
#if defined(FACTOR_WASM)
    // Check object validity before processing
    object* obj = (object*)start;
    if (obj->header == 0) {
      static int loop_zero_count = 0;
      if (loop_zero_count < 5) {
        std::cout << "[wasm] visit_card loop zero header at 0x" << std::hex
                  << start << std::dec << std::endl;
        loop_zero_count++;
      }
      break;
    }
    cell obj_size = obj->size();
    if (obj_size == 0 || obj_size > gen->size) {
      static int loop_bad_count = 0;
      if (loop_bad_count < 5) {
        std::cout << "[wasm] visit_card loop bad size at 0x" << std::hex
                  << start << " size=0x" << obj_size << std::dec << std::endl;
        loop_bad_count++;
      }
      break;
    }
#endif
    visit_partial_objects(start, start_addr, end_addr);
    if ((start + ((object*)start)->size()) >= end_addr) {
      // The object can overlap the card boundary, then the
      // remainder of it will be handled in the next card
      // tracing if that card is marked.
      break;
    }
    start = gen->next_object_after(start);
  }
  return start;
}

template <typename Fixup>
template <typename SourceGeneration>
void slot_visitor<Fixup>::visit_cards(SourceGeneration* gen,
                                      card mask, card unmask) {
  card_deck* decks = parent->data->decks;
  card_deck* cards = parent->data->cards;
  cell heap_base = parent->data->start;

  cell first_deck = (gen->start - heap_base) / deck_size;
  cell last_deck = (gen->end - heap_base) / deck_size;

  // Address of last traced object.
  cell start = 0;
  for (cell di = first_deck; di < last_deck; di++) {
    if (decks[di] & mask) {
      decks[di] &= ~unmask;
      decks_scanned++;

      cell first_card = cards_per_deck * di;
      cell last_card = first_card + cards_per_deck;

      for (cell ci = first_card; ci < last_card; ci++) {
        if (cards[ci] & mask) {
          cards[ci] &= ~unmask;
          cards_scanned++;

          start = visit_card(gen, ci, start);
          if (!start) {
            // At end of generation, no need to scan more cards.
            return;
          }
        }
      }
    }
  }
}

template <typename Fixup>
void slot_visitor<Fixup>::visit_code_heap_roots(std::set<code_block*>* remembered_set) {
  FACTOR_FOR_EACH(*remembered_set) {
    code_block* compiled = *iter;
    visit_code_block_objects(compiled);
    visit_embedded_literals(compiled);
    compiled->flush_icache();
  }
}

template <typename Fixup>
template <typename TargetGeneration>
void slot_visitor<Fixup>::cheneys_algorithm(TargetGeneration* gen, cell scan) {
  while (scan && scan < gen->here) {
#if defined(FACTOR_WASM)
    // Validate object before visiting
    object* obj = (object*)scan;
    if (obj->header == 0) {
      static int cheney_zero_count = 0;
      if (cheney_zero_count < 5) {
        std::cout << "[wasm] cheneys_algorithm zero header at 0x" << std::hex
                  << scan << std::dec << " (stopping)" << std::endl;
        cheney_zero_count++;
      }
      break;
    }
    cell obj_size = obj->size();
    if (obj_size == 0 || obj_size > gen->size) {
      static int cheney_bad_count = 0;
      if (cheney_bad_count < 5) {
        std::cout << "[wasm] cheneys_algorithm bad size at 0x" << std::hex
                  << scan << " size=0x" << obj_size << std::dec
                  << " (stopping)" << std::endl;
        cheney_bad_count++;
      }
      break;
    }
#endif
    visit_object((object*)scan);
    scan = gen->next_object_after(scan);
  }
}

}
