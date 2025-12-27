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
    fprintf(f, "BEFORE pop: datastack=0x%lx start=0x%lx depth=%ld\n",
            (unsigned long)ctx->datastack, (unsigned long)ctx->datastack_seg->start, (long)ctx->depth());

    // Print all stack items
    cell depth = ctx->depth();
    for (cell i = 0; i < depth && i < 10; i++) {
      cell* ptr = (cell*)(ctx->datastack - i * sizeof(cell));
      fprintf(f, "  stack[%ld] ptr=0x%lx", (long)i, (unsigned long)ptr);
      if (ptr >= (cell*)ctx->datastack_seg->start) {
        cell val = *ptr;
        cell tag = TAG(val);
        fprintf(f, " val=0x%lx (tag=%ld", (unsigned long)val, (long)tag);

        // Try to identify what this is
        if (tag == FIXNUM_TYPE) {
          fprintf(f, " fixnum=%ld", (long)untag_fixnum(val));
        } else if (!immediate_p(val)) {
          const char* type_str = type_name(tag);
          fprintf(f, " %s", type_str);
        }
        fprintf(f, ")\n");
      } else {
        fprintf(f, " INVALID (before start)\n");
      }
    }
    fclose(f);
  }
#endif

  data_root<tuple_layout> layout(ctx->pop(), this);
  tagged<tuple> t(allot<tuple>(tuple_size(layout.untagged())));
  t->layout = layout.value();

  cell size = untag_fixnum(layout.untagged()->size) * sizeof(cell);
  cell num_slots = untag_fixnum(layout.untagged()->size);
  // CRITICAL: datastack points AT top element, not past it!
  // So to get N items, we need: datastack - (N-1)*sizeof(cell) = datastack - size + sizeof(cell)
  cell* src = (cell*)(ctx->datastack - size + sizeof(cell));

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
#endif
  memcpy(t->data(), src, size);
#if defined(FACTOR_WASM)
  f = fopen("init-factor.log", "a");
  if (f) {
    fprintf(f, "BEFORE datastack-=size: datastack=0x%lx\n", (unsigned long)ctx->datastack);
    fclose(f);
  }
#endif
  ctx->datastack -= size;
#if defined(FACTOR_WASM)
  f = fopen("init-factor.log", "a");
  if (f) {
    fprintf(f, "AFTER datastack-=size: datastack=0x%lx (should be 0x%lx)\n",
            (unsigned long)ctx->datastack, (unsigned long)(ctx->datastack_seg->start - sizeof(cell)));
    fprintf(f, "About to push tuple\n");
    fclose(f);
  }
#endif

  ctx->push(t.value());
#if defined(FACTOR_WASM)
  f = fopen("init-factor.log", "a");
  if (f) {
    fprintf(f, "AFTER push: datastack=0x%lx depth=%ld\n",
            (unsigned long)ctx->datastack, (long)ctx->depth());
    fprintf(f, "=== RETURNING FROM primitive_tuple_boa ===\n");
    fclose(f);
  }
#endif
}

}
