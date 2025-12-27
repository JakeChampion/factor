#include "master.hpp"

namespace factor {

#if defined(FACTOR_WASM)

// Set to true to use recursive interpreter instead of trampoline
// This is for testing - recursive interpreter will overflow on WASM
// but helps verify the handler implementations are correct
static bool g_use_recursive_interpreter = false;  // Use trampoline

// Global cache for tuple layouts - enables O(1) type checks instead of string comparisons
// These are populated lazily when we first encounter each tuple type
static cell g_curried_layout = 0;
static cell g_composed_layout = 0;
static cell g_box_layout = 0;
static cell g_global_box_layout = 0;
static cell g_hashtable_layout = 0;
static cell g_tombstone_layout = 0;
static cell g_slice_layout = 0;
static cell g_vector_layout = 0;
static cell g_reversed_layout = 0;
static cell g_wrapped_sequence_layout = 0;

// Clear cached layout pointers - must be called after GC compaction
// to avoid stale pointer comparisons. The caches will be repopulated
// lazily on next use.
void clear_wasm_layout_caches() {
  g_curried_layout = 0;
  g_composed_layout = 0;
  g_box_layout = 0;
  g_global_box_layout = 0;
  g_hashtable_layout = 0;
  g_tombstone_layout = 0;
  g_slice_layout = 0;
  g_vector_layout = 0;
  g_reversed_layout = 0;
  g_wrapped_sequence_layout = 0;
}

// Track where call_callable was invoked from (for debugging)
static const char* g_call_callable_source = nullptr;

namespace {

// Helper to check if a tuple has a specific cached layout
// Returns true if layout matches, false if layout not cached yet or doesn't match
inline bool is_tuple_class(tuple* t, cell& cached_layout, const char* class_name) {
  if (cached_layout != 0) {
    return t->layout == cached_layout;
  }
  // Layout not cached - do string comparison and cache if match
  tuple_layout* layout = untag<tuple_layout>(t->layout);
  word* klass = untag<word>(layout->klass);
  string* name = untag<string>(klass->name);
  cell len = untag_fixnum(name->length);
  const char* bytes = reinterpret_cast<const char*>(name->data());
  size_t class_len = strlen(class_name);
  if (len == (cell)class_len && memcmp(bytes, class_name, class_len) == 0) {
    cached_layout = t->layout;
    return true;
  }
  return false;
}

// Macro for common pattern of checking tuple class with caching
#define IS_TUPLE_CLASS(t, name) is_tuple_class(t, g_##name##_layout, #name)

// Runtime FNV-1a hash function for switch-based dispatch
inline uint32_t runtime_hash(const char* s, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint32_t>(s[i]);
    hash *= 16777619u;
  }
  return hash;
}

inline uint32_t runtime_hash(const std::string& s) {
  return runtime_hash(s.data(), s.size());
}

// Track a small trace of recently executed words to help debug deep recursion
// in the wasm interpreter.
thread_local std::vector<std::string> wasm_call_trace;

bool trace_enabled() {
  static bool enabled = (std::getenv("FACTOR_WASM_TRACE") != nullptr);
  return enabled;
}

std::string word_name_string(word* w);
std::string tuple_class_name(tuple* t);
cell tuple_slot(tuple* t, fixnum slot_index);

void dump_stack(factor_vm* vm, int limit = 6) {
  cell* top = (cell*)vm->ctx->datastack;
  cell* base = (cell*)vm->ctx->datastack_seg->start;
  std::cout << "    [top] depth=" << vm->ctx->depth();
  for (int i = 0; i < limit && (top - i) >= base; i++) {
    cell v = top[-i];
    std::cout << " [" << i << "]=0x" << std::hex << v << "(t" << TAG(v)
              << ")" << std::dec;
    if (TAG(v) == WORD_TYPE) {
      std::cout << "<" << word_name_string(untag<word>(v)) << ">";
    } else if (TAG(v) == TUPLE_TYPE) {
      tuple* t = untag<tuple>(v);
      std::string cname = tuple_class_name(t);
      std::cout << "<" << cname << ">";
      if (cname == "curried" && i < 4) {
        cell obj = tuple_slot(t, 0);
        cell quot = tuple_slot(t, 1);
        std::cout << "{obj=0x" << std::hex << obj << std::dec;
        if (TAG(obj) == WORD_TYPE)
          std::cout << ":" << word_name_string(untag<word>(obj));
        std::cout << " quot=0x" << std::hex << quot << std::dec;
        if (TAG(quot) == WORD_TYPE)
          std::cout << ":" << word_name_string(untag<word>(quot));
        std::cout << "}";
      } else if (cname == "composed" && i < 4) {
        cell first = tuple_slot(t, 0);
        cell second = tuple_slot(t, 1);
        std::cout << "{first=0x" << std::hex << first << std::dec;
        if (TAG(first) == WORD_TYPE)
          std::cout << ":" << word_name_string(untag<word>(first));
        std::cout << " second=0x" << std::hex << second << std::dec;
        if (TAG(second) == WORD_TYPE)
          std::cout << ":" << word_name_string(untag<word>(second));
        std::cout << "}";
      }
    }
  }
  std::cout << std::endl;
}

struct call_trace_scope {
  explicit call_trace_scope(const std::string& name) {
    wasm_call_trace.push_back(name);
  }
  ~call_trace_scope() {
    if (!wasm_call_trace.empty())
      wasm_call_trace.pop_back();
  }
};

fixnum compute_string_hash(string* str) {
  fixnum len = untag_fixnum(str->length);
  fixnum hash = 0;
  for (fixnum i = 0; i < len; i++) {
    fixnum ch = (fixnum)str->data()[i];
    fixnum shifted = (hash >> 2) + (hash << 5);
    hash = hash ^ (shifted + ch);
  }
  return hash;
}

// Get string_view from byte_array without allocation
inline std::string_view byte_array_to_string_view(byte_array* ba) {
  const cell len = array_capacity(ba);
  const char* bytes = reinterpret_cast<const char*>(ba->data<uint8_t>());
  size_t actual = 0;
  while (actual < static_cast<size_t>(len) && bytes[actual] != '\0')
    actual++;
  return std::string_view(bytes, actual);
}

cell find_word_by_name_local(factor_vm* vm, const std::string& target) {
  data_root<array> words(vm->instances(WORD_TYPE), vm);
  cell n_words = array_capacity(words.untagged());
  for (cell i = 0; i < n_words; i++) {
    word* w = untag<word>(array_nth(words.untagged(), i));
    if (word_name_string(w) == target)
      return tag<word>(w);
  }
  return false_object;
}

std::string word_name_string(word* w) {
  string* name = untag<string>(w->name);
  cell len = untag_fixnum(name->length);
  const char* bytes = reinterpret_cast<const char*>(name->data());
  return std::string(bytes, static_cast<size_t>(len));
}

// Non-allocating version for read-only comparisons
inline std::string_view word_name_string_view(word* w) {
  string* name = untag<string>(w->name);
  cell len = untag_fixnum(name->length);
  const char* bytes = reinterpret_cast<const char*>(name->data());
  return std::string_view(bytes, static_cast<size_t>(len));
}

cell tuple_slot(tuple* t, fixnum slot_index) {
  return t->data()[slot_index];
}

// Resolve Factor-style resource: paths to the /work mount used in the wasmtime
// invocation. Leaves other paths unchanged.
std::string resolve_wasm_path(const std::string& path) {
#if defined(FACTOR_WASM)
  if (path.rfind("resource:", 0) == 0) {
    std::string rest = path.substr(9);
    while (!rest.empty() && rest[0] == '/')
      rest.erase(0, 1);
    return std::string("/work/") + rest;
  }
#endif
  return path;
}

std::string tuple_class_name(tuple* t) {
  tuple_layout* layout = untag<tuple_layout>(t->layout);
  word* klass = untag<word>(layout->klass);
  return word_name_string(klass);
}

// Fast path using cached layout - avoids string allocation
inline bool is_tombstone(tuple* t) {
  return IS_TUPLE_CLASS(t, tombstone);
}

bool tombstone_state(tuple* t) {
  // tombstone has a single slot "state"
  return to_boolean(tuple_slot(t, 0));
}

// Fast sentinel checks using cached layout
inline bool is_empty_sentinel(cell obj) {
  if (TAG(obj) != TUPLE_TYPE)
    return false;
  tuple* t = untag<tuple>(obj);
  return is_tombstone(t) && !tombstone_state(t);
}

inline bool is_tombstone_sentinel(cell obj) {
  if (TAG(obj) != TUPLE_TYPE)
    return false;
  tuple* t = untag<tuple>(obj);
  return is_tombstone(t) && tombstone_state(t);
}

bool strings_equal(string* a, string* b) {
  cell lena = untag_fixnum(a->length);
  if (lena != untag_fixnum(b->length))
    return false;
  return memcmp(a->data(), b->data(), (size_t)lena) == 0;
}

bool objects_equal(cell a, cell b) {
  if (a == b)
    return true;
  cell ta = TAG(a), tb = TAG(b);
  if (ta != tb)
    return false;
  switch (ta) {
    case FIXNUM_TYPE:
      // already handled a==b
      return false;
    case STRING_TYPE:
      return strings_equal(untag<string>(a), untag<string>(b));
    case TUPLE_TYPE: {
      tuple* ta_t = untag<tuple>(a);
      tuple* tb_t = untag<tuple>(b);
      // Fast path: compare layouts directly instead of string comparison
      if (ta_t->layout != tb_t->layout)
        return false;
      // Both are same class - check if tombstone
      if (is_tombstone(ta_t))
        return tombstone_state(ta_t) == tombstone_state(tb_t);
      return false;
    }
    default:
      return false;
  }
}

struct hashtable_lookup_result {
  cell array_cell;
  fixnum index;
  bool found;
};

hashtable_lookup_result hashtable_lookup(cell key, cell hash_cell) {
  hashtable_lookup_result result{false_object, 0, false};

  if (TAG(hash_cell) != TUPLE_TYPE)
    return result;
  tuple* h = untag<tuple>(hash_cell);
  // Fast path: use cached layout instead of string comparison
  if (!IS_TUPLE_CLASS(h, hashtable))
    return result;
  cell array_cell = tuple_slot(h, 2);
  if (TAG(array_cell) != ARRAY_TYPE)
    return result;

  array* arr = untag<array>(array_cell);
  fixnum len = (fixnum)array_capacity(arr);
  if (len <= 0)
    return result;

  fixnum mask = len - 1;
  auto hash_fix = [](cell obj) { return (fixnum)(obj >> TAG_BITS); };
  fixnum hcode = 0;
  // Use cached hashcode if available
  switch (TAG(key)) {
    case STRING_TYPE: {
      string* s = untag<string>(key);
      cell cached = s->hashcode;
      if (cached == false_object)
        cached = tag_fixnum(compute_string_hash(s));
      hcode = untag_fixnum(cached);
      break;
    }
    case FIXNUM_TYPE:
      hcode = untag_fixnum(key);
      break;
    default:
      hcode = hash_fix(key);
      break;
  }
  fixnum idx = (hcode + hcode) & mask;
  fixnum probe = 0;

  for (fixnum step = 0; step < len; step++) {
    cell entry = array_nth(arr, idx);
    if (is_empty_sentinel(entry)) {
      result.array_cell = array_cell;
      result.index = -1;
      result.found = false;
      return result;
    }
    if (!is_tombstone_sentinel(entry) && objects_equal(entry, key)) {
      result.array_cell = array_cell;
      result.index = idx;
      result.found = true;
      return result;
    }
    probe += 2;
    idx = (idx + probe) & mask;
  }

  result.array_cell = array_cell;
  result.index = -1;
  result.found = false;
  return result;
}

fixnum interp_length(cell obj);

fixnum tuple_length(tuple* t) {
  // Fast path using cached layouts - avoids string allocation
  if (IS_TUPLE_CLASS(t, slice)) {
    fixnum from = untag_fixnum(tuple_slot(t, 0));
    fixnum to = untag_fixnum(tuple_slot(t, 1));
    return to - from;
  }
  if (IS_TUPLE_CLASS(t, reversed)) {
    return interp_length(tuple_slot(t, 0));
  }
  if (IS_TUPLE_CLASS(t, wrapped_sequence)) {
    return interp_length(tuple_slot(t, 0));
  }
  if (IS_TUPLE_CLASS(t, curried)) {
    return interp_length(tuple_slot(t, 1)) + 1;
  }
  if (IS_TUPLE_CLASS(t, composed)) {
    cell first = tuple_slot(t, 0);
    cell second = tuple_slot(t, 1);
    static int composed_len_count = 0;
    composed_len_count++;
    if (composed_len_count <= 5) {
      std::cerr << "[wasm] tuple_length composed #" << composed_len_count
                << ": first=0x" << std::hex << first << ":t" << TAG(first)
                << " second=0x" << second << ":t" << TAG(second) << std::dec << std::endl;
    }
    return interp_length(first) + interp_length(second);
  }
  if (IS_TUPLE_CLASS(t, vector)) {
    return untag_fixnum(tuple_slot(t, 1));
  }

  if (__builtin_expect(trace_enabled(), 0)) {
    std::string cname = tuple_class_name(t);
    std::cout << "[wasm] length unsupported tuple class " << cname << " ptr "
              << (void*)t << std::endl;
  }
  fatal_error("length: unsupported tuple class in wasm interpreter", (cell)t);
  return 0;
}

bool tuple_length_supported(tuple* t) {
  // Fast path using cached layouts
  return IS_TUPLE_CLASS(t, slice) || 
         IS_TUPLE_CLASS(t, reversed) || 
         IS_TUPLE_CLASS(t, wrapped_sequence) ||
         IS_TUPLE_CLASS(t, curried) || 
         IS_TUPLE_CLASS(t, composed) || 
         IS_TUPLE_CLASS(t, vector);
}

bool interp_length_supported(cell obj) {
  switch (tagged<object>(obj).type()) {
    case FIXNUM_TYPE:
    case BIGNUM_TYPE:
      return true;
    case ARRAY_TYPE:
    case BYTE_ARRAY_TYPE:
    case STRING_TYPE:
      return true;
    case QUOTATION_TYPE:
      return true;
    case WORD_TYPE: {
      word* w = untag<word>(obj);
      return tagged<object>(w->def).type() == QUOTATION_TYPE;
    }
    case WRAPPER_TYPE:
      return interp_length_supported(untag<wrapper>(obj)->object);
    case TUPLE_TYPE:
      return tuple_length_supported(untag<tuple>(obj));
    default:
      return false;
  }
}

fixnum interp_length(cell obj) {
  switch (tagged<object>(obj).type()) {
    case ARRAY_TYPE:
      return (fixnum)array_capacity(untag<array>(obj));
    case BYTE_ARRAY_TYPE:
      return (fixnum)array_capacity(untag<byte_array>(obj));
    case STRING_TYPE:
      return untag_fixnum(untag<string>(obj)->length);
    case FIXNUM_TYPE:
    case BIGNUM_TYPE:
      // Non-sequence numeric types don't have a length; treat as 0 to keep
      // the interpreter moving.
      return 0;
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
      if (trace_enabled()) {
        std::cout << "[wasm] FATAL: length unsupported type "
                  << tagged<object>(obj).type() << " ptr " << (void*)obj
                  << std::endl;
      }
      fatal_error("length: unsupported object type", obj);
      return 0;
  }
}

// Word handler IDs for fast dispatch
enum class WordHandlerId {
  UNKNOWN = 0,
  // Special words (interpret_special_word)
  SET_STRING_HASHCODE,
  NATIVE_STRING_ENCODING,
  UNDERLYING,
  DECODE_UNTIL,
  REHASH_STRING,
  GET_DATASTACK,
  NORMALIZE_PATH,
  FILE_READER,
  DATASTACK_FOR,
  RECOVER,
  NEW_SEQUENCE,
  STRING_HASHCODE,
  HASHCODE,
  HASHCODE_STAR,
  CALL_EFFECT,
  EXECUTE_EFFECT,
  SET_NTH,
  NTH_UNSAFE,
  WRAP,
  HASH_AT,
  PROBE,
  KEY_AT,
  KEY_AT_PAREN,
  AT_STAR,
  ASSOC_SIZE,
  REHASH,
  SET_AT,
  RESIZE_ARRAY,
  RESIZE_BYTE_ARRAY,
  RESIZE_STRING,
  DISPATCH_STATS,
  LOOKUP_METHOD,
  MEGA_CACHE_LOOKUP,
  FIND_WORD,
  WORD_NAME,
  WORD_PROPS,
  ALIEN_TO_STRING,
  ALIEN_GT_STRING,
  VALUE,
  LENGTH,
  QUESTION_MARK,
  IF,
  CALLABLE_QUESTION,
  // Subprimitives (dispatch_subprimitive)  
  DUP,
  DUPD,
  DROP,
  NIP,
  TWO_DROP,
  TWO_NIP,
  THREE_DROP,
  FOUR_DROP,
  TWO_DUP,
  THREE_DUP,
  FOUR_DUP,
  OVER,
  TWO_OVER,
  THREE_OVER,
  PICK,
  UNDER,
  SWAP,
  ROT,
  NEG_ROT,
  ROLL,
  NEG_ROLL,
  TWO_SWAP,
  SWAPD,
  SPIN,
  KEEP,
  DIP,
  TWO_DIP,
  THREE_DIP,
  FOUR_DIP,
  LOOP,
  WHILE,
  UNTIL,
  CURRY,
  COMPOSE,
  BOTH_QUESTION,
  EITHER_QUESTION,
  CACHE,
  COMPARE,
  EQ,
  NEQ,
  LT,
  LE,
  GT,
  GE,
  PLUS,
  MINUS,
  TIMES,
  DIV,
  FIXNUM_PLUS,
  FIXNUM_MINUS,
  FIXNUM_TIMES,
  FIXNUM_SLASH,
  FIXNUM_PLUS_FAST,
  FIXNUM_MINUS_FAST,
  FIXNUM_TIMES_FAST,
  FIXNUM_SLASH_I,
  FIXNUM_SLASH_I_FAST,
  FIXNUM_MOD,
  FIXNUM_AND,
  FIXNUM_OR,
  FIXNUM_XOR,
  FIXNUM_NOT,
  FIXNUM_LT,
  FIXNUM_LE,
  FIXNUM_GT,
  FIXNUM_GE,
  FIXNUM_SHIFT,
  FIXNUM_SHIFT_FAST,
  BIGNUM_COMPARE,
  BIGNUM_LT,
  BIGNUM_LE,
  BIGNUM_GT,
  BIGNUM_GE,
  BIGNUM_EQ,
  BIGNUM_PLUS,
  BIGNUM_MINUS,
  BIGNUM_TIMES,
  BIGNUM_SLASH,
  BIGNUM_SLASH_I,
  BIGNUM_MOD,
  BIGNUM_BITAND,
  BIGNUM_BITOR,
  BIGNUM_BITXOR,
  BIGNUM_BITNOT,
  BIGNUM_SHIFT,
  BIGNUM_GCD,
  BIGNUM_GT_FIXNUM,
  BIGNUM_GT_FLOAT,
  FIXNUM_GT_BIGNUM,
  FLOAT_GT_BIGNUM,
  FIXNUM_GT_FLOAT,
  FLOAT_GT_FIXNUM,
  FLOAT_PLUS,
  FLOAT_MINUS,
  FLOAT_TIMES,
  FLOAT_DIV,
  FLOAT_FLOOR,
  FLOAT_LT,
  FLOAT_LE,
  FLOAT_GT,
  FLOAT_GE,
  FLOAT_EQ,
  TO_FIXNUM,
  TO_FLOAT,
  INTEGER_GT_FLOAT,
  FLOAT_GT_INTEGER,
  FLOAT_TO_BITS,
  DOUBLE_TO_BITS,
  BITS_TO_FLOAT,
  BITS_TO_DOUBLE,
  CONTEXT_OBJECT,
  SET_CONTEXT_OBJECT,
  SET_SPECIAL_OBJECT,
  EQ_QUESTION,
  DATASTACK_QUESTION,
  WRITE_BARRIER,
  HASHCODE_SUBPRIM,
  RAW_HASHCODE_SUBPRIM,
  TAG,
  SLOT,
  STRING_NTH_FAST,
  FPU_STATE,
  SET_FPU_STATE,
  SET_CALLSTACK,
  C_TO_FACTOR,
  UNWIND_NATIVE_FRAMES,
  SIGNAL_HANDLERS,
  SET_CONTEXT,
  SET_CONTEXT_AND_DELETE,
};

} // namespace

