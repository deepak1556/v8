// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_CPPGC_INTERNAL_HEAP_PAGE_H_
#define V8_HEAP_CPPGC_INTERNAL_HEAP_PAGE_H_

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>

#include "include/cppgc/internal/base-page-handle.h"
#include "src/base/hashing.h"
#include "src/base/iterator.h"
#include "src/base/macros.h"
#include "src/heap/base/active-system-pages.h"
#include "src/heap/base/basic-slot-set.h"
#include "src/heap/base/typed-slot-set.h"
#include "src/heap/cppgc-internal/globals.h"
#include "src/heap/cppgc-internal/heap-config.h"
#include "src/heap/cppgc-internal/heap-object-header.h"
#include "src/heap/cppgc-internal/heap-space.h"
#include "src/heap/cppgc-internal/object-start-bitmap.h"

namespace cppgc {
namespace internal {

class BaseSpace;
class NormalPageSpace;
class LargePageSpace;
class HeapBase;
class PageBackend;

class SlotSet : public ::heap::base::BasicSlotSet<kSlotSize> {};

enum class CompactionSlotType : uint8_t {
  kCompressedMember = 0,
  kUncompressedMember = 1,
  kTaggedUncompressedMember = 2,
  kCleared = 3,
  kLast = kCleared,
};

class CompactionTypedSlotSet
    : public ::heap::base::BasicTypedSlotSet<CompactionSlotType> {
 public:
  explicit CompactionTypedSlotSet(uintptr_t page_start)
      : ::heap::base::BasicTypedSlotSet<CompactionSlotType>(page_start) {}
};

class V8_EXPORT_PRIVATE BasePage : public BasePageHandle {
 public:
  static inline BasePage* FromPayload(void*);
  static inline const BasePage* FromPayload(const void*);

  static BasePage* FromInnerAddress(const HeapBase*, void*);
  static const BasePage* FromInnerAddress(const HeapBase*, const void*);

  static void Destroy(BasePage*);

  BasePage(const BasePage&) = delete;
  BasePage& operator=(const BasePage&) = delete;

  HeapBase& heap() const;

  BaseSpace& space() const { return *space_; }

  bool is_large() const { return type_ == PageType::kLarge; }

  Address PayloadStart();
  ConstAddress PayloadStart() const;
  Address PayloadEnd();
  ConstAddress PayloadEnd() const;

  // Size of the payload with the page header.
  size_t AllocatedSize() const;

  // Returns the size of live objects on the page at the last GC.
  // The counter is update after sweeping.
  size_t AllocatedBytesAtLastGC() const;

  // |address| must refer to real object.
  // These methods must run on the main thread as they may fix up object start
  // bits.
  template <AccessMode = AccessMode::kNonAtomic>
  HeapObjectHeader& ObjectHeaderFromInnerAddress(void* address) const;
  template <AccessMode = AccessMode::kNonAtomic>
  const HeapObjectHeader& ObjectHeaderFromInnerAddress(
      const void* address) const;
  // These methods assume that the object start bit exists for the object and
  // don't perform any fixups.
  template <AccessMode = AccessMode::kNonAtomic>
  HeapObjectHeader& ObjectHeaderFromInnerAddressConcurrent(void* address) const;
  template <AccessMode = AccessMode::kNonAtomic>
  const HeapObjectHeader& ObjectHeaderFromInnerAddressConcurrent(
      const void* address) const;

  // |address| is guaranteed to point into the page but not payload. Returns
  // nullptr when pointing into free list entries and the valid header
  // otherwise. The function is not thread-safe and cannot be called when
  // e.g. sweeping is in progress.
  HeapObjectHeader* TryObjectHeaderFromInnerAddress(void* address) const;
  const HeapObjectHeader* TryObjectHeaderFromInnerAddress(
      const void* address) const;

  // SynchronizedLoad and SynchronizedStore are used to sync pages after they
  // are allocated. std::atomic_thread_fence is sufficient in practice but is
  // not recognized by tsan. Atomic load and store of the |type_| field are
  // added for tsan builds.
  void SynchronizedLoad() const {
#if defined(THREAD_SANITIZER)
    (void)std::atomic_ref<PageType>(const_cast<PageType&>(type_))
        .load(std::memory_order_acquire);
#endif
  }
  void SynchronizedStore() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
#if defined(THREAD_SANITIZER)
    std::atomic_ref<PageType>(type_).store(type_, std::memory_order_release);
#endif
  }

