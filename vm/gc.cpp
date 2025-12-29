#include "master.hpp"

namespace factor {

gc_event::gc_event(gc_op op, factor_vm* parent)
    : op(op),
      cards_scanned(0),
      decks_scanned(0),
      code_blocks_scanned(0),
      start_time(nano_count()),
      times{0} {
  data_heap_before = parent->data_room();
  code_heap_before = parent->code->allocator->as_allocator_room();
  start_time = nano_count();
}

void gc_event::reset_timer() { temp_time = nano_count(); }

void gc_event::ended_phase(gc_phase phase) {
  times[phase] = (cell)(nano_count() - temp_time);
}

void gc_event::ended_gc(factor_vm* parent) {
  data_heap_after = parent->data_room();
  code_heap_after = parent->code->allocator->as_allocator_room();
  total_time = (cell)(nano_count() - start_time);
}

gc_state::gc_state(gc_op op, factor_vm* parent) : op(op) {
  if (parent->gc_events) {
    event = new gc_event(op, parent);
    start_time = nano_count();
  } else
    event = NULL;
}

gc_state::~gc_state() {
  if (event) {
    delete event;
    event = NULL;
  }
}

void factor_vm::start_gc_again() {
  if (current_gc->op == COLLECT_NURSERY_OP) {
    // Nursery collection can fail if aging does not have enough
    // free space to fit all live objects from nursery.
    current_gc->op = COLLECT_AGING_OP;
  } else if (current_gc->op == COLLECT_AGING_OP) {
    // Aging collection can fail if the aging semispace cannot fit
    // all the live objects from the other aging semispace and the
    // nursery.
    current_gc->op = COLLECT_TO_TENURED_OP;
  } else {
    // Nothing else should fail mid-collection due to insufficient
    // space in the target generation.
    critical_error("in start_gc_again, bad GC op", current_gc->op);
  }
}

void factor_vm::set_current_gc_op(gc_op op) {
  current_gc->op = op;
  if (gc_events)
    current_gc->event->op = op;
}

void factor_vm::gc(gc_op op, cell requested_size) {
  if (gc_off) {
#if defined(FACTOR_WASM)
    if (std::getenv("FACTOR_WASM_TRACE")) {
      std::cout << "[wasm] gc suppressed (gc_off) op=" << op << std::endl;
    }
#endif
    return;
  }

#if defined(FACTOR_WASM)
  // FACTOR_WASM_NOOP_GC: completely skip all GC operations for debugging
  // This helps identify whether crashes are due to GC or other issues
  static int noop_gc_mode = -1;
  if (noop_gc_mode == -1) {
    noop_gc_mode = std::getenv("FACTOR_WASM_NOOP_GC") != nullptr ? 1 : 0;
  }
  if (noop_gc_mode) {
    static int noop_count = 0;
    if (noop_count < 2) {  // Reduce logging
      std::cout << "[wasm] gc NOOP mode active, skipping op=" << op << std::endl;
      noop_count++;
    }
    return;
  }
#endif

  FACTOR_ASSERT(!current_gc);

  // Important invariant: tenured space must have enough contiguous free
  // space to fit the entire contents of the aging space and nursery. This is
  // because when doing a full collection, objects from younger generations
  // are promoted before any unreachable tenured objects are freed.
  //
  // Note: high_fragmentation_p() may be true here, which is fine - the GC
  // loop below will detect this and escalate to compaction as needed.
#if !defined(FACTOR_WASM)
  FACTOR_ASSERT(!data->high_fragmentation_p());
#endif

  #if defined(FACTOR_WASM)
  if (std::getenv("FACTOR_WASM_TRACE")) {
    std::cout << "[wasm] gc start op=" << op << " requested=" << requested_size
              << std::endl;
  }
  #endif
  current_gc = new gc_state(op, this);
  if (ctx)
    ctx->callstack_seg->set_border_locked(false);
  atomic::store(&current_gc_p, true);

  // Keep trying to GC higher and higher generations until we don't run
  // out of space in the target generation.
  for (;;) {
#if defined(FACTOR_WASM)
    if (gc_events)
      current_gc->event->op = current_gc->op;

    // Periodic progress output - only with FACTOR_WASM_DEBUG or FACTOR_WASM_TRACE
    static int gc_count = 0;
    gc_count++;
    if (wasm_debug_enabled() && (gc_count % 1000 == 0)) {
      std::cout << "[wasm] gc #" << gc_count << " op=" << current_gc->op
                << " nursery=0x" << std::hex << data->nursery->occupied_space()
                << " aging=0x" << data->aging->occupied_space()
                << std::dec << std::endl;
      std::cout.flush();
    }

    // On WASM, check if we need to escalate before running collection
    // (since we can't use exceptions to retry)
    gc_op original_op = current_gc->op;
    
    // Pre-check: if aging is nearly full and we're doing nursery collection,
    // escalate to aging collection first
    if (current_gc->op == COLLECT_NURSERY_OP) {
      cell nursery_size = data->nursery->occupied_space();
      cell aging_free = data->aging->size - data->aging->occupied_space();
      if (aging_free < nursery_size) {
        if (wasm_debug_enabled()) {
          static int escalate_nursery_count = 0;
          if (escalate_nursery_count < 2) {
            std::cout << "[wasm] gc escalating nursery->aging (aging_free=0x"
                      << std::hex << aging_free << " nursery_size=0x"
                      << nursery_size << std::dec << ")" << std::endl;
            escalate_nursery_count++;
          }
        }
        current_gc->op = COLLECT_AGING_OP;
      }
    }
    
    // Pre-check: if tenured has high fragmentation and we're doing aging collection,
    // escalate to full collection
    if (current_gc->op == COLLECT_AGING_OP && data->high_fragmentation_p()) {
      if (wasm_debug_enabled()) {
        static int escalate_aging_count = 0;
        if (escalate_aging_count < 2) {
          std::cout << "[wasm] gc escalating aging->full (high fragmentation)"
                    << std::endl;
          escalate_aging_count++;
        }
      }
      current_gc->op = COLLECT_FULL_OP;
    }

    switch (current_gc->op) {
      case COLLECT_NURSERY_OP:
        collect_nursery();
        break;
      case COLLECT_AGING_OP:
        collect_aging();
        if (data->high_fragmentation_p()) {
          set_current_gc_op(COLLECT_FULL_OP);
          collect_full();
        }
        break;
      case COLLECT_TO_TENURED_OP:
        collect_to_tenured();
        if (data->high_fragmentation_p()) {
          set_current_gc_op(COLLECT_FULL_OP);
          collect_full();
        }
        break;
      case COLLECT_FULL_OP:
        collect_full();
        break;
      case COLLECT_COMPACT_OP:
        collect_compact();
        break;
      case COLLECT_GROWING_DATA_HEAP_OP:
        collect_growing_data_heap(requested_size);
        break;
      default:
        critical_error("in gc, bad GC op", current_gc->op);
        break;
    }
    // On wasm we break after one iteration since we pre-escalate
    break;
#else
    try {
      if (gc_events)
        current_gc->event->op = current_gc->op;

      switch (current_gc->op) {
        case COLLECT_NURSERY_OP:
          collect_nursery();
          break;
        case COLLECT_AGING_OP:
          // We end up here if the above fails.
          collect_aging();
          if (data->high_fragmentation_p()) {
            // Change GC op so that if we fail again, we crash.
            set_current_gc_op(COLLECT_FULL_OP);
            collect_full();
          }
          break;
        case COLLECT_TO_TENURED_OP:
          // We end up here if the above fails.
          collect_to_tenured();
          if (data->high_fragmentation_p()) {
            // Change GC op so that if we fail again, we crash.
            set_current_gc_op(COLLECT_FULL_OP);
            collect_full();
          }
          break;
        case COLLECT_FULL_OP:
          collect_full();
          break;
        case COLLECT_COMPACT_OP:
          collect_compact();
          break;
        case COLLECT_GROWING_DATA_HEAP_OP:
          collect_growing_data_heap(requested_size);
          break;
        default:
          critical_error("in gc, bad GC op", current_gc->op);
          break;
      }

      break;
    }
    catch (const must_start_gc_again&) {
      // We come back here if the target generation is full.
      start_gc_again();
    }
#endif
  }

  if (gc_events) {
    current_gc->event->ended_gc(this);
    gc_events->push_back(*current_gc->event);
  }

  atomic::store(&current_gc_p, false);
  if (ctx)
    ctx->callstack_seg->set_border_locked(true);
  delete current_gc;
  current_gc = NULL;

#if defined(FACTOR_WASM)
  // Clear cached layout pointers after any GC since objects may have moved
  // This prevents stale pointers in the layout caches from causing crashes
  extern void clear_wasm_layout_caches();
  clear_wasm_layout_caches();
#endif

  // Check the invariant again, just in case.
  FACTOR_ASSERT(!data->high_fragmentation_p());
}

void factor_vm::primitive_minor_gc() {
  if (!gc_off)
    gc(COLLECT_NURSERY_OP, 0);
}

void factor_vm::primitive_full_gc() {
  if (!gc_off)
    gc(COLLECT_FULL_OP, 0);
}

void factor_vm::primitive_compact_gc() {
  if (!gc_off)
    gc(COLLECT_COMPACT_OP, 0);
}

void factor_vm::primitive_enable_gc_events() {
  gc_events = new std::vector<gc_event>();
}

// Allocates memory (byte_array_from_value, result.add)
// XXX: Remember that growable_array has a data_root already
void factor_vm::primitive_disable_gc_events() {
  if (gc_events) {
    growable_array result(this);

    std::vector<gc_event>* gc_events = this->gc_events;
    this->gc_events = NULL;

    FACTOR_FOR_EACH(*gc_events) {
      gc_event event = *iter;
      byte_array* obj = byte_array_from_value(&event);
      result.add(tag<byte_array>(obj));
    }

    result.trim();
    ctx->push(result.elements.value());

    delete this->gc_events;
  } else
    ctx->push(false_object);
}

}
