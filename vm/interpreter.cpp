#include "master.hpp"

namespace factor {

#if defined(FACTOR_WASM)

// Simple interpreter stub; currently just errors until wired.
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
    data_root<quotation> quot(array_nth(quotations.untagged(), i), this);
    quot->entry_point = 0;
  }
}

#else

void* factor_vm::interpreter_entry_point() { return NULL; }
void factor_vm::set_interpreter_entry_points() {}

#endif // FACTOR_WASM

}