  void IncrementDiscardedMemory(size_t value) {
    DCHECK_GE(discarded_memory_ + value, discarded_memory_);
    discarded_memory_ += value;
  }
  void DecrementDiscardedMemory(size_t value) {
    DCHECK_GE(discarded_memory_, value);
    discarded_memory_ -= value;
  }
  void ResetDiscardedMemory() { discarded_memory_ = 0; }
  size_t discarded_memory() const { return discarded_memory_; }

  void IncrementMarkedBytes(size_t value) {
    const size_t old_marked_bytes =
        marked_bytes_.fetch_add(value, std::memory_order_relaxed);
    USE(old_marked_bytes);
    DCHECK_GE(old_marked_bytes + value, old_marked_bytes);
  }
  void DecrementMarkedBytes(size_t value) {
    const size_t old_marked_bytes =
        marked_bytes_.fetch_sub(value, std::memory_order_relaxed);
    USE(old_marked_bytes);
    DCHECK_LE(old_marked_bytes - value, old_marked_bytes);
  }
  void ResetMarkedBytes(size_t new_value = 0) {
    marked_bytes_.store(new_value, std::memory_order_relaxed);
  }
  size_t marked_bytes() const {
    return marked_bytes_.load(std::memory_order_relaxed);
  }

  bool contains_young_objects() const { return contains_young_objects_; }
  void set_as_containing_young_objects(bool value) {
    contains_young_objects_ = value;
  }

  // Shared per-page slot sets used by generational remembered sets during minor
  // GC and by `ObjectCompactor` during pause-time major-GC compaction.
  V8_INLINE SlotSet* slot_set() const { return slot_set_.get(); }
  V8_INLINE SlotSet& GetOrAllocateSlotSet();
  void ResetSlotSet();

  V8_INLINE CompactionTypedSlotSet* compaction_typed_slot_set() const {
    return compaction_typed_slot_set_.get();
  }
  V8_INLINE CompactionTypedSlotSet& GetOrAllocateCompactionTypedSlotSet();
  void ResetCompactionTypedSlotSet() { compaction_typed_slot_set_.reset(); }

 protected:
  enum class PageType : uint8_t { kNormal, kLarge };
  BasePage(HeapBase&, BaseSpace&, PageType);

  void ChangeOwner(BaseSpace&);

 private:
  struct SlotSetDeleter {
    void operator()(SlotSet*) const;
    size_t page_size_ = 0;
  };
  void AllocateSlotSet();
  void AllocateCompactionTypedSlotSet();

  BaseSpace* space_;
  PageType type_;
  bool contains_young_objects_ = false;
  std::unique_ptr<SlotSet, SlotSetDeleter> slot_set_;
  std::unique_ptr<CompactionTypedSlotSet> compaction_typed_slot_set_;
  size_t discarded_memory_ = 0;
  std::atomic<size_t> marked_bytes_{0};
};

// Normal pages come in two layouts that depend on the space they belong to:
//
// - Mixed pages hold objects of arbitrary sizes. An `ObjectStartBitmap` that
//   directly follows the page header is used to find the object start for inner
//   pointers.
// - Size-class pages (see `NormalPageSpace::is_size_class()`) are split into
//   cells of the same size. Each cell holds a single object. The start of an
//   object is computed from the offset of the inner pointer into the payload.
//   Such pages don't have an `ObjectStartBitmap`; the memory is used as payload
//   instead.
//
// In both layouts all memory of the payload that is not part of the LAB is
// covered by `HeapObjectHeader`s that either describe objects or free list
// entries, which means that both layouts are iterable in the same way.
class V8_EXPORT_PRIVATE NormalPage final : public BasePage {
  template <typename T>
  class IteratorImpl : v8::base::iterator<std::forward_iterator_tag, T> {
   public:
    explicit IteratorImpl(T* p, ConstAddress lab_start = nullptr,
                          size_t lab_size = 0)
        : p_(p), lab_start_(lab_start), lab_size_(lab_size) {
      DCHECK(p);
      DCHECK_EQ(0, (lab_size & (sizeof(T) - 1)));
      if (reinterpret_cast<ConstAddress>(p_) == lab_start_) {
        p_ += (lab_size_ / sizeof(T));
      }
    }

    T& operator*() { return *p_; }
    const T& operator*() const { return *p_; }

    bool operator==(IteratorImpl other) const { return p_ == other.p_; }
    bool operator!=(IteratorImpl other) const { return !(*this == other); }

