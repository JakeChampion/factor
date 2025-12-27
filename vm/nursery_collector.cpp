#include "master.hpp"

namespace factor {

struct nursery_copier : no_fixup {
  bump_allocator* nursery;
  aging_space* aging;
  tenured_space* tenured;
  factor_vm* parent;

  nursery_copier(bump_allocator* nursery, aging_space* aging,
                 tenured_space* tenured, factor_vm* parent)
      : nursery(nursery), aging(aging), tenured(tenured), parent(parent) { }

  object* fixup_data(object* obj) {
    if (!nursery->contains_p(obj)) {
      return obj;
    }

    // The while-loop is a needed micro-optimization.
    while (obj->forwarding_pointer_p()) {
      obj = obj->forwarding_pointer();
    }

    if (!nursery->contains_p(obj)) {
      return obj;
    }

    cell size = obj->size();
    object* newpointer = aging->allot(size);
    if (!newpointer) {
#if defined(FACTOR_WASM)
      // If aging is full, fall back to promoting directly to tenured space.
      newpointer = tenured->allot(size);
      if (newpointer) {
        if (std::getenv("FACTOR_WASM_TRACE")) {
          std::cout << "[wasm] nursery promote to tenured obj=0x" << std::hex
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

void factor_vm::collect_nursery() {
  // Copy live objects from the nursery (as determined by the root set and
  // marked cards in aging and tenured) to aging space.
  slot_visitor<nursery_copier>
      visitor(this, nursery_copier(data->nursery, data->aging, data->tenured, this));

  cell scan = data->aging->start + data->aging->occupied_space();

  visitor.visit_all_roots();
  gc_event* event = current_gc->event;

  if (event)
    event->reset_timer();
  visitor.visit_cards(data->tenured, card_points_to_nursery,
                      card_points_to_nursery);
  visitor.visit_cards(data->aging, card_points_to_nursery, 0xff);
  if (event) {
    event->ended_phase(PHASE_CARD_SCAN);
    event->cards_scanned += visitor.cards_scanned;
    event->decks_scanned += visitor.decks_scanned;
  }

  if (event)
    event->reset_timer();
  visitor.visit_code_heap_roots(&code->points_to_nursery);
  if (event) {
    event->ended_phase(PHASE_CODE_SCAN);
    event->code_blocks_scanned += code->points_to_nursery.size();
  }

  visitor.cheneys_algorithm(data->aging, scan);

  data->reset_nursery();
  code->points_to_nursery.clear();
}

}