// Handle a handful of core combinators directly so we don't depend on
// compiler inlining or JIT stubs that are unavailable in the wasm build.
bool factor_vm::interpret_special_word(const std::string& name) {
  const uint32_t h = runtime_hash(name);
  auto type_of = [](cell obj) { return tagged<object>(obj).type(); };

  auto hashcode_value = [this, &type_of](cell obj) -> cell {
    switch (type_of(obj)) {
      case FIXNUM_TYPE:
        return obj;
      case F_TYPE:
        return tag_fixnum(0);
      case STRING_TYPE: {
        string* str = untag<string>(obj);
        cell cached = str->hashcode;
        if (cached == false_object ||
            (TAG(cached) == FIXNUM_TYPE && cached == tag_fixnum(0))) {
          cached = tag_fixnum(compute_string_hash(str));
          str->hashcode = cached;
        }
        return cached;
      }
      case WORD_TYPE: {
        word* w = untag<word>(obj);
        return w->hashcode;
      }
      default: {
        if (immediate_p(obj))
          return tag_fixnum(0);
        object* o = untag<object>(obj);
        cell cached = o->hashcode();
        if (cached == 0)
          cached = (cell)o >> TAG_BITS; // simple fallback
        return tag_fixnum((fixnum)cached);
      }
    }
  };

  switch (h) {

  case WORD_HASH("set-string-hashcode"):
    if (name == "set-string-hashcode") {
      cell str_cell = ctx->pop();
      cell hash = ctx->pop();
      if (type_of(str_cell) != STRING_TYPE)
        fatal_error("set-string-hashcode: expected string", str_cell);
      string* str = untag<string>(str_cell);
      str->hashcode = hash;
      return true;
    }
    break;

  case WORD_HASH("native-string-encoding"):
    if (name == "native-string-encoding") {
      static cell utf8_word = false_object;
      if (utf8_word == false_object)
        utf8_word = find_word_by_name_local(this, "utf8");
      ctx->push(utf8_word != false_object ? utf8_word : false_object);
      return true;
    }
    break;

  case WORD_HASH("underlying>>"):
  case WORD_HASH("underlying>>>"):
  case WORD_HASH("object=>underlying>>"):
    if (name == "underlying>>" || name == "underlying>>>"
        || name == "object=>underlying>>") {
      cell obj = ctx->pop();
      // For wasm, just return the underlying object unchanged; if it's a wrapper
      // around a pathname tuple, unwrap slot0.
      if (TAG(obj) == WRAPPER_TYPE)
        obj = untag<wrapper>(obj)->object;
      if (TAG(obj) == TUPLE_TYPE) {
        tuple* t = untag<tuple>(obj);
        // If the tuple layout has at least one slot, return slot 0; otherwise
        // fall through to identity.
        tuple_layout* layout = untag<tuple_layout>(t->layout);
        if (tuple_capacity(layout) > 0)
          obj = tuple_slot(t, 0);
      }
      ctx->push(obj);
      return true;
    }
    break;

  case WORD_HASH("decode-until"):
  case WORD_HASH("(decode-until)"):
    if (name == "decode-until" || name == "(decode-until)") {
      cell encoding = ctx->pop();
      (void)encoding; // utf8 by default for wasm
      cell stream = ctx->pop();
      cell seps = ctx->pop();

      auto to_string_data = [&](cell obj) -> std::pair<const uint8_t*, cell> {
        if (TAG(obj) == STRING_TYPE) {
          string* s = untag<string>(obj);
          return {reinterpret_cast<const uint8_t*>(s + 1), string_capacity(s)};
        }
        if (TAG(obj) == BYTE_ARRAY_TYPE) {
          byte_array* ba = untag<byte_array>(obj);
          return {ba->data<uint8_t>(), array_capacity(ba)};
        }
        return {nullptr, 0};
      };

      // Treat sbuf/memory-stream tuple slot0 as underlying string if present.
      if (TAG(stream) == TUPLE_TYPE) {
        tuple* t = untag<tuple>(stream);
        if (tuple_capacity(untag<tuple_layout>(t->layout)) > 0)
          stream = tuple_slot(t, 0);
      }

      auto sep_data = to_string_data(seps);
      auto src_data = to_string_data(stream);
      if (!sep_data.first || !src_data.first) {
        ctx->push(false_object);
        ctx->push(false_object);
        return true;
      }

      // Find first separator byte.
      const uint8_t* sep_bytes = sep_data.first;
      cell sep_len = sep_data.second;
      const uint8_t* src = src_data.first;
      cell src_len = src_data.second;
      cell hit_index = -1;
      uint8_t hit_sep = 0;
      for (cell i = 0; i < src_len && hit_index < 0; i++) {
        uint8_t ch = src[i];
        for (cell j = 0; j < sep_len; j++) {
          if (ch == sep_bytes[j]) {
            hit_index = i;
            hit_sep = ch;
            break;
          }
        }
      }

      cell out_len = (hit_index >= 0) ? hit_index : src_len;
      data_root<string> out(tag<string>(allot_string(out_len, 0)), this);
      memcpy(out->data(), src, (size_t)out_len);
      ctx->push(out.value());
      if (hit_index >= 0)
        ctx->push(tag_fixnum((fixnum)hit_sep));
      else
        ctx->push(false_object);
      return true;
    }
    break;

  case WORD_HASH("rehash-string"):
    if (name == "rehash-string") {
      cell str_cell = ctx->pop();
      if (type_of(str_cell) != STRING_TYPE)
        fatal_error("rehash-string: expected string", str_cell);
      string* str = untag<string>(str_cell);
      str->hashcode = tag_fixnum(compute_string_hash(str));
      return true;
    }
    break;

  case WORD_HASH("get-datastack"):
    if (name == "get-datastack") {
      ctx->push(datastack_to_array(ctx));
      return true;
    }
    break;

  case WORD_HASH("normalize-path"):
    if (name == "normalize-path") {
      cell path = ctx->pop();
      cell str_cell = path;
      // If we were handed a pathname tuple, extract the underlying string slot.
      if (TAG(path) == TUPLE_TYPE) {
        tuple* t = untag<tuple>(path);
        cell slot = tuple_slot(t, 0);
        if (TAG(slot) == STRING_TYPE)
          str_cell = slot;
      }
      if (TAG(str_cell) == STRING_TYPE) {
        data_root<string> str(str_cell, this);
        std::string raw(reinterpret_cast<const char*>(str->data()),
                        (size_t)untag_fixnum(str->length));
        std::string resolved = resolve_wasm_path(raw);
        data_root<string> out(
            tag<string>(allot_string((cell)resolved.size(), 0)), this);
        memcpy(out->data(), resolved.data(), resolved.size());
        ctx->push(out.value());
      } else {
        ctx->push(path);
      }
      return true;
    }
    break;

  case WORD_HASH("native-string>alien"):
    if (name == "native-string>alien") {
      cell obj = ctx->pop();
      // Accept strings, byte-arrays, or pathname tuples; fall back to identity.
      cell str_cell = obj;
      if (TAG(obj) == TUPLE_TYPE) {
        tuple* t = untag<tuple>(obj);
        cell slot = tuple_slot(t, 0);
        if (TAG(slot) == STRING_TYPE)
          str_cell = slot;
      }

      if (TAG(str_cell) == BYTE_ARRAY_TYPE) {
        ctx->push(str_cell);
        return true;
      }

      if (TAG(str_cell) == STRING_TYPE) {
        data_root<string> str(str_cell, this);
        std::string raw(reinterpret_cast<const char*>(str->data()),
                        (size_t)untag_fixnum(str->length));
        std::string resolved = resolve_wasm_path(raw);
        data_root<byte_array> ba(
            tag<byte_array>(allot_byte_array((cell)resolved.size() + 1)), this);
        uint8_t* out = ba->data<uint8_t>();
        memcpy(out, resolved.data(), resolved.size());
        out[resolved.size()] = 0;
        ctx->push(ba.value());
        return true;
      }

      // Unknown input; preserve it.
      ctx->push(obj);
      return true;
    }
    break;

#if defined(FACTOR_WASM)
  // WASM fallback: skip platform init hooks that are unimplemented in the boot image.
  case WORD_HASH("init-io"):
  case WORD_HASH("init-stdio"):
    if (name == "init-io" || name == "init-stdio") {
      return true;
    }
    break;

  case WORD_HASH("print"):
    if (name == "print") {
      cell obj = ctx->pop();
      if (TAG(obj) == STRING_TYPE) {
        string* s = untag<string>(obj);
        std::string out(reinterpret_cast<const char*>(s->data()),
                        (size_t)untag_fixnum(s->length));
        std::cout << out << std::endl;
      } else {
        std::cout << "[wasm] print <obj 0x" << std::hex << obj << " type "
                  << TAG(obj) << ">" << std::dec << std::endl;
      }
      return true;
    }
    break;
#endif

  case WORD_HASH("file-exists?"):
    if (name == "file-exists?") {
#if defined(FACTOR_WASM)
      cell path = ctx->pop();
      // primitive_existsp expects the path on the stack
      ctx->push(path);
      primitive_existsp();
      return true;
#endif
    }
    break;

  case WORD_HASH("(file-reader)"):
    if (name == "(file-reader)") {
      cell path_obj = ctx->pop();

      // Normalize to string
      cell str_cell = path_obj;
      if (TAG(path_obj) == TUPLE_TYPE) {
        tuple* t = untag<tuple>(path_obj);
        cell slot = tuple_slot(t, 0);
        if (TAG(slot) == STRING_TYPE)
          str_cell = slot;
      }
      std::string path_str;
      if (TAG(str_cell) == STRING_TYPE) {
        string* s = untag<string>(str_cell);
        path_str.assign(reinterpret_cast<const char*>(s->data()),
                        (size_t)untag_fixnum(s->length));
      } else {
        // Fallback: nothing to do.
        ctx->push(false_object);
        return true;
      }
      std::string orig_path = path_str;
      path_str = resolve_wasm_path(path_str);
      
      if (trace_enabled())
        std::cout << "[wasm] (file-reader) orig='" << orig_path << "' resolved='" << path_str << "'" << std::endl;

      // Build byte arrays for path and mode ("rb")
      data_root<byte_array> path_ba(
          tag<byte_array>(allot_byte_array((cell)path_str.size() + 1)), this);
      memcpy(path_ba->data<uint8_t>(), path_str.data(), path_str.size());
      path_ba->data<uint8_t>()[path_str.size()] = 0;

      data_root<byte_array> mode_ba(
          tag<byte_array>(allot_byte_array(3)), this);
      mode_ba->data<uint8_t>()[0] = 'r';
      mode_ba->data<uint8_t>()[1] = 'b';
      mode_ba->data<uint8_t>()[2] = 0;

      // Call primitive_fopen (expects path, mode on stack).
      ctx->push(path_ba.value());
      ctx->push(mode_ba.value());
      primitive_fopen();

      // Wrap the handle in a c-reader stream using the existing Factor word.
      cell c_reader_word = find_word_by_name_local(this, "<c-reader>");
      if (c_reader_word != false_object) {
        interpret_word(c_reader_word);
      }
      return true;
    }
    break;

  case WORD_HASH("datastack-for"):
    if (name == "datastack-for") {
      cell alien_ctx = ctx->pop();
      context* other = (context*)pinned_alien_offset(alien_ctx);
      ctx->push(datastack_to_array(other));
      return true;
    }
    break;

  case WORD_HASH("recover"):
    if (name == "recover") {
      cell recovery = ctx->pop();
      (void)recovery;
      cell try_quot = ctx->pop();
      call_callable(try_quot);
      return true;
    }
    break;

  case WORD_HASH("new-sequence"):
    if (name == "new-sequence") {
      cell len_cell = ctx->pop();
      cell exemplar = ctx->pop();
      fixnum len = 0;
      if (TAG(len_cell) == FIXNUM_TYPE)
        len = untag_fixnum(len_cell);
      if (len < 0)
        len = 0;
      data_root<array> arr(tag<array>(allot_array(len, false_object)), this);
      ctx->push(arr.value());
      (void)exemplar; // we ignore exemplar shape for now
      return true;
    }
    break;

  case WORD_HASH("string-hashcode"):
    if (name == "string-hashcode") {
      cell str_cell = ctx->pop();
      if (type_of(str_cell) != STRING_TYPE)
        fatal_error("string-hashcode: expected string", str_cell);
      string* str = untag<string>(str_cell);
      ctx->push(str->hashcode);
      return true;
    }
    break;

  case WORD_HASH("hashcode"):
    if (name == "hashcode") {
      cell obj = ctx->pop();
      ctx->push(hashcode_value(obj));
      return true;
    }
    break;

  case WORD_HASH("hashcode*"):
    if (name == "hashcode*") {
      ctx->pop(); // depth parameter, unused in wasm interpreter
      cell obj = ctx->pop();
      ctx->push(hashcode_value(obj));
      return true;
    }
    break;

  case WORD_HASH("call-effect"):
  case WORD_HASH("call-effect-unsafe"):
    if (name == "call-effect" || name == "call-effect-unsafe") {
      cell effect = ctx->pop();
      (void)effect;
      cell callable = ctx->pop();
      switch (tagged<object>(callable).type()) {
        case QUOTATION_TYPE:
        case WORD_TYPE:
        case WRAPPER_TYPE:
        case TUPLE_TYPE:
          g_call_callable_source = "slowpath:call-effect";
          call_callable(callable);
          break;
        default:
          if (trace_enabled()) {
            std::cout << "[wasm] call-effect: skipping non-callable 0x"
                      << std::hex << callable << std::dec
                      << " tag=" << TAG(callable) << std::endl;
          }
          break;
      }
      return true;
    }
    break;

  case WORD_HASH("execute-effect"):
  case WORD_HASH("execute-effect-unsafe"):
    if (name == "execute-effect" || name == "execute-effect-unsafe") {
      cell effect = ctx->pop();
      (void)effect;
      cell w = ctx->pop();
      interpret_word(w);
      return true;
    }
    break;

  case WORD_HASH("set-nth"):
  case WORD_HASH("set-nth-unsafe"):
    if (name == "set-nth" || name == "set-nth-unsafe") {
      cell seq = ctx->pop();
      cell index_cell = ctx->pop();
      cell elt = ctx->pop();
      if (TAG(index_cell) != FIXNUM_TYPE) {
        return true; // ignore bad index
    }
    fixnum idx = untag_fixnum(index_cell);
      switch (TAG(seq)) {
        case ARRAY_TYPE: {
          array* a = untag<array>(seq);
          if (idx >= 0 && idx < (fixnum)array_capacity(a))
            set_array_nth(a, idx, elt);
          break;
        }
        case BYTE_ARRAY_TYPE: {
          byte_array* ba = untag<byte_array>(seq);
          if (idx >= 0 && idx < (fixnum)array_capacity(ba))
            ba->data<uint8_t>()[idx] = (uint8_t)untag_fixnum(elt);
          break;
        }
        case STRING_TYPE: {
          string* s = untag<string>(seq);
          if (idx >= 0 && idx < untag_fixnum(s->length))
            s->data()[idx] = (uint8_t)untag_fixnum(elt);
          break;
        }
        default:
          break;
      }
      return true;
    }
    break;

  case WORD_HASH("stream-write"):
  case WORD_HASH("f=>stream-write"):
    if (name == "stream-write" || name == "f=>stream-write") {
      cell stream = ctx->pop();
      (void)stream;
      cell data = ctx->pop();
      if (TAG(data) == STRING_TYPE) {
        string* s = untag<string>(data);
        cell len = untag_fixnum(s->length);
        std::cout.write(reinterpret_cast<const char*>(s->data()),
                        (std::streamsize)len);
      } else if (TAG(data) == BYTE_ARRAY_TYPE) {
        byte_array* ba = untag<byte_array>(data);
        cell len = array_capacity(ba);
        std::cout.write(reinterpret_cast<const char*>(ba->data<uint8_t>()),
                        (std::streamsize)len);
      }
      return true;
    }
    break;

  case WORD_HASH("stream-nl"):
  case WORD_HASH("f=>stream-nl"):
    if (name == "stream-nl" || name == "f=>stream-nl") {
      cell stream = ctx->pop();
      (void)stream;
      std::cout.put('\n');
      return true;
    }
    break;

  case WORD_HASH(">>props"):
  case WORD_HASH("props<<"):
  case WORD_HASH("object=>props<<"):
    if (name == ">>props" || name == "props<<" || name == "object=>props<<") {
      cell obj = ctx->pop();
      cell val = ctx->pop();
      if (TAG(obj) == WORD_TYPE) {
        word* w = untag<word>(obj);
        w->props = val;
      }
      ctx->push(obj);
      return true;
    }
    break;

  case WORD_HASH("<"):
    if (name == "<") {
      cell b = ctx->pop();
      cell a = ctx->pop();
      bool result = false;
      if (TAG(a) == FIXNUM_TYPE && TAG(b) == FIXNUM_TYPE)
        result = untag_fixnum(a) < untag_fixnum(b);
      ctx->push(tag_boolean(result));
      return true;
    }
    break;

  case WORD_HASH("<="):
    if (name == "<=") {
      cell b = ctx->pop();
      cell a = ctx->pop();
      bool result = false;
      if (TAG(a) == FIXNUM_TYPE && TAG(b) == FIXNUM_TYPE)
        result = untag_fixnum(a) <= untag_fixnum(b);
      ctx->push(tag_boolean(result));
      return true;
    }
    break;

  case WORD_HASH(">"):
    if (name == ">") {
      cell b = ctx->pop();
      cell a = ctx->pop();
      bool result = false;
      if (TAG(a) == FIXNUM_TYPE && TAG(b) == FIXNUM_TYPE)
        result = untag_fixnum(a) > untag_fixnum(b);
      ctx->push(tag_boolean(result));
      return true;
    }
    break;

  case WORD_HASH(">="):
    if (name == ">=") {
      cell b = ctx->pop();
      cell a = ctx->pop();
      bool result = false;
      if (TAG(a) == FIXNUM_TYPE && TAG(b) == FIXNUM_TYPE)
        result = untag_fixnum(a) >= untag_fixnum(b);
      ctx->push(tag_boolean(result));
      return true;
    }
    break;

  case WORD_HASH("integer>fixnum-strict"):
  case WORD_HASH("fixnum=>integer>fixnum-strict"):
  case WORD_HASH("object=>integer>fixnum-strict"):
    if (name == "integer>fixnum-strict" ||
        name == "fixnum=>integer>fixnum-strict" ||
        name == "object=>integer>fixnum-strict") {
      cell obj = ctx->pop();
      switch (TAG(obj)) {
        case FIXNUM_TYPE:
          ctx->push(obj);
          return true;
        case BIGNUM_TYPE: {
          bignum* bn = untag<bignum>(obj);
          ctx->push(tag_fixnum(bignum_to_fixnum_strict(bn)));
          return true;
        }
        case WRAPPER_TYPE:
          // Wrapped value comes from parser coercions; treat as f/0
          ctx->push(tag_fixnum(0));
          return true;
        default:
#if defined(FACTOR_WASM)
          // Be permissive in the interpreter; coerce anything else to 0 to
          // avoid cascading errors during bootstrap.
          ctx->push(tag_fixnum(0));
          return true;
#else
          fatal_error("integer>fixnum-strict: unsupported type", obj);
          return true;
#endif
      }
    }
    break;

  case WORD_HASH("value<<"):
  case WORD_HASH("object=>value<<"):
    if (name == "value<<" || name == "object=>value<<") {
      cell obj = ctx->pop();
      cell val = ctx->pop();
      if (TAG(obj) == TUPLE_TYPE) {
        tuple* t = untag<tuple>(obj);
        // Fast path: use cached layouts
        if (IS_TUPLE_CLASS(t, box) || IS_TUPLE_CLASS(t, global_box)) {
          t->data()[0] = val; // value slot
        }
      }
      ctx->push(obj);
      return true;
    }
    break;

  case WORD_HASH("value>>"):
  case WORD_HASH("value>>>"):
  case WORD_HASH("object=>value>>"):
    if (name == "value>>" || name == "value>>>" || name == "object=>value>>") {
      cell obj = ctx->pop();
      cell result = false_object;
      if (TAG(obj) == TUPLE_TYPE) {
        tuple* t = untag<tuple>(obj);
        // Fast path: use cached layouts
        if (IS_TUPLE_CLASS(t, box) || IS_TUPLE_CLASS(t, global_box)) {
          result = t->data()[0];
        }
      }
      ctx->push(result);
      return true;
    }
    break;

  case WORD_HASH("wrap"):
    if (name == "wrap") {
      cell array_cell = ctx->pop();
      cell i_cell = ctx->pop();
      if (type_of(array_cell) != ARRAY_TYPE || TAG(i_cell) != FIXNUM_TYPE)
        fatal_error("wrap: expected (fixnum array)", array_cell);
      fixnum len = (fixnum)array_capacity(untag<array>(array_cell));
      if (len <= 0)
        fatal_error("wrap: zero-length array", array_cell);
      fixnum mask = len - 1;
      fixnum i = untag_fixnum(i_cell);
      ctx->push(tag_fixnum(i & mask));
      return true;
    }
    break;

  case WORD_HASH("hash@"):
    if (name == "hash@") {
      cell array_cell = ctx->pop();
      cell key = ctx->pop();
      if (type_of(array_cell) != ARRAY_TYPE)
        fatal_error("hash@: expected array", array_cell);
      fixnum len = (fixnum)array_capacity(untag<array>(array_cell));
      if (len <= 0)
        fatal_error("hash@: zero-length array", array_cell);
      fixnum mask = len - 1;
      fixnum h = untag_fixnum(hashcode_value(key));
      // Match Factor definition: (hashcode>fixnum dup +) masked by wrap
      fixnum idx = (h + h) & mask;
      ctx->push(tag_fixnum(idx));
      return true;
    }
    break;

  case WORD_HASH("probe"):
    if (name == "probe") {
      cell probe_cell = ctx->pop();
      cell i_cell = ctx->pop();
      cell array_cell = ctx->pop();
      if (type_of(array_cell) != ARRAY_TYPE ||
          TAG(i_cell) != FIXNUM_TYPE ||
          TAG(probe_cell) != FIXNUM_TYPE)
        fatal_error("probe: expected (array fixnum fixnum)", array_cell);
      fixnum len = (fixnum)array_capacity(untag<array>(array_cell));
      if (len <= 0)
        fatal_error("probe: zero-length array", array_cell);
      fixnum mask = len - 1;
      fixnum probe = untag_fixnum(probe_cell) + 2; // advance by 2 each probe
      fixnum i = untag_fixnum(i_cell);
      fixnum new_i = (i + probe) & mask;
      ctx->push(array_cell);
      ctx->push(tag_fixnum(new_i));
      ctx->push(tag_fixnum(probe));
      return true;
    }
    break;

  case WORD_HASH("key@"):
    if (name == "key@") {
      cell hash_cell = ctx->pop();
      cell key_cell = ctx->pop();
      hashtable_lookup_result r = hashtable_lookup(key_cell, hash_cell);
      ctx->push(r.array_cell);
      ctx->push(r.found ? tag_fixnum(r.index) : false_object);
      ctx->push(tag_boolean(r.found));
      return true;
    }
    break;

  case WORD_HASH("(key@)"):
    if (name == "(key@)") {
      // ( key array i probe# -- array n ? ) quadratic probe
      cell probe_cell = ctx->pop();
      cell i_cell = ctx->pop();
      cell array_cell = ctx->pop();
      cell key_cell = ctx->pop();
      if (TAG(array_cell) != ARRAY_TYPE || TAG(i_cell) != FIXNUM_TYPE ||
          TAG(probe_cell) != FIXNUM_TYPE) {
        ctx->push(array_cell);
        ctx->push(false_object);
        ctx->push(false_object);
        return true;
      }
      array* arr = untag<array>(array_cell);
      fixnum len = (fixnum)array_capacity(arr);
      fixnum mask = len > 0 ? len - 1 : 0;
      fixnum probe = untag_fixnum(probe_cell);
      fixnum idx = untag_fixnum(i_cell);
      for (fixnum step = 0; step < len; step++) {
        cell entry = array_nth(arr, idx);
        if (is_empty_sentinel(entry)) {
          ctx->push(array_cell);
          ctx->push(false_object);
          ctx->push(false_object);
          return true;
        }
        if (!is_tombstone_sentinel(entry) && objects_equal(entry, key_cell)) {
          ctx->push(array_cell);
          ctx->push(tag_fixnum(idx));
          ctx->push(tag_boolean(true));
          return true;
        }
        probe += 2;
        idx = (idx + probe) & mask;
      }
      ctx->push(array_cell);
      ctx->push(false_object);
      ctx->push(false_object);
      return true;
    }
    break;

  case WORD_HASH("at*"):
    if (name == "at*") {
      cell assoc = ctx->pop();
      cell key = ctx->pop();
      hashtable_lookup_result r = hashtable_lookup(key, assoc);
      if (!r.found || r.index < 0) {
        ctx->push(false_object);
        ctx->push(false_object);
      } else {
        array* arr = untag<array>(r.array_cell);
        if (r.index + 1 >= (fixnum)array_capacity(arr)) {
          ctx->push(false_object);
          ctx->push(false_object);
          return true;
        }
        cell val = array_nth(arr, r.index + 1);
        ctx->push(val);
        ctx->push(tag_boolean(true));
      }
      return true;
    }
    break;

  case WORD_HASH("assoc-size"):
    if (name == "assoc-size") {
      cell assoc = ctx->pop();
      if (TAG(assoc) == TUPLE_TYPE) {
        tuple* h = untag<tuple>(assoc);
        // Fast path: use cached layout
        if (IS_TUPLE_CLASS(h, hashtable)) {
          fixnum count = untag_fixnum(tuple_slot(h, 0));
          fixnum deleted = untag_fixnum(tuple_slot(h, 1));
          ctx->push(tag_fixnum(count - deleted));
          return true;
        }
      }
      ctx->push(tag_fixnum(0));
      return true;
    }
    break;

  case WORD_HASH("rehash"):
    if (name == "rehash") {
      ctx->pop(); // hash
      return true;
    }
    break;

  case WORD_HASH("word-prop"):
  case WORD_HASH("props>>"):
  case WORD_HASH("word=>props>>"):
    if (name == "word-prop" || name == "props>>" || name == "word=>props>>") {
      cell key = ctx->pop();
      cell w = ctx->pop();
      if (TAG(w) == WORD_TYPE) {
        word* ww = untag<word>(w);
        cell props = ww->props;
        if (props != false_object) {
          hashtable_lookup_result r = hashtable_lookup(key, props);
          if (r.found) {
            array* arr = untag<array>(r.array_cell);
            if (r.index + 1 < (fixnum)array_capacity(arr)) {
              cell val = array_nth(arr, r.index + 1);
              ctx->push(val);
              return true;
            }
          }
        }
      }
      ctx->push(false_object);
      return true;
    }
    break;

  case WORD_HASH("equal?"):
    if (name == "equal?") {
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(tag_boolean(objects_equal(a, b)));
      return true;
    }
    break;

  case WORD_HASH("call"):
  case WORD_HASH("(call)"):
    if (name == "call" || name == "(call)") {
      cell callable = ctx->pop();
      call_callable(callable);
      return true;
    }
    break;

  case WORD_HASH("execute"):
  case WORD_HASH("(execute)"):
    if (name == "execute" || name == "(execute)") {
      cell w = ctx->pop();
      if (type_of(w) != WORD_TYPE)
        fatal_error("execute: expected word", w);
      interpret_word(w);
      return true;
    }
    break;

  case WORD_HASH("dip"):
    if (name == "dip") {
      if (trace_enabled())
        std::cout << "[wasm] dip entering (depth=" << ctx->depth() << ")" << std::endl;
      data_root<object> quot(ctx->pop(), this);
      if (trace_enabled())
        std::cout << "[wasm] dip after pop1 (depth=" << ctx->depth() << ")" << std::endl;
      data_root<object> saved(ctx->pop(), this);
      if (trace_enabled())
        std::cout << "[wasm] dip: saved=0x" << std::hex << saved.value() 
                  << " type=" << TAG(saved.value()) << std::dec << std::endl;
      interpret_quotation(quot.value());
      ctx->push(saved.value());
      return true;
    }
    break;

  case WORD_HASH("2dip"):
    if (name == "2dip") {
      data_root<object> quot(ctx->pop(), this);
      data_root<object> b(ctx->pop(), this);
      data_root<object> a(ctx->pop(), this);
      interpret_quotation(quot.value());
      ctx->push(a.value());
      ctx->push(b.value());
      return true;
    }
    break;

  case WORD_HASH("3dip"):
    if (name == "3dip") {
      data_root<object> quot(ctx->pop(), this);
      data_root<object> c(ctx->pop(), this);
      data_root<object> b(ctx->pop(), this);
      data_root<object> a(ctx->pop(), this);
      interpret_quotation(quot.value());
      ctx->push(a.value());
      ctx->push(b.value());
      ctx->push(c.value());
      return true;
    }
    break;

  case WORD_HASH("mega-cache-lookup"):
    if (name == "mega-cache-lookup") {
      // This is a method dispatch subprimitive.
      // Stack: ( methods index cache -- )
      // The object to dispatch on is at datastack[-index]
      cell cache = ctx->pop();
      cell index = untag_fixnum(ctx->pop());
      cell methods = ctx->pop();
      
      // Get the object to dispatch on from the stack (without removing it)
      // dispatch# is 0-indexed: 0 = top of stack, 1 = second from top, etc.
      cell* sp = (cell*)ctx->datastack;
      cell object = sp[-index];
      
      // Look up the method
      cell method = lookup_method(object, methods);
      cell method_type = TAG(method);
      std::string method_name;
      if (method_type == WORD_TYPE)
        method_name = word_name_string(untag<word>(method));
      if (method_type == WORD_TYPE) {
        word* mw = untag<word>(method);
        std::string mname = word_name_string(mw);
        (void)mname;
      }
      
      if (trace_enabled()) {
        std::cout << "[wasm] mega-cache-lookup: index=" << index 
                  << " object=0x" << std::hex << object 
                  << " method=0x" << method << std::dec
                  << " method_type=" << method_type;
        if (method_type == WORD_TYPE) {
          std::cout << " method_name=" << word_name_string(untag<word>(method));
        }
        std::cout << std::endl;
        // Dump top 5 stack items with tags
        std::cout << "[wasm] stack (top 5):";
        cell* sp = (cell*)ctx->datastack;
        for (int i = 0; i < 5 && (cell)(sp - i) >= ctx->datastack_seg->start; i++) {
          cell v = sp[-i];
          std::cout << " [" << i << "]=0x" << std::hex << v << std::dec << "(t"
                    << TAG(v) << ")";
          if (TAG(v) == WORD_TYPE) {
            std::cout << "<" << word_name_string(untag<word>(v)) << ">";
          } else if (TAG(v) == TUPLE_TYPE) {
            std::cout << "<" << tuple_class_name(untag<tuple>(v)) << ">";
          }
        }
        std::cout << std::endl;
      }
      
      // Update the cache (optional for correctness but improves next lookup)
      update_method_cache(cache, object_class(object), method);

      if (trace_enabled()) {
        static int lookup_logs = 0;
        if (lookup_logs < 2) {  // Reduced from 5 to 2
          std::cout << "[wasm] mega-cache-lookup: obj tag=" << TAG(object);
          if (TAG(object) == TUPLE_TYPE)
            std::cout << " class=" << tuple_class_name(untag<tuple>(object));
          std::cout << " -> ";
          if (method_type == WORD_TYPE)
            std::cout << "word " << method_name;
          else if (method_type == QUOTATION_TYPE)
            std::cout << "quotation";
          else
            std::cout << "type " << method_type;
          std::cout << std::endl;
          lookup_logs++;
        }
      }

      if (trace_enabled() && method_type == WORD_TYPE &&
          method_name.find("underlying") != std::string::npos) {
        static int underlying_logs = 0;
        if (underlying_logs < 1) {  // Reduced from 2 to 1
          underlying_logs++;
          std::cout << "[wasm] mega-cache-lookup special: method=" << method_name
                    << " obj tag=" << TAG(object);
          if (TAG(object) == TUPLE_TYPE)
            std::cout << " class=" << tuple_class_name(untag<tuple>(object));
          std::cout << std::endl;
        }
      }

      if (trace_enabled() && method_type == WORD_TYPE && method_name == "no-method") {
        static int no_method_logs = 0;
        if (no_method_logs < 3) {
          no_method_logs++;
          std::cout << "[wasm] mega-cache-lookup no-method for obj tag="
                    << TAG(object);
          if (TAG(object) == TUPLE_TYPE)
            std::cout << " class=" << tuple_class_name(untag<tuple>(object));
          std::cout << " methods=" << (void*)methods << " cache=" << (void*)cache
                    << std::endl;
        }
      }
      
      // ALWAYS log no-method calls (unconditionally) to debug missing methods
      if (method_type == WORD_TYPE && method_name == "no-method") {
        static int no_method_count = 0;
        static std::map<std::string, int> no_method_by_class;
        no_method_count++;
        
        std::string obj_class;
        if (TAG(object) == TUPLE_TYPE)
          obj_class = tuple_class_name(untag<tuple>(object));
        else
          obj_class = "tag" + std::to_string(TAG(object));
        
        no_method_by_class[obj_class]++;
        
        // Print first 10 occurrences with full details
        if (no_method_count <= 10) {
          std::cerr << "[wasm] NO-METHOD #" << no_method_count 
                    << " object_type=" << obj_class;
          // Try to identify the generic word from the methods array
          if (TAG(methods) == ARRAY_TYPE) {
            array* methods_arr = untag<array>(methods);
            // The methods array often has the generic word at index 0 or 1
            if (array_capacity(methods_arr) > 0) {
              cell first = array_nth(methods_arr, 0);
              if (TAG(first) == WORD_TYPE)
                std::cerr << " generic=" << word_name_string(untag<word>(first));
            }
          }
          std::cerr << std::endl;
        }
        
        // Print summary at milestones
        if (no_method_count == 100 || no_method_count == 1000 || 
            no_method_count == 10000 || no_method_count == 50000) {
          std::cerr << "[wasm] NO-METHOD summary after " << no_method_count << " calls:" << std::endl;
          for (auto& p : no_method_by_class) {
            std::cerr << "  " << p.first << ": " << p.second << std::endl;
          }
        }
      }
      
      // Execute the found method (should be a word)
      if (method_type == WORD_TYPE) {
        if (trace_enabled()) {
          std::cout << "[wasm] mega-cache-lookup calling word "
                    << word_name_string(untag<word>(method)) << std::endl;
          if (word_name_string(untag<word>(method)) == "no-method") {
            static int no_method_logs = 0;
            if (no_method_logs < 4) {
              no_method_logs++;
              std::cout << "[wasm] mega-cache-lookup calling no-method; object tag="
                        << TAG(object) << " class=";
              if (TAG(object) == TUPLE_TYPE)
                std::cout << tuple_class_name(untag<tuple>(object));
              else
                std::cout << "(immediate)";
              std::cout << std::endl;
            }
          }
        }
        interpret_word(method);
      } else if (method_type == QUOTATION_TYPE) {
        // Might be a quotation or other callable
        if (trace_enabled())
          std::cout << "[wasm] mega-cache-lookup: quotation method, calling" << std::endl;
        interpret_quotation(method);
      } else {
        if (trace_enabled())
          std::cout << "[wasm] mega-cache-lookup: unexpected method type " << method_type << std::endl;
        fatal_error("mega-cache-lookup: unexpected method type", method);
      }
      return true;
    }
    break;

  case WORD_HASH("length"):
    if (name == "length") {
      cell obj = ctx->peek();
      if (!interp_length_supported(obj))
        return false; // Fallback to normal word execution/dispatch
      ctx->pop();
      if (trace_enabled()) {
        std::cout << "[wasm] length: obj=0x" << std::hex << obj 
                  << " type=" << TAG(obj) << std::dec << std::endl;
      }
      ctx->push(tag_fixnum(interp_length(obj)));
      return true;
    }
    break;

  case WORD_HASH("?"):
    if (name == "?") {
      cell false_val = ctx->pop();
      cell true_val = ctx->pop();
      cell cond = ctx->pop();
      ctx->push(to_boolean(cond) ? true_val : false_val);
      return true;
    }
    break;

  case WORD_HASH("if"):
    if (name == "if") {
      cell false_quot = ctx->pop();
      cell true_quot = ctx->pop();
      cell cond = ctx->pop();
      call_callable(to_boolean(cond) ? true_quot : false_quot);
      return true;
    }
    break;

  case WORD_HASH("callable?"):
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
          tuple* tup = untag<tuple>(obj);
          // Fast path: use cached layouts
          callable = IS_TUPLE_CLASS(tup, curried) || IS_TUPLE_CLASS(tup, composed);
        }
        break;
      }
      ctx->push(tag_boolean(callable));
      return true;
    }
    break;

  default:
    break;
  }

  return false;
}

