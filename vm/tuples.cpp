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
#if defined(FACTOR_WASM)
  // Log stack state BEFORE popping layout
  FILE* f = fopen("init-factor.log", "a");
  if (f) {
    fprintf(f, "\n=== primitive_tuple_boa ===\n");
    fprintf(f, "Stack depth before: %ld\n", (long)ctx->depth());
    fclose(f);
  }
#endif

  data_root<tuple_layout> layout(ctx->pop(), this);
  tagged<tuple> t(allot<tuple>(tuple_size(layout.untagged())));
  t->layout = layout.value();

  cell size = untag_fixnum(layout.untagged()->size) * sizeof(cell);
  cell num_slots = untag_fixnum(layout.untagged()->size);
  cell* src = (cell*)(ctx->datastack - size);

#if defined(FACTOR_WASM)
  // Get tuple class name for logging
  const char* class_name = "unknown";
  char class_buf[100];
  tuple_layout* lay = layout.untagged();
  cell klass = lay->klass;
  if (!immediate_p(klass) && TAG(klass) == WORD_TYPE) {
    word* w = untag<word>(klass);
    cell name_cell = w->name;
    if (!immediate_p(name_cell) && TAG(name_cell) == STRING_TYPE) {
      string* s = untag<string>(name_cell);
      cell slen = string_capacity(s);
      if (slen < 100) {
        memcpy(class_buf, s->data(), slen);
        class_buf[slen] = '\0';
        class_name = class_buf;
      }
    }
  }

  f = fopen("init-factor.log", "a");
  if (f) {
    fprintf(f, "Creating %s tuple: %ld slots (%lu bytes)\n",
            class_name, (long)num_slots, (unsigned long)size);
    fprintf(f, "  datastack=0x%lx start=0x%lx src=0x%lx\n",
            (unsigned long)ctx->datastack, (unsigned long)ctx->datastack_seg->start,
            (unsigned long)src);
    fprintf(f, "  depth_after_pop=%ld (need %ld slots)\n",
            (long)ctx->depth(), (long)num_slots);
    fclose(f);
  }

  if (src < (cell*)ctx->datastack_seg->start) {
    f = fopen("init-factor.log", "a");
    if (f) {
      fprintf(f, "*** UNDERFLOW: src before start by %ld bytes! ***\n",
              (long)((cell*)ctx->datastack_seg->start - src) * sizeof(cell));
      fclose(f);
    }
    fatal_error("tuple_boa: stack underflow for slots", size);
  }
#endif
  memcpy(t->data(), src, size);
  ctx->datastack -= size;

  ctx->push(t.value());
}

}
