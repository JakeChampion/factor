#include "master.hpp"

namespace factor {

// Constants for predicate dispatch
namespace {
  const char* const SUPERCLASS_KEY = "superclass";
  const size_t SUPERCLASS_KEY_LEN = 10;
  const char* const TUPLE_CLASS_KEY = "tuple-class";
  const size_t TUPLE_CLASS_KEY_LEN = 11;
}

static cell search_lookup_alist(cell table, cell klass) {
  array* elements = untag<array>(table);
  for (fixnum index = array_capacity(elements) - 2; index >= 0; index -= 2) {
    if (array_nth(elements, index) == klass)
      return array_nth(elements, index + 1);
  }
  return false_object;
}

static cell search_lookup_hash(cell table, cell klass, cell hashcode) {
  array* buckets = untag<array>(table);
  cell bucket = array_nth(buckets, hashcode & (array_capacity(buckets) - 1));
  if (TAG(bucket) == ARRAY_TYPE)
    return search_lookup_alist(bucket, klass);
  return bucket;
}

static cell nth_superclass(tuple_layout* layout, fixnum echelon) {
  cell* ptr = (cell*)(layout + 1);
  return ptr[echelon * 2];
}

static cell nth_hashcode(tuple_layout* layout, fixnum echelon) {
  cell* ptr = (cell*)(layout + 1);
  return ptr[echelon * 2 + 1];
}

cell factor_vm::lookup_tuple_method(cell obj, cell methods) {
  tuple_layout* layout = untag<tuple_layout>(untag<tuple>(obj)->layout);

  array* echelons = untag<array>(methods);

  fixnum echelon = std::min(untag_fixnum(layout->echelon),
                            (fixnum)array_capacity(echelons) - 1);

  while (echelon >= 0) {
    cell echelon_methods = array_nth(echelons, echelon);

    if (TAG(echelon_methods) == WORD_TYPE)
      return echelon_methods;
    else if (to_boolean(echelon_methods)) {
      cell klass = nth_superclass(layout, echelon);
      cell hashcode = untag_fixnum(nth_hashcode(layout, echelon));
      cell result = search_lookup_hash(echelon_methods, klass, hashcode);
      if (to_boolean(result))
        return result;
    }

    echelon--;
  }

  critical_error("Cannot find tuple method", methods);
  return false_object;
}

cell factor_vm::lookup_method(cell obj, cell methods) {
  cell tag = TAG(obj);
  cell method = array_nth(untag<array>(methods), tag);

  // Debug: log when we get an ARRAY method for non-tuple types
  if (TAG(method) == ARRAY_TYPE && tag != TUPLE_TYPE) {
    static int array_method_count = 0;
    if (++array_method_count <= 10) {
      std::cerr << "[wasm] lookup_method: ARRAY method for tag=" << tag
                << " (obj=0x" << std::hex << obj << std::dec << ")"
                << " method_array_len=" << array_capacity(untag<array>(method))
                << std::endl;
    }
  }

  if (tag == TUPLE_TYPE) {
    if (TAG(method) == ARRAY_TYPE)
      return lookup_tuple_method(obj, method);
    return method;
  }

  // For non-tuple, non-word types: if we get an ARRAY, this is predicate dispatch
  // that we can't handle in the interpreter. Look for a no-method word to return.
  if (TAG(method) == ARRAY_TYPE && tag != WORD_TYPE) {
    std::cerr << "[wasm] lookup_method: tag=" << tag << " has ARRAY method (predicate dispatch)"
              << " - cannot evaluate predicates, looking for no-method" << std::endl;

    // Predicate dispatch array structure is typically:
    // [ predicate1 method1 predicate2 method2 ... ]
    // We can't evaluate predicates, so we need to find the "no-method" word
    // The "no-method" word should be in slot 0 of the methods array (the parent array)
    array* methods_arr = untag<array>(methods);
    if (array_capacity(methods_arr) > 0) {
      cell slot_zero = array_nth(methods_arr, 0);
      std::cerr << "[wasm] lookup_method: methods[0] tag=" << TAG(slot_zero) << std::endl;

      // If slot 0 is a word, it's likely the default/no-method handler
      if (TAG(slot_zero) == WORD_TYPE) {
        return slot_zero;
      }
    }

    // If we can't find a no-method word, this is a fatal error
    std::cerr << "[wasm] lookup_method: FATAL - cannot find no-method handler for tag=" << tag << std::endl;
    critical_error("lookup_method: no method found and no no-method handler", method);
    return false_object;
  }

  // Handle predicate class dispatch for WORD_TYPE
  // This handles cases like M: tuple-class boa where tuple-class is a predicate on words
  if (TAG(method) == ARRAY_TYPE && tag == WORD_TYPE) {
    // Debug: log when we hit this path
    static int word_dispatch_count = 0;
    word_dispatch_count++;
    if (word_dispatch_count <= 5) {
      word* obj_word = untag<word>(obj);
      cell name_cell = obj_word->name;
      if (TAG(name_cell) == STRING_TYPE) {
        string* name_str = untag<string>(name_cell);
        std::cerr << "[wasm] lookup_method WORD_TYPE with array method, word="
                  << std::string(reinterpret_cast<const char*>(name_str->data()),
                                 (size_t)untag_fixnum(name_str->length))
                  << " array_len=" << array_capacity(untag<array>(method))
                  << std::endl;
      }
    }

    // The method slot contains a decision tree for predicate classes
    // For predicates, the structure is typically an array where we need to
    // find a method that matches based on the predicate test
    word* obj_word = untag<word>(obj);
    cell props = obj_word->props;

    // Quick check: does this word have props (required for tuple-class)?
    if (props == false_object || TAG(props) != TUPLE_TYPE) {
      // No props means it can't be a tuple-class, return the array as-is
      // (will likely result in no-method via the mega-cache path)
      return method;
    }
    
    // Check if this is a tuple-class by looking for "superclass" in props
    // tuple-class words have superclass property (even if f for base tuple)
    tuple* ht = untag<tuple>(props);
    tuple_layout* layout = untag<tuple_layout>(ht->layout);
    
    // hashtable has at least 3 slots: count, deleted, array
    if (tuple_capacity(layout) < 3)
      return method;
      
    cell arr_cell = ht->data()[2];  // array>> slot
    if (TAG(arr_cell) != ARRAY_TYPE)
      return method;
      
    array* arr = untag<array>(arr_cell);
    cell arr_len = array_capacity(arr);
    bool has_superclass = false;
    
    // Search for "superclass" key
    for (cell j = 0; j + 1 < arr_len; j += 2) {
      cell key = array_nth(arr, j);
      if (TAG(key) == WORD_TYPE) {
        word* key_word = untag<word>(key);
        cell name_cell = key_word->name;
        if (TAG(name_cell) == STRING_TYPE) {
          string* name_str = untag<string>(name_cell);
          cell name_len = untag_fixnum(name_str->length);
          if (name_len == SUPERCLASS_KEY_LEN) {
            bool match = true;
            for (cell k = 0; k < SUPERCLASS_KEY_LEN && match; k++) {
              if (name_str->data()[k] != (uint8_t)SUPERCLASS_KEY[k])
                match = false;
            }
            if (match) {
              has_superclass = true;
              break;
            }
          }
        }
      }
    }
    
    if (!has_superclass)
      return method;  // Not a tuple-class
    
    // This word IS a tuple-class - search the predicate dispatch alist for tuple-class method
    array* alist = untag<array>(method);
    cell len = array_capacity(alist);
    
    if (word_dispatch_count <= 5) {
      std::cerr << "[wasm] lookup_method: word is tuple-class, searching alist len=" << len << std::endl;
    }
    
    for (cell i = 0; i + 1 < len; i += 2) {
      cell klass = array_nth(alist, i);
      cell klass_method = array_nth(alist, i + 1);
      
      // Check if klass is the "tuple-class" word
      if (TAG(klass) == WORD_TYPE) {
        word* klass_word = untag<word>(klass);
        cell klass_name = klass_word->name;
        if (TAG(klass_name) == STRING_TYPE) {
          string* klass_name_str = untag<string>(klass_name);
          cell klass_name_len = untag_fixnum(klass_name_str->length);
          if (klass_name_len == TUPLE_CLASS_KEY_LEN) {
            bool match = true;
            for (cell k = 0; k < TUPLE_CLASS_KEY_LEN && match; k++) {
              if (klass_name_str->data()[k] != (uint8_t)TUPLE_CLASS_KEY[k])
                match = false;
            }
            if (match) {
              // Found tuple-class method!
              if (word_dispatch_count <= 5) {
                std::cerr << "[wasm] lookup_method: found tuple-class method!" << std::endl;
              }
              return klass_method;
            }
          }
        }
      }
    }
    
    // No tuple-class method found in alist - fall through
    if (word_dispatch_count <= 5) {
      std::cerr << "[wasm] lookup_method: no tuple-class method found in alist" << std::endl;
    }
  }
  
  return method;
}

void factor_vm::primitive_lookup_method() {
  cell methods = ctx->pop();
  cell obj = ctx->pop();
  ctx->push(lookup_method(obj, methods));
}

cell factor_vm::object_class(cell obj) {
  cell tag = TAG(obj);
  if (tag == TUPLE_TYPE)
    return untag<tuple>(obj)->layout;
  return tag_fixnum(tag);
}

static cell method_cache_hashcode(cell klass, array* array) {
  cell capacity = (array_capacity(array) >> 1) - 1;
  return ((klass >> TAG_BITS) & capacity) << 1;
}

void factor_vm::update_method_cache(cell cache, cell klass, cell method) {
  array* cache_elements = untag<array>(cache);
  cell hashcode = method_cache_hashcode(klass, cache_elements);
  set_array_nth(cache_elements, hashcode, klass);
  set_array_nth(cache_elements, hashcode + 1, method);
}

void factor_vm::primitive_mega_cache_miss() {
  dispatch_stats.megamorphic_cache_misses++;

  cell cache = ctx->pop();
  fixnum index = untag_fixnum(ctx->pop());
  cell methods = ctx->pop();

  cell object = ((cell*)ctx->datastack)[-index];
  cell klass = object_class(object);
  cell method = lookup_method(object, methods);

  update_method_cache(cache, klass, method);

  ctx->push(method);
}

void factor_vm::primitive_reset_dispatch_stats() {
  memset(&dispatch_stats, 0, sizeof(dispatch_statistics));
}

// Allocates memory
void factor_vm::primitive_dispatch_stats() {
  ctx->push(tag<byte_array>(byte_array_from_value(&dispatch_stats)));
}

}