// Primitive dispatch with compile-time hash switch
// This avoids string allocation and uses fast hash lookup
bool factor_vm::dispatch_primitive_call(byte_array* name) {
  std::string_view prim = byte_array_to_string_view(name);
  
  if (__builtin_expect(trace_enabled(), 0)) {
    std::cout << "[wasm] prim " << prim << std::endl;
    if (prim == "primitive_set_slot") {
      std::cout << "[wasm] before primitive_set_slot" << std::endl;
      dump_stack(this, 10);
    } else if (prim == "primitive_existsp") {
      std::cout << "[wasm] before primitive_existsp" << std::endl;
      dump_stack(this, 12);
    }
  }
  
  // Use fnv1a hash for switch dispatch
  uint32_t h = fnv1a_hash(prim.data(), prim.size());
  
  // Macro to generate case with hash collision check
  #define PRIM_HASH_CASE(id)                                    \
    case WORD_HASH("primitive_" #id):                           \
      if (prim == "primitive_" #id) { primitive_##id(); return true; } \
      break;

  switch (h) {
    EACH_PRIMITIVE(PRIM_HASH_CASE)
    default:
      break;
  }
  #undef PRIM_HASH_CASE
  
  return false;
}

// ============================================================================
// Trampoline-based Interpreter (Non-recursive)
// ============================================================================
// The WASM runtime has limited stack space. To avoid stack overflow during
// Factor bootstrap (which can require 200k+ nested quotation/word executions),
// we use a trampoline architecture where no interpretation function calls
// another. Instead, all work is pushed onto a global work stack.

// Work item types - what kind of work needs to be done
enum class WorkType : uint8_t {
  // Execute elements of a quotation array starting at index
  QUOTATION_CONTINUE,
  // Execute a word (dispatch by handler or run its definition)
  EXECUTE_WORD,
  // Push a value onto the data stack
  PUSH_VALUE,
  // Execute a callable (quotation/word/curried/composed)
  CALL_CALLABLE,
  // After executing, push N saved values back onto stack (for dip variants)
  RESTORE_VALUES,
  // Loop: re-execute quotation if result is true
  LOOP_CONTINUE,
  // While loop: test predicate, execute body if true
  WHILE_CONTINUE,
};

// A work item on the trampoline stack
struct WorkItem {
  WorkType type;
  union {
    struct {  // QUOTATION_CONTINUE
      cell array_tagged;  // Tagged array pointer
      cell length;
      cell index;
    } quot;
    struct {  // EXECUTE_WORD
      cell word_tagged;
    } word;
    struct {  // PUSH_VALUE, CALL_CALLABLE
      cell value;
    } single;
    struct {  // RESTORE_VALUES
      cell values[4];  // Up to 4 values (for 4dip)
      uint8_t count;
    } restore;
    struct {  // LOOP_CONTINUE
      cell quotation;  // The loop body
    } loop;
    struct {  // WHILE_CONTINUE
      cell pred;  // Predicate quotation
      cell body;  // Body quotation
    } while_loop;
  };
};

// Global trampoline work stack
// We use a large fixed-size stack to avoid heap allocations
static constexpr size_t TRAMPOLINE_STACK_CAPACITY = 1024 * 1024;  // 1M items
static WorkItem* g_trampoline_stack = nullptr;
static size_t g_trampoline_size = 0;

// Initialize the trampoline stack (called once at startup)
inline void init_trampoline_stack() {
  if (g_trampoline_stack == nullptr) {
    g_trampoline_stack = new WorkItem[TRAMPOLINE_STACK_CAPACITY];
    g_trampoline_size = 0;
  }
}

// Flag to indicate if we're currently inside the trampoline loop
// When true, handlers should push work instead of making recursive calls
static thread_local bool g_in_trampoline = false;

// RAII guard for trampoline mode
struct TrampolineGuard {
  bool was_active;
  TrampolineGuard() : was_active(g_in_trampoline) { g_in_trampoline = true; }
  ~TrampolineGuard() { g_in_trampoline = was_active; }
};

// Push a work item onto the trampoline stack
inline void trampoline_push(const WorkItem& item) {
  if (g_trampoline_size >= TRAMPOLINE_STACK_CAPACITY) {
    std::cerr << "[wasm] FATAL: trampoline stack overflow at " << g_trampoline_size << " items" << std::endl;
    critical_error("trampoline stack overflow", g_trampoline_size);
  }
  g_trampoline_stack[g_trampoline_size++] = item;
}

// Pop a work item from the trampoline stack
inline WorkItem trampoline_pop() {
  return g_trampoline_stack[--g_trampoline_size];
}

// Check if trampoline stack is empty
inline bool trampoline_empty() {
  return g_trampoline_size == 0;
}

// Helper to push a quotation continuation
inline void push_quotation_work(cell array_tagged, cell length, cell start_index) {
  if (start_index >= length) return;  // Nothing to do
  WorkItem item;
  item.type = WorkType::QUOTATION_CONTINUE;
  item.quot.array_tagged = array_tagged;
  item.quot.length = length;
  item.quot.index = start_index;
  trampoline_push(item);
}

// Helper to push word execution
inline void push_word_work(cell word_tagged) {
  WorkItem item;
  item.type = WorkType::EXECUTE_WORD;
  item.word.word_tagged = word_tagged;
  trampoline_push(item);
}

// Helper to push callable execution
inline void push_callable_work(cell callable) {
  WorkItem item;
  item.type = WorkType::CALL_CALLABLE;
  item.single.value = callable;
  trampoline_push(item);
}

// Helper to push a value restore (for dip variants)
inline void push_restore_1(cell v0) {
  WorkItem item;
  item.type = WorkType::RESTORE_VALUES;
  item.restore.values[0] = v0;
  item.restore.count = 1;
  trampoline_push(item);
}

inline void push_restore_2(cell v0, cell v1) {
  WorkItem item;
  item.type = WorkType::RESTORE_VALUES;
  item.restore.values[0] = v0;
  item.restore.values[1] = v1;
  item.restore.count = 2;
  trampoline_push(item);
}

inline void push_restore_3(cell v0, cell v1, cell v2) {
  WorkItem item;
  item.type = WorkType::RESTORE_VALUES;
  item.restore.values[0] = v0;
  item.restore.values[1] = v1;
  item.restore.values[2] = v2;
  item.restore.count = 3;
  trampoline_push(item);
}

// Helper to push loop continuation
inline void push_loop_work(cell quotation) {
  WorkItem item;
  item.type = WorkType::LOOP_CONTINUE;
  item.loop.quotation = quotation;
  trampoline_push(item);
}

// Helper to push while loop continuation
inline void push_while_work(cell pred, cell body) {
  WorkItem item;
  item.type = WorkType::WHILE_CONTINUE;
  item.while_loop.pred = pred;
  item.while_loop.body = body;
  trampoline_push(item);
}

// ============================================================================
// Handler ID Caching System
// ============================================================================
// We repurpose the word's pic_def field (unused in WASM since no JIT) to cache
// a handler ID. This eliminates string lookups after the first call to each word.
//
// Handler IDs:
//   - false_object (0x6): Not yet cached (sentinel)
//   - tag_fixnum(0): No special handler, use quotation def
//   - tag_fixnum(1-99): Special word handlers (interpret_special_word)
//   - tag_fixnum(100-199): Subprimitive handlers (dispatch_subprimitive)

enum WasmHandlerId : int32_t {
  HANDLER_UNCACHED = -1,    // Sentinel - use false_object
  HANDLER_NONE = 0,         // No special handler, use quotation
  
  // Special word handlers (1-99)
  HANDLER_SET_STRING_HASHCODE = 1,
  HANDLER_NATIVE_STRING_ENCODING,
  HANDLER_UNDERLYING,
  HANDLER_DECODE_UNTIL,
  HANDLER_PAREN_DECODE_UNTIL,
  HANDLER_REHASH_STRING,
  HANDLER_GET_DATASTACK,
  HANDLER_NORMALIZE_PATH,
  HANDLER_NATIVE_STRING_TO_ALIEN,
  HANDLER_INIT_IO,
  HANDLER_INIT_STDIO,
  HANDLER_PRINT,
  HANDLER_FILE_EXISTS,
  HANDLER_PAREN_FILE_READER,
  HANDLER_DATASTACK_FOR,
  HANDLER_RECOVER,
  HANDLER_NEW_SEQUENCE,
  HANDLER_STRING_HASHCODE,
  HANDLER_HASHCODE,
  HANDLER_HASHCODE_STAR,
  HANDLER_CALL_EFFECT,
  HANDLER_EXECUTE_EFFECT,
  HANDLER_SET_NTH,
  HANDLER_STREAM_WRITE,
  HANDLER_STREAM_NL,
  HANDLER_PROPS_WRITE,
  HANDLER_LESS_THAN,
  HANDLER_LESS_EQUAL,
  HANDLER_GREATER_THAN,
  HANDLER_GREATER_EQUAL,
  HANDLER_INTEGER_TO_FIXNUM_STRICT,
  HANDLER_VALUE_WRITE,
  HANDLER_VALUE_READ,
  HANDLER_WRAP,
  HANDLER_HASH_AT,
  HANDLER_PROBE,
  HANDLER_KEY_AT,
  HANDLER_PAREN_KEY_AT,
  HANDLER_AT_STAR,
  HANDLER_ASSOC_SIZE,
  HANDLER_REHASH,
  HANDLER_WORD_PROP,
  HANDLER_EQUAL,
  HANDLER_CALL,
  HANDLER_PAREN_CALL,
  HANDLER_EXECUTE,
  HANDLER_PAREN_EXECUTE,
  HANDLER_DIP,
  HANDLER_2DIP,
  HANDLER_3DIP,
  HANDLER_MEGA_CACHE_LOOKUP,
  HANDLER_LENGTH,
  HANDLER_QUESTION,
  HANDLER_IF,
  HANDLER_CALLABLE,
  HANDLER_BOX,
  HANDLER_CURRIED,
  HANDLER_COMPOSED,
  HANDLER_NO_METHOD,
  
  // High-frequency combinator handlers (70-99)
  HANDLER_UNLESS,
  HANDLER_WHEN,
  HANDLER_KEEP,
  HANDLER_2KEEP,
  HANDLER_3KEEP,
  HANDLER_BI,
  HANDLER_TRI,
  HANDLER_BI_STAR,
  HANDLER_TRI_STAR,
  HANDLER_BI_AT,
  HANDLER_TRI_AT,
  HANDLER_BOTH_QUESTION,
  HANDLER_EITHER_QUESTION,
  HANDLER_LOOP,
  HANDLER_WHILE,
  HANDLER_UNTIL,
  HANDLER_DO,
  HANDLER_TIMES,
  HANDLER_EACH,
  HANDLER_MAP,
  HANDLER_REDUCE,
  HANDLER_FILTER,
  HANDLER_TUPLE_BOA,
  HANDLER_CURRY_WORD,
  HANDLER_2CURRY,
  HANDLER_3CURRY,
  HANDLER_COMPOSE_WORD,
  HANDLER_PREPOSE,
  HANDLER_SPECIAL_OBJECT,
  HANDLER_OBJ_GLOBAL,
  HANDLER_OBJ_CURRENT_THREAD,
  HANDLER_CONTEXT_OBJECT,
  HANDLER_GET_CATCHSTACK,
  HANDLER_SET_GLOBAL,
  HANDLER_GET_GLOBAL,
  HANDLER_GLOBAL,
  HANDLER_T_CONSTANT,
  HANDLER_CONTEXT_OBJ_CONTEXT,
  HANDLER_CONTEXT_OBJ_NAMESTACK,
  HANDLER_CONTEXT_OBJ_CATCHSTACK,
  HANDLER_GET_NAMESTACK,
  HANDLER_GET_CALLSTACK,
  HANDLER_GET_RETAINSTACK,
  HANDLER_WORD_QUESTION,
  HANDLER_EQUALS,
  HANDLER_RETHROW,
  HANDLER_OR,
  HANDLER_AND,
  HANDLER_NOT,
  HANDLER_TUPLE_WORD,
  HANDLER_MINUS,
  HANDLER_CLONE,
  HANDLER_SET_AT,
  HANDLER_CACHE,
  HANDLER_IF_EMPTY,
  HANDLER_BOX_AT,
  
  // Subprimitive handlers (200-299) - moved to avoid collision
  HANDLER_SUBPRIM_BASE = 200,
  HANDLER_DUP = 200,
  HANDLER_DUPD,
  HANDLER_DROP,
  HANDLER_NIP,
  HANDLER_2DROP,
  HANDLER_2NIP,
  HANDLER_3DROP,
  HANDLER_4DROP,
  HANDLER_2DUP,
  HANDLER_3DUP,
  HANDLER_4DUP,
  HANDLER_OVER,
  HANDLER_2OVER,
  HANDLER_PICK,
  HANDLER_SWAP,
  HANDLER_SWAPD,
  HANDLER_ROT,
  HANDLER_MINUS_ROT,
  HANDLER_EQ,
  HANDLER_BOTH_FIXNUMS,
  HANDLER_FIXNUM_BITAND,
  HANDLER_FIXNUM_BITOR,
  HANDLER_FIXNUM_BITXOR,
  HANDLER_FIXNUM_BITNOT,
  HANDLER_FIXNUM_LT,
  HANDLER_FIXNUM_LE,
  HANDLER_FIXNUM_GT,
  HANDLER_FIXNUM_GE,
  HANDLER_FIXNUM_PLUS,
  HANDLER_FIXNUM_MINUS,
  HANDLER_FIXNUM_TIMES,
  HANDLER_FIXNUM_MOD,
  HANDLER_FIXNUM_DIVI,
  HANDLER_FIXNUM_DIVMOD,
  HANDLER_FIXNUM_SHIFT,
  HANDLER_DROP_LOCALS,
  HANDLER_LOAD_LOCAL,
  HANDLER_GET_LOCAL,
  HANDLER_TAG,
  HANDLER_SLOT,
  HANDLER_STRING_NTH_FAST,
  HANDLER_NTH_UNSAFE,
  HANDLER_FPU_STATE,
  HANDLER_SET_FPU_STATE,
  HANDLER_SET_CALLSTACK,
  HANDLER_C_TO_FACTOR,
  HANDLER_SET_CONTEXT,
  HANDLER_START_CONTEXT,
  HANDLER_SET_CONTEXT_DELETE,
  HANDLER_START_CONTEXT_DELETE,
  
  HANDLER_MAX
};

// Lookup table from word name to handler ID
static const std::unordered_map<std::string_view, WasmHandlerId>& get_handler_id_table() {
  static const std::unordered_map<std::string_view, WasmHandlerId> table = {
    // Special word handlers
    {"set-string-hashcode", HANDLER_SET_STRING_HASHCODE},
    {"native-string-encoding", HANDLER_NATIVE_STRING_ENCODING},
    {"underlying>>", HANDLER_UNDERLYING},
    {"underlying>>>", HANDLER_UNDERLYING},
    {"object=>underlying>>", HANDLER_UNDERLYING},
    {"decode-until", HANDLER_DECODE_UNTIL},
    {"(decode-until)", HANDLER_PAREN_DECODE_UNTIL},
    {"rehash-string", HANDLER_REHASH_STRING},
    {"get-datastack", HANDLER_GET_DATASTACK},
    {"normalize-path", HANDLER_NORMALIZE_PATH},
    {"native-string>alien", HANDLER_NATIVE_STRING_TO_ALIEN},
    {"init-io", HANDLER_INIT_IO},
    {"init-stdio", HANDLER_INIT_STDIO},
    {"print", HANDLER_PRINT},
    {"file-exists?", HANDLER_FILE_EXISTS},
    {"(file-reader)", HANDLER_PAREN_FILE_READER},
    {"datastack-for", HANDLER_DATASTACK_FOR},
    {"recover", HANDLER_RECOVER},
    {"new-sequence", HANDLER_NEW_SEQUENCE},
    {"string-hashcode", HANDLER_STRING_HASHCODE},
    {"hashcode", HANDLER_HASHCODE},
    {"hashcode*", HANDLER_HASHCODE_STAR},
    {"call-effect", HANDLER_CALL_EFFECT},
    {"call-effect-unsafe", HANDLER_CALL_EFFECT},
    {"execute-effect", HANDLER_EXECUTE_EFFECT},
    {"execute-effect-unsafe", HANDLER_EXECUTE_EFFECT},
    {"set-nth", HANDLER_SET_NTH},
    {"set-nth-unsafe", HANDLER_SET_NTH},
    {"stream-write", HANDLER_STREAM_WRITE},
    {"f=>stream-write", HANDLER_STREAM_WRITE},
    {"stream-nl", HANDLER_STREAM_NL},
    {"f=>stream-nl", HANDLER_STREAM_NL},
    {">>props", HANDLER_PROPS_WRITE},
    {"props<<", HANDLER_PROPS_WRITE},
    {"object=>props<<", HANDLER_PROPS_WRITE},
    {"<", HANDLER_LESS_THAN},
    {"<=", HANDLER_LESS_EQUAL},
    {">", HANDLER_GREATER_THAN},
    {">=", HANDLER_GREATER_EQUAL},
    {"integer>fixnum-strict", HANDLER_INTEGER_TO_FIXNUM_STRICT},
    {"fixnum=>integer>fixnum-strict", HANDLER_INTEGER_TO_FIXNUM_STRICT},
    {"object=>integer>fixnum-strict", HANDLER_INTEGER_TO_FIXNUM_STRICT},
    {"value<<", HANDLER_VALUE_WRITE},
    {"object=>value<<", HANDLER_VALUE_WRITE},
    {"value>>", HANDLER_VALUE_READ},
    {"value>>>", HANDLER_VALUE_READ},
    {"object=>value>>", HANDLER_VALUE_READ},
    {"wrap", HANDLER_WRAP},
    {"hash@", HANDLER_HASH_AT},
    {"probe", HANDLER_PROBE},
    {"key@", HANDLER_KEY_AT},
    {"(key@)", HANDLER_PAREN_KEY_AT},
    {"at*", HANDLER_AT_STAR},
    {"assoc-size", HANDLER_ASSOC_SIZE},
    {"rehash", HANDLER_REHASH},
    {"word-prop", HANDLER_WORD_PROP},
    {"props>>", HANDLER_WORD_PROP},
    {"word=>props>>", HANDLER_WORD_PROP},
    {"equal?", HANDLER_EQUAL},
    {"call", HANDLER_CALL},
    {"(call)", HANDLER_PAREN_CALL},
    {"execute", HANDLER_EXECUTE},
    {"(execute)", HANDLER_PAREN_EXECUTE},
    {"dip", HANDLER_DIP},
    {"2dip", HANDLER_2DIP},
    {"3dip", HANDLER_3DIP},
    {"mega-cache-lookup", HANDLER_MEGA_CACHE_LOOKUP},
    {"length", HANDLER_LENGTH},
    {"?", HANDLER_QUESTION},
    {"if", HANDLER_IF},
    {"callable?", HANDLER_CALLABLE},
    {"box", HANDLER_BOX},
    {"global-box", HANDLER_BOX},
    {"curried", HANDLER_CURRIED},
    {"composed", HANDLER_COMPOSED},
    {"prepose", HANDLER_PREPOSE},
    {"no-method", HANDLER_NO_METHOD},
    
    // High-frequency combinators
    {"unless", HANDLER_UNLESS},
    {"when", HANDLER_WHEN},
    {"keep", HANDLER_KEEP},
    {"2keep", HANDLER_2KEEP},
    {"3keep", HANDLER_3KEEP},
    {"bi", HANDLER_BI},
    {"tri", HANDLER_TRI},
    {"bi*", HANDLER_BI_STAR},
    {"tri*", HANDLER_TRI_STAR},
    {"bi@", HANDLER_BI_AT},
    {"tri@", HANDLER_TRI_AT},
    {"both?", HANDLER_BOTH_QUESTION},
    {"either?", HANDLER_EITHER_QUESTION},
    {"loop", HANDLER_LOOP},
    {"while", HANDLER_WHILE},
    {"until", HANDLER_UNTIL},
    {"do", HANDLER_DO},
    {"times", HANDLER_TIMES},
    {"each", HANDLER_EACH},
    {"map", HANDLER_MAP},
    {"reduce", HANDLER_REDUCE},
    {"filter", HANDLER_FILTER},
    {"special-object", HANDLER_SPECIAL_OBJECT},
    {"OBJ-GLOBAL", HANDLER_OBJ_GLOBAL},
    {"OBJ-CURRENT-THREAD", HANDLER_OBJ_CURRENT_THREAD},
    {"context-object", HANDLER_CONTEXT_OBJECT},
    {"get-catchstack", HANDLER_GET_CATCHSTACK},
    {"(get-catchstack)", HANDLER_GET_CATCHSTACK},
    // set-global, get-global: not mapped - need hashtable ops, would always fallback
    {"global", HANDLER_GLOBAL},
    {"t", HANDLER_T_CONSTANT},
    {"CONTEXT-OBJ-CONTEXT", HANDLER_CONTEXT_OBJ_CONTEXT},
    {"CONTEXT-OBJ-NAMESTACK", HANDLER_CONTEXT_OBJ_NAMESTACK},
    {"CONTEXT-OBJ-CATCHSTACK", HANDLER_CONTEXT_OBJ_CATCHSTACK},
    {"get-namestack", HANDLER_GET_NAMESTACK},
    {"(get-namestack)", HANDLER_GET_NAMESTACK},
    // get-callstack: not mapped - complex callstack capture, would always fallback
    {"word?", HANDLER_WORD_QUESTION},
    {"=", HANDLER_EQUALS},
    {"or", HANDLER_OR},
    {"and", HANDLER_AND},
    {"not", HANDLER_NOT},
    {"tuple", HANDLER_TUPLE_WORD},
    {"-", HANDLER_MINUS},
    // if-empty: not mapped - handler has a bug causing file creation
    // box-at: not mapped - uses cache/allocation, would always fallback
    
    // Curry/compose operations - now implemented
    {"curry", HANDLER_CURRY_WORD},
    {"2curry", HANDLER_2CURRY},
    {"3curry", HANDLER_3CURRY},
    {"compose", HANDLER_COMPOSE_WORD},
    
    // Subprimitive handlers
    {"dup", HANDLER_DUP},
    {"dupd", HANDLER_DUPD},
    {"drop", HANDLER_DROP},
    {"nip", HANDLER_NIP},
    {"2drop", HANDLER_2DROP},
    {"2nip", HANDLER_2NIP},
    {"3drop", HANDLER_3DROP},
    {"4drop", HANDLER_4DROP},
    {"2dup", HANDLER_2DUP},
    {"3dup", HANDLER_3DUP},
    {"4dup", HANDLER_4DUP},
    {"over", HANDLER_OVER},
    {"2over", HANDLER_2OVER},
    {"pick", HANDLER_PICK},
    {"swap", HANDLER_SWAP},
    {"swapd", HANDLER_SWAPD},
    {"rot", HANDLER_ROT},
    {"-rot", HANDLER_MINUS_ROT},
    {"eq?", HANDLER_EQ},
    {"both-fixnums?", HANDLER_BOTH_FIXNUMS},
    {"fixnum-bitand", HANDLER_FIXNUM_BITAND},
    {"fixnum-bitor", HANDLER_FIXNUM_BITOR},
    {"fixnum-bitxor", HANDLER_FIXNUM_BITXOR},
    {"fixnum-bitnot", HANDLER_FIXNUM_BITNOT},
    {"fixnum<", HANDLER_FIXNUM_LT},
    {"fixnum<=", HANDLER_FIXNUM_LE},
    {"fixnum>", HANDLER_FIXNUM_GT},
    {"fixnum>=", HANDLER_FIXNUM_GE},
    {"fixnum+", HANDLER_FIXNUM_PLUS},
    {"fixnum+fast", HANDLER_FIXNUM_PLUS},
    {"fixnum-", HANDLER_FIXNUM_MINUS},
    {"fixnum-fast", HANDLER_FIXNUM_MINUS},
    {"fixnum*", HANDLER_FIXNUM_TIMES},
    {"fixnum*fast", HANDLER_FIXNUM_TIMES},
    {"fixnum-mod", HANDLER_FIXNUM_MOD},
    {"fixnum/i-fast", HANDLER_FIXNUM_DIVI},
    {"fixnum/mod-fast", HANDLER_FIXNUM_DIVMOD},
    {"fixnum-shift-fast", HANDLER_FIXNUM_SHIFT},
    {"drop-locals", HANDLER_DROP_LOCALS},
    {"load-local", HANDLER_LOAD_LOCAL},
    {"get-local", HANDLER_GET_LOCAL},
    {"tag", HANDLER_TAG},
    {"slot", HANDLER_SLOT},
    {"string-nth-fast", HANDLER_STRING_NTH_FAST},
    {"nth-unsafe", HANDLER_NTH_UNSAFE},
    {"fpu-state", HANDLER_FPU_STATE},
    {"set-fpu-state", HANDLER_SET_FPU_STATE},
    {"set-callstack", HANDLER_SET_CALLSTACK},
    {"c-to-factor", HANDLER_C_TO_FACTOR},
    {"unwind-native-frames", HANDLER_C_TO_FACTOR},
    {"leaf-signal-handler", HANDLER_C_TO_FACTOR},
    {"signal-handler", HANDLER_C_TO_FACTOR},
    {"(set-context)", HANDLER_SET_CONTEXT},
    {"(start-context)", HANDLER_START_CONTEXT},
    {"(set-context-and-delete)", HANDLER_SET_CONTEXT_DELETE},
    {"(start-context-and-delete)", HANDLER_START_CONTEXT_DELETE},
  };
  return table;
}

// Get cached handler ID from word's pic_def field
// Returns HANDLER_UNCACHED if not yet cached
// Note: We use a magic value to detect uncached state since pic_def
// may have arbitrary values in the boot image
static const cell WASM_HANDLER_MAGIC = 0xFA570000;  // "FAST" marker in high bits

inline int32_t get_cached_handler_id(word* w) {
  cell cached = w->pic_def;
  // Check for our magic marker in high bits
  if ((cached & 0xFFFF0000) != WASM_HANDLER_MAGIC)
    return HANDLER_UNCACHED;
  return static_cast<int32_t>(cached & 0xFFFF);
}

// Set cached handler ID on word's pic_def field
inline void set_cached_handler_id(word* w, int32_t id) {
  w->pic_def = WASM_HANDLER_MAGIC | (static_cast<cell>(id) & 0xFFFF);
}

// Lookup handler ID for a word and cache it
inline int32_t lookup_and_cache_handler_id(word* w) {
  // Get word name as string_view (no allocation)
  string* name_str = untag<string>(w->name);
  cell len = untag_fixnum(name_str->length);
  const char* bytes = reinterpret_cast<const char*>(name_str->data());
  std::string_view name(bytes, static_cast<size_t>(len));
  
  // Look up in table
  static const auto& table = get_handler_id_table();
  auto it = table.find(name);
  
  int32_t id = (it != table.end())
    ? static_cast<int32_t>(it->second)
    : HANDLER_NONE;
  
  // Cache on word object
  set_cached_handler_id(w, id);
  
  return id;
}

// ============================================================================
// Fast Dispatch by Handler ID - No String Operations
// ============================================================================
// This function dispatches directly by handler ID, avoiding all string
// operations. Returns true if handled, false if not.

bool factor_vm::dispatch_by_handler_id(int32_t handler_id) {
  switch (handler_id) {
    // HANDLER_NONE means no special handling, use quotation
    case HANDLER_NONE:
      return false;
    
    // ========== Stack Operations (subprimitives) ==========
    case HANDLER_DUP: {
      cell v = ctx->peek();
      ctx->push(v);
      return true;
    }
    case HANDLER_DUPD: {
      cell top = ctx->pop();
      cell second = ctx->pop();
      ctx->push(second);
      ctx->push(second);
      ctx->push(top);
      return true;
    }
    case HANDLER_DROP: {
      ctx->pop();
      return true;
    }
    case HANDLER_NIP: {
      cell top = ctx->pop();
      ctx->pop();
      ctx->push(top);
      return true;
    }
    case HANDLER_2DROP: {
      ctx->pop();
      ctx->pop();
      return true;
    }
    case HANDLER_2NIP: {
      cell top = ctx->pop();
      ctx->pop();
      ctx->pop();
      ctx->push(top);
      return true;
    }
    case HANDLER_3DROP: {
      ctx->pop();
      ctx->pop();
      ctx->pop();
      return true;
    }
    case HANDLER_4DROP: {
      ctx->pop();
      ctx->pop();
      ctx->pop();
      ctx->pop();
      return true;
    }
    case HANDLER_2DUP: {
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(a);
      ctx->push(b);
      ctx->push(a);
      ctx->push(b);
      return true;
    }
    case HANDLER_3DUP: {
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
    case HANDLER_4DUP: {
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
    case HANDLER_OVER: {
      cell top = ctx->pop();
      cell second = ctx->peek();
      ctx->push(top);
      ctx->push(second);
      return true;
    }
    case HANDLER_2OVER: {
      cell z = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->peek();
      ctx->push(y);
      ctx->push(z);
      ctx->push(x);
      ctx->push(y);
      return true;
    }
    case HANDLER_PICK: {
      cell top = ctx->pop();
      cell second = ctx->pop();
      cell third = ctx->peek();
      ctx->push(second);
      ctx->push(top);
      ctx->push(third);
      return true;
    }
    case HANDLER_SWAP: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(y);
      ctx->push(x);
      return true;
    }
    case HANDLER_SWAPD: {
      cell c = ctx->pop();
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(b);
      ctx->push(a);
      ctx->push(c);
      return true;
    }
    case HANDLER_ROT: {
      cell c = ctx->pop();
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(b);
      ctx->push(c);
      ctx->push(a);
      return true;
    }
    case HANDLER_MINUS_ROT: {
      cell c = ctx->pop();
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(c);
      ctx->push(a);
      ctx->push(b);
      return true;
    }
    case HANDLER_EQ: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(tag_boolean(x == y));
      return true;
    }
    case HANDLER_BOTH_FIXNUMS: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      bool ok = TAG(x) == FIXNUM_TYPE && TAG(y) == FIXNUM_TYPE;
      ctx->push(tag_boolean(ok));
      return true;
    }
    
    // ========== Fixnum Arithmetic ==========
    case HANDLER_FIXNUM_BITAND: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_fixnum(x & y));
      return true;
    }
    case HANDLER_FIXNUM_BITOR: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_fixnum(x | y));
      return true;
    }
    case HANDLER_FIXNUM_BITXOR: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_fixnum(x ^ y));
      return true;
    }
    case HANDLER_FIXNUM_BITNOT: {
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_fixnum(~x));
      return true;
    }
    case HANDLER_FIXNUM_LT: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_boolean(x < y));
      return true;
    }
    case HANDLER_FIXNUM_LE: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_boolean(x <= y));
      return true;
    }
    case HANDLER_FIXNUM_GT: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_boolean(x > y));
      return true;
    }
    case HANDLER_FIXNUM_GE: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_boolean(x >= y));
      return true;
    }
    case HANDLER_FIXNUM_PLUS: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      // Use int64_t to detect overflow
      int64_t r = (int64_t)x + (int64_t)y;
      if (r > fixnum_max || r < fixnum_min) {
        data_root<bignum> bx(fixnum_to_bignum(x), this);
        data_root<bignum> by(fixnum_to_bignum(y), this);
        data_root<bignum> res(bignum_add(bx.untagged(), by.untagged()), this);
        ctx->push(bignum_maybe_to_fixnum(res.untagged()));
      } else {
        ctx->push(tag_fixnum((fixnum)r));
      }
      return true;
    }
    case HANDLER_FIXNUM_MINUS: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      // Use int64_t to detect overflow
      int64_t r = (int64_t)x - (int64_t)y;
      if (r > fixnum_max || r < fixnum_min) {
        data_root<bignum> bx(fixnum_to_bignum(x), this);
        data_root<bignum> by(fixnum_to_bignum(y), this);
        data_root<bignum> res(bignum_subtract(bx.untagged(), by.untagged()), this);
        ctx->push(bignum_maybe_to_fixnum(res.untagged()));
      } else {
        ctx->push(tag_fixnum((fixnum)r));
      }
      return true;
    }
    case HANDLER_FIXNUM_TIMES: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      // Use int64_t to detect overflow
      int64_t r = (int64_t)x * (int64_t)y;
      if (r > fixnum_max || r < fixnum_min) {
        data_root<bignum> bx(fixnum_to_bignum(x), this);
        data_root<bignum> by(fixnum_to_bignum(y), this);
        data_root<bignum> res(bignum_multiply(bx.untagged(), by.untagged()), this);
        ctx->push(bignum_maybe_to_fixnum(res.untagged()));
      } else {
        ctx->push(tag_fixnum((fixnum)r));
      }
      return true;
    }
    case HANDLER_FIXNUM_MOD: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      if (y == 0) {
        general_error(ERROR_DIVIDE_BY_ZERO, tag_fixnum(x), tag_fixnum(y));
        return true;
      }
      ctx->push(tag_fixnum(x % y));
      return true;
    }
    case HANDLER_FIXNUM_DIVI: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      if (y == 0) {
        general_error(ERROR_DIVIDE_BY_ZERO, tag_fixnum(x), tag_fixnum(y));
        return true;
      }
      ctx->push(tag_fixnum(x / y));
      return true;
    }
    case HANDLER_FIXNUM_DIVMOD: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      if (y == 0) {
        general_error(ERROR_DIVIDE_BY_ZERO, tag_fixnum(x), tag_fixnum(y));
        return true;
      }
      ctx->push(tag_fixnum(x / y));
      ctx->push(tag_fixnum(x % y));
      return true;
    }
    case HANDLER_FIXNUM_SHIFT: {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      // Clamp shift amount to avoid undefined behavior
      // On 32-bit WASM, fixnum is 32 bits; shifting by >= 32 is UB
      constexpr fixnum max_shift = sizeof(fixnum) * 8 - 1;
      if (y >= 0) {
        if (y > max_shift)
          ctx->push(tag_fixnum(0));  // Shift out all bits
        else
          ctx->push(tag_fixnum(x << y));
      } else {
        fixnum neg_y = -y;
        if (neg_y > max_shift)
          ctx->push(tag_fixnum(x < 0 ? -1 : 0));  // Arithmetic shift preserves sign
        else
          ctx->push(tag_fixnum(x >> neg_y));
      }
      return true;
    }
    
    // ========== Control Flow ==========
    case HANDLER_IF: {
      cell false_quot = ctx->pop();
      cell true_quot = ctx->pop();
      cell cond = ctx->pop();
      cell chosen = to_boolean(cond) ? true_quot : false_quot;
      cell tag = TAG(chosen);
      if (tag == QUOTATION_TYPE) {
        interpret_quotation(chosen);
      } else if (tag == WORD_TYPE) {
        interpret_word(chosen);
      } else if (tag == TUPLE_TYPE) {
        call_callable(chosen);
      }
      return true;
    }
    case HANDLER_QUESTION: {
      cell f = ctx->pop();
      cell t = ctx->pop();
      cell cond = ctx->pop();
      ctx->push(to_boolean(cond) ? t : f);
      return true;
    }
    case HANDLER_CALL:
    case HANDLER_PAREN_CALL: {
      cell quot = ctx->pop();
      g_call_callable_source = "HANDLER_CALL";
      call_callable(quot);
      return true;
    }
    case HANDLER_CALL_EFFECT: {
      // call-effect ( quot effect -- ) - effect is on top, drop it and call quot
      cell effect = ctx->pop();  // Pop and discard effect
      (void)effect;
      cell quot = ctx->pop();    // Now pop the quotation
      g_call_callable_source = "HANDLER_CALL_EFFECT";
      call_callable(quot);
      return true;
    }
    case HANDLER_EXECUTE:
    case HANDLER_PAREN_EXECUTE: {
      cell w = ctx->pop();
      interpret_word(w);
      return true;
    }
    case HANDLER_EXECUTE_EFFECT: {
      // execute-effect ( word effect -- ) - effect is on top, drop it and execute word
      cell effect = ctx->pop();  // Pop and discard effect
      (void)effect;
      cell w = ctx->pop();       // Now pop the word
      interpret_word(w);
      return true;
    }
    case HANDLER_DIP: {
      cell quot = ctx->pop();
      cell x = ctx->pop();
      if (TAG(quot) == QUOTATION_TYPE) interpret_quotation(quot);
      else if (TAG(quot) == WORD_TYPE) interpret_word(quot);
      else if (TAG(quot) == TUPLE_TYPE) call_callable(quot);
      ctx->push(x);
      return true;
    }
    case HANDLER_2DIP: {
      cell quot = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      if (TAG(quot) == QUOTATION_TYPE) interpret_quotation(quot);
      else if (TAG(quot) == WORD_TYPE) interpret_word(quot);
      else if (TAG(quot) == TUPLE_TYPE) call_callable(quot);
      ctx->push(x);
      ctx->push(y);
      return true;
    }
    case HANDLER_3DIP: {
      cell quot = ctx->pop();
      cell z = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      if (TAG(quot) == QUOTATION_TYPE) interpret_quotation(quot);
      else if (TAG(quot) == WORD_TYPE) interpret_word(quot);
      else if (TAG(quot) == TUPLE_TYPE) call_callable(quot);
      ctx->push(x);
      ctx->push(y);
      ctx->push(z);
      return true;
    }
    
    // ========== Slot Access ==========
    case HANDLER_SLOT: {
      cell n = ctx->pop();
      cell obj = ctx->pop();
      cell* slots = reinterpret_cast<cell*>(UNTAG(obj));
      ctx->push(slots[untag_fixnum(n)]);
      return true;
    }
    case HANDLER_TAG: {
      cell obj = ctx->pop();
      ctx->push(tag_fixnum(TAG(obj)));
      return true;
    }
    
    // ========== Length ==========
    case HANDLER_LENGTH: {
      cell obj = ctx->peek();
      if (!interp_length_supported(obj))
        return false; // Fall back to quotation for unsupported types
      ctx->pop();
      ctx->push(tag_fixnum(interp_length(obj)));
      return true;
    }
    
    // ========== Comparisons ==========
    case HANDLER_LESS_THAN: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      if (TAG(x) == FIXNUM_TYPE && TAG(y) == FIXNUM_TYPE) {
        ctx->push(tag_boolean(untag_fixnum(x) < untag_fixnum(y)));
      } else {
        // Restore stack before falling back to quotation
        ctx->push(x);
        ctx->push(y);
        return false;
      }
      return true;
    }
    case HANDLER_LESS_EQUAL: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      if (TAG(x) == FIXNUM_TYPE && TAG(y) == FIXNUM_TYPE) {
        ctx->push(tag_boolean(untag_fixnum(x) <= untag_fixnum(y)));
      } else {
        // Restore stack before falling back to quotation
        ctx->push(x);
        ctx->push(y);
        return false;
      }
      return true;
    }
    case HANDLER_GREATER_THAN: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      if (TAG(x) == FIXNUM_TYPE && TAG(y) == FIXNUM_TYPE) {
        ctx->push(tag_boolean(untag_fixnum(x) > untag_fixnum(y)));
      } else {
        // Restore stack before falling back to quotation
        ctx->push(x);
        ctx->push(y);
        return false;
      }
      return true;
    }
    case HANDLER_GREATER_EQUAL: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      if (TAG(x) == FIXNUM_TYPE && TAG(y) == FIXNUM_TYPE) {
        ctx->push(tag_boolean(untag_fixnum(x) >= untag_fixnum(y)));
      } else {
        // Restore stack before falling back to quotation
        ctx->push(x);
        ctx->push(y);
        return false;
      }
      return true;
    }
    
    // ========== Equality ==========
    case HANDLER_EQUAL: {
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(tag_boolean(objects_equal(a, b)));
      return true;
    }
    
    // ========== Callable Check ==========
    case HANDLER_CALLABLE: {
      cell obj = ctx->pop();
      cell tag = TAG(obj);
      bool is_callable = false;
      if (tag == QUOTATION_TYPE || tag == WORD_TYPE) {
        is_callable = true;
      } else if (tag == TUPLE_TYPE) {
        // Check if it's a curried or composed tuple
        tuple* t = untag<tuple>(obj);
        cell layout = t->layout;
        if ((g_curried_layout != 0 && layout == g_curried_layout) ||
            (g_composed_layout != 0 && layout == g_composed_layout)) {
          is_callable = true;
        } else {
          // Slow path - check by class name
          std::string tname = tuple_class_name(t);
          is_callable = (tname == "curried" || tname == "composed");
        }
      }
      ctx->push(tag_boolean(is_callable));
      return true;
    }
    
    // ========== Curried/Composed Quotations ==========
    case HANDLER_CURRIED: {
      // curried ( obj quot -- curried )
      // Use globally cached layout (set by call_callable when it first sees a curried tuple)
      if (g_curried_layout == 0)
        return false;  // Not cached yet, let quotation create one and cache it
      // Stack has: obj quot
      // We need to create tuple with slots [obj, quot]
      cell quot = ctx->pop();
      cell obj = ctx->pop();
      tuple_layout* layout = untag<tuple_layout>(g_curried_layout);
      data_root<tuple> t(allot<tuple>(tuple_size(layout)), this);
      t->layout = g_curried_layout;
      t->data()[0] = obj;
      t->data()[1] = quot;
      ctx->push(t.value());
      return true;
    }
    
    case HANDLER_COMPOSED: {
      // composed ( quot1 quot2 -- composed )
      // Use globally cached layout (set by call_callable when it first sees a composed tuple)
      if (g_composed_layout == 0)
        return false;  // Not cached yet
      cell quot2 = ctx->pop();
      cell quot1 = ctx->pop();
      tuple_layout* layout = untag<tuple_layout>(g_composed_layout);
      data_root<tuple> t(allot<tuple>(tuple_size(layout)), this);
      t->layout = g_composed_layout;
      t->data()[0] = quot1;
      t->data()[1] = quot2;
      ctx->push(t.value());
      return true;
    }
    
    case HANDLER_PREPOSE: {
      // prepose ( quot1 quot2 -- composed ) = swap compose
      // Result executes quot2 first, then quot1
      if (g_composed_layout == 0)
        return false;  // Not cached yet
      cell quot2 = ctx->pop();
      cell quot1 = ctx->pop();
      // swap: now quot1 is "first", quot2 is "second" in composed tuple
      tuple_layout* layout = untag<tuple_layout>(g_composed_layout);
      data_root<tuple> t(allot<tuple>(tuple_size(layout)), this);
      t->layout = g_composed_layout;
      t->data()[0] = quot2;  // first (executed first)
      t->data()[1] = quot1;  // second (executed second)
      ctx->push(t.value());
      return true;
    }

    // ========== Error Handling ==========
    case HANDLER_NO_METHOD: {
      // no-method ( object generic -- * ) throws an error
      // We intercept this to prevent infinite recursion when error construction
      // itself tries to call boa which then fails with no-method.
      cell generic = ctx->pop();
      cell object = ctx->pop();
      
      // Log the error for debugging
      std::string generic_name = "unknown";
      std::string object_info = "unknown";
      
      if (TAG(generic) == WORD_TYPE) {
        generic_name = word_name_string(untag<word>(generic));
      }
      
      if (TAG(object) == TUPLE_TYPE) {
        object_info = tuple_class_name(untag<tuple>(object));
      } else if (TAG(object) == WORD_TYPE) {
        object_info = "word:" + word_name_string(untag<word>(object));
      } else {
        object_info = "tag" + std::to_string(TAG(object));
      }
      
      std::cerr << "[wasm] no-method error: " << object_info 
                << " has no method for generic " << generic_name << std::endl;
      
      // Dump stack for context
      std::cerr << "[wasm] stack dump (top 10):" << std::endl;
      cell* sp = (cell*)ctx->datastack;
      for (int i = 0; i < 10 && (cell)(sp - i) >= ctx->datastack_seg->start; i++) {
        cell v = sp[-i];
        std::cerr << "  [" << i << "] 0x" << std::hex << v << std::dec << " tag=" << TAG(v);
        if (TAG(v) == WORD_TYPE) {
          std::cerr << " word:" << word_name_string(untag<word>(v));
        } else if (TAG(v) == TUPLE_TYPE) {
          std::cerr << " tuple:" << tuple_class_name(untag<tuple>(v));
        } else if (TAG(v) == STRING_TYPE) {
          string* s = untag<string>(v);
          std::string str(reinterpret_cast<const char*>(s->data()), 
                         std::min((cell)30, (cell)untag_fixnum(s->length)));
          std::cerr << " string:\"" << str << "\"";
        } else if (TAG(v) == FIXNUM_TYPE) {
          std::cerr << " fixnum:" << untag_fixnum(v);
        }
        std::cerr << std::endl;
      }
      
      // Try to let Factor handle it by constructing a no-method error tuple
      // Push object and generic back, then call the actual no-method word
      ctx->push(object);
      ctx->push(generic);
      // Look up the actual no-method word and call its quotation
      // For now, use fatal_error to avoid infinite loops
      fatal_error("no-method error - see stderr for details", object);
      return true;  // Never reached
    }
    
    // ========== String Operations ==========
    case HANDLER_STRING_NTH_FAST: {
      cell idx = ctx->pop();
      cell str_cell = ctx->pop();
      string* str = untag<string>(str_cell);
      ctx->push(tag_fixnum(str->data()[untag_fixnum(idx)]));
      return true;
    }
    
    // ========== Locals ==========
    case HANDLER_DROP_LOCALS: {
      fixnum count = untag_fixnum(ctx->pop());
      ctx->retainstack -= count * sizeof(cell);
      return true;
    }
    case HANDLER_LOAD_LOCAL: {
      cell val = ctx->pop();
      ctx->retainstack += sizeof(cell);
      *(cell*)ctx->retainstack = val;
      return true;
    }
    case HANDLER_GET_LOCAL: {
      fixnum index = untag_fixnum(ctx->pop());
      cell target = ctx->retainstack + index * sizeof(cell);
      ctx->push(*(cell*)target);
      return true;
    }
    
    // ========== Set Operations ==========
    case HANDLER_SET_NTH: {
      cell seq = ctx->pop();
      cell idx_cell = ctx->pop();
      cell elt = ctx->pop();
      fixnum idx = untag_fixnum(idx_cell);
      switch (TAG(seq)) {
        case ARRAY_TYPE: {
          array* a = untag<array>(seq);
          if (idx >= 0 && idx < (fixnum)array_capacity(a))
            set_array_nth(a, idx, elt);
          break;
        }
        case BYTE_ARRAY_TYPE: {
          byte_array* ba = untag<byte_array>(seq);
          if (idx >= 0 && idx < (fixnum)array_capacity(ba))
            ba->data<uint8_t>()[idx] = (uint8_t)untag_fixnum(elt);
          break;
        }
        case STRING_TYPE: {
          string* s = untag<string>(seq);
          if (idx >= 0 && idx < untag_fixnum(s->length))
            s->data()[idx] = (uint8_t)untag_fixnum(elt);
          break;
        }
        default:
          break;
      }
      return true;
    }
    
    // ========== Nth Unsafe ==========
    // NOTE: nth-unsafe is a GENERIC word in Factor!
    // We only handle the fast path for primitive sequence types.
    // For other types, we return false to fall through to generic dispatch.
    case HANDLER_NTH_UNSAFE: {
      cell seq = ctx->peek();  // Peek, don't pop yet
      cell seq_tag = TAG(seq);
      
      // Fast path only for primitive sequence types
      if (seq_tag == ARRAY_TYPE || seq_tag == BYTE_ARRAY_TYPE || seq_tag == STRING_TYPE) {
        ctx->pop();  // Now pop seq
        cell idx_cell = ctx->pop();
        fixnum idx = untag_fixnum(idx_cell);
        cell result;
        switch (seq_tag) {
          case ARRAY_TYPE:
            result = array_nth(untag<array>(seq), idx);
            break;
          case BYTE_ARRAY_TYPE:
            result = tag_fixnum(untag<byte_array>(seq)->data<uint8_t>()[idx]);
            break;
          case STRING_TYPE:
            result = tag_fixnum(untag<string>(seq)->data()[idx]);
            break;
          default:
            result = false_object;
            break;
        }
        ctx->push(result);
        return true;
      }
      // For other types (tuples, etc.), fall through to generic dispatch
      return false;
    }
    
    // ========== Hashcode ==========
    case HANDLER_HASHCODE:
    case HANDLER_HASHCODE_STAR: {
      if (handler_id == HANDLER_HASHCODE_STAR)
        ctx->pop(); // discard depth parameter
      cell obj = ctx->pop();
      cell result;
      switch (TAG(obj)) {
        case FIXNUM_TYPE:
          result = obj;
          break;
        case F_TYPE:
          result = tag_fixnum(0);
          break;
        case STRING_TYPE: {
          string* str = untag<string>(obj);
          result = str->hashcode;
          if (result == false_object || result == tag_fixnum(0)) {
            result = tag_fixnum(compute_string_hash(str));
            str->hashcode = result;
          }
          break;
        }
        case WORD_TYPE:
          result = untag<word>(obj)->hashcode;
          break;
        default:
          if (immediate_p(obj)) {
            result = tag_fixnum(0);
          } else {
            object* o = untag<object>(obj);
            cell cached = o->hashcode();
            if (cached == 0)
              cached = (cell)o >> TAG_BITS;
            result = tag_fixnum((fixnum)cached);
          }
          break;
      }
      ctx->push(result);
      return true;
    }
    
    // ========== Hash Table Operations ==========
    case HANDLER_HASH_AT: {
      cell array_cell = ctx->pop();
      cell key = ctx->pop();
      if (TAG(array_cell) != ARRAY_TYPE)
        return false; // fall back
      fixnum len = (fixnum)array_capacity(untag<array>(array_cell));
      if (len <= 0)
        return false;
      fixnum mask = len - 1;
      // Compute hashcode inline
      cell hc;
      switch (TAG(key)) {
        case FIXNUM_TYPE: hc = key; break;
        case F_TYPE: hc = tag_fixnum(0); break;
        case STRING_TYPE: {
          string* str = untag<string>(key);
          hc = str->hashcode;
          if (hc == false_object || hc == tag_fixnum(0)) {
            hc = tag_fixnum(compute_string_hash(str));
            str->hashcode = hc;
          }
          break;
        }
        case WORD_TYPE: hc = untag<word>(key)->hashcode; break;
        default: hc = tag_fixnum(0); break;
      }
      fixnum h = untag_fixnum(hc);
      fixnum idx = (h + h) & mask;
      ctx->push(tag_fixnum(idx));
      return true;
    }
    
    case HANDLER_PROBE: {
      cell probe_cell = ctx->pop();
      cell i_cell = ctx->pop();
      cell array_cell = ctx->pop();
      if (TAG(array_cell) != ARRAY_TYPE || TAG(i_cell) != FIXNUM_TYPE || TAG(probe_cell) != FIXNUM_TYPE)
        return false;
      fixnum len = (fixnum)array_capacity(untag<array>(array_cell));
      if (len <= 0)
        return false;
      fixnum mask = len - 1;
      fixnum probe = untag_fixnum(probe_cell) + 2;
      fixnum i = untag_fixnum(i_cell);
      fixnum new_i = (i + probe) & mask;
      ctx->push(array_cell);
      ctx->push(tag_fixnum(new_i));
      ctx->push(tag_fixnum(probe));
      return true;
    }
    
    case HANDLER_WRAP: {
      cell array_cell = ctx->pop();
      cell i_cell = ctx->pop();
      if (TAG(array_cell) != ARRAY_TYPE || TAG(i_cell) != FIXNUM_TYPE)
        return false;
      fixnum len = (fixnum)array_capacity(untag<array>(array_cell));
      if (len <= 0)
        return false;
      fixnum mask = len - 1;
      fixnum i = untag_fixnum(i_cell);
      ctx->push(tag_fixnum(i & mask));
      return true;
    }
    
    case HANDLER_AT_STAR: {
      cell assoc = ctx->pop();
      cell key = ctx->pop();
      hashtable_lookup_result r = hashtable_lookup(key, assoc);
      if (!r.found || r.index < 0) {
        ctx->push(false_object);
        ctx->push(false_object);
      } else {
        array* arr = untag<array>(r.array_cell);
        if (r.index + 1 >= (fixnum)array_capacity(arr)) {
          ctx->push(false_object);
          ctx->push(false_object);
          return true;
        }
        cell val = array_nth(arr, r.index + 1);
        ctx->push(val);
        ctx->push(tag_boolean(true));
      }
      return true;
    }
    
    case HANDLER_KEY_AT: {
      cell hash_cell = ctx->pop();
      cell key_cell = ctx->pop();
      hashtable_lookup_result r = hashtable_lookup(key_cell, hash_cell);
      ctx->push(r.array_cell);
      ctx->push(r.found ? tag_fixnum(r.index) : false_object);
      ctx->push(tag_boolean(r.found));
      return true;
    }
    
    case HANDLER_ASSOC_SIZE: {
      cell assoc = ctx->pop();
      if (TAG(assoc) == TUPLE_TYPE) {
        tuple* h = untag<tuple>(assoc);
        // Fast path: use cached layout
        if (IS_TUPLE_CLASS(h, hashtable)) {
          fixnum count = untag_fixnum(tuple_slot(h, 0));
          fixnum deleted = untag_fixnum(tuple_slot(h, 1));
          ctx->push(tag_fixnum(count - deleted));
          return true;
        }
      }
      ctx->push(tag_fixnum(0));
      return true;
    }
    
    // ========== Value Access (box/global-box) ==========
    case HANDLER_VALUE_READ: {
      cell obj = ctx->pop();
      cell result = false_object;
      if (TAG(obj) == TUPLE_TYPE) {
        tuple* t = untag<tuple>(obj);
        // Fast path: use cached layouts
        if (IS_TUPLE_CLASS(t, box) || IS_TUPLE_CLASS(t, global_box)) {
          result = t->data()[0];
        }
      }
      ctx->push(result);
      return true;
    }
    
    case HANDLER_VALUE_WRITE: {
      cell obj = ctx->pop();
      cell val = ctx->pop();
      if (TAG(obj) == TUPLE_TYPE) {
        tuple* t = untag<tuple>(obj);
        // Fast path: use cached layouts
        if (IS_TUPLE_CLASS(t, box) || IS_TUPLE_CLASS(t, global_box)) {
          t->data()[0] = val;
        }
      }
      ctx->push(obj);
      return true;
    }
    
    // ========== Word Properties ==========
    case HANDLER_WORD_PROP: {
      cell key = ctx->pop();
      cell w = ctx->pop();
      if (TAG(w) == WORD_TYPE) {
        word* ww = untag<word>(w);
        cell props = ww->props;
        if (props != false_object) {
          hashtable_lookup_result r = hashtable_lookup(key, props);
          if (r.found) {
            array* arr = untag<array>(r.array_cell);
            if (r.index + 1 < (fixnum)array_capacity(arr)) {
              ctx->push(array_nth(arr, r.index + 1));
              return true;
            }
          }
        }
      }
      ctx->push(false_object);
      return true;
    }
    
    // ========== Props Write ==========
    case HANDLER_PROPS_WRITE: {
      cell obj = ctx->pop();
      cell val = ctx->pop();
      if (TAG(obj) == WORD_TYPE) {
        word* w = untag<word>(obj);
        w->props = val;
      }
      ctx->push(obj);
      return true;
    }
    
    // ========== Integer Conversion ==========
    case HANDLER_INTEGER_TO_FIXNUM_STRICT: {
      cell obj = ctx->pop();
      switch (TAG(obj)) {
        case FIXNUM_TYPE:
          ctx->push(obj);
          break;
        case BIGNUM_TYPE: {
          bignum* bn = untag<bignum>(obj);
          ctx->push(tag_fixnum(bignum_to_fixnum_strict(bn)));
          break;
        }
        default:
          ctx->push(tag_fixnum(0));
          break;
      }
      return true;
    }
    
    // ========== Underlying Access ==========
    case HANDLER_UNDERLYING: {
      cell obj = ctx->pop();
      if (TAG(obj) == WRAPPER_TYPE)
        obj = untag<wrapper>(obj)->object;
      if (TAG(obj) == TUPLE_TYPE) {
        tuple* t = untag<tuple>(obj);
        tuple_layout* layout = untag<tuple_layout>(t->layout);
        if (tuple_capacity(layout) > 0)
          obj = tuple_slot(t, 0);
      }
      ctx->push(obj);
      return true;
    }
    
    // ========== New Sequence ==========
    case HANDLER_NEW_SEQUENCE: {
      cell len_cell = ctx->pop();
      cell exemplar = ctx->pop();
      (void)exemplar;
      fixnum len = 0;
      if (TAG(len_cell) == FIXNUM_TYPE)
        len = untag_fixnum(len_cell);
      if (len < 0) len = 0;
      data_root<array> arr(tag<array>(allot_array(len, false_object)), this);
      ctx->push(arr.value());
      return true;
    }
    
    // ========== Get Datastack ==========
    case HANDLER_GET_DATASTACK: {
      ctx->push(datastack_to_array(ctx));
      return true;
    }
    
    // ========== Rehash ==========
    case HANDLER_REHASH: {
      ctx->pop(); // discard hash, no-op in interpreter
      return true;
    }
    
    // ========== Recover ==========
    case HANDLER_RECOVER: {
      cell recovery = ctx->pop();
      (void)recovery;
      cell try_quot = ctx->pop();
      call_callable(try_quot);
      return true;
    }
    
    // ========== Init IO (WASM no-ops) ==========
    case HANDLER_INIT_IO:
    case HANDLER_INIT_STDIO: {
      return true;
    }
    
    // ========== Print ==========
    case HANDLER_PRINT: {
      cell obj = ctx->pop();
      if (TAG(obj) == STRING_TYPE) {
        string* s = untag<string>(obj);
        std::string out(reinterpret_cast<const char*>(s->data()),
                        (size_t)untag_fixnum(s->length));
        std::cout << out << std::endl;
      } else {
        std::cout << "[wasm] print <obj 0x" << std::hex << obj << " type "
                  << TAG(obj) << ">" << std::dec << std::endl;
      }
      return true;
    }
    
    // ========== Stream Write ==========
    case HANDLER_STREAM_WRITE: {
      cell stream = ctx->pop();
      (void)stream;
      cell data = ctx->pop();
      if (TAG(data) == STRING_TYPE) {
        string* s = untag<string>(data);
        cell len = untag_fixnum(s->length);
        std::cout.write(reinterpret_cast<const char*>(s->data()), (std::streamsize)len);
      } else if (TAG(data) == BYTE_ARRAY_TYPE) {
        byte_array* ba = untag<byte_array>(data);
        cell len = array_capacity(ba);
        std::cout.write(reinterpret_cast<const char*>(ba->data<uint8_t>()), (std::streamsize)len);
      }
      return true;
    }
    
    // ========== Stream Newline ==========
    case HANDLER_STREAM_NL: {
      ctx->pop(); // discard stream
      std::cout.put('\n');
      return true;
    }
    
    // ========== Mega Cache Lookup (method dispatch) ==========
    case HANDLER_MEGA_CACHE_LOOKUP: {
      cell cache = ctx->pop();
      cell index = untag_fixnum(ctx->pop());
      cell methods = ctx->pop();
      
      // Get the object to dispatch on from the stack (without removing it)
      // dispatch# is 0-indexed: 0 = top of stack, 1 = second from top, etc.
      cell* sp = (cell*)ctx->datastack;
      cell object = sp[-index];
      
      // Look up the method
      cell method = lookup_method(object, methods);
      
      // Update the cache
      update_method_cache(cache, object_class(object), method);
      
      // Execute the found method
      cell method_type = TAG(method);
      if (method_type == WORD_TYPE) {
        interpret_word(method);
      } else if (method_type == QUOTATION_TYPE) {
        interpret_quotation(method);
      } else {
        fatal_error("mega-cache-lookup: unexpected method type", method);
      }
      return true;
    }
    
    // ========== String Hashcode ==========
    case HANDLER_STRING_HASHCODE: {
      cell str_cell = ctx->pop();
      if (TAG(str_cell) != STRING_TYPE)
        return false;
      string* str = untag<string>(str_cell);
      ctx->push(str->hashcode);
      return true;
    }
    
    // ========== Rehash String ==========
    case HANDLER_REHASH_STRING: {
      cell str_cell = ctx->pop();
      if (TAG(str_cell) != STRING_TYPE)
        return false;
      string* str = untag<string>(str_cell);
      str->hashcode = tag_fixnum(compute_string_hash(str));
      return true;
    }
    
    // ========== Set String Hashcode ==========
    case HANDLER_SET_STRING_HASHCODE: {
      cell str_cell = ctx->pop();
      cell hash = ctx->pop();
      if (TAG(str_cell) != STRING_TYPE)
        return false;
      string* str = untag<string>(str_cell);
      str->hashcode = hash;
      return true;
    }
    
    // ========== Native String Encoding ==========
    case HANDLER_NATIVE_STRING_ENCODING: {
      static cell utf8_word = false_object;
      if (utf8_word == false_object)
        utf8_word = find_word_by_name_local(this, "utf8");
      ctx->push(utf8_word != false_object ? utf8_word : false_object);
      return true;
    }
    
    // ========== (key@) - Quadratic Probe ==========
    case HANDLER_PAREN_KEY_AT: {
      cell probe_cell = ctx->pop();
      cell i_cell = ctx->pop();
      cell array_cell = ctx->pop();
      cell key_cell = ctx->pop();
      if (TAG(array_cell) != ARRAY_TYPE || TAG(i_cell) != FIXNUM_TYPE ||
          TAG(probe_cell) != FIXNUM_TYPE) {
        ctx->push(array_cell);
        ctx->push(false_object);
        ctx->push(false_object);
        return true;
      }
      array* arr = untag<array>(array_cell);
      fixnum len = (fixnum)array_capacity(arr);
      fixnum mask = len > 0 ? len - 1 : 0;
      fixnum probe = untag_fixnum(probe_cell);
      fixnum idx = untag_fixnum(i_cell);
      for (fixnum step = 0; step < len; step++) {
        cell entry = array_nth(arr, idx);
        if (is_empty_sentinel(entry)) {
          ctx->push(array_cell);
          ctx->push(false_object);
          ctx->push(false_object);
          return true;
        }
        if (!is_tombstone_sentinel(entry) && objects_equal(entry, key_cell)) {
          ctx->push(array_cell);
          ctx->push(tag_fixnum(idx));
          ctx->push(tag_boolean(true));
          return true;
        }
        probe += 2;
        idx = (idx + probe) & mask;
      }
      ctx->push(array_cell);
      ctx->push(false_object);
      ctx->push(false_object);
      return true;
    }
    
    // ========== High-Frequency Combinators ==========
    
    // unless: ( ? quot -- ) if false, call quot
    case HANDLER_UNLESS: {
      cell quot = ctx->pop();
      cell cond = ctx->pop();
      if (cond == false_object) {
        // Condition is false - call the callable
        cell qt = TAG(quot);
        if (qt == QUOTATION_TYPE) {
          interpret_quotation(quot);
        } else if (qt == WORD_TYPE) {
          interpret_word(quot);
        } else if (qt == TUPLE_TYPE) {
          call_callable(quot);
        }
      }
      // If condition is true, do nothing
      return true;
    }
    
    // when: ( ? quot -- ) if true, call quot
    case HANDLER_WHEN: {
      cell quot = ctx->pop();
      cell cond = ctx->pop();
      if (cond != false_object) {
        // Condition is true - call the callable
        cell qt = TAG(quot);
        if (qt == QUOTATION_TYPE) {
          interpret_quotation(quot);
        } else if (qt == WORD_TYPE) {
          interpret_word(quot);
        } else if (qt == TUPLE_TYPE) {
          call_callable(quot);
        }
      }
      // If condition is false, do nothing
      return true;
    }
    
    // keep: ( x quot -- ...results x ) call quot with x on stack, then push x back
    // Factor definition: over [ call ] dip
    // Trace: ( x quot ) over -> ( x quot x ) -> [ call ] dip ->
    //   dip saves x, calls quot on ( x ), then restores x
    case HANDLER_KEEP: {
      cell quot = ctx->pop();
      cell x = ctx->peek();  // peek, not pop - x stays for quot
      // Call the callable (can be quotation, word, or curried/composed tuple)
      cell qt = TAG(quot);
      if (qt == QUOTATION_TYPE) {
        interpret_quotation(quot);
      } else if (qt == WORD_TYPE) {
        interpret_word(quot);
      } else if (qt == TUPLE_TYPE) {
        // Handle curried/composed tuples
        call_callable(quot);
      } else {
        // Unknown callable type - this shouldn't happen
        std::cerr << "[wasm] HANDLER_KEEP: unknown callable type " << qt << std::endl;
      }
      // Push x back (this is what keep does - preserves x after quot runs)
      ctx->push(x);
      return true;
    }
    
    // bi: ( x p q -- ) apply p to x (keeping x), then apply q to x
    // Factor definition: [ keep ] dip call
    // Trace: ( x p q ) -> [ keep ] dip -> dip saves q, runs keep on (x p), restores q
    //   keep: ( x p ) -> runs p with x, leaves ( ...results x )
    //   after dip: ( ...results x q ) -> call -> runs q with x
    case HANDLER_BI: {
      cell q = ctx->pop();
      cell p = ctx->pop();
      cell x = ctx->peek();  // x stays on stack for p
      
      // Call p with x (x is on stack)
      cell pt = TAG(p);
      if (pt == QUOTATION_TYPE) {
        interpret_quotation(p);
      } else if (pt == WORD_TYPE) {
        interpret_word(p);
      } else if (pt == TUPLE_TYPE) {
        call_callable(p);
      }
      
      // After p, push x for q (this is what keep does)
      ctx->push(x);
      
      // Call q with x
      cell qt = TAG(q);
      if (qt == QUOTATION_TYPE) {
        interpret_quotation(q);
      } else if (qt == WORD_TYPE) {
        interpret_word(q);
      } else if (qt == TUPLE_TYPE) {
        call_callable(q);
      }
      return true;
    }
    
    // 2keep: ( x y quot -- x y ) call quot with x y, keep both
    case HANDLER_2KEEP: {
      cell quot = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(x);
      ctx->push(y);
      // Call callable
      cell qt = TAG(quot);
      if (qt == QUOTATION_TYPE) {
        interpret_quotation(quot);
      } else if (qt == WORD_TYPE) {
        interpret_word(quot);
      } else if (qt == TUPLE_TYPE) {
        call_callable(quot);
      }
      // Push x and y back
      ctx->push(x);
      ctx->push(y);
      return true;
    }
    
    // 3keep: ( x y z quot -- x y z )
    case HANDLER_3KEEP: {
      cell quot = ctx->pop();
      cell z = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(x);
      ctx->push(y);
      ctx->push(z);
      cell qt = TAG(quot);
      if (qt == QUOTATION_TYPE) {
        interpret_quotation(quot);
      } else if (qt == WORD_TYPE) {
        interpret_word(quot);
      } else if (qt == TUPLE_TYPE) {
        call_callable(quot);
      }
      ctx->push(x);
      ctx->push(y);
      ctx->push(z);
      return true;
    }
    
    // bi*: ( x y p q -- ) apply p to x, apply q to y
    case HANDLER_BI_STAR: {
      cell q = ctx->pop();
      cell p = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      
      // Call p with x
      ctx->push(x);
      if (TAG(p) == QUOTATION_TYPE) {
        interpret_quotation(p);
      } else if (TAG(p) == WORD_TYPE) {
        interpret_word(p);
      }
      
      // Call q with y
      ctx->push(y);
      if (TAG(q) == QUOTATION_TYPE) {
        interpret_quotation(q);
      } else if (TAG(q) == WORD_TYPE) {
        interpret_word(q);
      }
      return true;
    }
    
    // tri*: ( x y z p q r -- ) apply p to x, q to y, r to z
    case HANDLER_TRI_STAR: {
      cell r = ctx->pop();
      cell q = ctx->pop();
      cell p = ctx->pop();
      cell z = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      
      ctx->push(x);
      if (TAG(p) == QUOTATION_TYPE) interpret_quotation(p);
      else if (TAG(p) == WORD_TYPE) interpret_word(p);
      else if (TAG(p) == TUPLE_TYPE) call_callable(p);
      
      ctx->push(y);
      if (TAG(q) == QUOTATION_TYPE) interpret_quotation(q);
      else if (TAG(q) == WORD_TYPE) interpret_word(q);
      else if (TAG(q) == TUPLE_TYPE) call_callable(q);
      
      ctx->push(z);
      if (TAG(r) == QUOTATION_TYPE) interpret_quotation(r);
      else if (TAG(r) == WORD_TYPE) interpret_word(r);
      else if (TAG(r) == TUPLE_TYPE) call_callable(r);
      return true;
    }
    
    // bi@: ( x y quot -- ) apply quot to both x and y
    case HANDLER_BI_AT: {
      cell quot = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      
      ctx->push(x);
      if (TAG(quot) == QUOTATION_TYPE) interpret_quotation(quot);
      else if (TAG(quot) == WORD_TYPE) interpret_word(quot);
      else if (TAG(quot) == TUPLE_TYPE) call_callable(quot);
      
      ctx->push(y);
      if (TAG(quot) == QUOTATION_TYPE) interpret_quotation(quot);
      else if (TAG(quot) == WORD_TYPE) interpret_word(quot);
      else if (TAG(quot) == TUPLE_TYPE) call_callable(quot);
      return true;
    }
    
    // tri@: ( x y z quot -- ) apply quot to x, y, and z
    case HANDLER_TRI_AT: {
      cell quot = ctx->pop();
      cell z = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      
      ctx->push(x);
      if (TAG(quot) == QUOTATION_TYPE) interpret_quotation(quot);
      else if (TAG(quot) == WORD_TYPE) interpret_word(quot);
      else if (TAG(quot) == TUPLE_TYPE) call_callable(quot);
      
      ctx->push(y);
      if (TAG(quot) == QUOTATION_TYPE) interpret_quotation(quot);
      else if (TAG(quot) == WORD_TYPE) interpret_word(quot);
      else if (TAG(quot) == TUPLE_TYPE) call_callable(quot);
      
      ctx->push(z);
      if (TAG(quot) == QUOTATION_TYPE) interpret_quotation(quot);
      else if (TAG(quot) == WORD_TYPE) interpret_word(quot);
      else if (TAG(quot) == TUPLE_TYPE) call_callable(quot);
      return true;
    }
    
    // tri: ( x p q r -- ) apply p, q, r each to x
    case HANDLER_TRI: {
      cell r = ctx->pop();
      cell q = ctx->pop();
      cell p = ctx->pop();
      cell x = ctx->peek();
      
      if (TAG(p) == QUOTATION_TYPE) interpret_quotation(p);
      else if (TAG(p) == WORD_TYPE) interpret_word(p);
      else if (TAG(p) == TUPLE_TYPE) call_callable(p);
      
      ctx->push(x);
      if (TAG(q) == QUOTATION_TYPE) interpret_quotation(q);
      else if (TAG(q) == WORD_TYPE) interpret_word(q);
      else if (TAG(q) == TUPLE_TYPE) call_callable(q);
      
      ctx->push(x);
      if (TAG(r) == QUOTATION_TYPE) interpret_quotation(r);
      else if (TAG(r) == WORD_TYPE) interpret_word(r);
      else if (TAG(r) == TUPLE_TYPE) call_callable(r);
      return true;
    }
    
    // loop: ( quot -- ) call quot repeatedly until it returns f
    case HANDLER_LOOP: {
      cell quot = ctx->pop();
      while (true) {
        if (TAG(quot) == QUOTATION_TYPE) interpret_quotation(quot);
        else if (TAG(quot) == WORD_TYPE) interpret_word(quot);
        else if (TAG(quot) == TUPLE_TYPE) call_callable(quot);
        cell result = ctx->pop();
        if (result == false_object) break;
      }
      return true;
    }
    
    // do: ( pred body -- pred body ) run body once, leaving pred and body on stack
    // Factor definition: dup 2dip
    // This is used as a helper for while - it runs the body once before the loop
    case HANDLER_DO: {
      cell body = ctx->peek();  // peek, not pop - body stays on stack
      // Run body with pred and body temporarily removed, then restore them
      // dup 2dip means: ( pred body ) -> dup -> ( pred body body ) -> 2dip ->
      //   saves pred and body, calls body, restores pred and body
      if (TAG(body) == QUOTATION_TYPE) interpret_quotation(body);
      else if (TAG(body) == WORD_TYPE) interpret_word(body);
      else if (TAG(body) == TUPLE_TYPE) call_callable(body);
      // pred and body are already on stack (we peeked, not popped)
      return true;
    }
    
    // ========== Tuple/Curry/Compose Operations ==========
    
    // <tuple-boa>: Complex - let Factor handle
    case HANDLER_TUPLE_BOA:
      return false;
    
    // curry: ( obj quot -- curried ) - create a curried callable
    case HANDLER_CURRY_WORD: {
      if (g_curried_layout == 0)
        return false;  // Not cached yet
      cell quot = ctx->pop();
      cell obj = ctx->pop();
      tuple_layout* layout = untag<tuple_layout>(g_curried_layout);
      data_root<tuple> t(allot<tuple>(tuple_size(layout)), this);
      t->layout = g_curried_layout;
      t->data()[0] = obj;
      t->data()[1] = quot;
      ctx->push(t.value());
      return true;
    }
    
    // 2curry: ( obj1 obj2 quot -- curried ) = [ curry ] dip curry
    // When called, the result should do: obj1 obj2 quot call
    // Which means push obj1, push obj2, call quot
    case HANDLER_2CURRY: {
      if (g_curried_layout == 0)
        return false;
      cell quot = ctx->pop();
      cell obj2 = ctx->pop();
      cell obj1 = ctx->pop();
      tuple_layout* layout = untag<tuple_layout>(g_curried_layout);
      // Create curried structure that when called does: obj1 obj2 <quot>
      // Inner curry: obj2 quot -> curried1 (pushes obj2 then calls quot)
      data_root<tuple> t1(allot<tuple>(tuple_size(layout)), this);
      t1->layout = g_curried_layout;
      t1->data()[0] = obj2;
      t1->data()[1] = quot;
      // Outer curry: obj1 curried1 -> curried2 (pushes obj1 then calls curried1)
      data_root<tuple> t2(allot<tuple>(tuple_size(layout)), this);
      t2->layout = g_curried_layout;
      t2->data()[0] = obj1;
      t2->data()[1] = t1.value();
      ctx->push(t2.value());
      return true;
    }
    
    // 3curry: ( obj1 obj2 obj3 quot -- curried ) = [ 2curry ] dip curry
    // When called, the result should do: obj1 obj2 obj3 quot call
    case HANDLER_3CURRY: {
      if (g_curried_layout == 0)
        return false;
      cell quot = ctx->pop();
      cell obj3 = ctx->pop();
      cell obj2 = ctx->pop();
      cell obj1 = ctx->pop();
      tuple_layout* layout = untag<tuple_layout>(g_curried_layout);
      // Innermost curry: obj3 quot (pushes obj3, calls quot)
      data_root<tuple> t1(allot<tuple>(tuple_size(layout)), this);
      t1->layout = g_curried_layout;
      t1->data()[0] = obj3;
      t1->data()[1] = quot;
      // Middle curry: obj2 curried1 (pushes obj2, calls curried1)
      data_root<tuple> t2(allot<tuple>(tuple_size(layout)), this);
      t2->layout = g_curried_layout;
      t2->data()[0] = obj2;
      t2->data()[1] = t1.value();
      // Outer curry: obj1 curried2 (pushes obj1, calls curried2)
      data_root<tuple> t3(allot<tuple>(tuple_size(layout)), this);
      t3->layout = g_curried_layout;
      t3->data()[0] = obj1;
      t3->data()[1] = t2.value();
      ctx->push(t3.value());
      return true;
    }
    
    // compose: ( quot1 quot2 -- composed ) - create a composed callable
    case HANDLER_COMPOSE_WORD: {
      if (g_composed_layout == 0)
        return false;
      cell quot2 = ctx->pop();
      cell quot1 = ctx->pop();
      tuple_layout* layout = untag<tuple_layout>(g_composed_layout);
      data_root<tuple> t(allot<tuple>(tuple_size(layout)), this);
      t->layout = g_composed_layout;
      t->data()[0] = quot1;
      t->data()[1] = quot2;
      ctx->push(t.value());
      return true;
    }
    
    // special-object: ( n -- obj ) get special object by index
    case HANDLER_SPECIAL_OBJECT: {
      cell n = ctx->pop();
      if (TAG(n) != FIXNUM_TYPE) {
        ctx->push(n);
        return false;
      }
      fixnum idx = untag_fixnum(n);
      if (idx < 0 || idx >= (fixnum)special_object_count) {
        ctx->push(false_object);
      } else {
        ctx->push(special_objects[idx]);
      }
      return true;
    }
    
    // OBJ-GLOBAL: ( -- n ) push the OBJ_GLOBAL constant index
    case HANDLER_OBJ_GLOBAL: {
      ctx->push(tag_fixnum(OBJ_GLOBAL));
      return true;
    }
    
    // OBJ-CURRENT-THREAD: ( -- n ) push the OBJ_CURRENT_THREAD constant
    case HANDLER_OBJ_CURRENT_THREAD: {
      ctx->push(tag_fixnum(OBJ_CURRENT_THREAD));
      return true;
    }
    
    // context-object: ( n -- obj ) get context object by index
    case HANDLER_CONTEXT_OBJECT: {
      cell n = ctx->pop();
      if (TAG(n) != FIXNUM_TYPE) {
        ctx->push(n);
        return false;
      }
      fixnum idx = untag_fixnum(n);
      // Context objects are stored in the context
      if (idx >= 0 && idx < (fixnum)context_object_count) {
        ctx->push(ctx->context_objects[idx]);
      } else {
        ctx->push(false_object);
      }
      return true;
    }
    
    // get-catchstack: ( -- catchstack ) get the current catchstack
    case HANDLER_GET_CATCHSTACK: {
      // CONTEXT-OBJ-CATCHSTACK is index 0 typically
      ctx->push(ctx->context_objects[0]);  // catchstack
      return true;
    }
    
    // global: ( -- global-namespace ) get the global namespace
    case HANDLER_GLOBAL: {
      ctx->push(special_objects[OBJ_GLOBAL]);
      return true;
    }
    
    // t: ( -- t ) push canonical true value
    case HANDLER_T_CONSTANT: {
      ctx->push(special_objects[OBJ_CANONICAL_TRUE]);
      return true;
    }
    
    // CONTEXT-OBJ-CONTEXT: ( -- n ) push context object index for context
    case HANDLER_CONTEXT_OBJ_CONTEXT: {
      // This is typically index 5
      ctx->push(tag_fixnum(5));  // CONTEXT-OBJ-CONTEXT
      return true;
    }
    
    // CONTEXT-OBJ-NAMESTACK: ( -- n ) push context object index for namestack
    case HANDLER_CONTEXT_OBJ_NAMESTACK: {
      ctx->push(tag_fixnum(1));  // CONTEXT-OBJ-NAMESTACK
      return true;
    }
    
    // CONTEXT-OBJ-CATCHSTACK: ( -- n ) push context object index for catchstack
    case HANDLER_CONTEXT_OBJ_CATCHSTACK: {
      ctx->push(tag_fixnum(0));  // CONTEXT-OBJ-CATCHSTACK
      return true;
    }
    
    // get-namestack: ( -- namestack ) get the current namestack
    case HANDLER_GET_NAMESTACK: {
      ctx->push(ctx->context_objects[1]);  // namestack at index 1
      return true;
    }
    
    // get-callstack: ( -- callstack ) get the current callstack
    case HANDLER_GET_CALLSTACK: {
      // This is complex - needs to capture the actual callstack
      // Let Factor handle it
      return false;
    }
    
    // get-retainstack: ( -- retainstack ) get the current retain stack
    case HANDLER_GET_RETAINSTACK: {
      // Return the retainstack as an array - complex, let Factor handle
      return false;
    }
    
    // word?: ( obj -- ? ) check if object is a word
    case HANDLER_WORD_QUESTION: {
      cell obj = ctx->pop();
      ctx->push(tag_boolean(TAG(obj) == WORD_TYPE));
      return true;
    }
    
    // =: ( obj1 obj2 -- ? ) structural equality
    case HANDLER_EQUALS: {
      cell obj2 = ctx->pop();
      cell obj1 = ctx->pop();
      // Use the existing objects_equal helper for deep comparison
      ctx->push(tag_boolean(objects_equal(obj1, obj2)));
      return true;
    }
    
    // rethrow: ( error -- * ) rethrow an error
    case HANDLER_RETHROW: {
      // Complex control flow - let Factor handle
      return false;
    }
    
    // or: ( obj1 obj2 -- obj1/obj2 ) return first truthy or last value
    case HANDLER_OR: {
      cell obj2 = ctx->pop();
      cell obj1 = ctx->pop();
      // or returns obj1 if truthy, else obj2
      ctx->push(obj1 != false_object ? obj1 : obj2);
      return true;
    }
    
    // and: ( obj1 obj2 -- obj1/obj2 ) return first falsy or last value
    case HANDLER_AND: {
      cell obj2 = ctx->pop();
      cell obj1 = ctx->pop();
      // and returns obj1 if falsy, else obj2
      ctx->push(obj1 == false_object ? obj1 : obj2);
      return true;
    }
    
    // not: ( obj -- ? ) logical not
    case HANDLER_NOT: {
      cell obj = ctx->pop();
      ctx->push(tag_boolean(obj == false_object));
      return true;
    }
    
    // tuple: ( -- tuple-class ) push the tuple class word
    case HANDLER_TUPLE_WORD: {
      // "tuple" is the base class for all tuples
      // It's typically looked up via a word - return the word itself
      static cell tuple_word_cache = 0;
      if (tuple_word_cache == 0) {
        tuple_word_cache = find_word_by_name_local(this, "tuple");
      }
      if (tuple_word_cache != false_object) {
        ctx->push(tuple_word_cache);
        return true;
      }
      return false;
    }
    
    // -: ( x y -- x-y ) subtraction
    case HANDLER_MINUS: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      // Fast path for fixnums
      if (TAG(x) == FIXNUM_TYPE && TAG(y) == FIXNUM_TYPE) {
        fixnum a = untag_fixnum(x);
        fixnum b = untag_fixnum(y);
        fixnum result = a - b;
        // Check for overflow - fixnum range is roughly -2^29 to 2^29-1 on 32-bit
        // Use a simple overflow check
        if ((b > 0 && a < fixnum_min + b) || (b < 0 && a > fixnum_max + b)) {
          // Overflow - fall back
          ctx->push(x);
          ctx->push(y);
          return false;
        }
        ctx->push(tag_fixnum(result));
        return true;
      }
      // Fall back for bignums, floats
      ctx->push(x);
      ctx->push(y);
      return false;
    }
    
    // set-global: ( value key -- ) set a global variable
    case HANDLER_SET_GLOBAL: {
      // This needs to call into hashtable set-at, complex
      return false;
    }
    
    // get-global: ( key -- value ) get a global variable
    case HANDLER_GET_GLOBAL: {
      // This needs to call into hashtable at, complex
      return false;
    }
    
    // clone: Complex - let Factor handle for safety
    case HANDLER_CLONE:
      return false;
    
    // if-empty: DISABLED - causes weird file creation bug
    // TODO: Debug why this handler creates files named with control characters
    case HANDLER_IF_EMPTY:
      return false;
    
    // box-at: ( key globals -- box ) - complex with cache, let Factor handle
    case HANDLER_BOX_AT:
      return false;
    
    // Default: not handled by fast path
    default:
      return false;
  }
}

