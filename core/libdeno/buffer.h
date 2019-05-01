// Copyright 2018-2019 the Deno authors. All rights reserved. MIT license.
#ifndef BUFFER_H_
#define BUFFER_H_

// Cpplint bans the use of <mutex> because it duplicates functionality in
// chromium //base. However Deno doensn't use that, so suppress that lint.
#include <memory>
#include <mutex>  // NOLINT
#include <string>
#include <unordered_map>
#include <utility>

#include "third_party/v8/include/v8.h"
#include "third_party/v8/src/base/logging.h"

namespace deno {

template <typename T, typename S>
static inline T BitMove(S* source) {
  static_assert(sizeof(T) == sizeof(S),
                "Source and target must have the same size.");
  static const auto empty = T();
  T moved;
  memcpy(&moved, source, sizeof(T));
  memcpy(source, &empty, sizeof(T));
  return moved;
}

template <typename T>
static inline T BitMove(T* source) {
  return BitMove<T, T>(source);
}

class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
 public:
  static ArrayBufferAllocator& global() {
    static ArrayBufferAllocator global_instance;
    return global_instance;
  }

  void* Allocate(size_t length) override { return new uint8_t[length](); }

  void* AllocateUninitialized(size_t length) override {
    return new uint8_t[length];
  }

  void Free(void* data, size_t length) override { Unref(data); }

 private:
  friend class PinnedBuf;

  void Ref(void* data) {
    std::lock_guard<std::mutex> lock(ref_count_map_mutex_);
    // Note:
    //  - `unordered_map::insert(make_pair(key, value))` returns the existing
    //    item if the key, already exists in the map, otherwise it creates an
    //    new entry with `value`.
    //  - Buffers not in the map have an implicit reference count of one.
    auto entry = ref_count_map_.insert(std::make_pair(data, 1)).first;
    ++entry->second;
  }

  void Unref(void* data) {
    {
      std::lock_guard<std::mutex> lock(ref_count_map_mutex_);
      auto entry = ref_count_map_.find(data);
      if (entry == ref_count_map_.end()) {
        // Buffers not in the map have an implicit ref count of one. After
        // dereferencing there are no references left, so we delete the buffer.
      } else if (--entry->second == 0) {
        // The reference count went to zero, so erase the map entry and free the
        // buffer.
        ref_count_map_.erase(entry);
      } else {
        // After decreasing the reference count the buffer still has references
        // left, so we leave the pin in place.
        return;
      }
      delete[] reinterpret_cast<uint8_t*>(data);
    }
  }

 private:
  ArrayBufferAllocator() {}

  ~ArrayBufferAllocator() {
    // TODO(pisciaureus): Enable this check. It currently fails sometimes
    // because the compiler worker isolate never actually exits, so when the
    // process exits this isolate still holds on to some buffers.
    // CHECK(ref_count_map_.empty());
  }

  std::unordered_map<void*, size_t> ref_count_map_;
  std::mutex ref_count_map_mutex_;
};

class PinnedBuf {
  struct Unref {
    // This callback gets called from the Pin destructor.
    void operator()(void* ptr) { ArrayBufferAllocator::global().Unref(ptr); }
  };
  // The Pin is a unique (non-copyable) smart pointer which automatically
  // unrefs the referenced ArrayBuffer in its destructor.
  using Pin = std::unique_ptr<void, Unref>;

  uint8_t* data_ptr_;
  size_t data_len_;
  Pin pin_;

 public:
  // PinnedBuf::Raw is a POD struct with the same memory layout as the PinBuf
  // itself. It is used to move a PinnedBuf between C and Rust.
  struct Raw {
    uint8_t* data_ptr;
    size_t data_len;
    void* pin;
  };

  PinnedBuf() : data_ptr_(nullptr), data_len_(0), pin_() {}

  explicit PinnedBuf(v8::Local<v8::ArrayBufferView> view) {
    auto buf = view->Buffer()->GetContents().Data();
    ArrayBufferAllocator::global().Ref(buf);

    data_ptr_ = reinterpret_cast<uint8_t*>(buf) + view->ByteOffset();
    data_len_ = view->ByteLength();
    pin_ = Pin(buf);
  }

  // This constructor recreates a PinnedBuf that has previously been converted
  // to a PinnedBuf::Raw using the AsRaw() method. This is a move operation;
  // the Raw struct is emptied in the process.
  explicit PinnedBuf(Raw raw)
      : data_ptr_(BitMove(&raw.data_ptr)),
        data_len_(BitMove(&raw.data_len)),
        pin_(BitMove<Pin>(&raw.pin)) {}

  // The AsRaw() method converts the PinnedBuf to a PinnedBuf::Raw so it's
  // ownership can be moved to Rust. The source PinnedBuf is emptied in the
  // process, but the pinned ArrayBuffer is not dereferenced. In order to
  // not leak it, the raw struct must eventually be turned back into a PinnedBuf
  // using the constructor above.
  Raw AsRaw() {
    return Raw{.data_ptr = BitMove(&data_ptr_),
               .data_len = BitMove(&data_len_),
               .pin = BitMove<void*>(&pin_)};
  }
};

}  // namespace deno

#endif  // BUFFER_H_
