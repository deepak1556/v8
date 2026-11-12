// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_BASE_TYPED_SLOT_SET_H_
#define V8_HEAP_BASE_TYPED_SLOT_SET_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "src/base/atomic-utils.h"
#include "src/base/bit-field.h"
#include "src/base/logging.h"
#include "src/base/macros.h"
#include "src/heap/base/basic-slot-set.h"

namespace heap::base {

// Data structure for maintaining a list of typed slots in a page.
// Shared between the V8 JavaScript heap (`v8::internal::TypedSlots`) and
// `cppgc` object compaction (`cppgc::internal::CompactionTypedSlotSet`).
template <typename SlotTypeEnum>
class BasicTypedSlots {
 public:
  static constexpr int kMaxOffset = 1 << 29;

  BasicTypedSlots() = default;
  virtual ~BasicTypedSlots() {
    Chunk* chunk = head_;
    while (chunk != nullptr) {
      Chunk* next = chunk->next;
      delete chunk;
      chunk = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
  }

  void Insert(SlotTypeEnum type, uint32_t offset) {
    static_assert(static_cast<uint8_t>(SlotTypeEnum::kLast) < 8);
    TypedSlot slot = {TypeField::encode(type) |
                      OffsetField::encode(static_cast<int>(offset))};
    Chunk* chunk = EnsureChunk();
    DCHECK_LT(chunk->buffer.size(), chunk->buffer.capacity());
    chunk->buffer.push_back(slot);
  }

  void Merge(BasicTypedSlots* other) {
    if (other->head_ == nullptr) {
      return;
    }
    if (head_ == nullptr) {
      head_ = other->head_;
      tail_ = other->tail_;
    } else {
      tail_->next = other->head_;
      tail_ = other->tail_;
    }
    other->head_ = nullptr;
    other->tail_ = nullptr;
  }

 protected:
  using OffsetField = v8::base::BitField<int, 0, 29>;
  using TypeField = v8::base::BitField<SlotTypeEnum, 29, 3>;
  struct TypedSlot {
    uint32_t type_and_offset;
  };
  struct Chunk {
    Chunk* next;
    std::vector<TypedSlot> buffer;
  };
  static constexpr size_t kInitialBufferSize = 100;
  static constexpr size_t kMaxBufferSize = 16 * 1024;
  static size_t NextCapacity(size_t capacity) {
    return std::min({kMaxBufferSize, capacity * 2});
  }

  Chunk* EnsureChunk() {
    if (!head_) {
      head_ = tail_ = NewChunk(nullptr, kInitialBufferSize);
    }
    if (head_->buffer.size() == head_->buffer.capacity()) {
      head_ = NewChunk(head_, NextCapacity(head_->buffer.capacity()));
    }
    return head_;
  }

  Chunk* NewChunk(Chunk* next, size_t capacity) {
    Chunk* chunk = new Chunk;
    chunk->next = next;
    chunk->buffer.reserve(capacity);
    DCHECK_EQ(chunk->buffer.capacity(), capacity);
    return chunk;
  }

  Chunk* head_ = nullptr;
  Chunk* tail_ = nullptr;
};

// Multiset of per-page typed slots supporting concurrent clearing of invalid
// ranges and pause-time slot migration when an evacuation-candidate object
// moves to a destination page.
template <typename SlotTypeEnum>
class BasicTypedSlotSet : public BasicTypedSlots<SlotTypeEnum> {
  using Base = BasicTypedSlots<SlotTypeEnum>;
  using typename Base::Chunk;
  using typename Base::OffsetField;
  using typename Base::TypedSlot;
  using typename Base::TypeField;

 public:
  using Address = uintptr_t;
  using FreeRangesMap = std::map<uint32_t, uint32_t>;

  enum IterationMode { FREE_EMPTY_CHUNKS, KEEP_EMPTY_CHUNKS };

  explicit BasicTypedSlotSet(Address page_start) : page_start_(page_start) {}