// ============================================================================

bool factor_vm::dispatch_subprimitive(const std::string& name) {
  const uint32_t h = runtime_hash(name);
  switch (h) {

  case WORD_HASH("dup"):
    if (name == "dup") {
      cell v = ctx->peek();
      ctx->push(v);
      return true;
    }
    break;

  case WORD_HASH("dupd"):
    if (name == "dupd") {
      cell top = ctx->pop();
      cell second = ctx->pop();
      if (trace_enabled())
        std::cout << "[wasm] dupd: top=0x" << std::hex << top << " (type " << TAG(top)
                  << ") second=0x" << second << " (type " << TAG(second) << ")" << std::dec << std::endl;
      ctx->push(second);
      ctx->push(second);
      ctx->push(top);
      return true;
    }
    break;

  case WORD_HASH("drop"):
    if (name == "drop") {
      ctx->pop();
      return true;
    }
    break;

  case WORD_HASH("nip"):
    if (name == "nip") {
      cell top = ctx->pop();
      ctx->pop();
      ctx->push(top);
      return true;
    }
    break;

  case WORD_HASH("2drop"):
    if (name == "2drop") {
      ctx->pop();
      ctx->pop();
      return true;
    }
    break;

  case WORD_HASH("2nip"):
    if (name == "2nip") {
      cell top = ctx->pop();
      ctx->pop();
      ctx->pop();
      ctx->push(top);
      return true;
    }
    break;

  case WORD_HASH("3drop"):
    if (name == "3drop") {
      ctx->pop();
      ctx->pop();
      ctx->pop();
      return true;
    }
    break;

  case WORD_HASH("4drop"):
    if (name == "4drop") {
      ctx->pop();
      ctx->pop();
      ctx->pop();
      ctx->pop();
      return true;
    }
    break;

  case WORD_HASH("2dup"):
    if (name == "2dup") {
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(a);
      ctx->push(b);
      ctx->push(a);
      ctx->push(b);
      return true;
    }
    break;

  case WORD_HASH("3dup"):
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
    break;

  case WORD_HASH("4dup"):
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
    break;

  case WORD_HASH("over"):
    if (name == "over") {
      cell top = ctx->pop();
      cell second = ctx->peek();
      ctx->push(top);
      ctx->push(second);
      return true;
    }
    break;

  case WORD_HASH("2over"):
    if (name == "2over") {
      // ( x y z -- x y z x y )
      cell z = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->peek();
      ctx->push(y);
      ctx->push(z);
      ctx->push(x);
      ctx->push(y);
      return true;
    }
    break;

  case WORD_HASH("pick"):
    if (name == "pick") {
      cell top = ctx->pop();
      cell second = ctx->pop();
      cell third = ctx->peek();
      ctx->push(second);
      ctx->push(top);
      ctx->push(third);
      return true;
    }
    break;

  case WORD_HASH("swap"):
    if (name == "swap") {
      cell y = ctx->pop();
      cell x = ctx->pop();
      if (trace_enabled()) {
        std::cout << "[wasm] swap: x=0x" << std::hex << x << " tag=" << (x & 0xf)
                  << " y=0x" << y << " tag=" << (y & 0xf) << std::dec << std::endl;
      }
      ctx->push(y);
      ctx->push(x);
      return true;
    }
    break;

  case WORD_HASH("swapd"):
    if (name == "swapd") {
      cell c = ctx->pop();
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(b);
      ctx->push(a);
      ctx->push(c);
      return true;
    }
    break;

  case WORD_HASH("rot"):
    if (name == "rot") {
      cell c = ctx->pop();
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(b);
      ctx->push(c);
      ctx->push(a);
      return true;
    }
    break;

  case WORD_HASH("-rot"):
    if (name == "-rot") {
      cell c = ctx->pop();
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(c);
      ctx->push(a);
      ctx->push(b);
      return true;
    }
    break;

  case WORD_HASH("eq?"):
    if (name == "eq?") {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(tag_boolean(x == y));
      return true;
    }
    break;

  case WORD_HASH("both-fixnums?"):
    if (name == "both-fixnums?") {
      cell y = ctx->pop();
      cell x = ctx->pop();
      bool ok = TAG(x) == FIXNUM_TYPE && TAG(y) == FIXNUM_TYPE;
      ctx->push(tag_boolean(ok));
      return true;
    }
    break;

  case WORD_HASH("fixnum-bitand"):
    if (name == "fixnum-bitand") {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_fixnum(x & y));
      return true;
    }
    break;

  case WORD_HASH("fixnum-bitor"):
    if (name == "fixnum-bitor") {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_fixnum(x | y));
      return true;
    }
    break;

  case WORD_HASH("fixnum-bitxor"):
    if (name == "fixnum-bitxor") {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_fixnum(x ^ y));
      return true;
    }
    break;

  case WORD_HASH("fixnum-bitnot"):
    if (name == "fixnum-bitnot") {
      ctx->replace(tag_fixnum(~untag_fixnum(ctx->peek())));
      return true;
    }
    break;

  case WORD_HASH("fixnum<"):
    if (name == "fixnum<") {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_boolean(x < y));
      return true;
    }
    break;

  case WORD_HASH("fixnum<="):
    if (name == "fixnum<=") {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_boolean(x <= y));
      return true;
    }
    break;

  case WORD_HASH("fixnum>"):
    if (name == "fixnum>") {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_boolean(x > y));
      return true;
    }
    break;

  case WORD_HASH("fixnum>="):
    if (name == "fixnum>=") {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      ctx->push(tag_boolean(x >= y));
      return true;
    }
    break;

  case WORD_HASH("fixnum+"):
  case WORD_HASH("fixnum+fast"):
    if (name == "fixnum+" || name == "fixnum+fast") {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      // Use int64_t to avoid signed overflow UB
      int64_t r = (int64_t)x + (int64_t)y;
      if (r > fixnum_max || r < fixnum_min) {
        data_root<bignum> bx(fixnum_to_bignum(x), this);
        data_root<bignum> by(fixnum_to_bignum(y), this);
        data_root<bignum> res(bignum_add(bx.untagged(), by.untagged()), this);
        ctx->push(bignum_maybe_to_fixnum(res.untagged()));
      } else
        ctx->push(tag_fixnum((fixnum)r));
      return true;
    }
    break;

  case WORD_HASH("fixnum-"):
  case WORD_HASH("fixnum-fast"):
    if (name == "fixnum-" || name == "fixnum-fast") {
      fixnum y = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      // Use int64_t to avoid signed overflow UB
      int64_t r = (int64_t)x - (int64_t)y;
      if (r > fixnum_max || r < fixnum_min) {
        data_root<bignum> bx(fixnum_to_bignum(x), this);
        data_root<bignum> by(fixnum_to_bignum(y), this);
        data_root<bignum> res(bignum_subtract(bx.untagged(), by.untagged()),
                              this);
        ctx->push(bignum_maybe_to_fixnum(res.untagged()));
      } else
        ctx->push(tag_fixnum((fixnum)r));
      return true;
    }
    break;

  case WORD_HASH("fixnum*"):
  case WORD_HASH("fixnum*fast"):
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
    break;

  case WORD_HASH("fixnum-mod"):
    if (name == "fixnum-mod") {
      primitive_fixnum_divmod();
      cell remainder = ctx->pop();
      ctx->pop(); // quotient
      ctx->push(remainder);
      return true;
    }
    break;

  case WORD_HASH("fixnum/i-fast"):
    if (name == "fixnum/i-fast") {
      primitive_fixnum_divint();
      return true;
    }
    break;

  case WORD_HASH("fixnum/mod-fast"):
    if (name == "fixnum/mod-fast") {
      primitive_fixnum_divmod();
      return true;
    }
    break;

  case WORD_HASH("fixnum-shift-fast"):
    if (name == "fixnum-shift-fast") {
      primitive_fixnum_shift();
      return true;
    }
    break;

  case WORD_HASH("drop-locals"):
    if (name == "drop-locals") {
      fixnum count = untag_fixnum(ctx->pop());
      ctx->retainstack -= count * sizeof(cell);
      return true;
    }
    break;

  case WORD_HASH("load-local"):
    if (name == "load-local") {
      cell value = ctx->pop();
      ctx->retainstack += sizeof(cell);
      *(cell*)ctx->retainstack = value;
      return true;
    }
    break;

  case WORD_HASH("get-local"):
    if (name == "get-local") {
      fixnum index = untag_fixnum(ctx->pop());
      cell target = ctx->retainstack + index * sizeof(cell);
      if (target < ctx->retainstack_seg->start || target >= ctx->retainstack_seg->end)
        fatal_error("get-local: index out of bounds", index);
      cell* slot = (cell*)target;
      ctx->push(*slot);
      return true;
    }
    break;

  case WORD_HASH("tag"):
    if (name == "tag") {
      cell obj = ctx->pop();
      ctx->push(tag_fixnum(TAG(obj)));
      return true;
    }
    break;

  case WORD_HASH("slot"):
    if (name == "slot") {
      fixnum slot_index = untag_fixnum(ctx->pop());
      cell obj_cell = ctx->pop();
#if defined(FACTOR_WASM)
      if (immediate_p(obj_cell))
        fatal_error("slot: expected heap object", obj_cell);
#endif
      object* obj = untag<object>(obj_cell);
      cell* base = reinterpret_cast<cell*>(obj);
      ctx->push(base[slot_index]);
      return true;
    }
    break;

  case WORD_HASH("string-nth-fast"):
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
    break;

  case WORD_HASH("fpu-state"):
    if (name == "fpu-state") {
      ctx->push(false_object);
      return true;
    }
    break;

  case WORD_HASH("set-fpu-state"):
    if (name == "set-fpu-state") {
      ctx->pop();
      return true;
    }
    break;

  case WORD_HASH("set-callstack"):
    if (name == "set-callstack") {
      ctx->pop();
      return true;
    }
    break;

  case WORD_HASH("c-to-factor"):
  case WORD_HASH("unwind-native-frames"):
  case WORD_HASH("leaf-signal-handler"):
  case WORD_HASH("signal-handler"):
    if (name == "c-to-factor" || name == "unwind-native-frames" ||
        name == "leaf-signal-handler" || name == "signal-handler") {
      // No threads/signals on wasm; treat as no-ops.
      return true;
    }
    break;

  case WORD_HASH("(set-context)"):
  case WORD_HASH("(start-context)"):
    if (name == "(set-context)" || name == "(start-context)") {
      // Return the object unchanged; ignore context/quotation.
      cell obj = ctx->pop();
      ctx->pop();
      ctx->push(obj);
      return true;
    }
    break;

  case WORD_HASH("(set-context-and-delete)"):
  case WORD_HASH("(start-context-and-delete)"):
    if (name == "(set-context-and-delete)" || name == "(start-context-and-delete)") {
      // Drop inputs; no result.
      ctx->pop();
      ctx->pop();
      return true;
    }
    break;

  default:
    break;
  }

  // Word is not a known subprimitive - return false so the caller can try
  // other methods (e.g., interpreting a quotation definition).
  return false;
}