    IteratorImpl& operator++() {
      const size_t size = p_->AllocatedSize();
      DCHECK_EQ(0, (size & (sizeof(T) - 1)));
      p_ += (size / sizeof(T));
      if (reinterpret_cast<ConstAddress>(p_) == lab_start_) {
        p_ += (lab_size_ / sizeof(T));
      }
      return *this;
    }
    IteratorImpl operator++(int) {
      IteratorImpl temp(*this);
      ++(*this);
      return temp;
    }

    T* base() const { return p_; }

   private:
    T* p_;
    ConstAddress lab_start_;
    size_t lab_size_;
  };

 public:
  using iterator = IteratorImpl<HeapObjectHeader>;
  using const_iterator = IteratorImpl<const HeapObjectHeader>;

  // Allocates a new page in the detached state.
  static NormalPage* TryCreate(PageBackend&, NormalPageSpace&);
  // Destroys and frees the page. The page must be detached from the
  // corresponding space (i.e. be swept when called).
  static void Destroy(NormalPage*);

  static NormalPage* From(BasePage* page) {
    DCHECK(!page->is_large());
    return static_cast<NormalPage*>(page);
  }
  static const NormalPage* From(const BasePage* page) {
    return From(const_cast<BasePage*>(page));
  }

  iterator begin();
  const_iterator begin() const;

  iterator end() {
    return iterator(reinterpret_cast<HeapObjectHeader*>(PayloadEnd()));
  }
  const_iterator end() const {
    return const_iterator(
        reinterpret_cast<const HeapObjectHeader*>(PayloadEnd()));
  }

  inline Address PayloadStart();
  inline ConstAddress PayloadStart() const;
  inline Address PayloadEnd();
  inline ConstAddress PayloadEnd() const;

  // Size of the page header for a page with cells of `cell_size` bytes. A
  // `cell_size` of 0 refers to a mixed page, in which case the header includes
  // the `ObjectStartBitmap`.
  static constexpr size_t PageHeaderSizeFor(size_t cell_size);
  // The maximum header size of all layouts.
  static constexpr size_t MaxPageHeaderSize();
  // Size of the payload for a page with cells of `cell_size` bytes. A
  // `cell_size` of 0 refers to a mixed page. For size-class pages, this only
  // covers whole cells.
  static constexpr size_t PayloadSizeFor(size_t cell_size);

  size_t PayloadSize() const { return payload_size_; }

  bool PayloadContains(ConstAddress address) const {
    return (PayloadStart() <= address) && (address < PayloadEnd());
  }

  size_t AllocatedBytesAtLastGC() const { return allocated_bytes_at_last_gc_; }

  void SetAllocatedBytesAtLastGC(size_t bytes) {
    allocated_bytes_at_last_gc_ = bytes;
  }

  // Size-class pages are split into cells of `cell_size()` bytes that each
  // hold a single object.
  bool is_size_class() const { return cell_size_ != 0; }
  size_t cell_size() const { return cell_size_; }

  // Returns the address of the header of the cell that contains `address`. The
  // cell may be free, in which case there's only a valid header if the cell is
  // the first cell of a free list entry. Only supported on size-class pages.
  inline const HeapObjectHeader* CellHeaderFromInnerAddress(
      ConstAddress address) const;

  // Changes the owner of the page. The layout of the page is adjusted to the
  // new space, which is only supported for empty pages where all the payload is
  // inaccessible (see `SetMemoryInaccessible()`).
  void ChangeOwner(NormalPageSpace&);

  // Only available for mixed pages.
  ObjectStartBitmap& object_start_bitmap() const {
    DCHECK(!is_size_class());
    return *reinterpret_cast<ObjectStartBitmap*>(const_cast<NormalPage*>(this) +
                                                 1);
  }

  // Division by the cell size is implemented as multiplication with the
  // reciprocal followed by a shift.
  static constexpr size_t kCellSizeReciprocalShift = 20;
  static constexpr uint32_t CellSizeReciprocal(size_t cell_size) {
    const uint32_t divisor =
        static_cast<uint32_t>(cell_size / kAllocationGranularity);
    return ((uint32_t{1} << kCellSizeReciprocalShift) + divisor - 1) / divisor;
  }
  static constexpr size_t CellIndexForOffset(size_t offset,
                                             uint32_t cell_size_reciprocal) {
    // The result is exact for all offsets on a page as long as the divisor is
    // smaller than 16, which is guaranteed by `kSizeClassCutoff`.
    return static_cast<size_t>(
        (static_cast<uint64_t>(offset / kAllocationGranularity) *
         cell_size_reciprocal) >>
        kCellSizeReciprocalShift);
  }

