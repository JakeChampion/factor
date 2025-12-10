#include "master.hpp"

namespace factor {

#if defined(FACTOR_WASM)

// Simple, non-optimizing interpreter for wasm. Supports words whose
// definitions are quotations; primitives are not yet handled.
void factor_vm::interpret_word(cell word_) {
  data_root<word> w(word_, this);

  if (to_boolean(w->subprimitive)) {
    fatal_error("Primitives are not supported in the wasm interpreter yet",
                w->subprimitive);
  }

  if (w->def == false_object) {
    fatal_error("Undefined word encountered in interpreter", w.value());
  }

  if (tagged<object>(w->def).type() == QUOTATION_TYPE) {
    interpret_quotation(w->def);
    return;
  }

  fatal_error("Cannot interpret word definition", w->def);
}

void factor_vm::interpret_quotation(cell quot_) {
  data_root<quotation> quot(quot_, this);
  data_root<array> elements(quot->array, this);

  cell len = array_capacity(elements.untagged());
  for (cell i = 0; i < len; i++) {
    cell obj = array_nth(elements.untagged(), i);
    switch (tagged<object>(obj).type()) {
      case FIXNUM_TYPE:
      case F_TYPE:
      case ARRAY_TYPE:
      case FLOAT_TYPE:
      case QUOTATION_TYPE:
      case BIGNUM_TYPE:
      case ALIEN_TYPE:
      case TUPLE_TYPE:
      case WRAPPER_TYPE:
      case BYTE_ARRAY_TYPE:
      case CALLSTACK_TYPE:
      case STRING_TYPE:
        ctx->push(obj);
        break;
      case WORD_TYPE:
        interpret_word(obj);
        break;
      default:
        fatal_error("Unsupported object in interpreter", obj);
        break;
    }
  }
}

void* factor_vm::interpreter_entry_point() { return NULL; }

void factor_vm::set_interpreter_entry_points() {
  data_root<array> words(instances(WORD_TYPE), this);
  cell n_words = array_capacity(words.untagged());
  for (cell i = 0; i < n_words; i++) {
    data_root<word> word(array_nth(words.untagged(), i), this);
    word->entry_point = 0;
  }

  data_root<array> quotations(instances(QUOTATION_TYPE), this);
  cell n_quots = array_capacity(quotations.untagged());
  for (cell i = 0; i < n_quots; i++) {
    data_root<quotation> q(array_nth(quotations.untagged(), i), this);
    q->entry_point = 0;
  }
}

#else

void factor_vm::interpret_word(cell word_) { (void)word_; }
void factor_vm::interpret_quotation(cell quot_) { (void)quot_; }
void* factor_vm::interpreter_entry_point() { return NULL; }
void factor_vm::set_interpreter_entry_points() {}

#endif // FACTOR_WASM

}
