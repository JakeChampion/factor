namespace factor {

template <typename Type> struct data_root : public tagged<Type> {
  factor_vm* parent;

  void push() {
    parent->data_roots.push_back(&this->value_);
  }

  data_root(const data_root&) = delete;
  data_root& operator=(const data_root&) = delete;

  data_root(data_root&& other) noexcept
      : tagged<Type>(other.value_), parent(other.parent) {
    if (parent) {
      FACTOR_ASSERT(!parent->data_roots.empty());
      // data_roots is treated as a stack, so the moved-from root should be
      // the most recent entry.
      cell*& slot = parent->data_roots.back();
      FACTOR_ASSERT(slot == &other.value_);
      slot = &this->value_;
      other.parent = nullptr;
    }
  }

  data_root& operator=(data_root&&) = delete;

  data_root(cell value, factor_vm* parent)
      : tagged<Type>(value), parent(parent) {
    push();
#if defined(FACTOR_WASM)
    if (wasm_debug_enabled()) {
      std::cout << "[wasm] data_root push value=0x" << std::hex
                << this->value_ << " handle=0x" << (cell)&this->value_
                << std::dec << std::endl;
    }
#endif
  }

  data_root(Type* value, factor_vm* parent)
      : tagged<Type>(value), parent(parent) {
    FACTOR_ASSERT(value);
    push();
#if defined(FACTOR_WASM)
    if (wasm_debug_enabled()) {
      std::cout << "[wasm] data_root push value=0x" << std::hex
                << this->value_ << " handle=0x" << (cell)&this->value_
                << std::dec << std::endl;
    }
#endif
  }

  ~data_root() {
    if (!parent)
      return;
#if defined(FACTOR_WASM)
    if (parent->data_roots.empty()) {
      std::cout << "[wasm] data_root pop underflow handle=0x" << std::hex
                << (cell)&this->value_ << " value=0x" << this->value_
                << std::dec << std::endl;
      return;
    }
#endif
    FACTOR_ASSERT(!parent->data_roots.empty());
    FACTOR_ASSERT(parent->data_roots.back() == &this->value_);
    parent->data_roots.pop_back();
  }

  friend void swap(data_root<Type>& a, data_root<Type>& b) {
    cell tmp = a.value_;
    a.value_ = b.value_;
    b.value_ = tmp;
  }
};

}
