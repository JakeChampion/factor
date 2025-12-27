#include "master.hpp"

namespace factor {

static bool wasm_trace_enabled() {
#if defined(FACTOR_WASM)
  static bool enabled = (std::getenv("FACTOR_WASM_TRACE") != nullptr);
  return enabled;
#else
  return false;
#endif
}

static std::string word_name_string(word* w) {
  string* name = untag<string>(w->name);
  cell len = untag_fixnum(name->length);
  return std::string(reinterpret_cast<const char*>(name->data()),
                     static_cast<size_t>(len));
}

static cell find_word_by_name(factor_vm* vm, const std::string& target) {
  data_root<array> words(vm->instances(WORD_TYPE), vm);
  if (wasm_trace_enabled()) {
    std::cout << "[wasm] find_word_by_name searching " << array_capacity(words.untagged())
              << " words for '" << target << "'" << std::endl;
  }
  int debug_hits = 0;
  cell n_words = array_capacity(words.untagged());
  for (cell i = 0; i < n_words; i++) {
    word* w = untag<word>(array_nth(words.untagged(), i));
    std::string name = word_name_string(w);
    if (name == target)
      return tag<word>(w);
    if (wasm_trace_enabled() && name.find("eval") != std::string::npos &&
        debug_hits < 20) {
      std::cout << "  [eval-like] " << name << std::endl;
      debug_hits++;
    }
  }
  return false_object;
}

// Compile code in boot image so that we can execute the startup quotation
// Allocates memory
void factor_vm::prepare_boot_image() {
#if defined(FACTOR_WASM)
  // Interpreter path: skip native compilation.
  set_interpreter_entry_points();
  special_objects[OBJ_STAGE2] = special_objects[OBJ_CANONICAL_TRUE];
  return;
#endif

  std::cout << "*** Stage 2 early init... " << std::flush;

  // Compile all words.
  data_root<array> words(instances(WORD_TYPE), this);

  cell n_words = array_capacity(words.untagged());
  for (cell i = 0; i < n_words; i++) {
    data_root<word> word(array_nth(words.untagged(), i), this);

    FACTOR_ASSERT(!word->entry_point);
    jit_compile_word(word.value(), word->def, false);
  }
  update_code_heap_words(true);

  // Initialize all quotations
  data_root<array> quotations(instances(QUOTATION_TYPE), this);

  cell n_quots = array_capacity(quotations.untagged());
  for (cell i = 0; i < n_quots; i++) {
    data_root<quotation> quot(array_nth(quotations.untagged(), i), this);

    if (!quot->entry_point)
      quot->entry_point = lazy_jit_compile_entry_point();
  }

  special_objects[OBJ_STAGE2] = special_objects[OBJ_CANONICAL_TRUE];

  std::cout << "done" << std::endl;
}

void factor_vm::init_factor(vm_parameters* p) {

  // Kilobytes
  p->datastack_size = align_page(p->datastack_size << 10);
  p->retainstack_size = align_page(p->retainstack_size << 10);
  p->callstack_size = align_page(p->callstack_size << 10);
  p->callback_size = align_page(p->callback_size << 10);

  // Megabytes
  p->young_size <<= 20;
  p->aging_size <<= 20;
  p->tenured_size <<= 20;
  p->code_size <<= 20;

  // Disable GC during init as a sanity check
  gc_off = true;

  // OS-specific initialization
  early_init();

  p->executable_path = vm_executable_path();

  if (p->image_path == NULL) {
    if (embedded_image_p()) {
      p->embedded_image = true;
      p->image_path = safe_strdup(p->executable_path);
    } else
      p->image_path = default_image_path();
  }

  srand((unsigned int)nano_count());
  init_ffi();

  datastack_size = p->datastack_size;
  retainstack_size = p->retainstack_size;
  callstack_size = p->callstack_size;

  ctx = NULL;
  spare_ctx = new_context();

  callbacks = new callback_heap(p->callback_size, this);
  load_image(p);
#if defined(FACTOR_WASM)
  // Set up the initial Factor context for the interpreter path.
  init_context(spare_ctx);
#endif
  max_pic_size = (int)p->max_pic_size;
  special_objects[OBJ_CELL_SIZE] = tag_fixnum(sizeof(cell));
  special_objects[OBJ_ARGS] = false_object;
  special_objects[OBJ_EMBEDDED] = false_object;

#ifdef WINDOWS
#define NO_ASSOCIATED_STREAM -2
#define VALID_HANDLE(handle,mode) (_fileno (handle)!= NO_ASSOCIATED_STREAM ? handle : fopen ("nul",(mode)))
#else
#define VALID_HANDLE(handle,mode) (handle)
#endif

  cell aliens[][2] = {
    {OBJ_STDIN,           (cell)(VALID_HANDLE (stdin ,"r"))},
    {OBJ_STDOUT,          (cell)(VALID_HANDLE (stdout,"w"))},
    {OBJ_STDERR,          (cell)(VALID_HANDLE (stderr,"w"))},
    {OBJ_CPU,             (cell)FACTOR_CPU_STRING},
    {OBJ_EXECUTABLE,      (cell)safe_strdup(p->executable_path)},
    {OBJ_IMAGE,           (cell)safe_strdup(p->image_path)},
    {OBJ_OS,              (cell)FACTOR_OS_STRING},
    {OBJ_VM_COMPILE_TIME, (cell)FACTOR_COMPILE_TIME},
    {OBJ_VM_COMPILER,     (cell)FACTOR_COMPILER_VERSION},
    {OBJ_VM_GIT_LABEL,    (cell)FACTOR_STRINGIZE(FACTOR_GIT_LABEL)},
    {OBJ_VM_VERSION,      (cell)FACTOR_STRINGIZE(FACTOR_VERSION)},
#if defined(WINDOWS)
    {WIN_EXCEPTION_HANDLER, (cell)&factor::exception_handler}
#endif
  };
  int n_items = sizeof(aliens) / sizeof(cell[2]);
  for (int n = 0; n < n_items; n++) {
    cell idx = aliens[n][0];
    special_objects[idx] = allot_alien(false_object, aliens[n][1]);
  }

  // We can GC now, unless explicitly disabled for debugging
  if (std::getenv("FACTOR_WASM_SKIP_GC_STARTUP") == nullptr) {
    gc_off = false;
  }

  if (!to_boolean(special_objects[OBJ_STAGE2]))
    prepare_boot_image();

  if (p->signals)
    init_signals();

  if (p->console)
    open_console();
}

// Allocates memory
void factor_vm::pass_args_to_factor(int argc, vm_char** argv) {
  growable_array args(this);

#if defined(FACTOR_WASM)
  // On WASM, normalize -resource-path to /work and copy strings into
  // Factor-managed byte-arrays to ensure they survive GC
  const std::string res_prefix = "-resource-path=";
  for (fixnum i = 0; i < argc; i++) {
    vm_char* arg = argv[i];
    std::string s = arg ? std::string(arg) : std::string();

    // Normalize empty/invalid resource paths
    if (s == "-resource-path" || s == res_prefix || s == res_prefix + "") {
      s = res_prefix + "/work";
    } else if (s.rfind(res_prefix, 0) == 0 && s.size() == res_prefix.size()) {
      s = res_prefix + "/work";
    }

    // Allocate byte-array in Factor heap (GC-safe)
    size_t len = s.size();
    byte_array* ba = allot_byte_array(len + 1);
    memcpy(ba->data<char>(), s.c_str(), len);
    ba->data<char>()[len] = '\0';
    args.add(allot_alien(false_object, (cell)ba->data<char>()));
  }
#else
  for (fixnum i = 0; i < argc; i++)
    args.add(allot_alien(false_object, (cell)argv[i]));
#endif

  args.trim();
  special_objects[OBJ_ARGS] = args.elements.value();
}

void factor_vm::stop_factor() {
  c_to_factor_toplevel(special_objects[OBJ_SHUTDOWN_QUOT]);
}

// Evaluate a Factor string using the interpreter. Returns a malloc'ed C string
// result or NULL on failure.
static char* eval_string_in_vm(factor_vm* vm, const char* string) {
#if defined(FACTOR_WASM)
  if (!string)
    return NULL;

  bool prev_gc_off = vm->gc_off;
  vm->gc_off = true;

  context* saved_ctx = vm->ctx;
  if (!vm->ctx) {
    vm->ctx = vm->spare_ctx;
    vm->ctx->reset();
    vm->init_context(vm->ctx);
  }
  context* ctx = vm->ctx;
  cell saved_ds = ctx->datastack;
  cell saved_rs = ctx->retainstack;
  cell saved_cs_top = ctx->callstack_top;
  cell saved_cs_bottom = ctx->callstack_bottom;

  if (wasm_trace_enabled()) {
    std::cout << "[wasm] eval_string_in_vm: depth=" << ctx->depth()
              << " string=\"" << string << "\"" << std::endl;
  }

  cell eval_word = find_word_by_name(vm, "eval>string");
  if (wasm_trace_enabled()) {
    if (eval_word == false_object) {
      std::cout << "[wasm] eval>string not found in word instances" << std::endl;
    } else {
      std::cout << "[wasm] eval>string found at 0x" << std::hex << eval_word
                << std::dec << std::endl;
    }
  }
  if (eval_word != false_object) {
    size_t len = strlen(string);
    data_root<factor::string> str_obj(vm->allot_string((cell)len, 0), vm);
    memcpy(str_obj->data(), string, len);
    data_root<array> elements(vm->allot_array(2, false_object), vm);
    vm->set_array_nth(elements.untagged(), 0, str_obj.value());
    vm->set_array_nth(elements.untagged(), 1, eval_word);
    data_root<quotation> quot(vm->allot<quotation>(sizeof(quotation)), vm);
    quot->array = elements.value();
    quot->cached_effect = false_object;
    quot->cache_counter = false_object;
    quot->entry_point = vm->lazy_jit_compile_entry_point();
#if defined(FACTOR_WASM)
    if (wasm_trace_enabled()) {
      std::cout << "[wasm] eval roots string=0x" << std::hex << str_obj.value()
                << " array=0x" << elements.value()
                << " quot=0x" << quot.value() << std::dec << std::endl;
      std::cout << "[wasm] data_roots size=" << vm->data_roots.size()
                << std::endl;
      size_t idx = 0;
      for (auto ptr : vm->data_roots) {
        std::cout << "  root[" << idx++ << "] handle=0x" << std::hex
                  << (cell)ptr << " value=0x" << *ptr << std::dec << std::endl;
      }
    }
#endif

    vm->c_to_factor_toplevel(quot.value());
    cell result_cell = ctx->pop();

    if (TAG(result_cell) == STRING_TYPE) {
      factor::string* res = untag<factor::string>(result_cell);
      size_t rlen = (size_t)untag_fixnum(res->length);
      char* out = (char*)malloc(rlen + 1);
      memcpy(out, res->data(), rlen);
      out[rlen] = '\0';
      ctx->datastack = saved_ds;
      ctx->retainstack = saved_rs;
      ctx->callstack_top = saved_cs_top;
      ctx->callstack_bottom = saved_cs_bottom;
      vm->ctx = saved_ctx;
      return out;
    }
  }
  ctx->datastack = saved_ds;
  ctx->retainstack = saved_rs;
  ctx->callstack_top = saved_cs_top;
  ctx->callstack_bottom = saved_cs_bottom;
  vm->ctx = saved_ctx;
  vm->gc_off = prev_gc_off;
#endif
  return NULL;
}

static char* wasm_eval_callback(char* string) {
#if defined(FACTOR_WASM)
  factor_vm* vm = current_vm();
  if (!vm)
    return NULL;
  return eval_string_in_vm(vm, string);
#else
  (void)string;
  return NULL;
#endif
}

char* factor_vm::factor_eval_string(char* string) {
#if defined(FACTOR_WASM)
  // If no callback was wired up in the image, fall back to calling the
  // Factor word eval>string directly.
  if (special_objects[OBJ_EVAL_CALLBACK] == false_object ||
      special_objects[OBJ_EVAL_CALLBACK] == 0) {
    return eval_string_in_vm(this, string);
  }
#endif
  void* func = alien_offset(special_objects[OBJ_EVAL_CALLBACK]);
  CODE_TO_FUNCTION_POINTER(func);
  return ((char * (*)(char*)) func)(string);
}

void factor_vm::factor_eval_free(char* result) { free(result); }

void factor_vm::factor_yield() {
  void* func = alien_offset(special_objects[OBJ_YIELD_CALLBACK]);
  CODE_TO_FUNCTION_POINTER(func);
  ((void(*)()) func)();
}

void factor_vm::factor_sleep(long us) {
  void* func = alien_offset(special_objects[OBJ_SLEEP_CALLBACK]);
  CODE_TO_FUNCTION_POINTER(func);
  ((void(*)(long)) func)(us);
}

static void install_wasm_eval_callback(factor_vm* vm) {
#if defined(FACTOR_WASM)
  if (vm->special_objects[OBJ_EVAL_CALLBACK] == false_object ||
      vm->special_objects[OBJ_EVAL_CALLBACK] == 0) {
    vm->special_objects[OBJ_EVAL_CALLBACK] =
        vm->allot_alien(false_object, (cell)&wasm_eval_callback);
  }
#endif
}

void factor_vm::start_standalone_factor(int argc, vm_char** argv) {
  vm_parameters p;
  if (wasm_trace_enabled())
    std::cout << "[wasm] init_from_args" << std::endl;
  p.init_from_args(argc, argv);
  {
    FILE* f = fopen("init-factor.log", "a");
    if (f) { fprintf(f, "start_standalone_factor after init_from_args\n"); fclose(f); }
  }
  if (wasm_trace_enabled())
    std::cout << "[wasm] init_factor" << std::endl;
  init_factor(&p);
  {
    FILE* f = fopen("init-factor.log", "a");
    if (f) { fprintf(f, "start_standalone_factor after init_factor\n"); fclose(f); }
  }
  if (wasm_trace_enabled())
    std::cout << "[wasm] pass_args_to_factor" << std::endl;
  pass_args_to_factor(argc, argv);
  {
    FILE* f = fopen("init-factor.log", "a");
    if (f) { fprintf(f, "start_standalone_factor after pass_args\n"); fclose(f); }
  }

  if (std::getenv("FACTOR_SKIP_STARTUP")) {
    FILE* f = fopen("init-factor.log", "a");
    if (f) { fprintf(f, "start_standalone_factor skipping startup\n"); fclose(f); }
    return;
  }

  install_wasm_eval_callback(this);

  if (p.fep)
    factorbug();

  if (wasm_trace_enabled()) {
    cell startup = special_objects[OBJ_STARTUP_QUOT];
    std::cout << "[wasm] startup quot tag=" << TAG(startup);
    if (TAG(startup) == QUOTATION_TYPE) {
      quotation* q = untag<quotation>(startup);
      cell len = array_capacity(untag<array>(q->array));
      std::cout << " quot-len=" << len;
    }
    std::cout << " raw=0x" << std::hex << startup << std::dec << std::endl;
    std::cout << "[wasm] argc=" << argc << " argv:";
    for (int i = 0; i < argc; i++) {
      std::cout << " \"" << argv[i] << "\"";
    }
    std::cout << std::endl;
  }

  if (wasm_trace_enabled())
    std::cout << "[wasm] c_to_factor_toplevel" << std::endl;
  {
    FILE* f = fopen("init-factor.log", "a");
    if (f) {
      cell startup = special_objects[OBJ_STARTUP_QUOT];
      if (!immediate_p(startup) && TAG(startup) == QUOTATION_TYPE) {
        quotation* q = untag<quotation>(startup);
        cell len = array_capacity(untag<array>(q->array));
        fprintf(f, "start_standalone_factor before c_to_factor_toplevel tag=quotation len=%lu raw=0x%lx\n",
                (unsigned long)len, (unsigned long)startup);
        // Dump a prefix of the startup quotation (and a shallow view of nested
        // quotations) to see what is running.
        auto dump = [&](auto&& self, cell obj, cell index, int depth) -> void {
          cell tag = TAG(obj);
          const int indent = depth * 2;
          if (immediate_p(obj)) {
            fprintf(f, "%*s[%lu] immediate type=%ld raw=0x%lx\n", indent, "",
                    (unsigned long)index, (long)tag, (unsigned long)obj);
            return;
          }
          const char* type = type_name(tag);
          fprintf(f, "%*s[%lu] type=%s raw=0x%lx", indent, "",
                  (unsigned long)index, type, (unsigned long)obj);
          if (tag == WORD_TYPE) {
            word* w = untag<word>(obj);
            cell name_cell = w->name;
            if (!immediate_p(name_cell) && TAG(name_cell) == STRING_TYPE) {
              string* s = untag<string>(name_cell);
              cell slen = string_capacity(s);
              std::string name;
              name.reserve((size_t)slen);
              uint8_t* data = reinterpret_cast<uint8_t*>(s + 1);
              for (cell j = 0; j < slen; j++)
                name.push_back((char)data[j]);
              fprintf(f, " word=\"%s\"", name.c_str());
            }
          } else if (tag == QUOTATION_TYPE) {
            quotation* sq = untag<quotation>(obj);
            array* sub = untag<array>(sq->array);
            cell sublen = array_capacity(sub);
            fprintf(f, " quotation-len=%lu", (unsigned long)sublen);
            if (depth < 2) {
              cell limit = sublen < 8 ? sublen : 8;
              fprintf(f, " nested-dump=%lu\n", (unsigned long)limit);
              for (cell k = 0; k < limit; k++)
                self(self, array_nth(sub, k), k, depth + 1);
              return;
            }
          }
          fprintf(f, "\n");
        };
        array* elements = untag<array>(q->array);
        cell to_dump = len < 200 ? len : 200;
        for (cell i = 0; i < to_dump; i++)
          dump(dump, array_nth(elements, i), i, 1);
      } else {
        fprintf(f, "start_standalone_factor before c_to_factor_toplevel tag=%ld raw=0x%lx\n",
                (long)TAG(startup), (unsigned long)startup);
      }
      fclose(f);
    }
  }
  if (std::getenv("FACTOR_OVERRIDE_STARTUP_QUOT")) {
    FILE* f = fopen("init-factor.log", "a");
    if (f) { fprintf(f, "start_standalone_factor overriding startup quotation\n"); fclose(f); }
    data_root<array> elems(tag<array>(allot_array(0, false_object)), this);
    quotation* q = allot<quotation>(sizeof(quotation));
    q->array = elems.value();
    q->cached_effect = false_object;
    q->cache_counter = false_object;
    q->entry_point = lazy_jit_compile_entry_point();
    special_objects[OBJ_STARTUP_QUOT] = tag<quotation>(q);
  }
  if (std::getenv("FACTOR_REPLACE_STARTUP")) {
    FILE* f = fopen("init-factor.log", "a");
    if (f) { fprintf(f, "replacing startup quotation, skipping call\n"); fclose(f); }
    return;
  }
  {
    FILE* f = fopen("init-factor.log", "a");
    if (f) { fprintf(f, "about to call c_to_factor_toplevel\n"); fclose(f); }
  }
  c_to_factor_toplevel(special_objects[OBJ_STARTUP_QUOT]);
  // Re-enable GC after startup if it was intentionally left off
  if (gc_off) {
    gc_off = false;
    FILE* f = fopen("init-factor.log", "a");
    if (f) { fprintf(f, "start_standalone_factor: re-enabled GC after startup\n"); fclose(f); }
  }
  {
    FILE* f = fopen("init-factor.log", "a");
    if (f) { fprintf(f, "start_standalone_factor after c_to_factor_toplevel\n"); fclose(f); }
  }
  if (wasm_trace_enabled()) {
    std::cout << "[wasm] after startup depth=" << (ctx ? ctx->depth() : -1)
              << std::endl;
  }

  // After startup, keep a clean context available for future calls such as
  // eval callbacks.
  ctx = spare_ctx;
  ctx->reset();
  init_context(ctx);

  // Simple -e=... support for wasm path: evaluate the given expression
  // after the startup quotation has run.
  std::string eval_expr;
  for (int i = 0; i < argc; i++) {
    const char* arg = argv[i];
    if (arg && strncmp(arg, "-e=", 3) == 0) {
      eval_expr = std::string(arg + 3);
      break;
    }
  }
  if (!eval_expr.empty()) {
    if (wasm_trace_enabled()) {
      std::cout << "[wasm] evaluating -e expression: " << eval_expr
                << " (eval_cb="
                << ((special_objects[OBJ_EVAL_CALLBACK] != false_object &&
                     special_objects[OBJ_EVAL_CALLBACK] != 0)
                        ? "yes" : "no")
                << ")" << std::endl;
    }
    char* result = factor_eval_string(eval_expr.data());
    if (result)
      factor_eval_free(result);
  }
}

factor_vm* new_factor_vm() {
  THREADHANDLE thread = thread_id();
  factor_vm* newvm = new factor_vm(thread);
  register_vm_with_thread(newvm);
  thread_vms[thread] = newvm;

  return newvm;
}

VM_C_API void start_standalone_factor(int argc, vm_char** argv) {
  factor_vm* newvm = new_factor_vm();
  newvm->start_standalone_factor(argc, argv);
  delete newvm;
}

}