  void DiscardOrDecommitPages(Address begin, size_t size);
  void RecommitMemory(Address begin, size_t size);
  uint64_t active_system_page_bits() const {
    return active_system_pages_.bits();
  }
  bool IsAddressOnCommittedSubPage(const void* address) const;

  void IncrementBorrowPinCount() {
    const uint32_t prev =
        active_borrow_pin_count_.fetch_add(1, std::memory_order_relaxed);
    CHECK_LT(prev, std::numeric_limits<uint32_t>::max());
  }
  void DecrementBorrowPinCount() {
    const uint32_t prev =
        active_borrow_pin_count_.fetch_sub(1, std::memory_order_relaxed);
    CHECK_GT(prev, 0u);
  }
  uint32_t borrow_pin_count() const {
    return active_borrow_pin_count_.load(std::memory_order_relaxed);
  }

  void PinByConservativeStack(uint64_t epoch) {
    conservative_pin_epoch_.store(epoch, std::memory_order_relaxed);
  }
  bool IsPinnedByConservativeStack(uint64_t epoch) const {
    return conservative_pin_epoch_.load(std::memory_order_relaxed) == epoch;
  }

  void PinByCrossThreadRoot(uint64_t epoch) {
    cross_thread_pin_epoch_.store(epoch, std::memory_order_relaxed);
  }
  bool IsPinnedByCrossThreadRoot(uint64_t epoch) const {
    return cross_thread_pin_epoch_.load(std::memory_order_relaxed) == epoch;
  }

  bool IsPinnedForCompaction(uint64_t epoch) const {
    return borrow_pin_count() > 0 || IsPinnedByConservativeStack(epoch) ||
           IsPinnedByCrossThreadRoot(epoch);
  }

  bool is_evacuation_candidate() const { return is_evacuation_candidate_; }
  void set_is_evacuation_candidate(bool value) {
    is_evacuation_candidate_ = value;
  }

 private:
  NormalPage(HeapBase& heap, NormalPageSpace& space);
  ~NormalPage() = default;

  void SetLayout(size_t cell_size);

  size_t allocated_bytes_at_last_gc_ = 0;
  std::atomic<uint32_t> active_borrow_pin_count_{0};
  std::atomic<uint64_t> conservative_pin_epoch_{0};
  std::atomic<uint64_t> cross_thread_pin_epoch_{0};
  bool is_evacuation_candidate_ = false;
  uint32_t payload_size_ = 0;
  // The following fields are only used for size-class pages.
  uint32_t cell_size_reciprocal_ = 0;
  uint16_t cell_size_ = 0;
  ::heap::base::ActiveSystemPages active_system_pages_;
};

class V8_EXPORT_PRIVATE LargePage final : public BasePage {
 public:
  static constexpr size_t PageHeaderSize() {
    // Header should be un-aligned to `kAllocationGranularity` so that adding a
    // `HeapObjectHeader` gets the user object aligned to
    // `kGuaranteedObjectAlignment`.
    return RoundUp<kGuaranteedObjectAlignment>(sizeof(LargePage) +
                                               sizeof(HeapObjectHeader)) -
           sizeof(HeapObjectHeader);
  }

  // Returns the allocation size required for a payload of size |size|.
  static size_t AllocationSize(size_t size);
  // Allocates a new page in the detached state.
  static LargePage* TryCreate(PageBackend&, LargePageSpace&, size_t);
  // Destroys and frees the page. The page must be detached from the
  // corresponding space (i.e. be swept when called).
  static void Destroy(LargePage*);

  static LargePage* From(BasePage* page) {
    DCHECK(page->is_large());
    return static_cast<LargePage*>(page);
  }
  static const LargePage* From(const BasePage* page) {
    return From(const_cast<BasePage*>(page));
  }

  HeapObjectHeader* ObjectHeader();
  const HeapObjectHeader* ObjectHeader() const;

  Address PayloadStart();
  ConstAddress PayloadStart() const;
  Address PayloadEnd();
  ConstAddress PayloadEnd() const;

  size_t PayloadSize() const { return payload_size_; }
  size_t ObjectSize() const {
    DCHECK_GT(payload_size_, sizeof(HeapObjectHeader));
    return payload_size_ - sizeof(HeapObjectHeader);
  }

