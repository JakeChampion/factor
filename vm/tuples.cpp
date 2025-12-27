#include "master.hpp"

namespace factor {

// push a new tuple on the stack, filling its slots with f
// Allocates memory
void factor_vm::primitive_tuple() {
  data_root<tuple_layout> layout(ctx->pop(), this);
  tagged<tuple> t(allot<tuple>(tuple_size(layout.untagged())));
  t->layout = layout.value();

  memset_cell(t->data(), false_object,
              tuple_size(layout.untagged()) - sizeof(cell));

  ctx->push(t.value());
}

// push a new tuple on the stack, filling its slots from the stack
// Allocates memory
void factor_vm::primitive_tuple_boa() {
  data_root<tuple_layout> layout(ctx->pop(), this);
  tagged<tuple> t(allot<tuple>(tuple_size(layout.untagged())));
  t->layout = layout.value();

  cell size = untag_fixnum(layout.untagged()->size) * sizeof(cell);
  cell* src = (cell*)(ctx->datastack - size + sizeof(cell));
#if defined(FACTOR_WASM)
  if (src < (cell*)ctx->datastack_seg->start) {
    fatal_error("tuple_boa: stack underflow for slots", size);
  }
#endif
  memcpy(t->data(), src, size);
  ctx->datastack -= size;

  ctx->push(t.value());
}

}
