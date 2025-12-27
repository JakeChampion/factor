#include "master.hpp"

namespace factor {

// Allocates memory
array* factor_vm::allot_array(cell capacity, cell fill_) {
  data_root<object> fill(fill_, this);
  array* new_array = allot_uninitialized_array<array>(capacity);
  memset_cell(new_array->data(), fill.value(), capacity * sizeof(cell));
  return new_array;
}

// Allocates memory
void factor_vm::primitive_array() {
#if defined(FACTOR_WASM)
  // Log stack state BEFORE popping anything
  FILE* f = fopen("init-factor.log", "a");
  if (f) {
    fprintf(f, "\n=== primitive_array (<array>) ===\n");
    fprintf(f, "Stack depth before: %ld\n", (long)ctx->depth());
    fprintf(f, "  datastack=0x%lx start=0x%lx\n",
            (unsigned long)ctx->datastack, (unsigned long)ctx->datastack_seg->start);

    // Print stack items
    cell depth = ctx->depth();
    for (cell i = 0; i < depth && i < 5; i++) {
      cell* ptr = (cell*)(ctx->datastack - i * sizeof(cell));
      if (ptr >= (cell*)ctx->datastack_seg->start) {
        cell val = *ptr;
        cell tag = TAG(val);
        fprintf(f, "  stack[%ld] = 0x%lx (tag=%ld", (long)i, (unsigned long)val, (long)tag);

        if (tag == FIXNUM_TYPE) {
          fprintf(f, " fixnum=%ld", (long)untag_fixnum(val));
        } else if (!immediate_p(val)) {
          const char* type_str = type_name(tag);
          fprintf(f, " %s", type_str);
        }
        fprintf(f, ")\n");
      }
    }
    fclose(f);
  }
#endif

  bool trace = std::getenv("FACTOR_WASM_TRACE") != nullptr;
  cell fill = ctx->pop();
  cell cap_raw = ctx->peek();
  if (trace) {
    std::cout << "[wasm] primitive_array fill=0x" << std::hex << fill
              << " tag=" << TAG(fill) << " cap_raw=0x" << cap_raw
              << " cap_tag=" << TAG(cap_raw) << std::dec << " cap_obj=";
    print_obj(std::cout, cap_raw);
    std::cout << std::endl;
  }
  cell capacity = unbox_array_size();
  array* new_array = allot_array(capacity, fill);

#if defined(FACTOR_WASM)
  f = fopen("init-factor.log", "a");
  if (f) {
    fprintf(f, "Created array: capacity=%ld, fill=0x%lx, array=0x%lx\n",
            (long)capacity, (unsigned long)fill, (unsigned long)tag<array>(new_array));
    fprintf(f, "About to push array, datastack before push=0x%lx\n",
            (unsigned long)ctx->datastack);
    fclose(f);
  }
#endif

  ctx->push(tag<array>(new_array));

#if defined(FACTOR_WASM)
  f = fopen("init-factor.log", "a");
  if (f) {
    fprintf(f, "After push: datastack=0x%lx depth=%ld\n",
            (unsigned long)ctx->datastack, (long)ctx->depth());
    fprintf(f, "  stack[0] (top) = 0x%lx\n", (unsigned long)ctx->peek());
    fclose(f);
  }
#endif
}

// Allocates memory
cell factor_vm::allot_array_4(cell v1_, cell v2_, cell v3_, cell v4_) {
  data_root<object> v1(v1_, this);
  data_root<object> v2(v2_, this);
  data_root<object> v3(v3_, this);
  data_root<object> v4(v4_, this);
  array *a = allot_uninitialized_array<array>(4);
  set_array_nth(a, 0, v1.value());
  set_array_nth(a, 1, v2.value());
  set_array_nth(a, 2, v3.value());
  set_array_nth(a, 3, v4.value());
  return tag<array>(a);
}

// Allocates memory
void factor_vm::primitive_resize_array() {
  data_root<array> a(ctx->pop(), this);
  check_tagged(a);
  cell capacity = unbox_array_size();
  ctx->push(tag<array>(reallot_array(a.untagged(), capacity)));
}

// Allocates memory
cell factor_vm::std_vector_to_array(std::vector<cell>& elements) {

  cell element_count = elements.size();
  bool prev_gc_off = gc_off;
  gc_off = true; // avoid GC so vector contents stay valid while copying
  tagged<array> objects(allot_uninitialized_array<array>(element_count));
  memcpy(objects->data(), &elements[0], element_count * sizeof(cell));
  gc_off = prev_gc_off;
  return objects.value();
}

// Allocates memory
void growable_array::reallot_array(cell count) {
  array *a_old = elements.untagged();
  array *a_new = elements.parent->reallot_array(a_old, count);
  elements.set_untagged(a_new);
}

// Allocates memory
void growable_array::add(cell elt_) {
  factor_vm* parent = elements.parent;
  data_root<object> elt(elt_, parent);
  if (count == array_capacity(elements.untagged())) {
    reallot_array(2 * count);
  }
  parent->set_array_nth(elements.untagged(), count++, elt.value());
}

// Allocates memory
void growable_array::append(array* elts_) {
  factor_vm* parent = elements.parent;
  data_root<array> elts(elts_, parent);
  cell capacity = array_capacity(elts.untagged());
  if (count + capacity > array_capacity(elements.untagged())) {
    reallot_array(2 * (count + capacity));
  }

  for (cell index = 0; index < capacity; index++)
    parent->set_array_nth(elements.untagged(), count++,
                          array_nth(elts.untagged(), index));
}

// Allocates memory
void growable_array::trim() {
  reallot_array(count);
}

}
