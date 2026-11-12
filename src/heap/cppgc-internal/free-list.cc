// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/cppgc-internal/free-list.h"

#include <algorithm>
#include <bit>

#include "include/cppgc/internal/logging.h"
#include "src/base/bits.h"
#include "src/base/sanitizer/asan.h"
#include "src/heap/cppgc-internal/globals.h"
#include "src/heap/cppgc-internal/heap-object-header.h"

namespace cppgc {
namespace internal {

namespace {
uint32_t BucketIndexForSize(uint32_t size) {
  return v8::base::bits::WhichPowerOfTwo(std::bit_floor(size));
}
}  // namespace

class FreeList::Entry : public HeapObjectHeader {
 public:
  static Entry& CreateAt(void* memory, size_t size) {
    // Make sure the freelist header is writable. SET_MEMORY_ACCESSIBLE is not
    // needed as we write the whole payload of Entry.
    ASAN_UNPOISON_MEMORY_REGION(memory, sizeof(Entry));
    return *new (memory) Entry(size);
  }

  Entry* Next() const { return next_; }
  Entry** NextSlot() { return &next_; }
  void SetNext(Entry* next) { next_ = next; }

  void Link(Entry** previous_next) {
    next_ = *previous_next;
    *previous_next = this;
  }
  void Unlink(Entry** previous_next) {
    *previous_next = next_;
    next_ = nullptr;
  }

 private:
  explicit Entry(size_t size) : HeapObjectHeader(size, kFreeListGCInfoIndex) {
    static_assert(sizeof(Entry) == kFreeListEntrySize, "Sizes must match");
  }