// Simple, non-optimizing interpreter for wasm. Supports words whose
// definitions are quotations and a minimal set of primitives/subprimitives.

// Word execution counter for performance monitoring
static std::atomic<uint64_t> word_exec_counter{0};
static bool word_counter_enabled() {
  static bool enabled = (std::getenv("FACTOR_WORD_COUNTER") != nullptr);
  return enabled;
}

// Profiling counters for handler dispatch effectiveness
static uint64_t g_interp_handler_fast = 0;
static uint64_t g_interp_handler_fallback = 0;
static uint64_t g_interp_quotation_only = 0;
static uint64_t g_interp_subprimitive = 0;

// Track top "quotation only" words - these are candidates for new handlers
static std::unordered_map<std::string, uint64_t> g_quotation_only_words;
static uint64_t g_quot_only_sample_counter = 0;

// Track which handlers are falling back - keyed by handler name
static std::unordered_map<std::string, uint64_t> g_handler_fallback_words;

static void print_top_quotation_only_words() {
  if (g_quotation_only_words.empty()) return;
  
  // Sort by frequency
  std::vector<std::pair<std::string, uint64_t>> sorted(
    g_quotation_only_words.begin(), g_quotation_only_words.end());
  std::sort(sorted.begin(), sorted.end(),
    [](const auto& a, const auto& b) { return a.second > b.second; });
  
  std::cerr << "[wasm] top 30 'quotation only' words (candidates for handlers):" << std::endl;
  for (size_t i = 0; i < std::min(sorted.size(), size_t(30)); i++) {
    std::cerr << "  " << (i+1) << ". " << sorted[i].first 
              << ": " << sorted[i].second << std::endl;
  }
}

