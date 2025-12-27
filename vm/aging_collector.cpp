#include "master.hpp"

namespace factor {

struct to_aging_copier : no_fixup {
  aging_space* aging;
  tenured_space* tenured;

  to_aging_copier(aging_space* aging, tenured_space* tenured)
      : aging(aging), tenured(tenured) { }

  object* fixup_data(object* obj) {
    if (aging->contains_p(obj) || tenured->contains_p(obj)) {
      return obj;
    }

    // Is there another forwarding pointer?
    while (obj->forwarding_pointer_p()) {
      object* dest = obj->forwarding_pointer();
      obj = dest;
    }

    if (aging->contains_p(obj) || tenured->contains_p(obj)) {
      return obj;
    }

    cell size = obj->size();
    object* newpointer = aging->allot(size);
    if (!newpointer) {
#if defined(FACTOR_WASM)
      // If the target aging semispace is full, promote directly to tenured.
      newpointer = tenured->allot(size);
      if (newpointer) {
        if (std::getenv("FACTOR_WASM_TRACE")) {
          std::cout << "[wasm] aging promote to tenured obj=0x" << std::hex
                    << (cell)obj << " size=0x" << size << std::dec << std::endl;
        }
      } else {
        fatal_error("Out of aging space on wasm", size);
      }
#else
      throw must_start_gc_again();
#endif
    }

    memcpy(newpointer, obj, size);
    obj->forward_to(newpointer);

    return newpointer;
  }
};

void factor_vm::collect_aging() {
  // Promote objects referenced from tenured space to tenured space, copy
  // everything else to the aging semi-space, and reset the nursery pointer.
#if defined(FACTOR_WASM)
  static int aging_collect_count = 0;
  bool trace = wasm_debug_enabled() && (aging_collect_count < 2);  // Only trace first 2 collections
  if (trace) {
    std::cout << "[wasm] collect_aging #" << aging_collect_count
              << " aging_occupied=0x" << std::hex << data->aging->occupied_space()
              << " nursery_occupied=0x" << data->nursery->occupied_space()
              << std::dec << std::endl;
  }
  aging_collect_count++;
#endif
  {
    // Change the op so that if we fail here, an assertion will be raised.
    current_gc->op = COLLECT_TO_TENURED_OP;

    slot_visitor<from_tenured_refs_copier>
        visitor(this, from_tenured_refs_copier(data->tenured, &mark_stack));

    gc_event* event = current_gc->event;

    if (event)
      event->reset_timer();
    visitor.visit_cards(data->tenured, card_points_to_aging, 0xff);
    if (event) {
      event->ended_phase(PHASE_CARD_SCAN);
      event->cards_scanned += visitor.cards_scanned;
      event->decks_scanned += visitor.decks_scanned;
    }

    if (event)
      event->reset_timer();
    visitor.visit_code_heap_roots(&code->points_to_aging);
    if (event) {
      event->ended_phase(PHASE_CODE_SCAN);
      event->code_blocks_scanned += code->points_to_aging.size();
    }
    visitor.visit_mark_stack(&mark_stack);
#if defined(FACTOR_WASM)
    if (trace)
      std::cout << "[wasm] collect_aging phase1 done (tenured refs scanned)" << std::endl;
#endif
  }
  {
    // If collection fails here, do a to_tenured collection.
    current_gc->op = COLLECT_AGING_OP;

    std::swap(data->aging, data->aging_semispace);
    data->reset_aging();
#if defined(FACTOR_WASM)
    if (trace)
      std::cout << "[wasm] collect_aging swapped semispaces, now aging_occupied=0x"
                << std::hex << data->aging->occupied_space() << std::dec << std::endl;
#endif

    aging_space *aging = data->aging;
    slot_visitor<to_aging_copier>
        visitor(this, to_aging_copier(aging, data->tenured));

    cell scan = aging->start + aging->occupied_space();

    visitor.visit_all_roots();
#if defined(FACTOR_WASM)
    if (trace)
      std::cout << "[wasm] collect_aging visited roots, now aging_occupied=0x"
                << std::hex << data->aging->occupied_space() << std::dec << std::endl;
#endif
    visitor.cheneys_algorithm(aging, scan);
#if defined(FACTOR_WASM)
    if (trace)
      std::cout << "[wasm] collect_aging cheneys done, now aging_occupied=0x"
                << std::hex << data->aging->occupied_space() << std::dec << std::endl;
#endif

    data->reset_nursery();
    code->clear_remembered_set();
#if defined(FACTOR_WASM)
    if (trace)
      std::cout << "[wasm] collect_aging complete" << std::endl;
#endif
  }
}

}
