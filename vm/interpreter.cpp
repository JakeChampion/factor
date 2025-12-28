#include "master.hpp"

namespace factor {

#if defined(FACTOR_WASM)

// Handler IDs for subprimitive dispatch
enum WasmHandlerId : int32_t {
  HANDLER_UNCACHED = -1,  // Not yet cached
  HANDLER_NONE = 0,       // No special handler, use quotation

  // Control flow handlers (1-99)
  HANDLER_IF = 1,
  HANDLER_WHEN,
  HANDLER_UNLESS,
  HANDLER_CALL,
  HANDLER_PAREN_CALL,
  HANDLER_CALL_EFFECT,
  HANDLER_EXECUTE,
  HANDLER_PAREN_EXECUTE,
  HANDLER_EXECUTE_EFFECT,
  HANDLER_DIP,
  HANDLER_2DIP,
  HANDLER_3DIP,
  HANDLER_KEEP,
  HANDLER_2KEEP,
  HANDLER_3KEEP,
  HANDLER_LOOP,
  HANDLER_MEGA_CACHE_LOOKUP,

  // Stack operation handlers (200+)
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
  HANDLER_NEG_ROT,
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
  HANDLER_TAG,
  HANDLER_SLOT,
  HANDLER_SET_SLOT,
  HANDLER_LENGTH,
  HANDLER_CALLABLE_Q,
  HANDLER_SPECIAL_OBJECT,
  HANDLER_SET_SPECIAL_OBJECT,
  HANDLER_CONTEXT_OBJECT,
  HANDLER_SET_CONTEXT_OBJECT,
};

// Handler ID table - maps word names to handler IDs
static const std::unordered_map<std::string_view, WasmHandlerId>& get_handler_id_table() {
  static const std::unordered_map<std::string_view, WasmHandlerId> table = {
    // Control flow - MUST be handled specially in trampoline
    {"if", HANDLER_IF},
    {"when", HANDLER_WHEN},
    {"unless", HANDLER_UNLESS},
    {"call", HANDLER_CALL},
    {"(call)", HANDLER_PAREN_CALL},
    {"call-effect", HANDLER_CALL_EFFECT},
    {"call-effect-unsafe", HANDLER_CALL_EFFECT},
    {"execute", HANDLER_EXECUTE},
    {"(execute)", HANDLER_PAREN_EXECUTE},
    {"execute-effect", HANDLER_EXECUTE_EFFECT},
    {"execute-effect-unsafe", HANDLER_EXECUTE_EFFECT},
    {"dip", HANDLER_DIP},
    {"2dip", HANDLER_2DIP},
    {"3dip", HANDLER_3DIP},
    {"keep", HANDLER_KEEP},
    {"2keep", HANDLER_2KEEP},
    {"3keep", HANDLER_3KEEP},
    {"loop", HANDLER_LOOP},
    {"mega-cache-lookup", HANDLER_MEGA_CACHE_LOOKUP},

    // Stack ops
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
    {"-rot", HANDLER_NEG_ROT},
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
    {"tag", HANDLER_TAG},
    {"slot", HANDLER_SLOT},
    {"set-slot", HANDLER_SET_SLOT},
    {"length", HANDLER_LENGTH},
    {"callable?", HANDLER_CALLABLE_Q},
    {"special-object", HANDLER_SPECIAL_OBJECT},
    {"set-special-object", HANDLER_SET_SPECIAL_OBJECT},
    {"context-object", HANDLER_CONTEXT_OBJECT},
    {"set-context-object", HANDLER_SET_CONTEXT_OBJECT},
  };
  return table;
}

// Magic value for handler caching
static const cell WASM_HANDLER_MAGIC = 0xFA570000;

// Get cached handler ID from word's pic_def field
inline int32_t get_cached_handler_id(word* w) {
  cell cached = w->pic_def;
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
  string* name_str = untag<string>(w->name);
  cell len = untag_fixnum(name_str->length);
  const char* bytes = reinterpret_cast<const char*>(name_str->data());
  std::string_view name(bytes, static_cast<size_t>(len));

  static const auto& table = get_handler_id_table();
  auto it = table.find(name);

  int32_t id = (it != table.end())
    ? static_cast<int32_t>(it->second)
    : HANDLER_NONE;

  set_cached_handler_id(w, id);
  return id;
}

// Cached tuple layouts for O(1) type checks
static cell g_curried_layout = 0;
static cell g_composed_layout = 0;

// VM instance pointer for from_boolean - set in run_trampoline
static thread_local factor_vm* g_vm = nullptr;

// Convert C++ bool to Factor boolean
inline cell from_boolean(bool b) {
  return b ? g_vm->special_objects[OBJ_CANONICAL_TRUE] : false_object;
}

void clear_wasm_layout_caches() {
  g_curried_layout = 0;
  g_composed_layout = 0;
}

// Track last executed word for debugging
char g_last_word_name[256] = "none";

namespace {

// Helper to check tuple class with caching
inline bool is_tuple_class(tuple* t, cell& cached_layout, const char* class_name) {
  if (cached_layout != 0) {
    return t->layout == cached_layout;
  }
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

#define IS_TUPLE_CLASS(t, name) is_tuple_class(t, g_##name##_layout, #name)

std::string word_name_string(word* w) {
  if (!w) return "<null>";
  cell name_cell = w->name;
  if (TAG(name_cell) != STRING_TYPE) return "<no-name>";
  string* s = untag<string>(name_cell);
  cell len = untag_fixnum(s->length);
  return std::string(reinterpret_cast<const char*>(s->data()), len);
}

} // anonymous namespace

// ============================================================================
// Trampoline-based interpreter
// ============================================================================

enum class WorkType : uint8_t {
  CALL_CALLABLE,      // Call a quotation/word/wrapper
  QUOTATION_CONTINUE, // Continue interpreting quotation at index
  RESTORE_VALUES,     // Push saved values back onto stack
  PUSH_VALUE,         // Push a single value
  LOOP_CONTINUE,      // Check loop condition and continue
};

struct WorkItem {
  WorkType type;
  union {
    struct { cell value; } single;
    struct { cell quot_array; cell length; cell index; } quot;
    struct { cell values[3]; uint8_t count; } restore;
  };
};

// Work stack for trampoline
static thread_local std::vector<WorkItem> g_work_stack;
static thread_local bool g_in_trampoline = false;

inline void trampoline_push(const WorkItem& item) {
  g_work_stack.push_back(item);
}

inline void push_callable_work(cell callable) {
  WorkItem item;
  item.type = WorkType::CALL_CALLABLE;
  item.single.value = callable;
  trampoline_push(item);
}

inline void push_quotation_work(cell quot_array, cell length, cell start_index) {
  if (start_index < length) {
    WorkItem item;
    item.type = WorkType::QUOTATION_CONTINUE;
    item.quot.quot_array = quot_array;
    item.quot.length = length;
    item.quot.index = start_index;
    trampoline_push(item);
  }
}

inline void push_word_work(cell word_cell) {
  push_callable_work(word_cell);
}

inline void push_restore_1(cell v) {
  WorkItem item;
  item.type = WorkType::RESTORE_VALUES;
  item.restore.count = 1;
  item.restore.values[0] = v;
  trampoline_push(item);
}

inline void push_restore_2(cell v1, cell v2) {
  WorkItem item;
  item.type = WorkType::RESTORE_VALUES;
  item.restore.count = 2;
  item.restore.values[0] = v1;
  item.restore.values[1] = v2;
  trampoline_push(item);
}

inline void push_restore_3(cell v1, cell v2, cell v3) {
  WorkItem item;
  item.type = WorkType::RESTORE_VALUES;
  item.restore.count = 3;
  item.restore.values[0] = v1;
  item.restore.values[1] = v2;
  item.restore.values[2] = v3;
  trampoline_push(item);
}

inline void push_loop_work(cell quot) {
  WorkItem item;
  item.type = WorkType::LOOP_CONTINUE;
  item.single.value = quot;
  trampoline_push(item);
}

// ============================================================================
// Primitive dispatch - uses EACH_PRIMITIVE macro for all VM primitives
// ============================================================================

// Get string_view from byte_array without allocation
inline std::string_view byte_array_to_string_view(byte_array* ba) {
  const cell len = array_capacity(ba);
  const char* bytes = reinterpret_cast<const char*>(ba->data<uint8_t>());
  size_t actual = 0;
  while (actual < static_cast<size_t>(len) && bytes[actual] != '\0')
    actual++;
  return std::string_view(bytes, actual);
}

bool factor_vm::dispatch_primitive_call(byte_array* name) {
  std::string_view prim = byte_array_to_string_view(name);

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
// Handler dispatch - for subprimitives with handler IDs
// ============================================================================

bool factor_vm::dispatch_by_handler_id(int32_t handler_id) {
  // Only implement essential stack operations
  // Let everything else fall back to Factor definitions

  switch (handler_id) {
    case HANDLER_NONE:
      return false;

    // Stack operations
    case HANDLER_DUP:
      ctx->push(ctx->peek());
      return true;
    case HANDLER_DROP:
      ctx->pop();
      return true;
    case HANDLER_SWAP: {
      cell a = ctx->pop();
      cell b = ctx->pop();
      ctx->push(a);
      ctx->push(b);
      return true;
    }
    case HANDLER_OVER: {
      cell a = ctx->pop();
      cell b = ctx->peek();
      ctx->push(a);
      ctx->push(b);
      return true;
    }
    case HANDLER_NIP: {
      cell top = ctx->pop();
      ctx->pop();
      ctx->push(top);
      return true;
    }
    case HANDLER_2DROP:
      ctx->pop();
      ctx->pop();
      return true;
    case HANDLER_2NIP: {
      // ( x y z -- z ) - remove 2nd and 3rd items
      cell top = ctx->pop();
      ctx->pop();
      ctx->pop();
      ctx->push(top);
      return true;
    }
    case HANDLER_2DUP: {
      cell a = ctx->pop();
      cell b = ctx->peek();
      ctx->push(a);
      ctx->push(b);
      ctx->push(a);
      return true;
    }
    case HANDLER_3DROP:
      ctx->pop();
      ctx->pop();
      ctx->pop();
      return true;
    case HANDLER_ROT: {
      cell c = ctx->pop();
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(b);
      ctx->push(c);
      ctx->push(a);
      return true;
    }
    case HANDLER_NEG_ROT: {
      cell c = ctx->pop();
      cell b = ctx->pop();
      cell a = ctx->pop();
      ctx->push(c);
      ctx->push(a);
      ctx->push(b);
      return true;
    }
    case HANDLER_PICK: {
      cell c = ctx->pop();
      cell b = ctx->pop();
      cell a = ctx->peek();
      ctx->push(b);
      ctx->push(c);
      ctx->push(a);
      return true;
    }
    case HANDLER_DUPD: {
      cell top = ctx->pop();
      cell second = ctx->peek();
      ctx->push(second);
      ctx->push(top);
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

    // Essential comparisons
    case HANDLER_EQ: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(from_boolean(x == y));
      return true;
    }
    case HANDLER_BOTH_FIXNUMS: {
      // Check if top two items are both fixnums
      cell* sp = (cell*)ctx->datastack;
      cell a = sp[0];
      cell b = sp[-1];
      ctx->push(from_boolean(TAG(a) == FIXNUM_TYPE && TAG(b) == FIXNUM_TYPE));
      return true;
    }
    case HANDLER_FIXNUM_LT: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(from_boolean(untag_fixnum(x) < untag_fixnum(y)));
      return true;
    }
    case HANDLER_FIXNUM_LE: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(from_boolean(untag_fixnum(x) <= untag_fixnum(y)));
      return true;
    }
    case HANDLER_FIXNUM_GT: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(from_boolean(untag_fixnum(x) > untag_fixnum(y)));
      return true;
    }
    case HANDLER_FIXNUM_GE: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(from_boolean(untag_fixnum(x) >= untag_fixnum(y)));
      return true;
    }