static void print_top_handler_fallbacks() {
  if (g_handler_fallback_words.empty()) return;
  
  // Sort by frequency
  std::vector<std::pair<std::string, uint64_t>> sorted(
    g_handler_fallback_words.begin(), g_handler_fallback_words.end());
  std::sort(sorted.begin(), sorted.end(),
    [](const auto& a, const auto& b) { return a.second > b.second; });
  
  std::cerr << "[wasm] top handler fallbacks (returning false):" << std::endl;
  for (size_t i = 0; i < std::min(sorted.size(), size_t(15)); i++) {
    std::cerr << "  " << (i+1) << ". " << sorted[i].first 
              << ": " << sorted[i].second << std::endl;
  }
}

static void print_interp_stats() {
  uint64_t total = g_interp_handler_fast + g_interp_handler_fallback + 
                   g_interp_quotation_only + g_interp_subprimitive;
  if (total > 0) {
    std::cerr << "[wasm] interpret_word stats:" << std::endl;
    std::cerr << "  handler fast: " << g_interp_handler_fast 
              << " (" << (100.0 * g_interp_handler_fast / total) << "%)" << std::endl;
    std::cerr << "  handler fallback: " << g_interp_handler_fallback
              << " (" << (100.0 * g_interp_handler_fallback / total) << "%)" << std::endl;
    std::cerr << "  quotation only: " << g_interp_quotation_only
              << " (" << (100.0 * g_interp_quotation_only / total) << "%)" << std::endl;
    std::cerr << "  subprimitive: " << g_interp_subprimitive
              << " (" << (100.0 * g_interp_subprimitive / total) << "%)" << std::endl;
  }
}

