#include "master.hpp"

namespace factor {

struct full_collection_copier : no_fixup {
  tenured_space* tenured;
  code_heap* code;
  std::vector<cell> *mark_stack;
  static const cell suspicious_lo = 0x468000;
  static const cell suspicious_hi = 0x469000;

  full_collection_copier(tenured_space* tenured,
                         code_heap* code,
                         std::vector<cell> *mark_stack)
      : tenured(tenured), code(code), mark_stack(mark_stack) { }

  object* fixup_data(object* obj) {
    // Check for invalid header early - zero headers indicate corruption
    #if defined(FACTOR_WASM)
    if (obj->header == 0) {
      static int zero_header_count = 0;
      if (zero_header_count < 10) {
        std::cout << "[wasm] full_gc fixup_data ZERO HEADER ptr=0x" << std::hex
                  << (cell)obj << std::dec << " (skipping)" << std::endl;
        zero_header_count++;
      }
      // Return obj unchanged - we cannot safely process this
      return obj;
    }
    
    // WASM: Check containment BEFORE computing size to avoid reading garbage
    factor_vm* vm = current_vm_p();
    if (vm) {
      bool in_nursery = vm->data->nursery->contains_allocated_p(obj);
      bool in_aging = vm->data->aging->contains_allocated_p(obj);
      bool in_tenured = tenured->contains_p(obj);
      if (!in_nursery && !in_aging && !in_tenured) {
        static int outside_heap_count = 0;
        if (outside_heap_count < 10 && std::getenv("FACTOR_WASM_TRACE")) {
          std::cout << "[wasm] full_gc fixup_data outside allocated ptr=0x"
                    << std::hex << (cell)obj << std::dec << " (skipping)" << std::endl;
          outside_heap_count++;
        }
        // Skip - return obj unchanged
        return obj;
      }
    }
    #endif
    cell sz = obj->size(no_fixup());
    #if defined(FACTOR_WASM)
    bool trace = std::getenv("FACTOR_WASM_TRACE") != nullptr;
    if (vm) {
      if (sz > vm->data->tenured->size || sz == 0) {
        static int implausible_count = 0;
        if (implausible_count < 10) {
          std::cout << "[wasm] full_gc fixup_data implausible size ptr=0x"
                    << std::hex << (cell)obj << " size=0x" << sz
                    << " header=0x" << obj->header << std::dec
                    << " (skipping)" << std::endl;
          cell* words = (cell*)obj;
          for (int k = 0; k < 6; k++) {
            std::cout << "    word[" << k << "]=0x" << std::hex << words[k]
                      << std::dec << std::endl;
          }
          implausible_count++;
        }
        // Skip instead of fatal
        return obj;
      }
      static const cell suspicious_lo = 0x468000;
      static const cell suspicious_hi = 0x469000;
      bool suspicious = ((cell)obj >= suspicious_lo && (cell)obj < suspicious_hi);
      if (trace && (suspicious || ((cell)obj & 0xFFF) == 0)) {
        std::cout << "[wasm] full_gc fixup_data obj=0x" << std::hex
                  << (cell)obj << " marked_p=" << std::dec
                  << tenured->state.marked_p((cell)obj) << std::endl;
      }
      if (trace && sz > 0x1000) {
        std::cout << "[wasm] full_gc fixup_data large obj=0x" << std::hex
                  << (cell)obj << " size=0x" << sz << " marked="
                  << std::dec << tenured->state.marked_p((cell)obj) << std::endl;
        cell* words = (cell*)obj;
        for (int k = 0; k < 4; k++) {
          std::cout << "    word[" << k << "]=0x" << std::hex << words[k]
                    << std::dec << std::endl;
        }
      }
    }
    #endif
    if (tenured->contains_p(obj)) {
      if (!tenured->state.marked_p((cell)obj)) {
        tenured->state.set_marked_p((cell)obj, sz);
        mark_stack->push_back((cell)obj);
        #if defined(FACTOR_WASM)
        if (trace && ((cell)obj >= suspicious_lo && (cell)obj < suspicious_hi)) {
          std::cout << "[wasm] full_gc push existing obj=0x" << std::hex
                    << (cell)obj << " size=0x" << sz
                    << " mark_stack=" << std::dec << mark_stack->size()
                    << std::endl;
        }
        #endif
      }
      return obj;
    }

    // Is there another forwarding pointer?
    while (obj->forwarding_pointer_p()) {
      object* dest = obj->forwarding_pointer();
      obj = dest;
    }

    if (tenured->contains_p(obj)) {
      if (!tenured->state.marked_p((cell)obj)) {
        tenured->state.set_marked_p((cell)obj, sz);
        mark_stack->push_back((cell)obj);
        #if defined(FACTOR_WASM)
        if (trace && sz > 0x1000) {
          std::cout << "[wasm] full_gc push existing large obj=0x" << std::hex
                    << (cell)obj << " size=0x" << sz << " mark_stack="
                    << std::dec << mark_stack->size() << std::endl;
        }
        #endif
      }
      return obj;
    }

    cell size = obj->size();
    object* newpointer = tenured->allot(size);
    if (!newpointer) {
#if defined(FACTOR_WASM)
      fatal_error("Out of tenured space on wasm", size);
#else
      throw must_start_gc_again();
#endif
    }
    memcpy(newpointer, obj, size);
    obj->forward_to(newpointer);

