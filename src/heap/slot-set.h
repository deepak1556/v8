// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_SLOT_SET_H_
#define V8_HEAP_SLOT_SET_H_

#include <map>
#include <memory>
#include <stack>
#include <vector>

#include "src/base/bit-field.h"
#include "src/base/logging.h"
#include "src/heap/base/basic-slot-set.h"
#include "src/heap/base/typed-slot-set.h"
#include "src/objects/compressed-slots.h"
#include "src/objects/slots.h"
#include "src/utils/allocation.h"
#include "src/utils/utils.h"
#include "testing/gtest/include/gtest/gtest_prod.h"  // nogncheck

namespace v8 {
namespace internal {

using ::heap::base::KEEP_SLOT;
using ::heap::base::REMOVE_SLOT;
using ::heap::base::SlotCallbackResult;

// Possibly empty buckets (buckets that do not contain any slots) are discovered
// by the scavenger. Buckets might become non-empty when promoting objects later
// or in another thread, so all those buckets need to be revisited.
// Track possibly empty buckets within a SlotSet in this data structure. The
// class contains a word-sized bitmap, in case more bits are needed the bitmap
// is replaced with a pointer to a malloc-allocated bitmap.
class PossiblyEmptyBuckets {
 public:
  PossiblyEmptyBuckets() = default;
  PossiblyEmptyBuckets(PossiblyEmptyBuckets&& other) V8_NOEXCEPT
      : bitmap_(other.bitmap_) {
    other.bitmap_ = kNullAddress;
  }

  ~PossiblyEmptyBuckets() { Release(); }

  PossiblyEmptyBuckets(const PossiblyEmptyBuckets&) = delete;
  PossiblyEmptyBuckets& operator=(const PossiblyEmptyBuckets&) = delete;

  void Release() {
    if (IsAllocated()) {
      AlignedFree(BitmapArray());
    }
    bitmap_ = kNullAddress;
    DCHECK(!IsAllocated());
  }

  void Insert(size_t bucket_index, size_t buckets) {
    if (IsAllocated()) {
      InsertAllocated(bucket_index);
    } else if (bucket_index + 1 < kBitsPerWord) {
      bitmap_ |= static_cast<uintptr_t>(1) << (bucket_index + 1);
    } else {
      Allocate(buckets);
      InsertAllocated(bucket_index);
    }
  }

  bool Contains(size_t bucket_index) {
    if (IsAllocated()) {
      size_t word_idx = bucket_index / kBitsPerWord;
      uintptr_t* word = BitmapArray() + word_idx;
      return *word &
             (static_cast<uintptr_t>(1) << (bucket_index % kBitsPerWord));
    } else if (bucket_index + 1 < kBitsPerWord) {
      return bitmap_ & (static_cast<uintptr_t>(1) << (bucket_index + 1));
    } else {
      return false;
    }
  }

  bool IsEmpty() const { return bitmap_ == kNullAddress; }

 private:
  static constexpr Address kPointerTag = 1;
  static constexpr int kWordSize = sizeof(uintptr_t);
  static constexpr int kBitsPerWord = kWordSize * kBitsPerByte;

  bool IsAllocated() { return bitmap_ & kPointerTag; }

  void Allocate(size_t buckets) {
    DCHECK(!IsAllocated());
    size_t words = WordsForBuckets(buckets);
    uintptr_t* ptr = reinterpret_cast<uintptr_t*>(
        AlignedAllocWithRetry(words * kWordSize, kSystemPointerSize));
    ptr[0] = bitmap_ >> 1;

    for (size_t word_idx = 1; word_idx < words; word_idx++) {
      ptr[word_idx] = 0;
    }
    bitmap_ = reinterpret_cast<Address>(ptr) + kPointerTag;
    DCHECK(IsAllocated());
  }

  void InsertAllocated(size_t bucket_index) {
    DCHECK(IsAllocated());
    size_t word_idx = bucket_index / kBitsPerWord;
    uintptr_t* word = BitmapArray() + word_idx;
    *word |= static_cast<uintptr_t>(1) << (bucket_index % kBitsPerWord);
  }

  static size_t WordsForBuckets(size_t buckets) {
    return (buckets + kBitsPerWord - 1) / kBitsPerWord;
  }

  uintptr_t* BitmapArray() {
    DCHECK(IsAllocated());
    return reinterpret_cast<uintptr_t*>(bitmap_ & ~kPointerTag);
  }

  Address bitmap_ = kNullAddress;

  FRIEND_TEST(PossiblyEmptyBucketsTest, WordsForBuckets);
};

static_assert(std::is_standard_layout_v<PossiblyEmptyBuckets>);
static_assert(sizeof(PossiblyEmptyBuckets) == kSystemPointerSize);

class SlotSet final : public ::heap::base::BasicSlotSet<kTaggedSize> {
  using BasicSlotSet = ::heap::base::BasicSlotSet<kTaggedSize>;

 public:
  static const int kBucketsRegularPage =
      (1 << kPageSizeBits) / kTaggedSize / kCellsPerBucket / kBitsPerCell;

  static SlotSet* Allocate(size_t buckets) {
    return static_cast<SlotSet*>(BasicSlotSet::Allocate(buckets));
  }

  template <v8::internal::AccessMode access_mode>
  static constexpr BasicSlotSet::AccessMode ConvertAccessMode() {
    switch (access_mode) {
      case v8::internal::AccessMode::ATOMIC:
        return BasicSlotSet::AccessMode::ATOMIC;
      case v8::internal::AccessMode::NON_ATOMIC:
        return BasicSlotSet::AccessMode::NON_ATOMIC;
    }
    UNREACHABLE();
  }