// ============================================================================
// Trampoline-based Interpreter Main Loop
// ============================================================================
// This is the heart of the non-recursive interpreter. It processes work items
// from the global trampoline stack until empty.
void factor_vm::run_trampoline() {
  init_trampoline_stack();

  while (!trampoline_empty()) {
    WorkItem item = trampoline_pop();
    
    switch (item.type) {
      case WorkType::QUOTATION_CONTINUE: {
        // Continue executing a quotation from the given index
        cell array_tagged = item.quot.array_tagged;
        cell len = item.quot.length;
        cell idx = item.quot.index;
        
        // Get the array (may have moved due to GC, so use tagged reference)
        array* elements = untag<array>(array_tagged);
        
        // Cache special object pointers for pattern matching
        cell prim_word = special_objects[JIT_PRIMITIVE_WORD];
        cell declare_word = special_objects[JIT_DECLARE_WORD];
        
        // Process elements one at a time, but use local loop for efficiency
        // Only push back to trampoline when we need to execute something
        while (idx < len) {
          cell obj = elements->data()[idx];
          cell tag = TAG(obj);
          idx++;
          
          // Primitive call pattern: BYTE_ARRAY followed by JIT_PRIMITIVE_WORD
          if (tag == BYTE_ARRAY_TYPE && idx < len && elements->data()[idx] == prim_word) {
            byte_array* prim = untag<byte_array>(obj);
            if (!dispatch_primitive_call(prim))
              fatal_error("Unknown primitive in wasm interpreter", obj);
            idx++; // Skip the primitive word marker
            // Refresh elements pointer after potential GC in primitive
            elements = untag<array>(array_tagged);
            continue;
          }
          
          // Declaration pattern: ARRAY followed by JIT_DECLARE_WORD
          if (tag == ARRAY_TYPE && idx < len && elements->data()[idx] == declare_word) {
            idx++; // Skip the declare word marker
            continue;
          }
          
          // Literals get pushed directly
          if (tag == FIXNUM_TYPE || tag == F_TYPE || tag == ARRAY_TYPE ||
              tag == FLOAT_TYPE || tag == QUOTATION_TYPE || tag == BIGNUM_TYPE ||
              tag == ALIEN_TYPE || tag == TUPLE_TYPE || tag == BYTE_ARRAY_TYPE ||
              tag == CALLSTACK_TYPE || tag == STRING_TYPE) {
            ctx->push(obj);
            continue;
          }
          
          // Wrapper: push wrapped object
          if (tag == WRAPPER_TYPE) {
            ctx->push(untag<wrapper>(obj)->object);
            continue;
          }
          
          // Word: need to execute - push continuation and word
          if (tag == WORD_TYPE) {
            // If there are more elements, save the continuation
            if (idx < len) {
              push_quotation_work(array_tagged, len, idx);
            }
            // Push word execution
            push_word_work(obj);
            break;  // Exit local loop, let trampoline handle word
          }
          
          // Unknown type - fatal error
          fatal_error("Unsupported object type in quotation", obj);
        }
        break;
      }
      
      case WorkType::EXECUTE_WORD: {
        cell word_tagged = item.word.word_tagged;
        word* w = untag<word>(word_tagged);
        
        // Check for cached handler ID
        int32_t cached_id = get_cached_handler_id(w);
        
        if (cached_id == HANDLER_UNCACHED) {
          // Cache miss - look up and cache
          cached_id = lookup_and_cache_handler_id(w);
        }
        
        if (cached_id == HANDLER_NONE) {
          // No special handler - use quotation definition
          if (tagged<object>(w->def).type() == QUOTATION_TYPE) {
            quotation* q = untag<quotation>(w->def);
            array* arr = untag<array>(q->array);
            cell len = array_capacity(arr);
            if (len > 0) {
              push_quotation_work(q->array, len, 0);
            }
          }
        } else {
          // Dispatch by handler ID
          // This calls dispatch_by_handler_id which may push more work
          if (!trampoline_dispatch_handler(cached_id)) {
            // Handler returned false - fall back to quotation def
            if (tagged<object>(w->def).type() == QUOTATION_TYPE) {
              quotation* q = untag<quotation>(w->def);
              array* arr = untag<array>(q->array);
              cell len = array_capacity(arr);
              if (len > 0) {
                push_quotation_work(q->array, len, 0);
              }
            }
          }
        }
        break;
      }
      
      case WorkType::PUSH_VALUE: {
        ctx->push(item.single.value);
        break;
      }
      
      case WorkType::CALL_CALLABLE: {
        cell callable = item.single.value;
        cell tag = TAG(callable);

        if (tag == QUOTATION_TYPE) {
          quotation* q = untag<quotation>(callable);
          array* arr = untag<array>(q->array);
          cell len = array_capacity(arr);
          if (len > 0) {
            push_quotation_work(q->array, len, 0);
          }
        } else if (tag == WORD_TYPE) {
          push_word_work(callable);
        } else if (tag == WRAPPER_TYPE) {
          // Unwrap and re-call
          push_callable_work(untag<wrapper>(callable)->object);
        } else if (tag == TUPLE_TYPE) {
          tuple* t = untag<tuple>(callable);
          cell layout = t->layout;
          
          // Check for curried
          if (g_curried_layout != 0 && layout == g_curried_layout) {
            cell obj = t->data()[0];
            cell quot = t->data()[1];
            ctx->push(obj);  // Push the curried value
            push_callable_work(quot);  // Then call the quotation
          }
          // Check for composed
          else if (g_composed_layout != 0 && layout == g_composed_layout) {
            cell first = t->data()[0];
            cell second = t->data()[1];
            // Push in reverse order (LIFO)
            push_callable_work(second);
            push_callable_work(first);
          }
          // Slow path - use IS_TUPLE_CLASS
          else if (IS_TUPLE_CLASS(t, curried)) {
            cell obj = t->data()[0];
            cell quot = t->data()[1];
            ctx->push(obj);
            push_callable_work(quot);
          }
          else if (IS_TUPLE_CLASS(t, composed)) {
            cell first = t->data()[0];
            cell second = t->data()[1];
            push_callable_work(second);
            push_callable_work(first);
          }
          // Unknown tuple type - ignore
        }
        break;
      }
      
      case WorkType::RESTORE_VALUES: {
        // Push saved values back onto stack (in order)
        for (uint8_t i = 0; i < item.restore.count; i++) {
          ctx->push(item.restore.values[i]);
        }
        break;
      }
      
      case WorkType::LOOP_CONTINUE: {
        // Check the result on top of stack
        if (ctx->depth() == 0) {
          std::cerr << "[LOOP_CONTINUE] underflow! stack empty when trying to check result" << std::endl;
          std::cerr << "[LOOP_CONTINUE] quotation type=" << TAG(item.loop.quotation) << std::endl;
          fatal_error("LOOP_CONTINUE with empty stack", item.loop.quotation);
        }
        cell result = ctx->pop();
        if (to_boolean(result)) {
          // Continue looping - push loop work then body execution
          push_loop_work(item.loop.quotation);
          push_callable_work(item.loop.quotation);
        }
        // If false, loop terminates
        break;
      }
      
      case WorkType::WHILE_CONTINUE: {
        // Check predicate result
        cell result = ctx->pop();
        if (to_boolean(result)) {
          // Continue: execute body, then check predicate again
          push_while_work(item.while_loop.pred, item.while_loop.body);
          push_callable_work(item.while_loop.pred);
          push_callable_work(item.while_loop.body);
        }
        // If false, while terminates
        break;
      }
    }
  }
}