    // Essential arithmetic
    case HANDLER_FIXNUM_PLUS: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(tag_fixnum(untag_fixnum(x) + untag_fixnum(y)));
      return true;
    }
    case HANDLER_FIXNUM_MINUS: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(tag_fixnum(untag_fixnum(x) - untag_fixnum(y)));
      return true;
    }
    case HANDLER_FIXNUM_TIMES: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(tag_fixnum(untag_fixnum(x) * untag_fixnum(y)));
      return true;
    }
    case HANDLER_FIXNUM_SHIFT: {
      fixnum shift = untag_fixnum(ctx->pop());
      fixnum x = untag_fixnum(ctx->pop());
      if (shift >= 0) {
        ctx->push(tag_fixnum(x << shift));
      } else {
        ctx->push(tag_fixnum(x >> (-shift)));
      }
      return true;
    }
    case HANDLER_FIXNUM_BITAND: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(tag_fixnum(untag_fixnum(x) & untag_fixnum(y)));
      return true;
    }
    case HANDLER_FIXNUM_BITOR: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(tag_fixnum(untag_fixnum(x) | untag_fixnum(y)));
      return true;
    }
    case HANDLER_FIXNUM_BITXOR: {
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(tag_fixnum(untag_fixnum(x) ^ untag_fixnum(y)));
      return true;
    }
    case HANDLER_FIXNUM_BITNOT: {
      cell x = ctx->pop();
      ctx->push(tag_fixnum(~untag_fixnum(x)));
      return true;
    }

    // Slot access
    case HANDLER_SLOT: {
      cell slot = untag_fixnum(ctx->pop());
      cell obj = ctx->pop();
      ctx->push(((cell*)UNTAG(obj))[slot]);
      return true;
    }
    case HANDLER_SET_SLOT: {
      cell slot = untag_fixnum(ctx->pop());
      cell obj = ctx->pop();
      cell value = ctx->pop();
      ((cell*)UNTAG(obj))[slot] = value;
      return true;
    }

    // Tag
    case HANDLER_TAG: {
      cell obj = ctx->pop();
      ctx->push(tag_fixnum(TAG(obj)));
      return true;
    }

    // Length operations
    case HANDLER_LENGTH: {
      cell obj = ctx->pop();
      cell tag = TAG(obj);
      if (tag == ARRAY_TYPE) {
        ctx->push(tag_fixnum(array_capacity(untag<array>(obj))));
      } else if (tag == STRING_TYPE) {
        ctx->push(untag<string>(obj)->length);
      } else if (tag == BYTE_ARRAY_TYPE) {
        ctx->push(tag_fixnum(array_capacity(untag<byte_array>(obj))));
      } else {
        return false;
      }
      return true;
    }

    // Callable check
    case HANDLER_CALLABLE_Q: {
      cell obj = ctx->peek();
      cell tag = TAG(obj);
      bool is_callable = (tag == QUOTATION_TYPE || tag == WORD_TYPE ||
                          tag == TUPLE_TYPE || tag == WRAPPER_TYPE);
      ctx->pop();
      ctx->push(from_boolean(is_callable));
      return true;
    }

    // Special object access
    case HANDLER_SPECIAL_OBJECT: {
      fixnum n = untag_fixnum(ctx->pop());
      ctx->push(special_objects[n]);
      return true;
    }

    case HANDLER_SET_SPECIAL_OBJECT: {
      fixnum n = untag_fixnum(ctx->pop());
      cell value = ctx->pop();
      special_objects[n] = value;
      return true;
    }

    // Context object access
    case HANDLER_CONTEXT_OBJECT: {
      fixnum n = untag_fixnum(ctx->pop());
      ctx->push(ctx->context_objects[n]);
      return true;
    }

    case HANDLER_SET_CONTEXT_OBJECT: {
      fixnum n = untag_fixnum(ctx->pop());
      cell value = ctx->pop();
      ctx->context_objects[n] = value;
      return true;
    }

    default:
      // All control flow and complex operations fall back to Factor
      return false;
  }
}