  // Similar to BasicSlotSet::Iterate() but Callback takes the parameter of type
  // MaybeObjectSlot.
  template <
      v8::internal::AccessMode access_mode = v8::internal::AccessMode::ATOMIC,
      typename Callback>
  size_t Iterate(Address chunk_start, size_t start_bucket, size_t end_bucket,
                 Callback callback, EmptyBucketMode mode) {
    return BasicSlotSet::Iterate<ConvertAccessMode<access_mode>()>(
        chunk_start, start_bucket, end_bucket,
        [&callback](Address slot) { return callback(MaybeObjectSlot(slot)); },
        [this, mode](size_t bucket_index) {
          if (mode == EmptyBucketMode::FREE_EMPTY_BUCKETS) {
            ReleaseBucket(bucket_index);
          }
        });
  }

  // Similar to SlotSet::Iterate() but marks potentially empty buckets
  // internally. Stores true in empty_bucket_found in case a potentially empty
  // bucket was found. Assumes that the possibly empty-array was already cleared
  // by CheckPossiblyEmptyBuckets.
  template <typename Callback>
  size_t IterateAndTrackEmptyBuckets(
      Address chunk_start, size_t start_bucket, size_t end_bucket,
      Callback callback, PossiblyEmptyBuckets* possibly_empty_buckets) {
    return BasicSlotSet::Iterate(
        chunk_start, start_bucket, end_bucket,
        [&callback](Address slot) { return callback(MaybeObjectSlot(slot)); },
        [possibly_empty_buckets, end_bucket](size_t bucket_index) {
          possibly_empty_buckets->Insert(bucket_index, end_bucket);
        });
  }

  // Check whether possibly empty buckets are really empty. Empty buckets are
  // freed and the possibly empty state is cleared for all buckets.
  bool CheckPossiblyEmptyBuckets(size_t buckets,
                                 PossiblyEmptyBuckets* possibly_empty_buckets) {
    bool empty = true;
    for (size_t bucket_index = 0; bucket_index < buckets; bucket_index++) {
      Bucket* bucket = LoadBucket<AccessMode::NON_ATOMIC>(bucket_index);
      if (bucket) {
        if (possibly_empty_buckets->Contains(bucket_index)) {
          if (bucket->IsEmpty()) {
            ReleaseBucket<AccessMode::NON_ATOMIC>(bucket_index);
          } else {
            empty = false;
          }
        } else {
          empty = false;
        }
      } else {
        // Unfortunately we cannot DCHECK here that the corresponding bit in
        // possibly_empty_buckets is not set. After scavenge, the
        // MergeOldToNewRememberedSets operation might remove a recorded bucket.
      }
    }

    possibly_empty_buckets->Release();

    return empty;
  }

  void Merge(SlotSet* other, size_t buckets) {
    for (size_t bucket_index = 0; bucket_index < buckets; bucket_index++) {
      Bucket* other_bucket =
          other->LoadBucket<AccessMode::NON_ATOMIC>(bucket_index);
      if (!other_bucket) continue;
      Bucket* bucket = LoadBucket<AccessMode::NON_ATOMIC>(bucket_index);
      if (bucket == nullptr) {
        other->StoreBucket<AccessMode::NON_ATOMIC>(bucket_index, nullptr);
        StoreBucket<AccessMode::NON_ATOMIC>(bucket_index, other_bucket);
      } else {
        for (int cell_index = 0; cell_index < kCellsPerBucket; cell_index++) {
          bucket->SetCellBits<AccessMode::NON_ATOMIC>(
              cell_index,
              other_bucket->LoadCell<AccessMode::NON_ATOMIC>(cell_index));
        }
      }
    }
  }
};

static_assert(std::is_standard_layout_v<SlotSet>);
static_assert(std::is_standard_layout_v<SlotSet::Bucket>);

enum class SlotType : uint8_t {
  // Full pointer sized slot storing an object start address.
  // RelocInfo::target_object/RelocInfo::set_target_object methods are used for
  // accessing. Used when pointer is stored in the instruction stream.
  kEmbeddedObjectFull,

  // Tagged sized slot storing an object start address.
  // RelocInfo::target_object/RelocInfo::set_target_object methods are used for
  // accessing. Used when pointer is stored in the instruction stream.
  kEmbeddedObjectCompressed,

  // Full pointer sized slot storing instruction start of Code object.
  // RelocInfo::target_address/RelocInfo::set_target_address methods are used
  // for accessing. Used when pointer is stored in the instruction stream.
  kCodeEntry,

  // Raw full pointer sized slot. Slot is accessed directly. Used when pointer
  // is stored in constant pool.
  kConstPoolEmbeddedObjectFull,

  // Raw tagged sized slot. Slot is accessed directly. Used when pointer is
  // stored in constant pool.
  kConstPoolEmbeddedObjectCompressed,

  // Raw full pointer sized slot storing instruction start of Code object. Slot
  // is accessed directly. Used when pointer is stored in constant pool.
  kConstPoolCodeEntry,

  // Slot got cleared but has not been removed from the slot set.
  kCleared,

  kLast = kCleared
};

// Data structure for maintaining a list of typed slots in a page.
// Shared implementation lives in `::heap::base::BasicTypedSlots` /
// `::heap::base::BasicTypedSlotSet` so both the JS heap and `cppgc` object
// compaction use the same chunked typed-slot storage.
class V8_EXPORT_PRIVATE TypedSlots
    : public ::heap::base::BasicTypedSlots<SlotType> {
 public:
  TypedSlots() = default;
  ~TypedSlots() override;
};

class V8_EXPORT_PRIVATE TypedSlotSet
    : public ::heap::base::BasicTypedSlotSet<SlotType> {
 public:
  explicit TypedSlotSet(Address page_start)
      : ::heap::base::BasicTypedSlotSet<SlotType>(page_start) {}
};

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_SLOT_SET_H_
