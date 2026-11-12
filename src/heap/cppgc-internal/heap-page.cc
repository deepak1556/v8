// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/cppgc-internal/heap-page.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>

#include "include/cppgc/internal/api-constants.h"
#include "src/base/bits.h"
#include "src/base/logging.h"
#include "src/base/platform/mutex.h"
#include "src/base/sanitizer/asan.h"
#include "src/base/sanitizer/msan.h"
#include "src/heap/cppgc-internal/globals.h"
#include "src/heap/cppgc-internal/heap-object-header.h"
#include "src/heap/cppgc-internal/heap-space.h"
#include "src/heap/cppgc-internal/heap.h"
#include "src/heap/cppgc-internal/memory.h"
#include "src/heap/cppgc-internal/object-start-bitmap.h"
#include "src/heap/cppgc-internal/page-memory.h"
#include "src/heap/cppgc-internal/raw-heap.h"
#include "src/heap/cppgc-internal/remembered-set.h"
#include "src/heap/cppgc-internal/stats-collector.h"

namespace cppgc::internal {

namespace {

// Returns true if the cell starting at `header` on a size-class page is free.
// Free cells are either the first cell of a free list entry, in which case
// they have a valid header, or they are in the inaccessible state, see
// `SetMemoryInaccessible()`.
bool IsFreeCell(const HeapObjectHeader* header) {
#if defined(V8_USE_MEMORY_SANITIZER)
  // Inaccessible memory is zeroed but marked as uninitialized.
  MSAN_MEMORY_IS_INITIALIZED(header, sizeof(HeapObjectHeader));
#elif defined(V8_USE_ADDRESS_SANITIZER)
  // Inaccessible memory is zeroed but poisoned.
  AsanUnpoisonScope unpoison_scope(header, sizeof(HeapObjectHeader));
#elif DEBUG
  // Inaccessible memory is zapped. Zapped memory never looks like a regular
  // object header as the bit pattern has an otherwise unused bit set.
  static constexpr uint8_t kZappedHeader[sizeof(HeapObjectHeader)] = {
      kZappedValue, kZappedValue, kZappedValue, kZappedValue,
#if defined(V8_HOST_ARCH_64_BIT)
      kZappedValue, kZappedValue, kZappedValue, kZappedValue,
#endif  // defined(V8_HOST_ARCH_64_BIT)
  };
  if (memcmp(header, kZappedHeader, sizeof(HeapObjectHeader)) == 0) {
    return true;
  }
#endif
  // In all other configurations inaccessible memory is zeroed and thus has
  // `kFreeListGCInfoIndex` as GCInfoIndex.
  static_assert(kFreeListGCInfoIndex == 0);
  return header->IsFree();
}

}  // namespace

HeapBase& BasePage::heap() const {
  return static_cast<HeapBase&>(heap_handle_);
}

// static
BasePage* BasePage::FromInnerAddress(const HeapBase* heap, void* address) {
  return const_cast<BasePage*>(
      FromInnerAddress(heap, const_cast<const void*>(address)));
}

// static
const BasePage* BasePage::FromInnerAddress(const HeapBase* heap,
                                           const void* address) {
  return reinterpret_cast<const BasePage*>(
      heap->page_backend()->Lookup(static_cast<ConstAddress>(address)));
}

// static
void BasePage::Destroy(BasePage* page) {
  if (page->is_large()) {
    if (page->discarded_memory()) {
      page->space()
          .raw_heap()
          ->heap()
          ->stats_collector()
          ->DecrementDiscardedMemory(page->discarded_memory());
    }
    LargePage::Destroy(LargePage::From(page));
  } else {
    NormalPage::Destroy(NormalPage::From(page));
  }
}

Address BasePage::PayloadStart() {
  return is_large() ? LargePage::From(this)->PayloadStart()
                    : NormalPage::From(this)->PayloadStart();
}

ConstAddress BasePage::PayloadStart() const {
  return const_cast<BasePage*>(this)->PayloadStart();
}

Address BasePage::PayloadEnd() {
  return is_large() ? LargePage::From(this)->PayloadEnd()
                    : NormalPage::From(this)->PayloadEnd();
}

ConstAddress BasePage::PayloadEnd() const {
  return const_cast<BasePage*>(this)->PayloadEnd();
}

size_t BasePage::AllocatedSize() const {
  return is_large() ? LargePage::PageHeaderSize() +
                          LargePage::From(this)->PayloadSize()
                    : kPageSize;
}

size_t BasePage::AllocatedBytesAtLastGC() const {
  return is_large() ? LargePage::From(this)->AllocatedBytesAtLastGC()
                    : NormalPage::From(this)->AllocatedBytesAtLastGC();
}

HeapObjectHeader* BasePage::TryObjectHeaderFromInnerAddress(
    void* address) const {
  return const_cast<HeapObjectHeader*>(
      TryObjectHeaderFromInnerAddress(const_cast<const void*>(address)));
}

const HeapObjectHeader* BasePage::TryObjectHeaderFromInnerAddress(
    const void* address) const {
  if (is_large()) {
    if (!LargePage::From(this)->PayloadContains(
            static_cast<ConstAddress>(address))) {
      return nullptr;
    }
    return LargePage::From(this)->ObjectHeader();
  } else {
    const NormalPage* normal_page = NormalPage::From(this);
    if (!normal_page->PayloadContains(static_cast<ConstAddress>(address))) {
      return nullptr;
    }
    // Check that the space has no linear allocation buffer.
    DCHECK(!NormalPageSpace::From(normal_page->space())
                .linear_allocation_buffer()
                .size());

    if (normal_page->is_size_class()) {
      const HeapObjectHeader* header = normal_page->CellHeaderFromInnerAddress(
          static_cast<ConstAddress>(address));
      if (normal_page->discarded_memory() &&
          (!normal_page->IsAddressOnCommittedSubPage(header) ||
           !normal_page->IsAddressOnCommittedSubPage(
               reinterpret_cast<ConstAddress>(header) +
               sizeof(HeapObjectHeader) - 1))) {
        return nullptr;
      }
      return IsFreeCell(header) ? nullptr : header;
    }

    ObjectStartBitmap& bitmap = normal_page->object_start_bitmap();
    const HeapObjectHeader* header = bitmap.FindHeader<AccessMode::kNonAtomic>(
        static_cast<ConstAddress>(address));

    // We may find other headers (before this object) because not all object
    // start bits are initially set. The fixup then makes sure that we indeed
    // find the right one.
    ConstAddress object_end =
        (!header || static_cast<const void*>(header) < PayloadStart())
            ? PayloadStart()
            : reinterpret_cast<ConstAddress>(header) +
                  header->AllocatedSize<AccessMode::kAtomic>();
    const ConstAddress page_end = PayloadEnd();
    USE(page_end);
    while (object_end <= address) {
      DCHECK_NE(object_end, page_end);
      bitmap.SetBit<AccessMode::kAtomic>(object_end);
      header = reinterpret_cast<const HeapObjectHeader*>(object_end);
      object_end += header->AllocatedSize<AccessMode::kAtomic>();
    }

    if (!header) return nullptr;

    // Boundary check.
    if (address >= reinterpret_cast<ConstAddress>(header) +
                       header->AllocatedSize<AccessMode::kAtomic>()) {
      // Found an object below the current address.
      return nullptr;
    }
    if (header->IsFree()) return nullptr;

    DCHECK_NE(kFreeListGCInfoIndex, header->GetGCInfoIndex());
    return header;
  }
}

#if defined(CPPGC_YOUNG_GENERATION)
void BasePage::AllocateSlotSet() {
  DCHECK_NULL(slot_set_);
  slot_set_ = decltype(slot_set_)(
      static_cast<SlotSet*>(
          SlotSet::Allocate(SlotSet::BucketsForSize(AllocatedSize()))),
      SlotSetDeleter{AllocatedSize()});
}

void BasePage::SlotSetDeleter::operator()(SlotSet* slot_set) const {
  DCHECK_NOT_NULL(slot_set);
  SlotSet::Delete(slot_set);
}

void BasePage::ResetSlotSet() { slot_set_.reset(); }
#endif  // defined(CPPGC_YOUNG_GENERATION)

BasePage::BasePage(HeapBase& heap, BaseSpace& space, PageType type)
    : BasePageHandle(heap),
      space_(&space),
      type_(type)
#if defined(CPPGC_YOUNG_GENERATION)
      ,
      slot_set_(nullptr, SlotSetDeleter{})
#endif  // defined(CPPGC_YOUNG_GENERATION)
{
  DCHECK_EQ(0u, reinterpret_cast<uintptr_t>(this) & kPageOffsetMask);
  DCHECK_EQ(&heap.raw_heap(), space_->raw_heap());
}

void BasePage::ChangeOwner(BaseSpace& space) {
  DCHECK_EQ(space_->raw_heap(), space.raw_heap());
  space_ = &space;
}

void NormalPage::ChangeOwner(NormalPageSpace& space) {
  BasePage::ChangeOwner(space);
  const size_t new_cell_size = space.cell_size();
  if (new_cell_size == cell_size_) return;

  // The layout of the page changes which requires the page to be empty.
  DCHECK_EQ(0u, marked_bytes());
  const Address old_payload_start = PayloadStart();
  SetLayout(new_cell_size);
  const Address new_payload_start = PayloadStart();
  if (new_payload_start < old_payload_start) {
    // Mixed page to size-class page: The memory used for the object start
    // bitmap is now part of the payload and must be in the same state as the
    // rest of the payload.
    DCHECK(is_size_class());
    SetMemoryInaccessible(new_payload_start,
                          old_payload_start - new_payload_start);
  } else if (old_payload_start < new_payload_start) {
    // Size-class page to mixed page: The start of the payload is now used for
    // the object start bitmap.
    DCHECK(!is_size_class());
    SetMemoryAccessible(old_payload_start,
                        new_payload_start - old_payload_start);
    ObjectStartBitmap* bitmap = new (&object_start_bitmap()) ObjectStartBitmap;
    // The page is empty and callers represent it as a single free list entry
    // that starts at the payload start.
    bitmap->SetBit(new_payload_start);
  }
  SynchronizedStore();
}

// static
NormalPage* NormalPage::TryCreate(PageBackend& page_backend,
                                  NormalPageSpace& space) {
  void* memory = page_backend.TryAllocateNormalPageMemory();
  if (!memory) return nullptr;

  auto* normal_page = new (memory) NormalPage(*space.raw_heap()->heap(), space);
  normal_page->SynchronizedStore();
  normal_page->heap().stats_collector()->NotifyAllocatedMemory(kPageSize);
  // Memory is zero initialized as
  // a) memory retrieved from the OS is zeroed;
  // b) memory retrieved from the page pool was swept and thus is zeroed except
  //    for the first header which will anyways serve as header again.
  //
  // The following is a subset of SetMemoryInaccessible() to establish the
  // invariant that memory is in the same state as it would be after sweeping.
  // This allows to return newly allocated pages to go into that LAB and back
  // into the free list.
  //
  // This covers all memory up to the end of the page, i.e., also the memory
  // after the last cell of a size-class page that is not part of the payload.
  // That memory may become payload when the layout of the page changes, see
  // `NormalPage::ChangeOwner()`.
  Address begin = normal_page->PayloadStart() + sizeof(HeapObjectHeader);
  const size_t size = static_cast<size_t>(
      reinterpret_cast<Address>(normal_page) + kPageSize - begin);
#if defined(V8_USE_MEMORY_SANITIZER)
  MSAN_ALLOCATED_UNINITIALIZED_MEMORY(begin, size);
#elif defined(V8_USE_ADDRESS_SANITIZER)
  ASAN_POISON_MEMORY_REGION(begin, size);
#elif DEBUG
  cppgc::internal::ZapMemory(begin, size);
#endif  // Release builds.
  CheckMemoryIsInaccessible(begin, size);
  return normal_page;
}

// static
void NormalPage::Destroy(NormalPage* page) {
  DCHECK(page);
  HeapBase& heap = page->heap();
  const BaseSpace& space = page->space();
  DCHECK_EQ(space.end(), std::find(space.begin(), space.end(), page));
  USE(space);
  const size_t discarded_memory = page->discarded_memory();
  if (discarded_memory) {
    heap.stats_collector()->DecrementDiscardedMemory(discarded_memory);
  }
  page->~NormalPage();
  PageBackend* backend = heap.page_backend();
  heap.stats_collector()->NotifyFreedMemory(kPageSize);
  backend->FreeNormalPageMemory(
      reinterpret_cast<Address>(page),
      heap.decommit_free_memory() && discarded_memory > 0);
}

NormalPage::NormalPage(HeapBase& heap, NormalPageSpace& space)
    : BasePage(heap, space, PageType::kNormal) {
  SetLayout(space.cell_size());
  if (!is_size_class()) {
    new (&object_start_bitmap()) ObjectStartBitmap;
    DCHECK_LT(kLargeObjectSizeThreshold,
              static_cast<size_t>(PayloadEnd() - PayloadStart()));
  }
  const size_t commit_page_size =
      heap.platform()->GetPageAllocator()->CommitPageSize();
  active_system_pages_.Init(
      kPageSize, v8::base::bits::WhichPowerOfTwo(commit_page_size), kPageSize);
}

void NormalPage::SetLayout(size_t cell_size) {
  DCHECK_GT(kSizeClassCutoff, cell_size);
  cell_size_ = static_cast<uint16_t>(cell_size);
  cell_size_reciprocal_ = cell_size ? CellSizeReciprocal(cell_size) : 0;
  payload_size_ = static_cast<uint32_t>(PayloadSizeFor(cell_size));
}

void NormalPage::DiscardOrDecommitPages(Address begin, size_t size) {
  PageAllocator& page_allocator = *heap().platform()->GetPageAllocator();
  const size_t commit_page_size = page_allocator.CommitPageSize();
  DCHECK_EQ(0u, reinterpret_cast<uintptr_t>(begin) % commit_page_size);
  DCHECK_EQ(0u, size % commit_page_size);
  DCHECK_LE(PayloadStart(), begin);
  DCHECK_LE(begin + size, PayloadEnd());
  const size_t page_size_bits =
      v8::base::bits::WhichPowerOfTwo(commit_page_size);
  const uintptr_t page_start = reinterpret_cast<uintptr_t>(this);
  const uintptr_t start_offset =
      reinterpret_cast<uintptr_t>(begin) - page_start;
  const uintptr_t end_offset = start_offset + size;
  const size_t removed_pages =
      active_system_pages_.Remove(start_offset, end_offset, page_size_bits);
  if (removed_pages == 0) {
    return;
  }
  const size_t discarded_size = removed_pages << page_size_bits;
  if (heap().decommit_free_memory()) {
    CHECK(page_allocator.DecommitPages(begin, size));
  } else {
    CHECK(page_allocator.DiscardSystemPages(begin, size));
  }
  IncrementDiscardedMemory(discarded_size);
  heap().stats_collector()->IncrementDiscardedMemory(discarded_size);
}

void NormalPage::RecommitMemory(Address begin, size_t size) {
  if (!discarded_memory()) {
    return;
  }
  DCHECK_LE(PayloadStart(), begin);
  DCHECK_LE(begin + size, PayloadEnd());
  PageAllocator& page_allocator = *heap().platform()->GetPageAllocator();
  const size_t commit_page_size = page_allocator.CommitPageSize();
  const size_t page_size_bits =
      v8::base::bits::WhichPowerOfTwo(commit_page_size);
  const uintptr_t page_start = reinterpret_cast<uintptr_t>(this);
  const uintptr_t start_offset =
      reinterpret_cast<uintptr_t>(begin) - page_start;
  const uintptr_t end_offset = start_offset + size;
  const uint64_t old_bits = active_system_pages_.bits();
  const size_t added_pages =
      active_system_pages_.Add(start_offset, end_offset, page_size_bits);
  if (added_pages == 0) {
    return;
  }
  uint64_t newly_added = active_system_pages_.bits() & ~old_bits;
  const size_t recommitted_size = added_pages << page_size_bits;
  if (heap().decommit_free_memory()) {
    while (newly_added != 0) {
      const size_t first_bit = std::countr_zero(newly_added);
      const size_t run_pages = std::countr_one(newly_added >> first_bit);
      void* run_addr =
          reinterpret_cast<void*>(page_start + (first_bit << page_size_bits));
      const size_t run_size = run_pages << page_size_bits;
      CHECK(page_allocator.RecommitPages(
          run_addr, run_size, v8::PageAllocator::Permission::kReadWrite));
      CHECK(page_allocator.SetPermissions(
          run_addr, run_size, v8::PageAllocator::Permission::kReadWrite));
#if CPPGC_SHOULD_ZAP_MEMORY
      SetMemoryInaccessible(run_addr, run_size);
#endif
      if (first_bit + run_pages >= 64) {
        newly_added = 0;
      } else {
        newly_added &= ~(((uint64_t{1} << run_pages) - 1) << first_bit);
      }
    }
  }
  DecrementDiscardedMemory(recommitted_size);
  heap().stats_collector()->DecrementDiscardedMemory(recommitted_size);
}

bool NormalPage::IsAddressOnCommittedSubPage(const void* address) const {
  const uintptr_t page_start = reinterpret_cast<uintptr_t>(this);
  const uintptr_t addr_uptr = reinterpret_cast<uintptr_t>(address);
  if (addr_uptr < page_start || addr_uptr >= page_start + kPageSize) {
    return false;
  }
  if (!discarded_memory()) return true;
  const size_t commit_page_size =
      heap().platform()->GetPageAllocator()->CommitPageSize();
  const size_t unit_idx = (addr_uptr - page_start) / commit_page_size;
  if (unit_idx < ::heap::base::ActiveSystemPages::kMaxPages) {
    return (active_system_pages_.bits() & (uint64_t{1} << unit_idx)) != 0;
  }
  return true;
}

NormalPage::iterator NormalPage::begin() {
  const auto& lab = NormalPageSpace::From(space()).linear_allocation_buffer();
  return iterator(reinterpret_cast<HeapObjectHeader*>(PayloadStart()),
                  lab.start(), lab.size());
}

NormalPage::const_iterator NormalPage::begin() const {
  const auto& lab = NormalPageSpace::From(space()).linear_allocation_buffer();
  return const_iterator(
      reinterpret_cast<const HeapObjectHeader*>(PayloadStart()), lab.start(),
      lab.size());
}

LargePage::LargePage(HeapBase& heap, BaseSpace& space, size_t size)
    : BasePage(heap, space, PageType::kLarge), payload_size_(size) {}

// static
size_t LargePage::AllocationSize(size_t payload_size) {
  return PageHeaderSize() + payload_size;
}

// static
LargePage* LargePage::TryCreate(PageBackend& page_backend,
                                LargePageSpace& space, size_t size) {
  // Ensure that the API-provided alignment guarantees does not violate the
  // internally guaranteed alignment of large page allocations.
  static_assert(kGuaranteedObjectAlignment <=
                api_constants::kMaxSupportedAlignment);
  static_assert(
      api_constants::kMaxSupportedAlignment % kGuaranteedObjectAlignment == 0);

  DCHECK_LE(kLargeObjectSizeThreshold, size);
  const size_t allocation_size = AllocationSize(size);

  auto* heap = space.raw_heap()->heap();
  void* memory = page_backend.TryAllocateLargePageMemory(allocation_size);
  if (!memory) return nullptr;

  LargePage* page = new (memory) LargePage(*heap, space, size);
  page->SynchronizedStore();
  page->heap().stats_collector()->NotifyAllocatedMemory(allocation_size);
  return page;
}

// static
void LargePage::Destroy(LargePage* page) {
  DCHECK(page);
  HeapBase& heap = page->heap();
  const size_t payload_size = page->PayloadSize();
#if DEBUG
  const BaseSpace& space = page->space();
  {
    // Destroy() happens on the mutator but another concurrent sweeper task may
    // add add a live object using `BaseSpace::AddPage()` while iterating the
    // pages.
    v8::base::MutexGuard guard(&space.pages_mutex());
    DCHECK_EQ(space.end(), std::find(space.begin(), space.end(), page));
  }
#endif  // DEBUG
  page->~LargePage();
  PageBackend* backend = heap.page_backend();
  heap.stats_collector()->NotifyFreedMemory(AllocationSize(payload_size));
  backend->FreeLargePageMemory(reinterpret_cast<Address>(page));
}

HeapObjectHeader* LargePage::ObjectHeader() {
  return reinterpret_cast<HeapObjectHeader*>(PayloadStart());
}

const HeapObjectHeader* LargePage::ObjectHeader() const {
  return reinterpret_cast<const HeapObjectHeader*>(PayloadStart());
}

Address LargePage::PayloadStart() {
  return reinterpret_cast<Address>(this) + PageHeaderSize();
}

ConstAddress LargePage::PayloadStart() const {
  return const_cast<LargePage*>(this)->PayloadStart();
}

Address LargePage::PayloadEnd() { return PayloadStart() + PayloadSize(); }

ConstAddress LargePage::PayloadEnd() const {
  return const_cast<LargePage*>(this)->PayloadEnd();
}

}  // namespace cppgc::internal