  template <typename Callback>
  int Iterate(Callback callback, IterationMode mode) {
    static_assert(static_cast<uint8_t>(SlotTypeEnum::kLast) < 8);
    Chunk* chunk = this->head_;
    Chunk* previous = nullptr;
    int new_count = 0;
    while (chunk != nullptr) {
      bool empty = true;
      for (TypedSlot& slot : chunk->buffer) {
        SlotTypeEnum type = TypeField::decode(slot.type_and_offset);
        if (type != SlotTypeEnum::kCleared) {
          uint32_t offset =
              static_cast<uint32_t>(OffsetField::decode(slot.type_and_offset));
          Address addr = page_start_ + offset;
          if (callback(type, addr) == KEEP_SLOT) {
            new_count++;
            empty = false;
          } else {
            slot = ClearedTypedSlot();
          }
        }
      }
      Chunk* next = chunk->next;
      if (mode == FREE_EMPTY_CHUNKS && empty) {
        if (previous) {
          StoreNext(previous, next);
          if (chunk == this->tail_) {
            this->tail_ = previous;
          }
        } else {
          StoreHead(next);
          if (chunk == this->tail_) {
            this->tail_ = nullptr;
          }
        }
        delete chunk;
      } else {
        previous = chunk;
      }
      chunk = next;
    }
    return new_count;
  }

  // Migrates any recorded typed slots in `[start_offset, end_offset)` from this
  // source page to `dst_set` at `(slot_offset - start_offset) + dst_start`,
  // clearing the migrated slots on the source page.
  void MigrateSlotsInRange(uint32_t start_offset, uint32_t end_offset,
                           BasicTypedSlotSet* dst_set,
                           uint32_t dst_start_offset) {
    DCHECK_LE(start_offset, end_offset);
    for (Chunk* chunk = this->head_; chunk != nullptr; chunk = chunk->next) {
      for (TypedSlot& slot : chunk->buffer) {
        const SlotTypeEnum type = TypeField::decode(slot.type_and_offset);
        if (type == SlotTypeEnum::kCleared) continue;
        const uint32_t offset =
            static_cast<uint32_t>(OffsetField::decode(slot.type_and_offset));
        if (offset >= start_offset && offset < end_offset) {
          const uint32_t new_offset =
              dst_start_offset + (offset - start_offset);
          slot = ClearedTypedSlot();
          dst_set->Insert(type, new_offset);
        }
      }
    }
  }

  void ClearInvalidSlots(const FreeRangesMap& invalid_ranges) {
    IterateSlotsInRanges([](TypedSlot* slot) { *slot = ClearedTypedSlot(); },
                         invalid_ranges);
  }

  void AssertNoInvalidSlots(const FreeRangesMap& invalid_ranges) {
    IterateSlotsInRanges(
        [](TypedSlot* slot) {
          CHECK_WITH_MSG(false, "No slot in ranges expected.");
        },
        invalid_ranges);
  }

 private:
  template <typename Callback>
  void IterateSlotsInRanges(Callback callback, const FreeRangesMap& ranges) {
    if (ranges.empty()) return;

    Chunk* chunk = LoadHead();
    while (chunk != nullptr) {
      for (TypedSlot& slot : chunk->buffer) {
        SlotTypeEnum type = TypeField::decode(slot.type_and_offset);
        if (type == SlotTypeEnum::kCleared) continue;
        uint32_t offset =
            static_cast<uint32_t>(OffsetField::decode(slot.type_and_offset));
        typename FreeRangesMap::const_iterator upper_bound =
            ranges.upper_bound(offset);
        if (upper_bound == ranges.begin()) continue;
        --upper_bound;
        DCHECK_LE(upper_bound->first, offset);
        if (upper_bound->second > offset) {
          callback(&slot);
        }
      }
      chunk = LoadNext(chunk);
    }
  }

  Chunk* LoadNext(Chunk* chunk) {
    return v8::base::AsAtomicPointer::Relaxed_Load(&chunk->next);
  }
  void StoreNext(Chunk* chunk, Chunk* next) {
    v8::base::AsAtomicPointer::Relaxed_Store(&chunk->next, next);
  }
  Chunk* LoadHead() {
    return v8::base::AsAtomicPointer::Relaxed_Load(&this->head_);
  }
  void StoreHead(Chunk* chunk) {
    v8::base::AsAtomicPointer::Relaxed_Store(&this->head_, chunk);
  }
  static TypedSlot ClearedTypedSlot() {
    return TypedSlot{TypeField::encode(SlotTypeEnum::kCleared) |
                     OffsetField::encode(0)};
  }

  Address page_start_;
};

}  // namespace heap::base

#endif  // V8_HEAP_BASE_TYPED_SLOT_SET_H_