// ============================================================================
// Trampoline dispatch for control flow
// ============================================================================

bool factor_vm::trampoline_dispatch_handler(int32_t handler_id) {
  switch (handler_id) {
    case HANDLER_IF: {
      cell f_quot = ctx->pop();
      cell t_quot = ctx->pop();
      cell cond = ctx->pop();
      push_callable_work(to_boolean(cond) ? t_quot : f_quot);
      return true;
    }

    case HANDLER_WHEN: {
      cell quot = ctx->pop();
      cell cond = ctx->pop();
      if (to_boolean(cond)) {
        push_callable_work(quot);
      }
      return true;
    }

    case HANDLER_UNLESS: {
      cell quot = ctx->pop();
      cell cond = ctx->pop();
      if (!to_boolean(cond)) {
        push_callable_work(quot);
      }
      return true;
    }

    case HANDLER_DIP: {
      cell quot = ctx->pop();
      cell x = ctx->pop();
      push_restore_1(x);
      push_callable_work(quot);
      return true;
    }

    case HANDLER_2DIP: {
      cell quot = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      push_restore_2(x, y);
      push_callable_work(quot);
      return true;
    }

    case HANDLER_3DIP: {
      cell quot = ctx->pop();
      cell z = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      push_restore_3(x, y, z);
      push_callable_work(quot);
      return true;
    }

    case HANDLER_KEEP: {
      cell quot = ctx->pop();
      cell x = ctx->pop();
      ctx->push(x);
      push_restore_1(x);
      push_callable_work(quot);
      return true;
    }

    case HANDLER_2KEEP: {
      cell quot = ctx->pop();
      cell y = ctx->pop();
      cell x = ctx->pop();
      ctx->push(x);
      ctx->push(y);
      push_restore_2(x, y);
      push_callable_work(quot);
      return true;
    }

    case HANDLER_3KEEP: {
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

    case HANDLER_CALL:
    case HANDLER_PAREN_CALL:
    case HANDLER_CALL_EFFECT: {
      cell quot = ctx->pop();
      push_callable_work(quot);
      return true;
    }

    case HANDLER_EXECUTE:
    case HANDLER_PAREN_EXECUTE:
    case HANDLER_EXECUTE_EFFECT: {
      cell word = ctx->pop();
      push_word_work(word);
      return true;
    }

    case HANDLER_LOOP: {
      // loop ( pred -- ) - executes pred, if true, loops
      cell quot = ctx->pop();
      push_loop_work(quot);  // Will check result and maybe continue
      push_callable_work(quot);  // Execute the quotation first
      return true;
    }

    case HANDLER_MEGA_CACHE_LOOKUP: {
      // mega-cache-lookup ( methods index cache -- )
      // Object to dispatch on is at datastack[-index]
      cell cache = ctx->pop();
      cell index = untag_fixnum(ctx->pop());
      cell methods = ctx->pop();

      // Get the object to dispatch on from the stack (without removing it)
      cell* sp = (cell*)ctx->datastack;
      cell object = sp[-(fixnum)index];

      // Look up the method
      cell method = lookup_method(object, methods);

      // Update the cache
      update_method_cache(cache, object_class(object), method);

      // Execute the found method
      cell method_type = TAG(method);
      if (method_type == WORD_TYPE || method_type == QUOTATION_TYPE) {
        push_callable_work(method);
      } else {
        fatal_error("mega-cache-lookup: unexpected method type", method);
      }
      return true;
    }

    default:
      // Fall back to dispatch_by_handler_id for stack ops/arithmetic
      return dispatch_by_handler_id(handler_id);
  }
}

// ============================================================================
// Main trampoline loop
// ============================================================================

void factor_vm::run_trampoline() {
  g_in_trampoline = true;
  g_vm = this;

  static uint64_t iter_count = 0;
  while (!g_work_stack.empty()) {
    iter_count++;
    // Log first 50 and then every 1M
    if (iter_count <= 50 || iter_count % 1000000 == 0) {
      FILE* f = fopen("init-factor.log", "a");
      if (f) {
        fprintf(f, "[TRAMP %llu] stack=%zu word=%s\n",
                iter_count, g_work_stack.size(), g_last_word_name);
        fclose(f);
      }
    }
    WorkItem item = g_work_stack.back();
    g_work_stack.pop_back();

    switch (item.type) {
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
          word* w = untag<word>(callable);

          // Save word name for debugging
          std::string name = word_name_string(w);
          strncpy(g_last_word_name, name.c_str(), sizeof(g_last_word_name) - 1);

          // Check for subprimitive (VM primitive)
          cell subprim = w->subprimitive;
          if (subprim != false_object && TAG(subprim) == BYTE_ARRAY_TYPE) {
            byte_array* ba = untag<byte_array>(subprim);
            if (dispatch_primitive_call(ba)) {
              break;
            }
          }

          // Check for handler ID (control flow, stack ops, etc.)
          int32_t cached_id = get_cached_handler_id(w);
          if (cached_id == HANDLER_UNCACHED) {
            cached_id = lookup_and_cache_handler_id(w);
          }

          if (cached_id != HANDLER_NONE) {
            // Dispatch by handler ID
            if (trampoline_dispatch_handler(cached_id)) {
              break;
            }
          }

          // Fall back to quotation definition
          cell def = w->def;
          if (TAG(def) == QUOTATION_TYPE) {
            quotation* q = untag<quotation>(def);
            array* arr = untag<array>(q->array);
            cell len = array_capacity(arr);
            if (len > 0) {
              push_quotation_work(q->array, len, 0);
            }
          }
        } else if (tag == TUPLE_TYPE) {
          tuple* t = untag<tuple>(callable);

          if (IS_TUPLE_CLASS(t, curried)) {
            cell obj = t->data()[0];
            cell quot = t->data()[1];
            ctx->push(obj);
            push_callable_work(quot);
          } else if (IS_TUPLE_CLASS(t, composed)) {
            cell first = t->data()[0];
            cell second = t->data()[1];
            push_callable_work(second);
            push_callable_work(first);
          }
        } else if (tag == WRAPPER_TYPE) {
          push_callable_work(untag<wrapper>(callable)->object);
        }
        break;
      }

      case WorkType::QUOTATION_CONTINUE: {
        array* arr = untag<array>(item.quot.quot_array);
        cell index = item.quot.index;
        cell length = item.quot.length;

        // Get current element
        cell elem = array_nth(arr, index);
        cell elem_tag = TAG(elem);

        // Check for primitive call pattern: BYTE_ARRAY followed by JIT_PRIMITIVE_WORD
        cell prim_word = special_objects[JIT_PRIMITIVE_WORD];
        if (elem_tag == BYTE_ARRAY_TYPE && (index + 1) < length &&
            array_nth(arr, index + 1) == prim_word) {
          // Skip the primitive word marker by advancing past it
          if (index + 2 < length) {
            push_quotation_work(item.quot.quot_array, length, index + 2);
          }
          // Dispatch the primitive
          byte_array* ba = untag<byte_array>(elem);
          if (!dispatch_primitive_call(ba)) {
            fatal_error("Unknown primitive in wasm interpreter", elem);
          }
          break;
        }

        // Check for declare pattern: ARRAY followed by JIT_DECLARE_WORD
        cell declare_word = special_objects[JIT_DECLARE_WORD];
        if (elem_tag == ARRAY_TYPE && (index + 1) < length &&
            array_nth(arr, index + 1) == declare_word) {
          // Skip the declare pattern
          if (index + 2 < length) {
            push_quotation_work(item.quot.quot_array, length, index + 2);
          }
          break;
        }

        // Schedule rest of quotation
        if (index + 1 < length) {
          push_quotation_work(item.quot.quot_array, length, index + 1);
        }

        // Process current element
        cell tag = elem_tag;
        if (tag == WORD_TYPE) {
          push_word_work(elem);
        } else if (tag == WRAPPER_TYPE) {
          ctx->push(untag<wrapper>(elem)->object);
        } else {
          // Literals (including quotations, tuples, etc.)
          ctx->push(elem);
        }
        break;
      }

      case WorkType::RESTORE_VALUES: {
        for (uint8_t i = 0; i < item.restore.count; i++) {
          ctx->push(item.restore.values[i]);
        }
        break;
      }

      case WorkType::PUSH_VALUE: {
        ctx->push(item.single.value);
        break;
      }

      case WorkType::LOOP_CONTINUE: {
        // Check if the loop body returned true
        cell result = ctx->pop();
        if (to_boolean(result)) {
          // Continue looping
          cell quot = item.single.value;
          push_loop_work(quot);  // Will check result again
          push_callable_work(quot);  // Execute body
        }
        break;
      }
    }
  }

  g_in_trampoline = false;
}

// ============================================================================
// Entry points
// ============================================================================

void factor_vm::interpret_word(cell word_) {
  push_word_work(word_);
  run_trampoline();
}

void factor_vm::interpret_quotation(cell quot_) {
  push_callable_work(quot_);
  run_trampoline();
}

void factor_vm::call_callable(cell callable) {
  push_callable_work(callable);
  run_trampoline();
}

void factor_vm::set_interpreter_entry_points() {
  // Nothing needed for trampoline interpreter
}

#else
// Non-WASM stubs
void factor_vm::interpret_word(cell word_) { (void)word_; }
void factor_vm::interpret_quotation(cell quot_) { (void)quot_; }
void factor_vm::call_callable(cell callable) { (void)callable; }
void clear_wasm_layout_caches() {}
void factor_vm::set_interpreter_entry_points() {}
#endif

} // namespace factor