// Trampoline-aware handler dispatch
// Returns true if handled completely, false if should fall back to quotation def
// Unlike dispatch_by_handler_id, this NEVER calls interpret_word/quotation/call_callable
// Instead it pushes work items to the trampoline stack
bool factor_vm::trampoline_dispatch_handler(int32_t handler_id) {
  // NOTE: Many Factor combinator definitions are self-referential (e.g., dip uses dip)
  // So we CANNOT use Factor fallbacks for those - we must implement them here.
  // Setting this to true will cause infinite loops for self-referential combinators.
  static bool disable_control_flow_handlers = false;  // Must be false!

  // Stack validation for debugging stack leaks
  static bool validate_stack_effects = std::getenv("FACTOR_VALIDATE_STACK") != nullptr;
  cell depth_before = validate_stack_effects ? ctx->depth() : 0;

  // For now, delegate to original dispatch for non-control-flow handlers
  // Control flow handlers need special handling to push work items

  switch (handler_id) {
    case HANDLER_NONE:
      return false;
    
    // Control flow handlers that need trampoline-aware implementation
    case HANDLER_CALL:
    case HANDLER_PAREN_CALL: {
      // call is fundamental - must always be handled
      cell quot = ctx->pop();
      push_callable_work(quot);
      return true;
    }
    
    case HANDLER_CALL_EFFECT: {
      ctx->pop();  // Discard effect
      cell quot = ctx->pop();
      push_callable_work(quot);
      return true;
    }
    
    case HANDLER_EXECUTE:
    case HANDLER_PAREN_EXECUTE: {
      // execute is fundamental - must always be handled
      cell w = ctx->pop();
      push_word_work(w);
      return true;
    }
    
    case HANDLER_EXECUTE_EFFECT: {
      ctx->pop();  // Discard effect
      cell w = ctx->pop();
      push_word_work(w);
      return true;
    }
    
    // dip handlers - can be disabled for testing
    case HANDLER_DIP:
      if (disable_control_flow_handlers) return false;
      {
        cell quot = ctx->pop();
        cell x = ctx->pop();
        push_restore_1(x);  // Pushed first, popped second (after quot)
        push_callable_work(quot);  // Pushed second, popped first (executed first)
        return true;
      }
    
    case HANDLER_2DIP:
      if (disable_control_flow_handlers) return false;
      {
        cell quot = ctx->pop();
        cell y = ctx->pop();
        cell x = ctx->pop();
        push_restore_2(x, y);
        push_callable_work(quot);
        return true;
      }
    
    case HANDLER_3DIP:
      if (disable_control_flow_handlers) return false;
      {
        cell quot = ctx->pop();
        cell z = ctx->pop();
        cell y = ctx->pop();
        cell x = ctx->pop();
        push_restore_3(x, y, z);
        push_callable_work(quot);
        return true;
      }
    
    case HANDLER_IF: {
      cell false_quot = ctx->pop();
      cell true_quot = ctx->pop();
      cell cond = ctx->pop();
      push_callable_work(to_boolean(cond) ? true_quot : false_quot);
      return true;
    }
    
    case HANDLER_WHEN:
      if (disable_control_flow_handlers) return false;
      {
        cell quot = ctx->pop();
        cell cond = ctx->pop();
        if (to_boolean(cond)) {
          push_callable_work(quot);
        }
        return true;
      }
    
    case HANDLER_UNLESS:
      if (disable_control_flow_handlers) return false;
      {
        cell quot = ctx->pop();
        cell cond = ctx->pop();
        if (!to_boolean(cond)) {
          push_callable_work(quot);
        }
        return true;
      }
    
    case HANDLER_LOOP:
      if (disable_control_flow_handlers) return false;
      {
        cell quot = ctx->pop();
        // First execute the quotation, then check result
        push_loop_work(quot);
        push_callable_work(quot);
        return true;
      }
    
    case HANDLER_DO:
      if (disable_control_flow_handlers) return false;
      {
        // do: ( pred body -- pred body ) run body once, leaving pred and body on stack
        // Factor definition: dup 2dip
        cell body = ctx->peek();  // peek, not pop - body stays on stack
        push_callable_work(body);
        return true;
      }
    
    // keep handlers - can be disabled for testing
    case HANDLER_KEEP:
      if (disable_control_flow_handlers) return false;
      {
        // keep: ( x quot -- ...results x ) call quot with x on stack, then push x back
        // Equivalent to: over [ call ] dip
        // Stack: x quot → x x quot → (after quot) x
        cell quot = ctx->pop();
        cell x = ctx->pop();
        ctx->push(x);  // x is available for quot
        push_restore_1(x);  // After quot, restore x
        push_callable_work(quot);
        return true;
      }
    
    case HANDLER_2KEEP:
      if (disable_control_flow_handlers) return false;
      {
        // 2keep: ( x y quot -- ...results x y )
        cell quot = ctx->pop();
        cell y = ctx->pop();
        cell x = ctx->pop();
        ctx->push(x);
        ctx->push(y);
        push_restore_2(x, y);
        push_callable_work(quot);
        return true;
      }
    
    case HANDLER_3KEEP:
      if (disable_control_flow_handlers) return false;
      {
        // 3keep: ( x y z quot -- ...results x y z )
        cell quot = ctx->pop();
        cell z = ctx->pop();
        cell y = ctx->pop();
        cell x = ctx->pop();
        ctx->push(x);
        ctx->push(y);
        ctx->push(z);
        push_restore_3(x, y, z);
        push_callable_work(quot);
        return true;
      }
    
    // bi/tri handlers - can be disabled for testing
    case HANDLER_BI:
      if (disable_control_flow_handlers) return false;
      {
        // bi: ( x p q -- ) apply p to x (keeping x), then apply q to x
        cell q = ctx->pop();
        cell p = ctx->pop();
        cell x = ctx->peek();  // x stays on stack for p
        // Execute q with x, then p with x (reversed order for stack)
        // After p, push x for q
        // Work order: p executes, then x is pushed, then q executes
        push_callable_work(q);
        WorkItem push_x;
        push_x.type = WorkType::PUSH_VALUE;
        push_x.single.value = x;
        trampoline_push(push_x);
        push_callable_work(p);
        return true;
      }
    
    case HANDLER_BI_STAR:
      if (disable_control_flow_handlers) return false;
      {
        // bi*: ( x y p q -- ) apply p to x, apply q to y
        cell q = ctx->pop();
        cell p = ctx->pop();
        cell y = ctx->pop();
        cell x = ctx->pop();
        // Execute p with x, then q with y
        // Work order: push x, p executes, push y, q executes
        push_callable_work(q);
        WorkItem push_y;
        push_y.type = WorkType::PUSH_VALUE;
        push_y.single.value = y;
        trampoline_push(push_y);
        push_callable_work(p);
      WorkItem push_x;
      push_x.type = WorkType::PUSH_VALUE;
      push_x.single.value = x;
      trampoline_push(push_x);
      return true;
    }
    
    case HANDLER_BI_AT:
      if (disable_control_flow_handlers) return false;
      {
        // bi@: ( x y quot -- ) apply quot to x, then apply quot to y
        cell quot = ctx->pop();
        cell y = ctx->pop();
        cell x = ctx->pop();
        // Work order: push x, quot, push y, quot
        push_callable_work(quot);
        WorkItem push_y;
        push_y.type = WorkType::PUSH_VALUE;
        push_y.single.value = y;
        trampoline_push(push_y);
        push_callable_work(quot);
        WorkItem push_x;
        push_x.type = WorkType::PUSH_VALUE;
        push_x.single.value = x;
        trampoline_push(push_x);
        return true;
      }
    
    case HANDLER_TRI:
      if (disable_control_flow_handlers) return false;
      {
        // tri: ( x p q r -- ) apply p, q, r to x (keeping x for each)
        cell r = ctx->pop();
        cell q = ctx->pop();
        cell p = ctx->pop();
        cell x = ctx->peek();
        // Work order: p with x, push x, q with x, push x, r with x
        push_callable_work(r);
        WorkItem push_x2;
        push_x2.type = WorkType::PUSH_VALUE;
        push_x2.single.value = x;
        trampoline_push(push_x2);
        push_callable_work(q);
        WorkItem push_x1;
        push_x1.type = WorkType::PUSH_VALUE;
        push_x1.single.value = x;
        trampoline_push(push_x1);
        push_callable_work(p);
        return true;
      }
    
    case HANDLER_TRI_STAR:
      if (disable_control_flow_handlers) return false;
      {
        // tri*: ( x y z p q r -- ) apply p to x, q to y, r to z
        cell r = ctx->pop();
        cell q = ctx->pop();
        cell p = ctx->pop();
        cell z = ctx->pop();
        cell y = ctx->pop();
        cell x = ctx->pop();
        // Work order: push x, p, push y, q, push z, r
        push_callable_work(r);
        WorkItem push_z;
        push_z.type = WorkType::PUSH_VALUE;
        push_z.single.value = z;
        trampoline_push(push_z);
        push_callable_work(q);
        WorkItem push_y;
        push_y.type = WorkType::PUSH_VALUE;
        push_y.single.value = y;
        trampoline_push(push_y);
        push_callable_work(p);
        WorkItem push_x;
        push_x.type = WorkType::PUSH_VALUE;
        push_x.single.value = x;
        trampoline_push(push_x);
        return true;
      }
    
    case HANDLER_TRI_AT:
      if (disable_control_flow_handlers) return false;
      {
        // tri@: ( x y z quot -- ) apply quot to x, y, z in order
        cell quot = ctx->pop();
        cell z = ctx->pop();
        cell y = ctx->pop();
        cell x = ctx->pop();
        push_callable_work(quot);
        WorkItem push_z;
        push_z.type = WorkType::PUSH_VALUE;
        push_z.single.value = z;
        trampoline_push(push_z);
        push_callable_work(quot);
        WorkItem push_y;
        push_y.type = WorkType::PUSH_VALUE;
        push_y.single.value = y;
        trampoline_push(push_y);
        push_callable_work(quot);
        WorkItem push_x;
        push_x.type = WorkType::PUSH_VALUE;
        push_x.single.value = x;
        trampoline_push(push_x);
        return true;
      }
    
    // ========== Method Dispatch (needs trampoline-aware handling) ==========
    case HANDLER_MEGA_CACHE_LOOKUP: {
      cell cache = ctx->pop();
      cell index = untag_fixnum(ctx->pop());
      cell methods = ctx->pop();
      
      // Get the object to dispatch on from the stack (without removing it)
      // dispatch# is 0-indexed: 0 = top of stack, 1 = second from top, etc.
      cell* sp = (cell*)ctx->datastack;
      cell object = sp[-index];
      
      // Look up the method
      cell method = lookup_method(object, methods);
      
      // Update the cache
      
      // Update the cache
      update_method_cache(cache, object_class(object), method);
      
      // Push method execution to trampoline (don't call directly)
      push_callable_work(method);
      return true;
    }

    // Stack manipulation handlers - these must be in trampoline to avoid
    // recursive dispatch overhead during bootstrap
    case HANDLER_DUP: {
      cell v = ctx->peek();
      ctx->push(v);
      return true;
    }
    case HANDLER_2DUP: {
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(a);
      ctx->push(b);
      ctx->push(a);
      ctx->push(b);
      return true;
    }
    case HANDLER_3DUP: {
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
    case HANDLER_4DUP: {
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
    case HANDLER_OVER: {
      cell top = ctx->pop();
      cell second = ctx->peek();
      ctx->push(top);
      ctx->push(second);
      return true;
    }
    case HANDLER_2OVER: {
      // ( a b c d -- a b c d a b )
      // Copy the 3rd and 4th items from top
      cell d = ctx->pop();
      cell c = ctx->pop();
      cell b = ctx->pop();
      cell a = ctx->peek();
      ctx->push(b);
      ctx->push(c);
      ctx->push(d);
      ctx->push(a);
      ctx->push(b);
      return true;
    }
    case HANDLER_PICK: {
      cell top = ctx->pop();
      cell second = ctx->pop();
      cell third = ctx->peek();
      ctx->push(second);
      ctx->push(top);
      ctx->push(third);
      return true;
    }
    case HANDLER_DROP: {
      ctx->pop();
      return true;
    }
    case HANDLER_2DROP: {
      ctx->pop();
      ctx->pop();
      return true;
    }
    case HANDLER_3DROP: {
      ctx->pop();
      ctx->pop();
      ctx->pop();
      return true;
    }
    case HANDLER_4DROP: {
      ctx->pop();
      ctx->pop();
      ctx->pop();
      ctx->pop();
      return true;
    }
    case HANDLER_NIP: {
      cell top = ctx->pop();
      ctx->pop();
      ctx->push(top);
      return true;
    }
    case HANDLER_2NIP: {
      cell top = ctx->pop();
      ctx->pop();
      ctx->pop();
      ctx->push(top);
      return true;
    }
    case HANDLER_SWAP: {
      cell a = ctx->pop();
      cell b = ctx->pop();
      ctx->push(a);
      ctx->push(b);
      return true;
    }
    case HANDLER_SWAPD: {
      cell top = ctx->pop();
      cell a = ctx->pop();
      cell b = ctx->pop();
      ctx->push(a);
      ctx->push(b);
      ctx->push(top);
      return true;
    }
    case HANDLER_ROT: {
      cell a = ctx->pop();
      cell b = ctx->pop();
      cell c = ctx->pop();
      ctx->push(b);
      ctx->push(a);
      ctx->push(c);
      return true;
    }
    case HANDLER_MINUS_ROT: {
      cell a = ctx->pop();
      cell b = ctx->pop();
      cell c = ctx->pop();
      ctx->push(a);
      ctx->push(c);
      ctx->push(b);
      return true;
    }
    case HANDLER_DUPD: {
      cell top = ctx->pop();
      cell second = ctx->pop();
      ctx->push(second);
      ctx->push(second);
      ctx->push(top);
      return true;
    }

    // For all other handlers, delegate to the original dispatch
    // which handles arithmetic/comparison operations and other primitives
    default: {
      return dispatch_by_handler_id(handler_id);
    }
  }
}

void factor_vm::interpret_word(cell word_) {
  // Use recursive interpreter if flag is set (for debugging)
  if (g_use_recursive_interpreter) {
    interpret_word_recursive(word_);
    return;
  }
  
  // If already in trampoline mode, just push work and return
  if (g_in_trampoline) {
    push_word_work(word_);
    return;
  }
  
  // Not in trampoline mode - enter trampoline
  init_trampoline_stack();
  TrampolineGuard guard;
  push_word_work(word_);
  run_trampoline();
}

// Keep original recursive version
void factor_vm::interpret_word_recursive(cell word_) {
  // Track no-method calls for debugging
  word* w_check = untag<word>(word_);
  std::string_view name_view = word_name_string_view(w_check);
  if (name_view == "no-method") {
    static int no_method_count = 0;
    static std::map<std::string, int> no_method_by_caller;
    no_method_count++;
    
    // Try to get info about what's on the stack (the object that has no method)
    std::string info = "unknown";
    if (ctx->depth() >= 1) {
      cell top = ctx->peek();
      if (TAG(top) == TUPLE_TYPE)
        info = tuple_class_name(untag<tuple>(top));
      else if (TAG(top) == WORD_TYPE)
        info = "word:" + word_name_string(untag<word>(top));
      else
        info = "tag" + std::to_string(TAG(top));
    }
    no_method_by_caller[info]++;
    
    if (no_method_count <= 10) {
      std::cerr << "[wasm] NO-METHOD #" << no_method_count 
                << " stack_top=" << info << std::endl;
    }
    
    if (no_method_count == 100 || no_method_count == 1000 || 
        no_method_count == 10000 || no_method_count == 50000) {
      std::cerr << "[wasm] NO-METHOD summary after " << no_method_count << " calls:" << std::endl;
      for (auto& p : no_method_by_caller) {
        std::cerr << "  " << p.first << ": " << p.second << std::endl;
      }
    }
  }
  
  if (word_counter_enabled()) {
    uint64_t count = ++word_exec_counter;
    if (count % 10000000 == 0) {
      std::cerr << "[wasm] words executed: " << count << std::endl;
    }
    // Print profiling stats every 500M words
    if (count % 50000000 == 0) {
      print_interp_stats();
      print_top_handler_fallbacks();
    }
    // Print top quotation-only words every 5000M
    if (count % 50000000 == 0) {
      print_top_quotation_only_words();
    }
  }
  
  // Fast path: check cache BEFORE creating data_root to avoid GC overhead
  word* w_raw = untag<word>(word_);
  
  // Check for cached handler ID (works for both regular words and subprimitives)
  int32_t cached_id = get_cached_handler_id(w_raw);
  
  if (cached_id != HANDLER_UNCACHED) {
    // Cache hit - dispatch without any string operations
    if (cached_id == HANDLER_NONE) {
      // Known non-special word with quotation def - track for profiling
      g_interp_quotation_only++;
      // Sample every 100th call to avoid excessive overhead
      if (word_counter_enabled() && (++g_quot_only_sample_counter % 100) == 0) {
        std::string name = word_name_string(w_raw);
        g_quotation_only_words[name]++;
      }
      interpret_quotation(w_raw->def);
      return;
    }
    // Dispatch by handler ID (handles both special words and subprimitives)
    if (dispatch_by_handler_id(cached_id)) {
      g_interp_handler_fast++;
      return;
    }
    // Handler ID was cached but dispatch_by_handler_id returned false
    // This means the handler exists in the table but isn't fully implemented
    // Fall back to string dispatch
    g_interp_handler_fallback++;
    data_root<word> w(word_, this);
    std::string name = word_name_string(w.untagged());
    // Track which handlers are falling back
    if (word_counter_enabled()) {
      g_handler_fallback_words[name]++;
    }
    if (to_boolean(w->subprimitive)) {
      if (dispatch_subprimitive(name)) return;
    }
    if (interpret_special_word(name)) return;
    if (tagged<object>(w->def).type() == QUOTATION_TYPE) {
      interpret_quotation(w->def);
      return;
    }
    // No handler worked - should not happen for cached IDs
    fatal_error("Cached handler ID but no handler worked", word_);
  }
  
  // Cache miss - look up handler ID and cache it
  // Use lookup_and_cache_handler_id which uses string_view (no allocation)
  int32_t id = lookup_and_cache_handler_id(w_raw);
  
  if (id == HANDLER_NONE) {
    // No special handler - check for quotation def
    if (tagged<object>(w_raw->def).type() == QUOTATION_TYPE) {
      g_interp_quotation_only++;
      interpret_quotation(w_raw->def);
      return;
    }
    // No quotation def - fall through to slow path
  } else {
    // Try dispatch by handler ID
    if (dispatch_by_handler_id(id)) {
      g_interp_handler_fast++;
      return;
    }
    // Handler ID found but dispatch returned false - fallback to string dispatch
    g_interp_handler_fallback++;
    data_root<word> w(word_, this);
    std::string name = word_name_string(w.untagged());
    if (to_boolean(w->subprimitive)) {
      if (dispatch_subprimitive(name)) return;
    }
    if (interpret_special_word(name)) return;
    if (tagged<object>(w->def).type() == QUOTATION_TYPE) {
      interpret_quotation(w->def);
      return;
    }
  }
  
  // Slow path: no handler matched, need full word examination
  // Create data_root for GC safety since we'll be doing more work
  data_root<word> w(word_, this);
  
  // For subprimitives without handlers, check quotation def
  if (to_boolean(w->subprimitive)) {
    if (tagged<object>(w->def).type() == QUOTATION_TYPE) {
      interpret_quotation(w->def);
      return;
    }
    // Unhandled subprimitive with no quotation def - fatal
    std::string name = word_name_string(w.untagged());
    std::cout << "[wasm] unsupported subprimitive: " << name 
              << " subprimitive=0x" << std::hex << w->subprimitive << std::dec
              << std::endl;
    fatal_error("Unsupported subprimitive in wasm interpreter", w->name);
  }

  // Non-subprimitive without handler - try quotation def
  if (tagged<object>(w->def).type() == QUOTATION_TYPE) {
    interpret_quotation(w->def);
    return;
  }

  if (w->def == false_object) {
    std::string name = word_name_string(w.untagged());
    std::cerr << "[wasm] Undefined word: " << name << std::endl;
    fatal_error("Undefined word encountered in interpreter", w.value());
  }

  fatal_error("Cannot interpret word definition", w->def);
}

// Quotation length profiling
static uint64_t g_quot_len_0 = 0;
static uint64_t g_quot_len_1 = 0;
static uint64_t g_quot_len_2 = 0;
static uint64_t g_quot_len_3_5 = 0;
static uint64_t g_quot_len_6_plus = 0;
static uint64_t g_quot_len_3_hit = 0;

static void print_quot_stats() {
  uint64_t total = g_quot_len_0 + g_quot_len_1 + g_quot_len_2 + g_quot_len_3_5 + g_quot_len_6_plus;
  if (total > 0) {
    std::cerr << "[wasm] quotation length stats:" << std::endl;
    std::cerr << "  len=0: " << g_quot_len_0 << " (" << (100.0 * g_quot_len_0 / total) << "%)" << std::endl;
    std::cerr << "  len=1: " << g_quot_len_1 << " (" << (100.0 * g_quot_len_1 / total) << "%)" << std::endl;
    std::cerr << "  len=2: " << g_quot_len_2 << " (" << (100.0 * g_quot_len_2 / total) << "%)" << std::endl;
    std::cerr << "  len=3-5: " << g_quot_len_3_5 << " (" << (100.0 * g_quot_len_3_5 / total) << "%) len3-hit=" << g_quot_len_3_hit << std::endl;
    std::cerr << "  len>=6: " << g_quot_len_6_plus << " (" << (100.0 * g_quot_len_6_plus / total) << "%)" << std::endl;
  }
}

void factor_vm::interpret_quotation(cell quot_) {
  // Use recursive interpreter if flag is set (for debugging)
  if (g_use_recursive_interpreter) {
    interpret_quotation_recursive(quot_);
    return;
  }
  
  // If already in trampoline mode, just push work and return
  // The outer trampoline loop will process it
  if (g_in_trampoline) {
    quotation* q = untag<quotation>(quot_);
    array* arr = untag<array>(q->array);
    cell len = array_capacity(arr);
    if (len > 0) {
      push_quotation_work(q->array, len, 0);
    }
    return;
  }
  
  // Not in trampoline mode - enter trampoline
  init_trampoline_stack();
  TrampolineGuard guard;  // Sets g_in_trampoline = true
  
  quotation* q = untag<quotation>(quot_);
  array* arr = untag<array>(q->array);
  cell len = array_capacity(arr);
  
  if (len > 0) {
    push_quotation_work(q->array, len, 0);
  }
  
  // Run the trampoline loop
  run_trampoline();
}

// Keep the original recursive version for reference/fallback
void factor_vm::interpret_quotation_recursive(cell quot_) {
  data_root<quotation> quot(quot_, this);
  array* elements_raw = untag<array>(quot->array);
  cell len = array_capacity(elements_raw);
  
  // Profile quotation lengths
  if (word_counter_enabled()) {
    static uint64_t quot_count = 0;
    if (len == 0) g_quot_len_0++;
    else if (len == 1) g_quot_len_1++;
    else if (len == 2) g_quot_len_2++;
    else if (len <= 5) g_quot_len_3_5++;
    else g_quot_len_6_plus++;
    if (++quot_count % 200000000 == 0) print_quot_stats();
  }
  
  // Fast path for empty quotations
  if (__builtin_expect(len == 0, 0))
    return;
  
  // Fast path for single-element quotations containing just a word
  // This is safe because interpret_word handles its own GC safety
  if (len == 1) {
    cell obj = elements_raw->data()[0];
    cell tag = TAG(obj);
    if (tag == WORD_TYPE) {
      interpret_word(obj);
      return;
    }
    // Single fixnum - just push it
    if (tag == FIXNUM_TYPE) {
      ctx->push(obj);
      return;
    }
    // Fall through to general case for other types
  }
  
  // Fast path for two-element quotations (24.7% of all quotations)
  // Common patterns: word+word, literal+word, word+literal
  if (len == 2) {
    cell* data = elements_raw->data();
    cell obj0 = data[0];
    cell obj1 = data[1];
    cell tag0 = TAG(obj0);
    cell tag1 = TAG(obj1);
    
    // Pattern: word word (most common - execute both)
    if (tag0 == WORD_TYPE && tag1 == WORD_TYPE) {
      interpret_word(obj0);
      interpret_word(obj1);
      return;
    }
    
    // Pattern: fixnum word (push literal, execute word)
    if (tag0 == FIXNUM_TYPE && tag1 == WORD_TYPE) {
      ctx->push(obj0);
      interpret_word(obj1);
      return;
    }
    
    // Pattern: word fixnum (execute word, push literal)
    if (tag0 == WORD_TYPE && tag1 == FIXNUM_TYPE) {
      interpret_word(obj0);
      ctx->push(obj1);
      return;
    }
    
    // Pattern: fixnum fixnum (push both)
    if (tag0 == FIXNUM_TYPE && tag1 == FIXNUM_TYPE) {
      ctx->push(obj0);
      ctx->push(obj1);
      return;
    }
    
    // Pattern: f word (push f, execute word)
    if (tag0 == F_TYPE && tag1 == WORD_TYPE) {
      ctx->push(obj0);
      interpret_word(obj1);
      return;
    }
    
    // Pattern: quotation word (push quotation, execute word - common for combinators)
    if (tag0 == QUOTATION_TYPE && tag1 == WORD_TYPE) {
      ctx->push(obj0);
      interpret_word(obj1);
      return;
    }
    
    // Pattern: word quotation (execute word, push quotation)
    if (tag0 == WORD_TYPE && tag1 == QUOTATION_TYPE) {
      interpret_word(obj0);
      ctx->push(obj1);
      return;
    }
    
    // Pattern: array word (push array, execute word)
    if (tag0 == ARRAY_TYPE && tag1 == WORD_TYPE) {
      ctx->push(obj0);
      interpret_word(obj1);
      return;
    }
    
    // Pattern: wrapper word (push wrapped object, execute word)
    if (tag0 == WRAPPER_TYPE && tag1 == WORD_TYPE) {
      ctx->push(untag<wrapper>(obj0)->object);
      interpret_word(obj1);
      return;
    }
    
    // Pattern: string word (push string, execute word)
    if (tag0 == STRING_TYPE && tag1 == WORD_TYPE) {
      ctx->push(obj0);
      interpret_word(obj1);
      return;
    }
    
    // Fall through to general case for other patterns
  }
  
  // Fast path for three-element quotations (common in Factor)
  // Common patterns: literal word word, word literal word, word word literal
  if (len == 3) {
    cell* data = elements_raw->data();
    cell obj0 = data[0];
    cell obj1 = data[1];
    cell obj2 = data[2];
    cell tag0 = TAG(obj0);
    cell tag1 = TAG(obj1);
    cell tag2 = TAG(obj2);
    
    // Pattern: word word word (execute all three)
    if (tag0 == WORD_TYPE && tag1 == WORD_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      interpret_word(obj0);
      interpret_word(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Pattern: fixnum word word (push, execute, execute)
    if (tag0 == FIXNUM_TYPE && tag1 == WORD_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      ctx->push(obj0);
      interpret_word(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Pattern: word fixnum word (execute, push, execute)
    if (tag0 == WORD_TYPE && tag1 == FIXNUM_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      interpret_word(obj0);
      ctx->push(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Pattern: word word fixnum (execute, execute, push)
    if (tag0 == WORD_TYPE && tag1 == WORD_TYPE && tag2 == FIXNUM_TYPE) {
      g_quot_len_3_hit++;
      interpret_word(obj0);
      interpret_word(obj1);
      ctx->push(obj2);
      return;
    }
    
    // Pattern: fixnum fixnum word (push both, execute)
    if (tag0 == FIXNUM_TYPE && tag1 == FIXNUM_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      ctx->push(obj0);
      ctx->push(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Pattern: quotation word word (push quot, execute both words - common for combinators)
    if (tag0 == QUOTATION_TYPE && tag1 == WORD_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      ctx->push(obj0);
      interpret_word(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Pattern: word quotation word (execute, push, execute - common for control flow)
    if (tag0 == WORD_TYPE && tag1 == QUOTATION_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      interpret_word(obj0);
      ctx->push(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Pattern: quotation quotation word (push both, execute - common for if/when)
    if (tag0 == QUOTATION_TYPE && tag1 == QUOTATION_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      ctx->push(obj0);
      ctx->push(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Pattern: array word word (push array, execute both)
    if (tag0 == ARRAY_TYPE && tag1 == WORD_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      ctx->push(obj0);
      interpret_word(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Pattern: wrapper word word (push wrapped, execute both)
    if (tag0 == WRAPPER_TYPE && tag1 == WORD_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      ctx->push(untag<wrapper>(obj0)->object);
      interpret_word(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Pattern: string word word (push string, execute both)
    if (tag0 == STRING_TYPE && tag1 == WORD_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      ctx->push(obj0);
      interpret_word(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Pattern: f word word (push f, execute both)
    if (tag0 == F_TYPE && tag1 == WORD_TYPE && tag2 == WORD_TYPE) {
      g_quot_len_3_hit++;
      ctx->push(obj0);
      interpret_word(obj1);
      interpret_word(obj2);
      return;
    }
    
    // Fall through to general case
  }
  
  if (__builtin_expect(trace_enabled(), 0)) {
    std::cout << "[wasm] interpret_quotation len=" << len << " elements: ";
    for (cell j = 0; j < std::min(len, (cell)10); j++) {
      cell e = elements_raw->data()[j];
      std::cout << "0x" << std::hex << e << "(t" << TAG(e) << ") " << std::dec;
    }
    if (len > 10) std::cout << "...";
    std::cout << std::endl;
  }
  
  // Cache special object pointers for pattern matching
  cell prim_word = special_objects[JIT_PRIMITIVE_WORD];
  cell declare_word = special_objects[JIT_DECLARE_WORD];
  
  // Walk array using raw pointer - refresh after any GC-triggering operation
  for (cell i = 0; i < len; i++) {
    // Refresh elements pointer after potential GC
    elements_raw = untag<array>(quot->array);
    cell* data = elements_raw->data();
    cell obj = data[i];
    cell tag = TAG(obj);

    // Fast path: most common types first
    if (tag == FIXNUM_TYPE) {
      ctx->push(obj);
      continue;
    }
    
    if (tag == WORD_TYPE) {
      if (__builtin_expect(trace_enabled(), 0))
        std::cout << "[wasm] word " << word_name_string(untag<word>(obj)) 
                  << " (depth=" << ctx->depth() << ")" << std::endl;
      interpret_word(obj);
      continue;
    }

    // Primitive call pattern: BYTE_ARRAY followed by JIT_PRIMITIVE_WORD
    if (tag == BYTE_ARRAY_TYPE && (i + 1) < len && data[i + 1] == prim_word) {
      data_root<byte_array> prim(obj, this);
      if (!dispatch_primitive_call(prim.untagged()))
        fatal_error("Unknown primitive in wasm interpreter", obj);
      i++; // Skip the primitive word marker
      continue;
    }

    // Declaration pattern: ARRAY followed by JIT_DECLARE_WORD
    if (tag == ARRAY_TYPE && (i + 1) < len && data[i + 1] == declare_word) {
      if (__builtin_expect(trace_enabled(), 0)) {
        std::cout << "[wasm] skipping declare pattern" << std::endl;
      }
      i++; // Skip the declare word marker
      continue;
    }

    // Other pushable types
    switch (tag) {
      case F_TYPE:
      case ARRAY_TYPE:
      case FLOAT_TYPE:
      case QUOTATION_TYPE:
      case BIGNUM_TYPE:
      case ALIEN_TYPE:
      case TUPLE_TYPE:
        if (__builtin_expect(trace_enabled(), 0)) {
          std::cout << "[wasm] push 0x" << std::hex << obj 
                    << " type=" << tag;
          if (tag == TUPLE_TYPE) {
            tuple* t = untag<tuple>(obj);
            std::cout << " class=" << tuple_class_name(t);
          }
          std::cout << std::dec << " (depth=" << ctx->depth() << "->" << (ctx->depth()+1) << ")" << std::endl;
        }
        ctx->push(obj);
        break;
      case BYTE_ARRAY_TYPE:
      case CALLSTACK_TYPE:
      case STRING_TYPE:
        ctx->push(obj);
        break;
      case WRAPPER_TYPE: {
        // Wrappers quote their contents - push the wrapped object
        cell wrapped = untag<wrapper>(obj)->object;
        if (__builtin_expect(trace_enabled(), 0)) {
          std::cout << "[wasm] unwrap wrapper 0x" << std::hex << obj 
                    << " -> 0x" << wrapped << " type=" << TAG(wrapped) 
                    << std::dec << std::endl;
        }
        ctx->push(wrapped);
        break;
      }
      default:
        fatal_error("Unsupported object in interpreter", obj);
        break;
    }
  }
}

// Call any callable object (quotation, word, curried, composed, wrapper)
// Uses a fixed-size inline buffer to avoid heap allocation for common cases
void factor_vm::call_callable(cell callable) {
  // Log the first few call_callable invocations with tuples
  static int call_callable_entry_count = 0;
  if (TAG(callable) == TUPLE_TYPE) {
    call_callable_entry_count++;
    if (call_callable_entry_count <= 0) {
      tuple* t = untag<tuple>(callable);
      std::string tname = tuple_class_name(t);
      std::cerr << "[wasm] call_callable ENTRY #" << call_callable_entry_count
                << " source=" << (g_call_callable_source ? g_call_callable_source : "?")
                << " callable=tuple:" << tname
                << " stack_depth=" << ctx->depth();
      if (tname == "curried") {
        // Dump the full curried tree
        std::cerr << std::endl << "[wasm] CURRIED TREE:" << std::endl;
        std::function<void(tuple*, int)> dump_curried = [&](tuple* cur, int depth) {
          std::string indent(depth * 2, ' ');
          cell obj = cur->data()[0];
          cell quot = cur->data()[1];
          std::cerr << indent << "curried obj=0x" << std::hex << obj << ":t" << TAG(obj);
          if (TAG(obj) == WORD_TYPE) {
            std::cerr << "(" << word_name_string(untag<word>(obj)) << ")";
          }
          std::cerr << " quot=0x" << quot << ":t" << TAG(quot);
          if (TAG(quot) == QUOTATION_TYPE) {
            array* arr = untag<array>(untag<quotation>(quot)->array);
            cell len = array_capacity(arr);
            std::cerr << "(len=" << std::dec << len << ")";
          }
          std::cerr << std::dec << std::endl;
          if (TAG(obj) == TUPLE_TYPE) {
            tuple* inner = untag<tuple>(obj);
            std::string inner_name = tuple_class_name(inner);
            if (inner_name == "curried" && depth < 5) {
              dump_curried(inner, depth + 1);
            } else {
              std::cerr << indent << "  (inner is " << inner_name << ")" << std::endl;
            }
          }
          if (TAG(quot) == TUPLE_TYPE) {
            tuple* quot_inner = untag<tuple>(quot);
            std::string quot_name = tuple_class_name(quot_inner);
            if (quot_name == "curried" && depth < 5) {
              std::cerr << indent << "  quot is curried:" << std::endl;
              dump_curried(quot_inner, depth + 1);
            }
          }
        };
        dump_curried(t, 1);
      } else {
        std::cerr << std::endl;
      }
    }
  }
  
  // Inline buffer for work stack - most calls need only 1-2 entries
  // Falls back to heap allocation only for deeply nested composed/curried
  static constexpr size_t INLINE_CAPACITY = 8;
  cell inline_buffer[INLINE_CAPACITY];
  size_t work_size = 0;
  cell* work_data = inline_buffer;
  size_t work_capacity = INLINE_CAPACITY;
  
  // Helper to check if a cell is callable
  auto callable_p = [](cell c) {
    cell t = TAG(c);
    return t == QUOTATION_TYPE || t == WORD_TYPE || t == WRAPPER_TYPE ||
           t == TUPLE_TYPE;
  };
  
  // Helper to push onto work stack
  auto work_push = [&](cell c) {
    if (work_size >= work_capacity) {
      // Need to grow - allocate on heap
      size_t new_capacity = work_capacity * 2;
      cell* new_data = new cell[new_capacity];
      memcpy(new_data, work_data, work_size * sizeof(cell));
      if (work_data != inline_buffer) delete[] work_data;
      work_data = new_data;
      work_capacity = new_capacity;
    }
    work_data[work_size++] = c;
  };
  
  work_push(callable);

  // Debug counter for tracing quotation execution in call_callable
  static int call_callable_quot_count = 0;
  
  // Track if we just processed a curried tuple
  static bool just_processed_curried = false;

  // For iterative quotation execution, we use work items that can be:
  // - A callable (quotation/word/curried/composed) to execute
  // - A "continuation" marker indicating where to resume in a quotation
  // We use a simple scheme: odd addresses are continuations (quotation array + index)
  // Actually simpler: just track quotation context separately
  
  // Quotation execution state - eliminates recursion for sequential execution
  struct QuotState {
    array* arr;
    cell len;
    cell idx;
  };
  static constexpr size_t QSTACK_CAPACITY = 256;
  QuotState qstack[QSTACK_CAPACITY];
  size_t qstack_size = 0;
  
  auto qstack_push = [&](array* arr, cell len, cell idx) {
    if (qstack_size >= QSTACK_CAPACITY) {
      std::cerr << "[wasm] FATAL: quotation stack overflow" << std::endl;
      critical_error("qstack overflow", qstack_size);
    }
    qstack[qstack_size++] = {arr, len, idx};
  };

  while (work_size > 0 || qstack_size > 0) {
    // If we have active quotation execution, process next element
    if (qstack_size > 0) {
      QuotState& qs = qstack[qstack_size - 1];
      if (qs.idx >= qs.len) {
        // Finished this quotation
        qstack_size--;
        continue;
      }
      // Get next element to process
      cell elem = qs.arr->data()[qs.idx++];
      cell elem_tag = TAG(elem);
      
      // Words are executed
      if (elem_tag == WORD_TYPE) {
        // For now, we'll just call interpret_word but this still recurses
        interpret_word(elem);
        continue;
      }
      
      // Quotations are pushed to data stack (not executed)
      if (elem_tag == QUOTATION_TYPE) {
        ctx->push(elem);
        continue;
      }
      
      // All other types are literals - push to data stack
      if (elem_tag == WRAPPER_TYPE) {
        ctx->push(untag<wrapper>(elem)->object);
      } else {
        ctx->push(elem);
      }
      continue;
    }
    
    // No active quotation - pop from work stack
    cell current = work_data[--work_size];
    cell tag = TAG(current);

    // Fast path for quotation (most common)
    if (tag == QUOTATION_TYPE) {
      call_callable_quot_count++;
      // Log if this is a quotation right after a curried tuple
      if (just_processed_curried) {
        array* arr = untag<array>(untag<quotation>(current)->array);
        cell arr_len = array_capacity(arr);
        std::cerr << "[wasm] CURRIED QUOT EXEC: 0x" << std::hex << current << std::dec 
                  << " stack_depth=" << ctx->depth() << " len=" << arr_len << std::endl;
        // Dump the quotation contents
        std::cerr << "[wasm] CURRIED QUOT CONTENTS: [ ";
        for (cell i = 0; i < arr_len && i < 10; i++) {
          cell elem = arr->data()[i];
          cell elem_tag = TAG(elem);
          if (elem_tag == WORD_TYPE) {
            std::cerr << "word:" << word_name_string(untag<word>(elem)) << " ";
          } else if (elem_tag == QUOTATION_TYPE) {
            std::cerr << "quot ";
          } else if (elem_tag == FIXNUM_TYPE) {
            std::cerr << "fix:" << untag_fixnum(elem) << " ";
          } else {
            std::cerr << "0x" << std::hex << elem << std::dec << ":t" << elem_tag << " ";
          }
        }
        std::cerr << "]" << std::endl;
        just_processed_curried = false;
      }
      if (call_callable_quot_count <= 0) {
        array* arr = untag<array>(untag<quotation>(current)->array);
        cell arr_len = array_capacity(arr);
        std::cerr << "[wasm] call_callable executing quotation 0x" << std::hex << current
                  << " len=" << std::dec << arr_len;
        if (arr_len > 0 && arr_len <= 5) {
          std::cerr << " [ ";
          for (cell i = 0; i < arr_len; i++) {
            cell elem = arr->data()[i];
            std::cerr << "0x" << std::hex << elem << ":" << TAG(elem) << " ";
          }
          std::cerr << std::dec << "]";
        }
        std::cerr << " stack_depth=" << ctx->depth() << std::endl;
      }
      // Push quotation onto qstack for iterative execution instead of recursive call
      {
        array* arr = untag<array>(untag<quotation>(current)->array);
        cell arr_len = array_capacity(arr);
        if (arr_len > 0) {
          qstack_push(arr, arr_len, 0);
        }
      }
      continue;
    }
    
    // Fast path for word
    if (tag == WORD_TYPE) {
      interpret_word(current);
      continue;
    }
    
    // Fast path for wrapper
    if (tag == WRAPPER_TYPE) {
      cell wrapped = untag<wrapper>(current)->object;
      if (__builtin_expect(trace_enabled(), 0))
        std::cout << "[wasm] call_callable unwrap wrapper -> " << (void*)wrapped
                  << std::endl;
      work_push(wrapped);
      continue;
    }
    
    // Tuple path - curried/composed
    if (tag == TUPLE_TYPE) {
      tuple* t_raw = untag<tuple>(current);
      cell layout = t_raw->layout;
      
      // Unconditional logging for first N tuples to debug
      static int tuple_call_count = 0;
      tuple_call_count++;
      if (tuple_call_count <= 0) {
        std::string tname = tuple_class_name(t_raw);
        std::cerr << "[wasm] call_callable tuple #" << tuple_call_count
                  << " class=" << tname << " layout=0x" << std::hex << layout
                  << " g_curried=0x" << g_curried_layout 
                  << " g_composed=0x" << g_composed_layout << std::dec << std::endl;
      }
      
      // Fast path using cached layouts
      if (g_curried_layout != 0 && layout == g_curried_layout) {
        cell obj = t_raw->data()[0];
        cell quot = t_raw->data()[1];
        
        // Debug: log curried calls to find corruption
        static int curried_call_count = 0;
        curried_call_count++;
        if (curried_call_count <= 0) {
          std::cerr << "[wasm] call_callable curried #" << curried_call_count 
                    << ": obj=0x" << std::hex << obj << " tag=" << TAG(obj);
          if (TAG(obj) == WORD_TYPE)
            std::cerr << " word:" << word_name_string(untag<word>(obj));
          else if (TAG(obj) == TUPLE_TYPE)
            std::cerr << " tuple:" << tuple_class_name(untag<tuple>(obj));
          std::cerr << ", quot=0x" << quot << " tag=" << TAG(quot);
          if (TAG(quot) == WORD_TYPE)
            std::cerr << " word:" << word_name_string(untag<word>(quot));
          else if (TAG(quot) == QUOTATION_TYPE)
            std::cerr << " quotation";
          else if (TAG(quot) == TUPLE_TYPE)
            std::cerr << " tuple:" << tuple_class_name(untag<tuple>(quot));
          std::cerr << std::dec << std::endl;
          
          // Dump stack BEFORE push to see what's there
          std::cerr << "[wasm] stack BEFORE curried push (top 5):";
          for (int i = 0; i < 5 && i < (int)ctx->depth(); i++) {
            cell item = ((cell*)ctx->datastack)[-(i+1)];
            std::cerr << " [" << i << "]=0x" << std::hex << item << ":" << TAG(item);
          }
          std::cerr << std::dec << " depth=" << ctx->depth() << std::endl;
        }
        
        ctx->push(obj);
        if (curried_call_count <= 0) {
          std::cerr << "[wasm] pushed obj, stack[0]=0x" << std::hex << ctx->peek() 
                    << " depth=" << std::dec << ctx->depth() 
                    << " work_size=" << work_size << std::endl;
        }
        work_push(quot);
        if (curried_call_count <= 0) {
          std::cerr << "[wasm] pushed quot to work stack, work_size=" << work_size 
                    << " quot=0x" << std::hex << quot << std::dec << std::endl;
        }
        // just_processed_curried = true;  // Disabled for debug
        continue;
      }
      
      if (g_composed_layout != 0 && layout == g_composed_layout) {
        cell first = t_raw->data()[0];
        cell second = t_raw->data()[1];
        
        // Debug: log composed calls with more detail
        static int composed_call_count = 0;
        composed_call_count++;
        if (composed_call_count <= 0) {
          std::cerr << "[wasm] call_callable composed #" << composed_call_count 
                    << ": first=0x" << std::hex << first << ":t" << TAG(first);
          if (TAG(first) == TUPLE_TYPE)
            std::cerr << "(" << tuple_class_name(untag<tuple>(first)) << ")";
          else if (TAG(first) == WORD_TYPE)
            std::cerr << "(word:" << word_name_string(untag<word>(first)) << ")";
          std::cerr << " second=0x" << second << ":t" << TAG(second);
          if (TAG(second) == TUPLE_TYPE)
            std::cerr << "(" << tuple_class_name(untag<tuple>(second)) << ")";
          else if (TAG(second) == WORD_TYPE)
            std::cerr << "(word:" << word_name_string(untag<word>(second)) << ")";
          std::cerr << std::dec << std::endl;
        }
        
        if (__builtin_expect(trace_enabled(), 0)) {
          std::cout << "[wasm] call_callable composed (fast): first=0x" << std::hex
                    << first << " second=0x" << second << std::dec << std::endl;
        }
        // Execute first then second; push in reverse order because work is LIFO
        if (callable_p(second)) work_push(second);
        if (callable_p(first)) work_push(first);
        continue;
      }
      
      // Slow path - use IS_TUPLE_CLASS macro which will cache the layout
      tuple* t_untagged = t_raw;
      if (IS_TUPLE_CLASS(t_untagged, curried)) {
        cell obj = tuple_slot(t_untagged, 0);
        cell quot = tuple_slot(t_untagged, 1);
        
        // Debug slow path
        static int slow_curried_count = 0;
        slow_curried_count++;
        if (slow_curried_count <= 0) {
          std::cerr << "[wasm] SLOW PATH curried #" << slow_curried_count 
                    << ": obj=0x" << std::hex << obj << ":t" << TAG(obj);
          if (TAG(obj) == WORD_TYPE)
            std::cerr << "(word:" << word_name_string(untag<word>(obj)) << ")";
          else if (TAG(obj) == TUPLE_TYPE)
            std::cerr << "(tuple:" << tuple_class_name(untag<tuple>(obj)) << ")";
          std::cerr << " quot=0x" << quot << ":t" << TAG(quot);
          if (TAG(quot) == QUOTATION_TYPE) {
            array* arr = untag<array>(untag<quotation>(quot)->array);
            cell len = array_capacity(arr);
            std::cerr << "(len=" << std::dec << len << " [";
            for (cell i = 0; i < len && i < 5; i++) {
              cell elem = arr->data()[i];
              if (TAG(elem) == WORD_TYPE)
                std::cerr << " word:" << word_name_string(untag<word>(elem));
              else
                std::cerr << " 0x" << std::hex << elem << ":t" << TAG(elem);
            }
            std::cerr << std::dec << " ])";
          }
          std::cerr << std::dec << std::endl;
        }
        
        if (__builtin_expect(trace_enabled(), 0)) {
          std::cout << "[wasm] call_callable curried: obj=0x" << std::hex
                    << obj << " quot=0x" << quot << std::dec << std::endl;
        }
        ctx->push(obj);
        work_push(quot);
        continue;
      }
      if (IS_TUPLE_CLASS(t_untagged, composed)) {
        cell first = tuple_slot(t_untagged, 0);
        cell second = tuple_slot(t_untagged, 1);
        if (__builtin_expect(trace_enabled(), 0)) {
          std::cout << "[wasm] call_callable composed: first=0x" << std::hex
                    << first << " second=0x" << second << std::dec << std::endl;
        }
        // Execute first then second; push in reverse order because work is LIFO
        if (callable_p(second)) work_push(second);
        if (callable_p(first)) work_push(first);
        continue;
      }
      if (__builtin_expect(trace_enabled(), 0)) {
        std::string cname = tuple_class_name(t_untagged);
        std::cout << "[wasm] call_callable unsupported tuple class " << cname
                  << std::endl;
      }
      continue;
    }
    
    if (__builtin_expect(trace_enabled(), 0))
      std::cout << "[wasm] call_callable unsupported type " << tag 
                << " ptr " << (void*)current << std::endl;
  }
  
  // Clean up heap allocation if used
  if (work_data != inline_buffer) {
    delete[] work_data;
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
void factor_vm::call_callable(cell callable) { (void)callable; }
void* factor_vm::interpreter_entry_point() { return NULL; }
void factor_vm::set_interpreter_entry_points() {}

#endif // FACTOR_WASM

}