    tenured->state.set_marked_p((cell)newpointer, newpointer->size());
    mark_stack->push_back((cell)newpointer);
    #if defined(FACTOR_WASM)
    if (trace &&
        (((cell)newpointer >= suspicious_lo && (cell)newpointer < suspicious_hi) ||
         ((cell)obj >= suspicious_lo && (cell)obj < suspicious_hi))) {
      std::cout << "[wasm] full_gc copied obj=0x" << std::hex << (cell)obj
                << " -> 0x" << (cell)newpointer << " size=0x" << size
                << " mark_stack=" << std::dec << mark_stack->size()
                << std::endl;
    }
    if (trace && size > 0x1000) {
      std::cout << "[wasm] full_gc push large new obj=0x" << std::hex
                << (cell)newpointer << " from=0x" << (cell)obj
                << " size=0x" << size << " mark_stack=" << std::dec
                << mark_stack->size() << std::endl;
    }
    #endif
    return newpointer;
  }

  code_block* fixup_code(code_block* compiled) {
    #if defined(FACTOR_WASM)
    if (!code->allocator->contains_p(compiled)) {
      std::cout << "[wasm] full_gc fixup_code outside code heap ptr=0x"
                << std::hex << (cell)compiled << std::dec << std::endl;
      fatal_error("full_gc fixup_code outside code heap", (cell)compiled);
    }
    #endif
    if (!code->allocator->state.marked_p((cell)compiled)) {
      code->allocator->state.set_marked_p((cell)compiled, compiled->size());
      mark_stack->push_back((cell)compiled + 1);
    }
    return compiled;
  }
};

void factor_vm::collect_mark_impl() {
  gc_event* event = current_gc->event;
  if (event)
    event->reset_timer();

  slot_visitor<full_collection_copier>
      visitor(this, full_collection_copier(data->tenured, code, &mark_stack));

  mark_stack.clear();

  code->allocator->state.clear_mark_bits();
  data->tenured->state.clear_mark_bits();

  visitor.visit_all_roots();
  #if defined(FACTOR_WASM)
  if (std::getenv("FACTOR_WASM_TRACE"))
    std::cout << "[wasm] collect_mark_impl after roots mark_stack="
              << mark_stack.size() << std::endl;
  #endif
  visitor.visit_context_code_blocks();
  #if defined(FACTOR_WASM)
  if (std::getenv("FACTOR_WASM_TRACE"))
    std::cout << "[wasm] collect_mark_impl after context code mark_stack="
              << mark_stack.size() << std::endl;
  #endif
  visitor.visit_uninitialized_code_blocks();
  #if defined(FACTOR_WASM)
  if (std::getenv("FACTOR_WASM_TRACE"))
    std::cout << "[wasm] collect_mark_impl after uninitialized mark_stack="
              << mark_stack.size() << std::endl;
  #endif

  visitor.visit_mark_stack(&mark_stack);
  #if defined(FACTOR_WASM)
  if (std::getenv("FACTOR_WASM_TRACE"))
    std::cout << "[wasm] collect_mark_impl after mark_stack mark_stack="
              << mark_stack.size() << std::endl;
  #endif

  data->reset_tenured();
  data->reset_aging();
  data->reset_nursery();
  code->clear_remembered_set();

  if (event)
    event->ended_phase(PHASE_MARKING);
}

void factor_vm::collect_sweep_impl() {
  gc_event* event = current_gc->event;
  if (event)
    event->reset_timer();
  data->tenured->sweep();
  if (event)
    event->ended_phase(PHASE_DATA_SWEEP);

  // After a sweep, invalidate any code heap roots which are not
  // marked, so that if a block makes a tail call to a generic word,
  // and the PIC compiler triggers a GC, and the caller block gets GCd
  // as a result, the PIC code won't try to overwrite the call site
  mark_bits* state = &code->allocator->state;
  FACTOR_FOR_EACH(code_roots) {
    code_root* root = *iter;
    cell block = root->value & (~data_alignment - 1);
    if (root->valid && !state->marked_p(block))
      root->valid = false;
  }

  if (event)
    event->reset_timer();
  code->sweep();
  if (event)
    event->ended_phase(PHASE_CODE_SWEEP);
}

void factor_vm::collect_full() {
#if defined(FACTOR_WASM)
  if (wasm_debug_enabled())
    std::cout << "[wasm] collect_full starting mark_impl" << std::endl;
#endif
  collect_mark_impl();
#if defined(FACTOR_WASM)
  if (wasm_debug_enabled())
    std::cout << "[wasm] collect_full starting sweep_impl" << std::endl;
#endif
  collect_sweep_impl();
#if defined(FACTOR_WASM)
  if (wasm_debug_enabled())
    std::cout << "[wasm] collect_full sweep complete" << std::endl;
#endif

  if (data->low_memory_p()) {
    // Full GC did not free up enough memory. Grow the heap.
#if defined(FACTOR_WASM)
    if (wasm_debug_enabled())
      std::cout << "[wasm] collect_full low memory, growing heap" << std::endl;
#endif
    set_current_gc_op(COLLECT_GROWING_DATA_HEAP_OP);
    collect_growing_data_heap(0);
  } else if (data->high_fragmentation_p()) {
    // Enough free memory, but it is not contiguous. Perform a
    // compaction.
#if defined(FACTOR_WASM)
    if (wasm_debug_enabled())
      std::cout << "[wasm] collect_full high fragmentation, compacting" << std::endl;
#endif
    set_current_gc_op(COLLECT_COMPACT_OP);
    collect_compact_impl();
  }

#if defined(FACTOR_WASM)
  if (wasm_debug_enabled())
    std::cout << "[wasm] collect_full about to flush_icache" << std::endl;
#endif
  code->flush_icache();
#if defined(FACTOR_WASM)
  if (wasm_debug_enabled())
    std::cout << "[wasm] collect_full complete" << std::endl;
#endif
}

}