  size_t AllocatedBytesAtLastGC() const { return ObjectSize(); }

  bool PayloadContains(ConstAddress address) const {
    return (PayloadStart() <= address) && (address < PayloadEnd());
  }

 private:
  static constexpr size_t kGuaranteedObjectAlignment =
      2 * kAllocationGranularity;

  LargePage(HeapBase& heap, BaseSpace& space, size_t);
  ~LargePage() = default;

  size_t payload_size_;
};

// static
BasePage* BasePage::FromPayload(void* payload) {
  return static_cast<BasePage*>(BasePageHandle::FromPayload(payload));
}

// static
const BasePage* BasePage::FromPayload(const void* payload) {
  return static_cast<const BasePage*>(BasePageHandle::FromPayload(payload));
}

template <AccessMode mode>
HeapObjectHeader& BasePage::ObjectHeaderFromInnerAddress(void* address) const {
  return const_cast<HeapObjectHeader&>(
      ObjectHeaderFromInnerAddress<mode>(const_cast<const void*>(address)));
}

template <AccessMode mode>
const HeapObjectHeader& BasePage::ObjectHeaderFromInnerAddress(
    const void* address) const {
  // This method might be called for |address| found via a Trace method of
  // another object. If |address| is on a newly allocated page , there will
  // be no sync between the page allocation and a concurrent marking thread,
  // resulting in a race with page initialization (specifically with writing
  // the page |type_| field). This can occur when tracing a Member holding a
  // reference to a mixin type
  SynchronizedLoad();
  if (is_large()) {
    return *LargePage::From(this)->ObjectHeader();
  }
  const NormalPage* normal_page = NormalPage::From(this);
  if (normal_page->is_size_class()) {
    // The object start can be directly computed for size-class pages.
    const HeapObjectHeader* header = normal_page->CellHeaderFromInnerAddress(
        static_cast<ConstAddress>(address));
    DCHECK_NE(kFreeListGCInfoIndex, header->GetGCInfoIndex<mode>());
    return *header;
  }
  // We may find other headers (before this object) because not all object start
  // bits are initially set. The fixup then makes sure that we indeed find the
  // right one.
  ObjectStartBitmap& bitmap = normal_page->object_start_bitmap();
  const HeapObjectHeader* header =
      bitmap.FindHeader<mode>(static_cast<ConstAddress>(address));
  ConstAddress object_end =
      (!header || static_cast<const void*>(header) < PayloadStart())
          ? PayloadStart()
          : reinterpret_cast<ConstAddress>(header) +
                header->AllocatedSize<AccessMode::kAtomic>();
  const ConstAddress page_end = PayloadEnd();
  USE(page_end);
  const auto& lab = NormalPageSpace::From(space()).linear_allocation_buffer();
  ConstAddress lab_start = lab.size() ? lab.start() : nullptr;
  const size_t lab_size = lab.size();
  while (object_end <= address) {
    // Assuming no past-the-end pointers, object_end may not refer to the
    // payload end.
    DCHECK_NE(object_end, page_end);
    if (lab_start && object_end == lab_start) {
      // Jump over current lab.
      object_end = lab_start + lab_size;
      continue;
    }
    bitmap.SetBit<AccessMode::kAtomic>(object_end);
    header = reinterpret_cast<const HeapObjectHeader*>(object_end);
    object_end += header->AllocatedSize<AccessMode::kAtomic>();
  }
  DCHECK_LT(address, object_end);
  // Make sure that the pointer is within the object bounds.
  CHECK_LT(address, reinterpret_cast<ConstAddress>(header) +
                        header->AllocatedSize<AccessMode::kAtomic>());
  DCHECK_NE(kFreeListGCInfoIndex, header->GetGCInfoIndex<mode>());
  return *header;
}

template <AccessMode mode>
HeapObjectHeader& BasePage::ObjectHeaderFromInnerAddressConcurrent(
    void* address) const {
  return const_cast<HeapObjectHeader&>(
      ObjectHeaderFromInnerAddressConcurrent<mode>(
          const_cast<const void*>(address)));
}

template <AccessMode mode>
const HeapObjectHeader& BasePage::ObjectHeaderFromInnerAddressConcurrent(
    const void* address) const {
  SynchronizedLoad();
  if (is_large()) {
    return *LargePage::From(this)->ObjectHeader();
  }
  const NormalPage* normal_page = NormalPage::From(this);
  if (normal_page->is_size_class()) {
    const HeapObjectHeader* header = normal_page->CellHeaderFromInnerAddress(
        static_cast<ConstAddress>(address));
    DCHECK_NE(kFreeListGCInfoIndex, header->GetGCInfoIndex<mode>());
    return *header;
  }
  ObjectStartBitmap& bitmap = normal_page->object_start_bitmap();
  const HeapObjectHeader* header =
      bitmap.FindHeader<mode>(static_cast<ConstAddress>(address));
  // Make sure that the pointer is within the object bounds.
  CHECK_LT(address, reinterpret_cast<ConstAddress>(header) +
                        header->AllocatedSize<AccessMode::kAtomic>());
  DCHECK_NE(kFreeListGCInfoIndex, header->GetGCInfoIndex<mode>());
  return *header;
}

SlotSet& BasePage::GetOrAllocateSlotSet() {
  if (!slot_set_) AllocateSlotSet();
  return *slot_set_;
}

CompactionTypedSlotSet& BasePage::GetOrAllocateCompactionTypedSlotSet() {
  if (!compaction_typed_slot_set_) AllocateCompactionTypedSlotSet();
  return *compaction_typed_slot_set_;
}

// static
constexpr inline size_t NormalPage::PageHeaderSizeFor(size_t cell_size) {
  if (cell_size == 0) {
    // The object start bitmap directly follows the `NormalPage` object.
    return RoundUp(sizeof(NormalPage) + sizeof(ObjectStartBitmap),
                   kAllocationGranularity);
  }
  // Header should be un-aligned to `kAllocationGranularity` so that adding a
  // `HeapObjectHeader` gets the user object in the first cell aligned to
  // `kMaxSupportedAlignment`. This way all objects in cells with a size that is
  // a multiple of `kMaxSupportedAlignment` are aligned to
  // `kMaxSupportedAlignment` as well.
  return RoundUp(sizeof(NormalPage) + sizeof(HeapObjectHeader),
                 api_constants::kMaxSupportedAlignment) -
         sizeof(HeapObjectHeader);
}

// static
constexpr inline size_t NormalPage::MaxPageHeaderSize() {
  return std::max(PageHeaderSizeFor(0), PageHeaderSizeFor(kMinSizeClassSize));
}

// static
constexpr inline size_t NormalPage::PayloadSizeFor(size_t cell_size) {
  const size_t max_payload_size = kPageSize - PageHeaderSizeFor(cell_size);
  if (cell_size == 0) {
    return max_payload_size;
  }
  // Only whole cells are part of the payload.
  return max_payload_size - (max_payload_size % cell_size);
}

Address NormalPage::PayloadStart() {
  return reinterpret_cast<Address>(this) + PageHeaderSizeFor(cell_size_);
}

ConstAddress NormalPage::PayloadStart() const {
  return const_cast<NormalPage*>(this)->PayloadStart();
}

Address NormalPage::PayloadEnd() { return PayloadStart() + PayloadSize(); }

ConstAddress NormalPage::PayloadEnd() const {
  return const_cast<NormalPage*>(this)->PayloadEnd();
}

const HeapObjectHeader* NormalPage::CellHeaderFromInnerAddress(
    ConstAddress address) const {
  DCHECK(is_size_class());
  // Make sure that the pointer is within the payload and thus within the bounds
  // of the cell returned.
  CHECK(PayloadContains(address));
  const ConstAddress payload_start = PayloadStart();
  const size_t offset = static_cast<size_t>(address - payload_start);
  const size_t cell_index = CellIndexForOffset(offset, cell_size_reciprocal_);
  DCHECK_EQ(offset / cell_size_, cell_index);
  return reinterpret_cast<const HeapObjectHeader*>(payload_start +
                                                   cell_index * cell_size_);
}

}  // namespace internal
}  // namespace cppgc

namespace v8::base {

template <>
struct hash<const cppgc::internal::BasePage*> {
  V8_INLINE size_t
  operator()(const cppgc::internal::BasePage* base_page) const {
#ifdef CPPGC_POINTER_COMPRESSION
    using AddressType = uint32_t;
#else
    using AddressType = uintptr_t;
#endif
    return static_cast<AddressType>(reinterpret_cast<uintptr_t>(base_page)) >>
           cppgc::internal::api_constants::kPageSizeBits;
  }
};

}  // namespace v8::base

#endif  // V8_HEAP_CPPGC_INTERNAL_HEAP_PAGE_H_
