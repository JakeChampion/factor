#include "master.hpp"

namespace factor {

#if defined(FACTOR_WASM)

namespace {

bool trace_enabled() {
  static bool enabled = (std::getenv("FACTOR_WASM_TRACE") != nullptr);
  return enabled;
}

std::string byte_array_to_cstring(byte_array* ba) {
  const cell len = array_capacity(ba);
  const char* bytes = reinterpret_cast<const char*>(ba->data<uint8_t>());
  // Names are stored with a terminating NUL; fall back to the array length
  // if it is missing.
  size_t actual = 0;
  while (actual < static_cast<size_t>(len) && bytes[actual] != '\0')
    actual++;
  return std::string(bytes, actual);
}

std::string word_name_string(word* w) {
  string* name = untag<string>(w->name);
  cell len = untag_fixnum(name->length);
  const char* bytes = reinterpret_cast<const char*>(name->data());
  return std::string(bytes, static_cast<size_t>(len));
}

cell tuple_slot(tuple* t, fixnum slot_index) {
  cell* base = reinterpret_cast<cell*>(t);
  return base[slot_index];
}

std::string tuple_class_name(tuple* t) {
  tuple_layout* layout = untag<tuple_layout>(t->layout);
  word* klass = untag<word>(layout->klass);
  return word_name_string(klass);
}

fixnum interp_length(cell obj);

fixnum tuple_length(tuple* t) {
  std::string cname = tuple_class_name(t);

  if (cname == "slice") {
    fixnum from = untag_fixnum(tuple_slot(t, 2));
    fixnum to = untag_fixnum(tuple_slot(t, 3));
    return to - from;
  }
  if (cname == "reversed" || cname == "wrapped-sequence") {
    return interp_length(tuple_slot(t, 2));
  }
  if (cname == "curried") {
    return interp_length(tuple_slot(t, 3)) + 1;
  }
  if (cname == "composed") {
    return interp_length(tuple_slot(t, 2)) + interp_length(tuple_slot(t, 3));
  }
  if (cname == "vector") {
    return untag_fixnum(tuple_slot(t, 3));
  }

  if (trace_enabled())
    std::cout << "[wasm] length unsupported tuple class " << cname << " ptr "
              << (void*)t << std::endl;
  fatal_error("length: unsupported tuple class in wasm interpreter", (cell)t);
  return 0;
}

fixnum interp_length(cell obj) {
  switch (tagged<object>(obj).type()) {
    case ARRAY_TYPE:
      return (fixnum)array_capacity(untag<array>(obj));
    case BYTE_ARRAY_TYPE:
      return (fixnum)array_capacity(untag<byte_array>(obj));
    case STRING_TYPE:
      return untag_fixnum(untag<string>(obj)->length);
    case QUOTATION_TYPE: {
      quotation* q = untag<quotation>(obj);
      return (fixnum)array_capacity(untag<array>(q->array));
    }
    case WORD_TYPE: {
      word* w = untag<word>(obj);
      if (tagged<object>(w->def).type() == QUOTATION_TYPE)
        return interp_length(w->def);
      return 0;
    }
    case WRAPPER_TYPE:
      return interp_length(untag<wrapper>(obj)->object);
    case TUPLE_TYPE:
      return tuple_length(untag<tuple>(obj));
    default:
      if (trace_enabled())
        std::cout << "[wasm] length unsupported type "
                  << tagged<object>(obj).type() << " ptr " << (void*)obj
                  << std::endl;
      fatal_error("length: unsupported object type", obj);
      return 0;
  }
}

} // namespace

// Handle a handful of core combinators directly so we don't depend on
// compiler inlining or JIT stubs that are unavailable in the wasm build.
bool factor_vm::interpret_special_word(const std::string& name) {
  auto type_of = [](cell obj) { return tagged<object>(obj).type(); };

  if (name == "call" || name == "(call)") {
    cell callable = ctx->pop();
    while (true) {
      switch (type_of(callable)) {
        case QUOTATION_TYPE:
          interpret_quotation(callable);
          return true;
        case WORD_TYPE:
          interpret_word(callable);
          return true;
        case WRAPPER_TYPE: {
          callable = untag<wrapper>(callable)->object;
          if (trace_enabled())
            std::cout << "[wasm] call unwrap wrapper -> " << (void*)callable
                      << std::endl;
          continue;
        }
        case TUPLE_TYPE: {
          tuple* t = untag<tuple>(callable);
          std::string cname = tuple_class_name(t);
          if (cname == "curried") {
            cell obj = tuple_slot(t, 2);
            cell quot = tuple_slot(t, 3);
            ctx->push(obj);
            interpret_quotation(quot);
            return true;
          }
          if (cname == "composed") {
            cell first = tuple_slot(t, 2);
            cell second = tuple_slot(t, 3);
            interpret_quotation(first);
            interpret_quotation(second);
            return true;
          }
          if (trace_enabled())
            std::cout << "[wasm] call tuple class " << cname << " ptr "
                      << (void*)callable << std::endl;
          fatal_error("call: unsupported tuple class in wasm interpreter",
                      callable);
        }
        default:
          if (trace_enabled())
            std::cout << "[wasm] call unsupported type " << type_of(callable)
                      << " ptr " << (void*)callable << std::endl;
          fatal_error("call: unsupported callable in wasm interpreter",
                      callable);
      }
    }
  }

  if (name == "execute" || name == "(execute)") {
    cell w = ctx->pop();
    if (type_of(w) != WORD_TYPE)
      fatal_error("execute: expected word", w);
    interpret_word(w);
    return true;
  }

  if (name == "dip") {
    cell quot = ctx->pop();
    cell saved = ctx->pop();
    interpret_quotation(quot);
    ctx->push(saved);
    return true;
  }

  if (name == "2dip") {
    cell quot = ctx->pop();
    cell b = ctx->pop();
    cell a = ctx->pop();
    interpret_quotation(quot);
    ctx->push(a);
    ctx->push(b);
    return true;
  }

  if (name == "3dip") {
    cell quot = ctx->pop();
    cell c = ctx->pop();
    cell b = ctx->pop();
    cell a = ctx->pop();
    interpret_quotation(quot);
    ctx->push(a);
    ctx->push(b);
    ctx->push(c);
    return true;
  }

  if (name == "mega-cache-lookup") {
    // Cache update primitive: drop inputs and pretend miss handled.
    ctx->pop(); // cache
    ctx->pop(); // index
    ctx->pop(); // methods
    return true;
  }

  if (name == "length") {
    cell obj = ctx->pop();
    ctx->push(tag_fixnum(interp_length(obj)));
    return true;
  }

  if (name == "?") {
    cell false_val = ctx->pop();
    cell true_val = ctx->pop();
    cell cond = ctx->pop();
    ctx->push(to_boolean(cond) ? true_val : false_val);
    return true;
  }

  if (name == "if") {
    cell false_quot = ctx->pop();
    cell true_quot = ctx->pop();
    cell cond = ctx->pop();
    interpret_quotation(to_boolean(cond) ? true_quot : false_quot);
    return true;
  }

  if (name == "callable?") {
    cell obj = ctx->pop();
    bool callable = false;
    while (true) {
      cell t = type_of(obj);
      if (t == QUOTATION_TYPE || t == WORD_TYPE) {
        callable = true;
        break;
      }
      if (t == WRAPPER_TYPE) {
        obj = untag<wrapper>(obj)->object;
        continue;
      }
      if (t == TUPLE_TYPE) {
        std::string cname = tuple_class_name(untag<tuple>(obj));
        callable = (cname == "curried" || cname == "composed");
      }
      break;
    }
    ctx->push(tag_boolean(callable));
    return true;
  }

  return false;
}

bool factor_vm::dispatch_primitive_call(byte_array* name) {
  std::string prim = byte_array_to_cstring(name);

  if (trace_enabled())
    std::cout << "[wasm] prim " << prim << std::endl;

#define PRIM_CASE(id)                 \
  if (prim == std::string("primitive_" #id)) { \
    primitive_##id();                 \
    return true;                      \
  }

  EACH_PRIMITIVE(PRIM_CASE)
#undef PRIM_CASE
  return false;
}

bool factor_vm::dispatch_subprimitive(word* w) {
  const std::string name = word_name_string(w);

  if (name == "dup") {
    cell v = ctx->peek();
    ctx->push(v);
    return true;
  }
  if (name == "dupd") {
    cell top = ctx->pop();
    cell second = ctx->peek();
    ctx->push(top);
    ctx->push(second);
    return true;
  }
  if (name == "drop") {
    ctx->pop();
    return true;
  }
  if (name == "nip") {
    cell top = ctx->pop();
    ctx->pop();
    ctx->push(top);
    return true;
  }
  if (name == "2drop") {
    ctx->pop();
    ctx->pop();
    return true;
  }
  if (name == "2nip") {
    cell top = ctx->pop();
    ctx->pop();
    ctx->pop();
    ctx->push(top);
    return true;
  }
  if (name == "3drop") {
    ctx->pop();
    ctx->pop();
    ctx->pop();
    return true;
  }
  if (name == "4drop") {
    ctx->pop();
    ctx->pop();
    ctx->pop();
    ctx->pop();
    return true;
  }
  if (name == "2dup") {
    cell b = ctx->pop();
    cell a = ctx->pop();
    ctx->push(a);
    ctx->push(b);
    ctx->push(a);
    ctx->push(b);
    return true;
  }
  if (name == "3dup") {
    cell c = ctx->pop();
    cell b = ctx->pop();
    cell a = ctx->pop();
    ctx->push(a);
    ctx->push(b);
    ctx->push(c);
    ctx->push(a);
    ctx->push(b);
    ctx->push(c);
    return true;
  }
  if (name == "4dup") {
    cell d = ctx->pop();
    cell c = ctx->pop();
    cell b = ctx->pop();
    cell a = ctx->pop();
    ctx->push(a);
    ctx->push(b);
    ctx->push(c);
    ctx->push(d);
    ctx->push(a);
    ctx->push(b);
    ctx->push(c);
    ctx->push(d);
    return true;
  }
  if (name == "over") {
    cell top = ctx->pop();
    cell second = ctx->peek();
    ctx->push(top);
    ctx->push(second);
    return true;
  }
  if (name == "pick") {
    cell top = ctx->pop();
    cell second = ctx->pop();
    cell third = ctx->peek();
    ctx->push(second);
    ctx->push(top);
    ctx->push(third);
    return true;
  }
  if (name == "swap") {
    cell y = ctx->pop();
    cell x = ctx->pop();
    ctx->push(y);
    ctx->push(x);
    return true;
  }
  if (name == "swapd") {
    cell c = ctx->pop();
    cell b = ctx->pop();
    cell a = ctx->pop();
    ctx->push(b);
    ctx->push(a);
    ctx->push(c);
    return true;
  }
  if (name == "rot") {
    cell c = ctx->pop();
    cell b = ctx->pop();
    cell a = ctx->pop();
    ctx->push(b);
    ctx->push(c);
    ctx->push(a);
    return true;
  }
  if (name == "-rot") {
    cell c = ctx->pop();
    cell b = ctx->pop();
    cell a = ctx->pop();
    ctx->push(c);
    ctx->push(a);
    ctx->push(b);
    return true;
  }

  if (name == "eq?") {
    cell y = ctx->pop();
    cell x = ctx->pop();
    ctx->push(tag_boolean(x == y));
    return true;
  }
  if (name == "both-fixnums?") {
    cell y = ctx->pop();
    cell x = ctx->pop();
    bool ok = TAG(x) == FIXNUM_TYPE && TAG(y) == FIXNUM_TYPE;
    ctx->push(tag_boolean(ok));
    return true;
  }

  auto binary_fixnum_logic = [this](auto fn) {
    fixnum y = untag_fixnum(ctx->pop());
    fixnum x = untag_fixnum(ctx->pop());
    ctx->push(tag_fixnum(fn(x, y)));
  };

  if (name == "fixnum-bitand") {
    binary_fixnum_logic([](fixnum a, fixnum b) { return a & b; });
    return true;
  }
  if (name == "fixnum-bitor") {
    binary_fixnum_logic([](fixnum a, fixnum b) { return a | b; });
    return true;
  }
  if (name == "fixnum-bitxor") {
    binary_fixnum_logic([](fixnum a, fixnum b) { return a ^ b; });
    return true;
  }
  if (name == "fixnum-bitnot") {
    ctx->replace(tag_fixnum(~untag_fixnum(ctx->peek())));
    return true;
  }

  auto push_comparison = [this](auto cmp) {
    fixnum y = untag_fixnum(ctx->pop());
    fixnum x = untag_fixnum(ctx->pop());
    ctx->push(tag_boolean(cmp(x, y)));
  };

  if (name == "fixnum<") {
    push_comparison([](fixnum a, fixnum b) { return a < b; });
    return true;
  }
  if (name == "fixnum<=") {
    push_comparison([](fixnum a, fixnum b) { return a <= b; });
    return true;
  }
  if (name == "fixnum>") {
    push_comparison([](fixnum a, fixnum b) { return a > b; });
    return true;
  }
  if (name == "fixnum>=") {
    push_comparison([](fixnum a, fixnum b) { return a >= b; });
    return true;
  }

  if (name == "fixnum+" || name == "fixnum+fast") {
    fixnum y = untag_fixnum(ctx->pop());
    fixnum x = untag_fixnum(ctx->pop());
    fixnum r = x + y;
    if (r > fixnum_max || r < fixnum_min) {
      data_root<bignum> bx(fixnum_to_bignum(x), this);
      data_root<bignum> by(fixnum_to_bignum(y), this);
      data_root<bignum> res(bignum_add(bx.untagged(), by.untagged()), this);
      ctx->push(bignum_maybe_to_fixnum(res.untagged()));
    } else
      ctx->push(tag_fixnum(r));
    return true;
  }

  if (name == "fixnum-" || name == "fixnum-fast") {
    fixnum y = untag_fixnum(ctx->pop());
    fixnum x = untag_fixnum(ctx->pop());
    fixnum r = x - y;
    if (r > fixnum_max || r < fixnum_min) {
      data_root<bignum> bx(fixnum_to_bignum(x), this);
      data_root<bignum> by(fixnum_to_bignum(y), this);
      data_root<bignum> res(bignum_subtract(bx.untagged(), by.untagged()),
                            this);
      ctx->push(bignum_maybe_to_fixnum(res.untagged()));
    } else
      ctx->push(tag_fixnum(r));
    return true;
  }

  if (name == "fixnum*" || name == "fixnum*fast") {
    fixnum y = untag_fixnum(ctx->pop());
    fixnum x = untag_fixnum(ctx->pop());
    // Use wider type to detect overflow.
    int64_t r = (int64_t)x * (int64_t)y;
    if (r > fixnum_max || r < fixnum_min) {
      data_root<bignum> bx(fixnum_to_bignum(x), this);
      data_root<bignum> by(fixnum_to_bignum(y), this);
      data_root<bignum> res(bignum_multiply(bx.untagged(), by.untagged()),
                            this);
      ctx->push(bignum_maybe_to_fixnum(res.untagged()));
    } else {
      ctx->push(tag_fixnum((fixnum)r));
    }
    return true;
  }

  if (name == "fixnum-mod") {
    primitive_fixnum_divmod();
    cell remainder = ctx->pop();
    ctx->pop(); // quotient
    ctx->push(remainder);
    return true;
  }

  if (name == "fixnum/i-fast") {
    primitive_fixnum_divint();
    return true;
  }

  if (name == "fixnum/mod-fast") {
    primitive_fixnum_divmod();
    return true;
  }

  if (name == "fixnum-shift-fast") {
    primitive_fixnum_shift();
    return true;
  }

  if (name == "drop-locals") {
    fixnum count = untag_fixnum(ctx->pop());
    ctx->retainstack -= count * sizeof(cell);
    return true;
  }

  if (name == "load-local") {
    cell value = ctx->pop();
    ctx->retainstack += sizeof(cell);
    *(cell*)ctx->retainstack = value;
    return true;
  }

  if (name == "get-local") {
    fixnum index = untag_fixnum(ctx->pop());
    cell* slot = (cell*)(ctx->retainstack + index * sizeof(cell));
    ctx->push(*slot);
    return true;
  }

  if (name == "tag") {
    cell obj = ctx->pop();
    ctx->push(tag_fixnum(TAG(obj)));
    return true;
  }

  if (name == "slot") {
    fixnum slot_index = untag_fixnum(ctx->pop());
    object* obj = untag<object>(ctx->pop());
    cell* base = reinterpret_cast<cell*>(obj);
    ctx->push(base[slot_index]);
    return true;
  }

  if (name == "string-nth-fast") {
    data_root<string> str(ctx->pop(), this);
    fixnum index = untag_fixnum(ctx->pop());
    fixnum len = untag_fixnum(str->length);
    if (index < 0 || index >= len)
      fatal_error("string-nth-fast index out of bounds", index);
    uint8_t ch = str->data()[index];
    ctx->push(tag_fixnum((fixnum)ch));
    return true;
  }

  if (name == "fpu-state") {
    ctx->push(false_object);
    return true;
  }

  if (name == "set-fpu-state") {
    ctx->pop();
    return true;
  }

  if (name == "set-callstack") {
    ctx->pop();
    return true;
  }

  if (name == "c-to-factor" || name == "unwind-native-frames" ||
      name == "leaf-signal-handler" || name == "signal-handler") {
    // No threads/signals on wasm; treat as no-ops.
    return true;
  }

  if (name == "(set-context)" || name == "(start-context)") {
    // Return the object unchanged; ignore context/quotation.
    cell obj = ctx->pop();
    ctx->pop();
    ctx->push(obj);
    return true;
  }

  if (name == "(set-context-and-delete)" || name == "(start-context-and-delete)") {
    // Drop inputs; no result.
    ctx->pop();
    ctx->pop();
    return true;
  }

  return false;
}

// Simple, non-optimizing interpreter for wasm. Supports words whose
// definitions are quotations and a minimal set of primitives/subprimitives.
void factor_vm::interpret_word(cell word_) {
  data_root<word> w(word_, this);
  const std::string name = word_name_string(w.untagged());

  if (interpret_special_word(name))
    return;

  if (to_boolean(w->subprimitive)) {
    if (dispatch_subprimitive(w.untagged()))
      return;
    fatal_error("Unsupported subprimitive in wasm interpreter", w->name);
  }

  if (w->def == false_object) {
    fatal_error("Undefined word encountered in interpreter", w.value());
  }

  if (tagged<object>(w->def).type() == QUOTATION_TYPE) {
    if (trace_enabled())
      std::cout << "[wasm] word " << name << std::endl;
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

    // Primitive call pattern: BYTE_ARRAY followed by the special
    // do-primitive word (JIT_PRIMITIVE_WORD).
    if (tagged<object>(obj).type() == BYTE_ARRAY_TYPE && (i + 1) < len &&
        array_nth(elements.untagged(), i + 1) ==
            special_objects[JIT_PRIMITIVE_WORD]) {
      data_root<byte_array> prim(obj, this);
      if (!dispatch_primitive_call(prim.untagged()))
        fatal_error("Unknown primitive in wasm interpreter", obj);
      i++; // Skip the primitive word marker
      continue;
    }

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
        if (trace_enabled())
          std::cout << "[wasm] word " << word_name_string(untag<word>(obj)) << std::endl;
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