  Entry* next_ = nullptr;
};

FreeList::FreeList() { Clear(); }

FreeList::FreeList(FreeList&& other) V8_NOEXCEPT
    : free_list_heads_(std::move(other.free_list_heads_)),
      free_list_tails_(std::move(other.free_list_tails_)),
      category_cache_(other.category_cache_),
      biggest_free_list_index_(std::move(other.biggest_free_list_index_)) {
  other.Clear();
}

FreeList& FreeList::operator=(FreeList&& other) V8_NOEXCEPT {
  Clear();
  Append(std::move(other));
  DCHECK(other.IsEmpty());
  return *this;
}

void FreeList::Add(FreeList::Block block) { AddReturningUnusedBounds(block); }

std::pair<Address, Address> FreeList::AddReturningUnusedBounds(Block block) {
  const size_t size = block.size;
  DCHECK_GT(kPageSize, size);
  DCHECK_LE(sizeof(HeapObjectHeader), size);

  if (size < sizeof(Entry)) {
    // Create wasted entry. This can happen when an almost emptied linear
    // allocation buffer is returned to the freelist.
    // This could be SET_MEMORY_ACCESSIBLE. Since there's no payload, the next
    // operating overwrites the memory completely, and we can thus avoid
    // zeroing it out.
    auto& filler = Filler::CreateAt(block.address, size);
    USE(filler);
    DCHECK_EQ(reinterpret_cast<Address>(block.address) + size,
              filler.ObjectEnd());
    DCHECK_EQ(reinterpret_cast<Address>(&filler + 1), filler.ObjectEnd());
    return {reinterpret_cast<Address>(&filler + 1),
            reinterpret_cast<Address>(&filler + 1)};
  }

  Entry& entry = Entry::CreateAt(block.address, size);
  const size_t index = BucketIndexForSize(static_cast<uint32_t>(size));
  const bool was_empty = (free_list_heads_[index] == nullptr);
  entry.Link(&free_list_heads_[index]);
  if (was_empty) {
    category_cache_.UpdateAfterAddition(static_cast<int>(index));
  }
  biggest_free_list_index_ = std::max(biggest_free_list_index_, index);
  if (!entry.Next()) {
    free_list_tails_[index] = &entry;
  }
  DCHECK_EQ(entry.ObjectEnd(), reinterpret_cast<Address>(&entry) + size);
  return {reinterpret_cast<Address>(&entry + 1),
          reinterpret_cast<Address>(&entry) + size};
}

void FreeList::Append(FreeList&& other) {
  DCHECK_NE(this, &other);
#if DEBUG
  const size_t expected_size = Size() + other.Size();
#endif
  // Newly created entries get added to the head.
  for (size_t index = 0; index < free_list_tails_.size(); ++index) {
    Entry* other_tail = other.free_list_tails_[index];
    Entry*& this_head = this->free_list_heads_[index];
    if (other_tail) {
      other_tail->SetNext(this_head);
      if (!this_head) {
        this->free_list_tails_[index] = other_tail;
        category_cache_.UpdateAfterAddition(static_cast<int>(index));
      }
      this_head = other.free_list_heads_[index];
      other.free_list_heads_[index] = nullptr;
      other.free_list_tails_[index] = nullptr;
    }
  }

  other.category_cache_.Reset();
  biggest_free_list_index_ =
      std::max(biggest_free_list_index_, other.biggest_free_list_index_);
  other.biggest_free_list_index_ = 0;
#if DEBUG
  category_cache_.CheckIntegrity(
      static_cast<int>(kPageSizeLog2) - 1,
      [this](int cat) { return free_list_heads_[cat] == nullptr; });
  DCHECK_EQ(expected_size, Size());
#endif
  DCHECK(other.IsEmpty());
}

void FreeList::AppendToTail(FreeList&& other) {
  DCHECK_NE(this, &other);
#if DEBUG
  const size_t expected_size = Size() + other.Size();
#endif
  for (size_t index = 0; index < free_list_tails_.size(); ++index) {
    Entry* other_head = other.free_list_heads_[index];
    if (other_head) {
      Entry*& this_tail = this->free_list_tails_[index];
      if (this_tail) {
        this_tail->SetNext(other_head);
      } else {
        DCHECK_NULL(this->free_list_heads_[index]);
        this->free_list_heads_[index] = other_head;
        category_cache_.UpdateAfterAddition(static_cast<int>(index));
      }
      this_tail = other.free_list_tails_[index];
      other.free_list_heads_[index] = nullptr;
      other.free_list_tails_[index] = nullptr;
    }
  }

  other.category_cache_.Reset();
  biggest_free_list_index_ =
      std::max(biggest_free_list_index_, other.biggest_free_list_index_);
  other.biggest_free_list_index_ = 0;
#if DEBUG
  category_cache_.CheckIntegrity(
      static_cast<int>(kPageSizeLog2) - 1,
      [this](int cat) { return free_list_heads_[cat] == nullptr; });
  DCHECK_EQ(expected_size, Size());
#endif
  DCHECK(other.IsEmpty());
}

FreeList::Block FreeList::Allocate(size_t allocation_size) {
  // Mirror V8 JS's `FreeListManyCachedFastPathBase`: jump directly to non-empty
  // medium LAB categories (>= 2 KiB, bucket 11) via `FreeListCategoryCache`
  // before falling back to smaller precise categories or larger nearly-empty
  // page blocks.
  static constexpr int kMinLabBucket = 11;
  static constexpr int kNumBuckets = static_cast<int>(kPageSizeLog2);
  const uint32_t clamped_size =
      static_cast<uint32_t>(std::max(allocation_size, sizeof(Entry)));
  const int min_bucket = static_cast<int>(BucketIndexForSize(clamped_size));

  const auto try_pop_bucket = [this, allocation_size](int index) -> Block {
    DCHECK(IsConsistent(static_cast<size_t>(index)));
    Entry* entry = free_list_heads_[index];
    if (!entry || entry->AllocatedSize() < allocation_size) {
      return {nullptr, 0u};
    }
    if (!entry->Next()) {
      DCHECK_EQ(entry, free_list_tails_[index]);
      free_list_tails_[index] = nullptr;
      category_cache_.UpdateAfterRemoval(index);
    }
    entry->Unlink(&free_list_heads_[index]);
    if (static_cast<size_t>(index) == biggest_free_list_index_) {
      while (biggest_free_list_index_ > 0 &&
             !free_list_heads_[biggest_free_list_index_]) {
        --biggest_free_list_index_;
      }
    }
    return {entry, entry->AllocatedSize()};
  };

  const int fast_start = std::max(min_bucket, kMinLabBucket);
  for (int index = category_cache_.FirstNonEmpty(fast_start);
       index < kNumBuckets; index = category_cache_.FirstNonEmpty(index + 1)) {
    if (Block block = try_pop_bucket(index); block.address) {
      return block;
    }
  }

  if (min_bucket < kMinLabBucket) {
    // Prefer the largest non-empty bucket below `kMinLabBucket` down to
    // `min_bucket`, skipping empty ranges via `category_cache_`.
    int candidate = -1;
    for (int index = category_cache_.FirstNonEmpty(min_bucket);
         index < kMinLabBucket;
         index = category_cache_.FirstNonEmpty(index + 1)) {
      if (free_list_heads_[index]->AllocatedSize() >= allocation_size) {
        candidate = index;
      }
    }
    if (candidate >= 0) {
      return try_pop_bucket(candidate);
    }
  }

  return {nullptr, 0u};
}

void FreeList::Clear() {
  std::fill(free_list_heads_.begin(), free_list_heads_.end(), nullptr);
  std::fill(free_list_tails_.begin(), free_list_tails_.end(), nullptr);
  category_cache_.Reset();
  biggest_free_list_index_ = 0;
}

size_t FreeList::Size() const {
  size_t size = 0;
  for (auto* entry : free_list_heads_) {
    while (entry) {
      size += entry->AllocatedSize();
      entry = entry->Next();
    }
  }
  return size;
}

bool FreeList::IsEmpty() const {
  return std::all_of(free_list_heads_.cbegin(), free_list_heads_.cend(),
                     [](const auto* entry) { return !entry; });
}

bool FreeList::ContainsForTesting(Block block) const {
  for (Entry* list : free_list_heads_) {
    for (Entry* entry = list; entry; entry = entry->Next()) {
      if (entry <= block.address &&
          (reinterpret_cast<Address>(block.address) + block.size <=
           reinterpret_cast<Address>(entry) + entry->AllocatedSize())) {
        return true;
      }
    }
  }
  return false;
}

bool FreeList::IsConsistent(size_t index) const {
  // Check that freelist head and tail pointers are consistent, i.e.
  // - either both are nulls (no entries in the bucket);
  // - or both are non-nulls and the tail points to the end.
  return (!free_list_heads_[index] && !free_list_tails_[index]) ||
         (free_list_heads_[index] && free_list_tails_[index] &&
          !free_list_tails_[index]->Next());
}

void FreeList::CollectStatistics(
    HeapStatistics::FreeListStatistics& free_list_stats) {
  std::vector<size_t>& bucket_size = free_list_stats.bucket_size;
  std::vector<size_t>& free_count = free_list_stats.free_count;
  std::vector<size_t>& free_size = free_list_stats.free_size;
  DCHECK(bucket_size.empty());
  DCHECK(free_count.empty());
  DCHECK(free_size.empty());
  for (size_t i = 0; i < kPageSizeLog2; ++i) {
    size_t entry_count = 0;
    size_t entry_size = 0;
    for (Entry* entry = free_list_heads_[i]; entry; entry = entry->Next()) {
      ++entry_count;
      entry_size += entry->AllocatedSize();
    }
    bucket_size.push_back(static_cast<size_t>(1) << i);
    free_count.push_back(entry_count);
    free_size.push_back(entry_size);
  }
}

}  // namespace internal
}  // namespace cppgc
