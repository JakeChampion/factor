namespace factor {

// Interpreter-only path; callstack grows down from the end of the segment.
#define CALLSTACK_BOTTOM(ctx) (ctx->callstack_seg->end)

inline static void flush_icache(cell start, cell len) {
  (void)start;
  (void)len;
}

// JIT is disabled on wasm; provide stub values to satisfy interfaces.
static const fixnum xt_tail_pic_offset = 0;
static const unsigned char call_opcode = 0;
static const unsigned char jmp_opcode = 0;

inline static unsigned char call_site_opcode(cell return_address) {
  (void)return_address;
  return 0;
}

inline static void check_call_site(cell return_address) { (void)return_address; }

inline static void* get_call_target(cell return_address) {
  (void)return_address;
  return NULL;
}

inline static void set_call_target(cell return_address, cell target) {
  (void)return_address;
  (void)target;
}

inline static bool tail_call_site_p(cell return_address) {
  (void)return_address;
  return false;
}

inline static unsigned int fpu_status(unsigned int status) {
  (void)status;
  return 0;
}

}
