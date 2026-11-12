// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/cppgc-internal/object-compactor.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "include/cppgc/heap-consistency.h"
#include "include/cppgc/internal/member-storage.h"
#if !defined(CPPGC_IS_STANDALONE)
#include "include/v8-cppgc.h"
#endif
#include "src/base/lazy-instance.h"
#include "src/base/logging.h"
#include "src/base/sanitizer/asan.h"
#include "src/base/sanitizer/msan.h"
#include "src/base/small-vector.h"
#include "src/heap/cppgc-internal/compaction-worklists.h"
#include "src/heap/cppgc-internal/compactor.h"
#include "src/heap/cppgc-internal/free-list.h"
#include "src/heap/cppgc-internal/gc-info-table.h"
#include "src/heap/cppgc-internal/heap-base.h"
#include "src/heap/cppgc-internal/heap-object-header.h"
#include "src/heap/cppgc-internal/heap-page.h"
#include "src/heap/cppgc-internal/heap-space.h"
#include "src/heap/cppgc-internal/memory.h"
#include "src/heap/cppgc-internal/object-allocator.h"
#include "src/heap/cppgc-internal/object-start-bitmap.h"
#include "src/heap/cppgc-internal/page-memory.h"
#include "src/heap/cppgc-internal/platform.h"
#include "src/heap/cppgc-internal/prefinalizer-handler.h"
#include "src/heap/cppgc-internal/raw-heap.h"
#include "src/heap/cppgc-internal/stats-collector.h"
#include "src/heap/cppgc-internal/visitor.h"

#if defined(CPPGC_CAGED_HEAP)
#include "include/cppgc/internal/caged-heap-local-data.h"
#include "src/heap/cppgc-internal/caged-heap.h"
#endif

namespace cppgc::internal {

namespace {

#if defined(CPPGC_POINTER_COMPRESSION)
using MemberSlotStorage = CompressedPointer;
#else
using MemberSlotStorage = RawPointer;
#endif
static_assert(sizeof(MemberSlotStorage) == kSlotSize);

std::atomic<bool> g_testing_force_compaction{false};

v8::base::LazyMutex g_compactor_registry_mutex = LAZY_MUTEX_INITIALIZER;
std::atomic<size_t> g_enabled_compactor_count{0};
constexpr size_t kUntracedDedupCacheSize = 4096;
std::atomic<uintptr_t> g_untraced_dedup_cache[kUntracedDedupCacheSize]{};

void ClearUntracedDedupCache() {
  for (size_t i = 0; i < kUntracedDedupCacheSize; ++i) {
    g_untraced_dedup_cache[i].store(0, std::memory_order_relaxed);
  }
}

std::vector<ObjectCompactor*>& GetCompactorRegistryLocked() {
  static v8::base::LeakyObject<std::vector<ObjectCompactor*>> registry;
  return *registry.get();
}

bool IsAddressOnCommittedSubPage(const NormalPage& normal_page,
                                 const void* addr, size_t commit_page_size) {
  const uintptr_t page_uptr = reinterpret_cast<uintptr_t>(&normal_page);
  const uintptr_t addr_uptr = reinterpret_cast<uintptr_t>(addr);
  if (addr_uptr < page_uptr || addr_uptr >= page_uptr + kPageSize) {
    return false;
  }
  const size_t unit_idx = (addr_uptr - page_uptr) / commit_page_size;
  if (unit_idx < 64) {
    return (normal_page.active_system_page_bits() &
            (uint64_t{1} << unit_idx)) != 0;
  }
  return true;
}

DISABLE_ASAN bool PayloadHasUnmanagedInteriorSelfPointer(
    ConstAddress payload, size_t payload_size,
    const absl::flat_hash_set<const void*>& managed_uncompressed_slots) {
  if (payload_size < sizeof(uintptr_t)) return false;
  const uintptr_t start = reinterpret_cast<uintptr_t>(payload);
  const uintptr_t end = start + payload_size;
  for (size_t off = 0; off + sizeof(uintptr_t) <= payload_size;
       off += alignof(uint32_t)) {
    uintptr_t word = 0;
    std::memcpy(&word, payload + off, sizeof(uintptr_t));
    MSAN_MEMORY_IS_INITIALIZED(&word, sizeof(uintptr_t));
    if (word >= start && word <= end) {
      if ((off % alignof(uintptr_t) == 0) &&
          managed_uncompressed_slots.contains(payload + off)) {
        continue;
      }
      return true;
    }
  }
  return false;
}

#if !defined(CPPGC_IS_STANDALONE)
using SlotRecordingVisitorParent = v8::JSVisitor;
#else
using SlotRecordingVisitorParent = VisitorBase;
#endif

}  // namespace

void SetTestingForceObjectCompaction(bool enabled) {
  g_testing_force_compaction.store(enabled, std::memory_order_relaxed);
}

bool GetTestingForceObjectCompaction() {
  return g_testing_force_compaction.load(std::memory_order_relaxed);
}

// static
PinToken ObjectPinningApi::AcquireBorrowPin(const void* payload) {
  CHECK_NOT_NULL(payload);
  BasePage* page = BasePage::FromPayload(const_cast<void*>(payload));
  CHECK_NOT_NULL(page);
  HeapBase& heap = page->heap();
  CHECK(heap.CurrentThreadIsHeapThread());
  if (page->is_large()) {
    return PinToken{nullptr, &heap};
  }
  NormalPage* normal_page = NormalPage::From(page);
  normal_page->IncrementBorrowPinCount();
  return PinToken{normal_page, &heap};
}

// static
void ObjectPinningApi::ReleaseBorrowPin(PinToken token) noexcept {
  if (!token.IsValid()) return;
  auto* normal_page = static_cast<NormalPage*>(token.normal_page);
  CHECK_NOT_NULL(normal_page);
  CHECK(normal_page->heap().CurrentThreadIsHeapThread());
  normal_page->DecrementBorrowPinCount();
}

// static
void ObjectPinningApi::EnterNoSafepointBorrowScope(const void* payload) {
  CHECK_NOT_NULL(payload);
  BasePage* page = BasePage::FromPayload(const_cast<void*>(payload));
  CHECK_NOT_NULL(page);
  HeapBase& heap = page->heap();
  CHECK(heap.CurrentThreadIsHeapThread());
  heap.EnterDisallowGCScope();
}

// static
void ObjectPinningApi::LeaveNoSafepointBorrowScope(
    const void* payload) noexcept {
  CHECK_NOT_NULL(payload);
  BasePage* page = BasePage::FromPayload(const_cast<void*>(payload));
  CHECK_NOT_NULL(page);
  HeapBase& heap = page->heap();
  CHECK(heap.CurrentThreadIsHeapThread());
  heap.LeaveDisallowGCScope();
}

// static
bool ObjectPinningApi::IsRelocatableObject(const void* payload) noexcept {
  if (!payload) return false;
  const auto& header = HeapObjectHeader::FromObject(payload);
  return GlobalGCInfoTable::GCInfoFromIndex(header.GetGCInfoIndex())
             .relocation != nullptr;
}

// static
void ObjectPinningApi::ObserveForeignTargetWrite(const void* payload) noexcept {
  ObjectCompactor::ObserveGlobalForeignTargetWrite(payload);
}

// static
void ObjectPinningApi::RecordUntracedMemberTarget(
    const void* payload) noexcept {
  ObjectCompactor::RecordGlobalUntracedMemberTarget(payload);
}

// static
void ObjectPinningApi::RegisterAddressKeyedContainer(
    cppgc::HeapHandle& heap_handle, void* context, void (*callback)(void*)) {
  HeapBase::From(heap_handle)
      .object_compactor()
      .RegisterAddressKeyedContainer(context, callback);
}

// static
void ObjectPinningApi::UnregisterAddressKeyedContainer(
    cppgc::HeapHandle& heap_handle, void* context) {
  HeapBase::From(heap_handle)
      .object_compactor()
      .UnregisterAddressKeyedContainer(context);
}

// static
void ObjectPinningApi::SetCompactionEnabled(cppgc::HeapHandle& heap_handle,
                                            bool enabled) {
  HeapBase::From(heap_handle).object_compactor().SetEnabled(enabled);
}

// static
ObjectCompactionSummary ObjectPinningApi::GetLastCompactionSummary(
    const cppgc::HeapHandle& heap_handle) {
  return HeapBase::From(heap_handle).object_compactor().last_summary();
}

ObjectCompactor::ObjectCompactor(HeapBase& heap) : heap_(heap) {
  v8::base::MutexGuard guard(g_compactor_registry_mutex.Pointer());
  GetCompactorRegistryLocked().push_back(this);
}

ObjectCompactor::~ObjectCompactor() {
  v8::base::MutexGuard guard(g_compactor_registry_mutex.Pointer());
  auto& reg = GetCompactorRegistryLocked();
  auto it = std::find(reg.begin(), reg.end(), this);
  if (it != reg.end()) {
    reg.erase(it);
  }
  if (enabled_) {
    g_enabled_compactor_count.fetch_sub(1, std::memory_order_relaxed);
  }
}

void ObjectCompactor::SetEnabled(bool enabled) {
  explicitly_configured_ = true;
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  if (enabled_) {
    g_enabled_compactor_count.fetch_add(1, std::memory_order_release);
  } else {
    g_enabled_compactor_count.fetch_sub(1, std::memory_order_relaxed);
  }
}

void ObjectCompactor::SyncWithFlagsIfNotOverridden(bool flag_enabled) {
  if (!explicitly_configured_) {
    SetEnabled(flag_enabled);
    explicitly_configured_ = false;
  }
}

void ObjectCompactor::BeginForeignEdgeEpoch(bool is_major_gc) {
  if (!enabled_ || !is_major_gc) return;
  v8::base::MutexGuard guard(&foreign_edge_mutex_);
  pending_foreign_raw_addresses_.clear();
  observed_foreign_targets_.clear();
  foreign_observation_complete_ = true;
  foreign_epoch_active_ = true;
}

void ObjectCompactor::EndForeignEdgeEpoch() {
  if (!enabled_) return;
  v8::base::MutexGuard guard(&foreign_edge_mutex_);
  foreign_epoch_active_ = false;
}

void ObjectCompactor::ObserveForeignTarget(const void* target_address) {
  if (!target_address) return;
#if defined(CPPGC_CAGED_HEAP)
  if (!CagedHeapBase::IsWithinCage(target_address)) return;
#endif
  v8::base::MutexGuard guard(&foreign_edge_mutex_);
  if (!foreign_epoch_active_) return;
  pending_foreign_raw_addresses_.push_back(target_address);
}

// static
void ObjectCompactor::ObserveGlobalForeignTargetWrite(
    const void* target_address) {
  if (!target_address) return;
  if (V8_LIKELY(g_enabled_compactor_count.load(std::memory_order_acquire) ==
                0)) {
    return;
  }
#if defined(CPPGC_CAGED_HEAP)
  if (!CagedHeapBase::IsWithinCage(target_address)) return;
#endif
  v8::base::MutexGuard guard(g_compactor_registry_mutex.Pointer());
  for (ObjectCompactor* compactor : GetCompactorRegistryLocked()) {
    if (compactor->is_enabled()) {
      compactor->ObserveForeignTarget(target_address);
    }
  }
}

// static
void ObjectCompactor::RecordGlobalUntracedMemberTarget(
    const void* target_address) {
  if (!target_address) return;
  if (V8_LIKELY(g_enabled_compactor_count.load(std::memory_order_relaxed) ==
                0)) {
    return;
  }
#if defined(CPPGC_CAGED_HEAP)
  if (!CagedHeapBase::IsWithinCage(target_address)) return;
#endif
  const uintptr_t addr_uptr = reinterpret_cast<uintptr_t>(target_address);
  const size_t cache_idx =
      ((addr_uptr >> 4) ^ (addr_uptr >> 16)) & (kUntracedDedupCacheSize - 1);
  if (V8_LIKELY(g_untraced_dedup_cache[cache_idx].load(
                    std::memory_order_relaxed) == addr_uptr)) {
    return;
  }
  g_untraced_dedup_cache[cache_idx].store(addr_uptr, std::memory_order_relaxed);

  v8::base::MutexGuard guard(g_compactor_registry_mutex.Pointer());
  for (ObjectCompactor* compactor : GetCompactorRegistryLocked()) {
    if (compactor->is_enabled()) {
      compactor->RecordUntracedMemberTarget(target_address);
    }
  }
}

void ObjectCompactor::RecordUntracedMemberTarget(const void* target_address) {
  if (!target_address) return;
#if defined(CPPGC_CAGED_HEAP)
  if (!CagedHeapBase::IsWithinCage(target_address)) return;
#endif
  v8::base::MutexGuard guard(&foreign_edge_mutex_);
  pending_untraced_raw_addresses_.push_back(target_address);
}

void ObjectCompactor::FilterUntracedTargetsAfterMarking() {
  std::vector<const void*> raw_untraced;
  {
    v8::base::MutexGuard guard(&foreign_edge_mutex_);
    if (pending_untraced_raw_addresses_.empty() &&
        sticky_untraced_headers_.empty()) {
      return;
    }
    raw_untraced.swap(pending_untraced_raw_addresses_);
    ClearUntracedDedupCache();
  }

  heap_.object_allocator().ResetLinearAllocationBuffers();
  const size_t commit_page_size =
      heap_.platform()->GetPageAllocator()->CommitPageSize();

  absl::erase_if(sticky_untraced_headers_, [&](HeapObjectHeader* hdr) {
    const BasePage* page = BasePage::FromInnerAddress(&heap_, hdr);
    return !page || page->is_large() ||
           !IsAddressOnCommittedSubPage(*NormalPage::From(page), hdr,
                                        commit_page_size) ||
           hdr->IsFree<AccessMode::kAtomic>() ||
           !hdr->IsMarked<AccessMode::kAtomic>();
  });

  for (const void* addr : raw_untraced) {
    const BasePage* page = BasePage::FromInnerAddress(&heap_, addr);
    if (!page || page->is_large()) continue;
    const NormalPage* normal_page = NormalPage::From(page);
    if (!IsAddressOnCommittedSubPage(*normal_page, addr, commit_page_size)) {
      continue;
    }
    HeapObjectHeader* hdr = const_cast<HeapObjectHeader*>(
        normal_page->TryObjectHeaderFromInnerAddress(addr));
    if (!hdr ||
        !IsAddressOnCommittedSubPage(*normal_page, hdr, commit_page_size) ||
        hdr->IsFree<AccessMode::kAtomic>() ||
        !hdr->IsMarked<AccessMode::kAtomic>()) {
      continue;
    }
    sticky_untraced_headers_.insert(hdr);
  }
}

void ObjectCompactor::MarkForeignObservationIncomplete(const char*) {
  v8::base::MutexGuard guard(&foreign_edge_mutex_);
  foreign_observation_complete_ = false;
}

bool ObjectCompactor::IsForeignTargetObserved(const void* payload) const {
  v8::base::MutexGuard guard(&foreign_edge_mutex_);
  return observed_foreign_targets_.contains(payload);
}

uint64_t ObjectCompactor::BeginConservativePinEpoch() {
  return current_pin_epoch_.fetch_add(1, std::memory_order_relaxed) + 1;
}

void ObjectCompactor::RecordConservativePageHit(BasePage* page) {
  if (!page || page->is_large()) return;
  NormalPage::From(page)->PinByConservativeStack(current_pin_epoch());
}

void ObjectCompactor::PinCrossThreadPersistentTargets() {
  const uint64_t epoch = current_pin_epoch();
  class CrossThreadPinVisitor final : public RootVisitorBase {
   public:
    explicit CrossThreadPinVisitor(uint64_t pin_epoch)
        : pin_epoch_(pin_epoch) {}

   protected:
    void VisitRoot(const void* object, TraceDescriptor desc,
                   SourceLocation) final {
      PinIfNormal(desc.base_object_payload ? desc.base_object_payload : object);
    }
    void VisitWeakRoot(const void* object, TraceDescriptor desc, WeakCallback,
                       const void*, SourceLocation) final {
      PinIfNormal(desc.base_object_payload ? desc.base_object_payload : object);
    }

   private:
    void PinIfNormal(const void* ptr) {
      if (!ptr) return;
      BasePage* page = BasePage::FromPayload(const_cast<void*>(ptr));
      if (page && !page->is_large()) {
        NormalPage::From(page)->PinByCrossThreadRoot(pin_epoch_);
      }
    }
    const uint64_t pin_epoch_;
  } pin_visitor(epoch);

  PersistentRegionLock guard;
  heap_.GetStrongCrossThreadPersistentRegion().Iterate(pin_visitor);
  heap_.GetWeakCrossThreadPersistentRegion().Iterate(pin_visitor);
}

void ObjectCompactor::RegisterAddressKeyedContainer(
    void* context, AddressKeyedContainerRehashCallback callback) {
  if (!context || !callback) return;
  for (const auto& entry : address_keyed_containers_) {
    if (entry.first == context) return;
  }
  address_keyed_containers_.emplace_back(context, callback);
}

void ObjectCompactor::UnregisterAddressKeyedContainer(void* context) {
  address_keyed_containers_.erase(
      std::remove_if(
          address_keyed_containers_.begin(), address_keyed_containers_.end(),
          [context](const auto& pair) { return pair.first == context; }),
      address_keyed_containers_.end());
}

bool ObjectCompactor::CanRelocateObject(
    const HeapObjectHeader& header, const NormalPage& page,
    const ObjectRelocationDescriptor& desc, uint64_t pin_epoch,
    const absl::flat_hash_set<const void*>& prefinalizer_or_untraced) const {
  if (header.IsInConstruction<AccessMode::kAtomic>() ||
      page.IsPinnedForCompaction(pin_epoch)) {
    return false;
  }
  const Address payload = header.ObjectStart();
  const size_t payload_size = header.ObjectSize();
  if (IsForeignTargetObserved(payload) ||
      prefinalizer_or_untraced.contains(payload) ||
      (desc.has_wrapper_association && desc.has_wrapper_association(payload)) ||
      payload_size < desc.min_payload_size ||
      (desc.validate_payload_size &&
       !desc.validate_payload_size(payload, payload_size)) ||
      !desc.relocate) {
    return false;
  }
  return true;
}

void ObjectCompactor::ResolveDeferredPinnedTargets(
    absl::flat_hash_set<const void*>* prefinalizer_or_untraced_payloads) {
  prefinalizer_or_untraced_payloads->clear();
  const size_t commit_page_size =
      heap_.platform()->GetPageAllocator()->CommitPageSize();

  std::vector<const void*> raw_foreign;
  std::vector<const void*> raw_untraced;
  {
    v8::base::MutexGuard guard(&foreign_edge_mutex_);
    raw_foreign.swap(pending_foreign_raw_addresses_);
    raw_untraced.swap(pending_untraced_raw_addresses_);
    ClearUntracedDedupCache();
  }

  for (const void* addr : raw_foreign) {
    const BasePage* page = BasePage::FromInnerAddress(&heap_, addr);
    if (!page || page->is_large()) continue;
    const NormalPage* normal_page = NormalPage::From(page);
    if (!IsAddressOnCommittedSubPage(*normal_page, addr, commit_page_size)) {
      continue;
    }
    const HeapObjectHeader* hdr =
        normal_page->TryObjectHeaderFromInnerAddress(addr);
    if (!hdr ||
        !IsAddressOnCommittedSubPage(*normal_page, hdr, commit_page_size) ||
        hdr->IsFree<AccessMode::kAtomic>()) {
      continue;
    }
    v8::base::MutexGuard guard(&foreign_edge_mutex_);
    observed_foreign_targets_.insert(hdr->ObjectStart());
  }

  absl::erase_if(sticky_untraced_headers_, [&](HeapObjectHeader* hdr) {
    const BasePage* page = BasePage::FromInnerAddress(&heap_, hdr);
    if (!page || page->is_large() ||
        !IsAddressOnCommittedSubPage(*NormalPage::From(page), hdr,
                                     commit_page_size) ||
        hdr->IsFree<AccessMode::kAtomic>() ||
        !hdr->IsMarked<AccessMode::kAtomic>()) {
      return true;
    }
    prefinalizer_or_untraced_payloads->insert(hdr->ObjectStart());
    return false;
  });

  for (const void* addr : raw_untraced) {
    const BasePage* page = BasePage::FromInnerAddress(&heap_, addr);
    if (!page || page->is_large()) continue;
    const NormalPage* normal_page = NormalPage::From(page);
    if (!IsAddressOnCommittedSubPage(*normal_page, addr, commit_page_size)) {
      continue;
    }
    HeapObjectHeader* hdr = const_cast<HeapObjectHeader*>(
        normal_page->TryObjectHeaderFromInnerAddress(addr));
    if (!hdr ||
        !IsAddressOnCommittedSubPage(*normal_page, hdr, commit_page_size) ||
        hdr->IsFree<AccessMode::kAtomic>() ||
        !hdr->IsMarked<AccessMode::kAtomic>()) {
      continue;
    }
    sticky_untraced_headers_.insert(hdr);
    prefinalizer_or_untraced_payloads->insert(hdr->ObjectStart());
  }

  if (const PreFinalizerHandler* pf_handler = heap_.prefinalizer_handler()) {
    for (const auto& pf : pf_handler->ordered_pre_finalizers()) {
      if (!pf.object) continue;
      const BasePage* page = BasePage::FromInnerAddress(&heap_, pf.object);
      if (!page || page->is_large()) continue;
      const NormalPage* normal_page = NormalPage::From(page);
      if (!IsAddressOnCommittedSubPage(*normal_page, pf.object,
                                       commit_page_size)) {
        continue;
      }
      const HeapObjectHeader* hdr =
          normal_page->TryObjectHeaderFromInnerAddress(pf.object);
      if (hdr &&
          IsAddressOnCommittedSubPage(*normal_page, hdr, commit_page_size) &&
          !hdr->IsFree<AccessMode::kAtomic>()) {
        prefinalizer_or_untraced_payloads->insert(hdr->ObjectStart());
      }
    }
  }
}

void ObjectCompactor::SelectEvacuationCandidates(
    const absl::flat_hash_set<const void*>& prefinalizer_or_untraced_payloads,
    const absl::flat_hash_set<NormalPage*>& prev_destination_pages,
    std::vector<NormalPage*>* candidate_pages,
    std::vector<DestinationBuffer>* dest_buffers,
    absl::flat_hash_set<NormalPage*>* retained_packed_pages,
    ObjectCompactionSummary* summary) {
  candidate_pages->clear();
  dest_buffers->clear();
  retained_packed_pages->clear();

  const uint64_t pin_epoch = current_pin_epoch();
  const bool force_for_unit_test = GetTestingForceObjectCompaction();

  for (auto& space_ptr : heap_.raw_heap()) {
    BaseSpace* space = space_ptr.get();
    if (!space || space->is_large()) continue;

    NormalPageSpace& normal_space = NormalPageSpace::From(*space);
    for (BasePage* base_page : normal_space) {
      NormalPage::From(base_page)->set_is_evacuation_candidate(false);
    }

    if (normal_space.is_compactable() ||
        !normal_space.supports_object_compaction()) {
      continue;
    }

    // Retain already-packed prefix pages across GCs and expose their clean free
    // tails as reusable destination buffers.
    size_t space_reusable_tail_bytes = 0;
    for (BasePage* base_page : normal_space) {
      NormalPage* page = NormalPage::From(base_page);
      bool all_live_registered = true;
      bool in_live_prefix = true;
      bool has_any_live = false;
      bool tail_is_clean_free = true;
      Address live_prefix_end = page->PayloadStart();
      size_t live_count = 0;

      for (Address cur = page->PayloadStart(), end = page->PayloadEnd();
           cur < end;) {
        auto* hdr = reinterpret_cast<HeapObjectHeader*>(cur);
        const size_t sz = hdr->AllocatedSize();
        if (hdr->IsFree<AccessMode::kAtomic>()) {
          in_live_prefix = false;
          cur += sz;
          continue;
        }
        if (!hdr->IsMarked<AccessMode::kAtomic>()) {
          tail_is_clean_free = false;
          break;
        }
        if (!in_live_prefix ||
            !GlobalGCInfoTable::GCInfoFromIndex(hdr->GetGCInfoIndex())
                 .relocation) {
          all_live_registered = false;
          break;
        }
        has_any_live = true;
        ++live_count;
        live_prefix_end = cur + sz;
        cur += sz;
      }

      const size_t live_prefix_bytes =
          static_cast<size_t>(live_prefix_end - page->PayloadStart());
      const bool should_retain = prev_destination_pages.contains(page) ||
                                 live_prefix_bytes >= page->PayloadSize() / 2;
      if (!has_any_live || !all_live_registered || !tail_is_clean_free ||
          !should_retain) {
        continue;
      }

      const size_t tail_size =
          static_cast<size_t>(page->PayloadEnd() - live_prefix_end);
      bool tail_reserved = (tail_size == 0);
      if (!tail_reserved) {
        auto* tail_hdr = reinterpret_cast<HeapObjectHeader*>(live_prefix_end);
        if (tail_hdr->IsFree<AccessMode::kAtomic>() &&
            tail_hdr->AllocatedSize() == tail_size) {
          if (page->discarded_memory()) {
            page->RecommitMemory(live_prefix_end, tail_size);
          }
          tail_reserved = true;
        }
      }
      if (tail_reserved) {
        retained_packed_pages->insert(page);
        ++summary->reused_packed_destination_pages;
        summary->candidate_objects += live_count;
        summary->retained_packed_objects += live_count;
        if (tail_size >= sizeof(HeapObjectHeader)) {
          dest_buffers->push_back(
              {page, &normal_space, live_prefix_end, page->PayloadEnd(), true});
          space_reusable_tail_bytes += tail_size;
        }
      }
    }

    struct PageScan {
      NormalPage* page = nullptr;
      size_t movable_bytes = 0;
      size_t movable_count = 0;
      bool has_unmovable_live = false;
    };
    v8::base::SmallVector<PageScan, 16> space_candidates;
    size_t emptiable_pages = 0;
    size_t total_movable_bytes = 0;

    for (BasePage* base_page : normal_space) {
      NormalPage* page = NormalPage::From(base_page);
      if (retained_packed_pages->contains(page)) continue;
      if (page->IsPinnedForCompaction(pin_epoch)) {
        ++summary->pinned_pages;
      }

      PageScan scan{page, 0, 0, false};
      size_t page_live_bytes = 0;
      for (Address cur = page->PayloadStart(), end = page->PayloadEnd();
           cur < end;) {
        auto* header = reinterpret_cast<HeapObjectHeader*>(cur);
        const size_t alloc_size = header->AllocatedSize();
        if (header->IsFree<AccessMode::kAtomic>() ||
            !header->IsMarked<AccessMode::kAtomic>()) {
          cur += alloc_size;
          continue;
        }
        page_live_bytes += alloc_size;
        const auto* desc =
            GlobalGCInfoTable::GCInfoFromIndex(header->GetGCInfoIndex())
                .relocation;
        if (!desc) {
          scan.has_unmovable_live = true;
          cur += alloc_size;
          continue;
        }
        ++summary->candidate_objects;
        if (CanRelocateObject(*header, *page, *desc, pin_epoch,
                              prefinalizer_or_untraced_payloads)) {
          scan.movable_bytes += alloc_size;
          ++scan.movable_count;
        } else {
          scan.has_unmovable_live = true;
        }
        cur += alloc_size;
      }

      if (scan.movable_count == 0) continue;
      if (!force_for_unit_test &&
          page_live_bytes * 20 >= page->PayloadSize() * 17) {
        summary->retained_packed_objects += scan.movable_count;
        continue;
      }
      if (!scan.has_unmovable_live) ++emptiable_pages;
      total_movable_bytes += scan.movable_bytes;
      space_candidates.push_back(scan);
    }

    if (space_candidates.empty()) continue;

    if (!force_for_unit_test) {
      const size_t net_bytes =
          total_movable_bytes > space_reusable_tail_bytes
              ? (total_movable_bytes - space_reusable_tail_bytes)
              : 0;
      const size_t usable_per_page =
          NormalPage::PayloadSizeFor(normal_space.cell_size()) * 9 / 10;
      const size_t estimated_new_pages =
          net_bytes == 0
              ? 0
              : ((net_bytes + usable_per_page - 1) / usable_per_page);
      if (!(emptiable_pages > estimated_new_pages ||
            (emptiable_pages > 0 && estimated_new_pages == 0))) {
        continue;
      }
    }

    for (const auto& cand : space_candidates) {
      cand.page->set_is_evacuation_candidate(true);
      candidate_pages->push_back(cand.page);
    }
  }
}

void ObjectCompactor::RecordRelocatableSlots(
    const absl::flat_hash_set<const void*>& prefinalizer_payloads,
    absl::flat_hash_set<BasePage*>* pages_with_recorded_slots,
    std::vector<RootSlotRecord>* root_slots,
    std::vector<CustomHeapSlotRecord>* custom_slots,
    absl::flat_hash_set<const void*>* pinned_by_unretargetable_slot,
    absl::flat_hash_set<const void*>* managed_uncompressed_slots) {
  pages_with_recorded_slots->clear();
  root_slots->clear();
  custom_slots->clear();
  pinned_by_unretargetable_slot->clear();
  managed_uncompressed_slots->clear();

  class CandidateSlotRecordingVisitor final
      : public SlotRecordingVisitorParent {
   public:
    CandidateSlotRecordingVisitor(
        HeapBase& heap, absl::flat_hash_set<BasePage*>* pages_with_slots,
        std::vector<CustomHeapSlotRecord>* custom_slots,
        absl::flat_hash_set<const void*>* pinned_targets,
        absl::flat_hash_set<const void*>* managed_uncompressed)
        :
#if !defined(CPPGC_IS_STANDALONE)
          SlotRecordingVisitorParent(VisitorFactory::CreateKey()),
#endif
          heap_(heap),
          pages_with_slots_(pages_with_slots),
          custom_slots_(custom_slots),
          pinned_targets_(pinned_targets),
          managed_uncompressed_(managed_uncompressed) {
      set_is_relocation_recording(true);
    }

    void SetCurrentOwner(BasePage* page, HeapObjectHeader* header,
                         ConstAddress start, ConstAddress end,
                         bool is_prefinalizer, bool on_evacuation_candidate) {
      current_owner_page_ = page;
      current_owner_header_ = header;
      current_owner_start_ = start;
      current_owner_end_ = end;
      current_owner_is_prefinalizer_ = is_prefinalizer;
      current_owner_on_candidate_ = on_evacuation_candidate;
    }

   protected:
    void VisitWeakContainer(const void* self, TraceDescriptor strong_desc,
                            TraceDescriptor weak_desc, WeakCallback,
                            const void*) final {
      if (!self) return;
      if (strong_desc.callback) strong_desc.callback(this, self);
      if (weak_desc.callback && weak_desc.callback != strong_desc.callback) {
        weak_desc.callback(this, self);
      }
    }

    void VisitUnretargetableContainerBacking(TraceDescriptor desc) final {
      if (desc.base_object_payload) {
        pinned_targets_->insert(desc.base_object_payload);
      }
    }

    void VisitRelocatableMemberSlot(
        const void* slot_address, const void* pointee_address,
        TraceDescriptor desc, RelocationStorageCodec codec,
        RelocationReferenceStrength,
        TypedSlotRetargetCallback retarget_callback) final {
      if (!slot_address) return;
      if (current_owner_on_candidate_ &&
          (codec == RelocationStorageCodec::kUncompressedMember ||
           codec == RelocationStorageCodec::kUncompressedWeakMember ||
           codec == RelocationStorageCodec::kTaggedUncompressedMember)) {
        managed_uncompressed_->insert(slot_address);
      }

      if (!pointee_address || !desc.base_object_payload || !retarget_callback) {
        return;
      }
      BasePage* target_page =
          BasePage::FromPayload(const_cast<void*>(desc.base_object_payload));
      if (!target_page || target_page->is_large() ||
          !NormalPage::From(target_page)->is_evacuation_candidate()) {
        return;
      }

      const auto* target_base =
          static_cast<ConstAddress>(desc.base_object_payload);
      const uintptr_t pointee_uptr =
          reinterpret_cast<uintptr_t>(pointee_address);
      const uintptr_t base_uptr = reinterpret_cast<uintptr_t>(target_base);
      if (pointee_uptr < base_uptr) return;

      if (is_tracing_address_keyed_slot() || current_owner_is_prefinalizer_) {
        pinned_targets_->insert(target_base);
        return;
      }

      BasePage* owner_page = nullptr;
      HeapObjectHeader* owner_header = nullptr;
      const auto* slot_byte = static_cast<ConstAddress>(slot_address);
      if (V8_LIKELY(slot_byte >= current_owner_start_ &&
                    slot_byte < current_owner_end_)) {
        owner_page = current_owner_page_;
        owner_header = current_owner_header_;
      } else {
#if defined(CPPGC_CAGED_HEAP)
        if (!CagedHeapBase::IsWithinCage(slot_address)) {
          pinned_targets_->insert(target_base);
          return;
        }
#endif
        owner_page =
            BasePage::FromInnerAddress(&heap_, const_cast<void*>(slot_address));
        if (owner_page) {
          owner_header = owner_page->TryObjectHeaderFromInnerAddress(
              const_cast<void*>(slot_address));
          if (owner_header && !owner_header->IsMarked<AccessMode::kAtomic>()) {
            owner_header = nullptr;
          }
        }
        if (!owner_page || !owner_header) {
          pinned_targets_->insert(target_base);
          return;
        }
      }

      const size_t slot_offset = reinterpret_cast<uintptr_t>(slot_address) -
                                 reinterpret_cast<uintptr_t>(owner_page);
      if (codec == RelocationStorageCodec::kCompressedMember ||
          codec == RelocationStorageCodec::kCompressedWeakMember) {
        if (V8_LIKELY(IsAligned(slot_offset, kSlotSize))) {
          owner_page->GetOrAllocateSlotSet()
              .Insert<SlotSet::AccessMode::NON_ATOMIC>(slot_offset);
          pages_with_slots_->insert(owner_page);
          return;
        }
      } else if ((codec == RelocationStorageCodec::kUncompressedMember ||
                  codec == RelocationStorageCodec::kUncompressedWeakMember ||
                  codec == RelocationStorageCodec::kTaggedUncompressedMember) &&
                 slot_offset <
                     static_cast<size_t>(CompactionTypedSlotSet::kMaxOffset)) {
        const CompactionSlotType slot_type =
            (codec == RelocationStorageCodec::kTaggedUncompressedMember)
                ? CompactionSlotType::kTaggedUncompressedMember
                : CompactionSlotType::kUncompressedMember;
        owner_page->GetOrAllocateCompactionTypedSlotSet().Insert(
            slot_type, static_cast<uint32_t>(slot_offset));
        pages_with_slots_->insert(owner_page);
        return;
      }

      custom_slots_->push_back(
          {owner_header,
           static_cast<size_t>(slot_byte - owner_header->ObjectStart()),
           &HeapObjectHeader::FromObject(
               const_cast<void*>(desc.base_object_payload)),
           pointee_uptr - base_uptr, retarget_callback});
    }

   private:
    HeapBase& heap_;
    BasePage* current_owner_page_ = nullptr;
    HeapObjectHeader* current_owner_header_ = nullptr;
    ConstAddress current_owner_start_ = nullptr;
    ConstAddress current_owner_end_ = nullptr;
    bool current_owner_is_prefinalizer_ = false;
    bool current_owner_on_candidate_ = false;
    absl::flat_hash_set<BasePage*>* pages_with_slots_;
    std::vector<CustomHeapSlotRecord>* custom_slots_;
    absl::flat_hash_set<const void*>* pinned_targets_;
    absl::flat_hash_set<const void*>* managed_uncompressed_;
  } slot_visitor(heap_, pages_with_recorded_slots, custom_slots,
                 pinned_by_unretargetable_slot, managed_uncompressed_slots);

  for (auto& space_ptr : heap_.raw_heap()) {
    BaseSpace* space = space_ptr.get();
    if (!space) continue;
    if (space->is_large()) {
      for (BasePage* base_page : *space) {
        LargePage* large_page = LargePage::From(base_page);
        HeapObjectHeader* header = large_page->ObjectHeader();
        if (!header->IsMarked<AccessMode::kAtomic>() ||
            header->IsInConstruction<AccessMode::kAtomic>()) {
          continue;
        }
        const Address payload = header->ObjectStart();
        if (const TraceCallback trace_cb =
                GlobalGCInfoTable::GCInfoFromIndex(header->GetGCInfoIndex())
                    .trace) {
          slot_visitor.SetCurrentOwner(large_page, header, payload,
                                       payload + large_page->ObjectSize(),
                                       prefinalizer_payloads.contains(payload),
                                       /*on_evacuation_candidate=*/false);
          trace_cb(&slot_visitor, payload);
        }
      }
      continue;
    }

    NormalPageSpace& normal_space = NormalPageSpace::From(*space);
    for (BasePage* base_page : normal_space) {
      NormalPage* page = NormalPage::From(base_page);
      const bool on_candidate = page->is_evacuation_candidate();
      for (Address cur = page->PayloadStart(), end = page->PayloadEnd();
           cur < end;) {
        auto* header = reinterpret_cast<HeapObjectHeader*>(cur);
        const size_t alloc_size = header->AllocatedSize();
        if (header->IsFree<AccessMode::kAtomic>() ||
            !header->IsMarked<AccessMode::kAtomic>() ||
            header->IsInConstruction<AccessMode::kAtomic>()) {
          cur += alloc_size;
          continue;
        }
        const Address payload = header->ObjectStart();
        if (const TraceCallback trace_cb =
                GlobalGCInfoTable::GCInfoFromIndex(header->GetGCInfoIndex())
                    .trace) {
          slot_visitor.SetCurrentOwner(
              page, header, payload, payload + header->ObjectSize(),
              prefinalizer_payloads.contains(payload), on_candidate);
          trace_cb(&slot_visitor, payload);
        }
        cur += alloc_size;
      }
    }
  }

  class CandidateRootRecordingVisitor final : public RootVisitorBase {
   public:
    CandidateRootRecordingVisitor(HeapBase& heap,
                                  std::vector<RootSlotRecord>* root_slots)
        : heap_(heap), root_slots_(root_slots) {
      set_is_relocation_recording(true);
    }

   protected:
    void VisitRelocatableRootSlot(const void* root_slot,
                                  const void* pointee_address,
                                  TraceDescriptor desc,
                                  RelocationStorageCodec codec,
                                  TypedSlotRetargetCallback retarget_callback,
                                  SourceLocation) final {
      if (!root_slot || !pointee_address || !desc.base_object_payload) return;
      BasePage* target_page =
          BasePage::FromPayload(const_cast<void*>(desc.base_object_payload));
      if (!target_page || target_page->is_large()) return;
      if (codec == RelocationStorageCodec::kCrossThreadPersistent) {
        NormalPage::From(target_page)
            ->PinByCrossThreadRoot(
                heap_.object_compactor().current_pin_epoch());
        return;
      }
      if (!retarget_callback ||
          !NormalPage::From(target_page)->is_evacuation_candidate()) {
        return;
      }
      const auto* target_base =
          static_cast<ConstAddress>(desc.base_object_payload);
      const uintptr_t pointee_uptr =
          reinterpret_cast<uintptr_t>(pointee_address);
      const uintptr_t base_uptr = reinterpret_cast<uintptr_t>(target_base);
      if (pointee_uptr < base_uptr) return;

      root_slots_->push_back({root_slot, const_cast<Address>(target_base),
                              pointee_uptr - base_uptr, retarget_callback});
    }

   private:
    HeapBase& heap_;
    std::vector<RootSlotRecord>* root_slots_;
  } root_visitor(heap_, root_slots);

  heap_.GetStrongPersistentRegion().Iterate(root_visitor);
  heap_.GetWeakPersistentRegion().Iterate(root_visitor);
}

bool ObjectCompactor::EvacuateCandidatePages(
    const std::vector<NormalPage*>& candidate_pages,
    const absl::flat_hash_set<const void*>& prefinalizer_or_untraced_payloads,
    const absl::flat_hash_set<const void*>& pinned_by_unretargetable_slot,
    const absl::flat_hash_set<const void*>& managed_uncompressed_slots,
    std::vector<DestinationBuffer>* dest_buffers,
    absl::flat_hash_set<BasePage*>* pages_with_recorded_slots,
    std::vector<MovedObjectRecord>* moved_objects,
    ObjectCompactionSummary* summary) {
  moved_objects->clear();
  const uint64_t pin_epoch = current_pin_epoch();

  absl::flat_hash_map<NormalPageSpace*, std::vector<HeapObjectHeader*>>
      eligible_headers_by_space;

  for (NormalPage* page : candidate_pages) {
    NormalPageSpace& space = NormalPageSpace::From(page->space());
    for (Address cur = page->PayloadStart(), end = page->PayloadEnd();
         cur < end;) {
      auto* header = reinterpret_cast<HeapObjectHeader*>(cur);
      const size_t alloc_size = header->AllocatedSize();
      if (header->IsFree<AccessMode::kAtomic>() ||
          !header->IsMarked<AccessMode::kAtomic>()) {
        cur += alloc_size;
        continue;
      }
      const auto* desc =
          GlobalGCInfoTable::GCInfoFromIndex(header->GetGCInfoIndex())
              .relocation;
      if (!desc || !CanRelocateObject(*header, *page, *desc, pin_epoch,
                                      prefinalizer_or_untraced_payloads)) {
        cur += alloc_size;
        continue;
      }
      const Address payload = header->ObjectStart();
      if (pinned_by_unretargetable_slot.contains(payload) ||
          (!desc->supports_inline_storage_rebasing &&
           PayloadHasUnmanagedInteriorSelfPointer(
               payload, header->ObjectSize(), managed_uncompressed_slots))) {
        cur += alloc_size;
        continue;
      }
      eligible_headers_by_space[&space].push_back(header);
      cur += alloc_size;
    }
  }

  for (auto& [space, headers] : eligible_headers_by_space) {
    if (headers.empty()) continue;
    const bool is_size_class = space->is_size_class();
    if (!is_size_class) {
      std::stable_sort(
          headers.begin(), headers.end(),
          [](const HeapObjectHeader* a, const HeapObjectHeader* b) {
            return a->AllocatedSize() > b->AllocatedSize();
          });
    }

    for (HeapObjectHeader* src_header : headers) {
      NormalPage* src_page =
          NormalPage::From(BasePage::FromPayload(src_header));
      const GCInfoIndex gc_info_index = src_header->GetGCInfoIndex();
      const auto* desc =
          GlobalGCInfoTable::GCInfoFromIndex(gc_info_index).relocation;
      const size_t alloc_size = src_header->AllocatedSize();
      const size_t payload_size = src_header->ObjectSize();
      const Address src_payload = src_header->ObjectStart();
      const size_t req_align =
          std::max<size_t>(kAllocationGranularity, desc->required_alignment);

      size_t chosen_buf = dest_buffers->size();
      size_t best_remaining_after = std::numeric_limits<size_t>::max();
      for (size_t b = 0; b < dest_buffers->size(); ++b) {
        auto& buf = (*dest_buffers)[b];
        if (buf.space != space) continue;
        const uintptr_t frontier_uptr =
            reinterpret_cast<uintptr_t>(buf.frontier);
        const uintptr_t aligned_header =
            RoundUp(frontier_uptr + sizeof(HeapObjectHeader), req_align) -
            sizeof(HeapObjectHeader);
        const size_t pad = is_size_class ? 0 : (aligned_header - frontier_uptr);
        DCHECK_IMPLIES(
            is_size_class,
            IsAligned(frontier_uptr + sizeof(HeapObjectHeader), req_align));
        if (pad != 0 && pad < sizeof(HeapObjectHeader)) continue;
        if (buf.remaining() >= pad + alloc_size) {
          const size_t rem = buf.remaining() - (pad + alloc_size);
          if (rem < best_remaining_after) {
            best_remaining_after = rem;
            chosen_buf = b;
          }
        }
      }

      if (chosen_buf == dest_buffers->size()) {
        NormalPage* new_page =
            NormalPage::TryCreate(*heap_.page_backend(), *space);
        if (!new_page) return false;
#if defined(CPPGC_YOUNG_GENERATION)
        if (heap_.generational_gc_supported()) {
          CagedHeapLocalData::Get().age_table.SetAgeForRange(
              CagedHeap::OffsetFromAddress(new_page->PayloadStart()),
              CagedHeap::OffsetFromAddress(new_page->PayloadEnd()),
              AgeTable::Age::kOld, AgeTable::AdjacentCardsPolicy::kIgnore);
          new_page->set_as_containing_young_objects(false);
        }
#endif  // defined(CPPGC_YOUNG_GENERATION)
        chosen_buf = dest_buffers->size();
        dest_buffers->push_back({new_page, space, new_page->PayloadStart(),
                                 new_page->PayloadEnd(), false});
        ++summary->new_destination_pages;
      }

      auto& buf = (*dest_buffers)[chosen_buf];
      const uintptr_t frontier_uptr = reinterpret_cast<uintptr_t>(buf.frontier);
      const uintptr_t aligned_header =
          RoundUp(frontier_uptr + sizeof(HeapObjectHeader), req_align) -
          sizeof(HeapObjectHeader);
      const size_t pad = is_size_class ? 0 : (aligned_header - frontier_uptr);
      if (pad > 0) {
        DCHECK(!buf.page->is_size_class());
        DCHECK_GE(pad, sizeof(HeapObjectHeader));
        SetMemoryAccessible(buf.frontier, sizeof(Filler));
        Filler::CreateAt(buf.frontier, pad);
        buf.page->object_start_bitmap().SetBit<AccessMode::kAtomic>(
            buf.frontier);
        buf.frontier += pad;
      }

      const Address dest_header_addr = buf.frontier;
      const Address dest_payload_addr =
          dest_header_addr + sizeof(HeapObjectHeader);
      buf.frontier += alloc_size;

      SetMemoryAccessible(dest_header_addr, alloc_size);
      ASAN_UNPOISON_MEMORY_REGION(src_payload, payload_size);

      auto* dest_header =
          new (dest_header_addr) HeapObjectHeader(alloc_size, gc_info_index);
      desc->relocate(src_payload, dest_payload_addr, payload_size);
      dest_header->MarkAsFullyConstructed();
      if (!buf.page->is_size_class()) {
        buf.page->object_start_bitmap().SetBit<AccessMode::kAtomic>(
            dest_header_addr);
      }
      dest_header->TryMarkAtomic();
      buf.page->IncrementMarkedBytes(alloc_size);

      // Migrate any recorded outgoing slots inside `[src_payload, src_end)`
      // onto `buf.page` before installing the forwarding pointer in
      // `src_payload`.
      const size_t src_start_off = reinterpret_cast<uintptr_t>(src_payload) -
                                   reinterpret_cast<uintptr_t>(src_page);
      const size_t src_end_off = src_start_off + payload_size;
      const size_t dst_start_off =
          reinterpret_cast<uintptr_t>(dest_payload_addr) -
          reinterpret_cast<uintptr_t>(buf.page);
      if (SlotSet* src_slots = src_page->slot_set()) {
        const size_t aligned_start = RoundUp(src_start_off, kSlotSize);
        const size_t aligned_end = RoundDown(src_end_off, kSlotSize);
        if (aligned_start < aligned_end) {
          src_slots->MigrateSlotsInRange(
              aligned_start, aligned_end, &buf.page->GetOrAllocateSlotSet(),
              dst_start_off + (aligned_start - src_start_off));
          pages_with_recorded_slots->insert(buf.page);
        }
      }
      if (CompactionTypedSlotSet* src_typed =
              src_page->compaction_typed_slot_set()) {
        src_typed->MigrateSlotsInRange(
            static_cast<uint32_t>(src_start_off),
            static_cast<uint32_t>(src_end_off),
            &buf.page->GetOrAllocateCompactionTypedSlotSet(),
            static_cast<uint32_t>(dst_start_off));
        pages_with_recorded_slots->insert(buf.page);
      }

      src_header->SetForwardedAddress(dest_payload_addr);
      moved_objects->push_back(
          {src_header, src_page, dest_header_addr, alloc_size});

      ++summary->moved_objects;
      summary->moved_allocated_bytes += alloc_size;
    }
  }

  return true;
}

void ObjectCompactor::UpdatePointers(
    const absl::flat_hash_set<BasePage*>& pages_with_recorded_slots,
    const std::vector<RootSlotRecord>& root_slots,
    const std::vector<CustomHeapSlotRecord>& custom_slots,
    ObjectCompactionSummary* summary) {
  auto resolve_forwarded_pointee = [this](const void* pointee) -> void* {
    if (!pointee || pointee == kSentinelPointer) return nullptr;
    BasePage* target_page =
        BasePage::FromInnerAddress(&heap_, const_cast<void*>(pointee));
    if (!target_page || target_page->is_large() ||
        !NormalPage::From(target_page)->is_evacuation_candidate()) {
      return nullptr;
    }
    const HeapObjectHeader& target_hdr =
        target_page->ObjectHeaderFromInnerAddress(pointee);
    if (!target_hdr.IsForwarded()) return nullptr;
    const size_t interior_offset =
        static_cast<ConstAddress>(pointee) - target_hdr.ObjectStart();
    return target_hdr.GetForwardedAddress() + interior_offset;
  };

  for (BasePage* page : pages_with_recorded_slots) {
    if (SlotSet* slot_set = page->slot_set()) {
      const uintptr_t page_start = reinterpret_cast<uintptr_t>(page);
      const size_t buckets = SlotSet::BucketsForSize(page->AllocatedSize());
      slot_set->Iterate<SlotSet::AccessMode::NON_ATOMIC>(
          page_start, 0, buckets,
          [&](SlotSet::Address slot_addr) {
            const auto* storage =
                reinterpret_cast<const MemberSlotStorage*>(slot_addr);
            if (void* new_pointee =
                    resolve_forwarded_pointee(storage->Load())) {
              storage->StoreFromGC(new_pointee);
              ++summary->typed_member_slots_updated;
            }
            return ::heap::base::REMOVE_SLOT;
          },
          SlotSet::EmptyBucketMode::FREE_EMPTY_BUCKETS);
      page->ResetSlotSet();
    }

    if (CompactionTypedSlotSet* typed_set = page->compaction_typed_slot_set()) {
      typed_set->Iterate(
          [&](CompactionSlotType type, CompactionTypedSlotSet::Address addr) {
            const auto* storage = reinterpret_cast<const RawPointer*>(addr);
            if (type == CompactionSlotType::kTaggedUncompressedMember) {
              const uintptr_t old_raw =
                  reinterpret_cast<uintptr_t>(storage->Load());
              const uintptr_t low_bits =
                  old_raw & (api_constants::kAllocationGranularity - 1);
              const void* untagged =
                  reinterpret_cast<const void*>(old_raw & ~low_bits);
              if (void* new_untagged = resolve_forwarded_pointee(untagged)) {
                storage->StoreFromGC(reinterpret_cast<void*>(
                    reinterpret_cast<uintptr_t>(new_untagged) | low_bits));
                ++summary->typed_member_slots_updated;
              }
            } else if (void* new_pointee =
                           resolve_forwarded_pointee(storage->Load())) {
              storage->StoreFromGC(new_pointee);
              ++summary->typed_member_slots_updated;
            }
            return ::heap::base::REMOVE_SLOT;
          },
          CompactionTypedSlotSet::FREE_EMPTY_CHUNKS);
      page->ResetCompactionTypedSlotSet();
    }
  }

  for (const auto& custom : custom_slots) {
    if (!custom.target_header->IsForwarded() || !custom.retarget_callback) {
      continue;
    }
    const Address owner_base = custom.owner_header->IsForwarded()
                                   ? custom.owner_header->GetForwardedAddress()
                                   : custom.owner_header->ObjectStart();
    custom.retarget_callback(
        owner_base + custom.offset_in_owner,
        custom.target_header->GetForwardedAddress() + custom.interior_offset);
    ++summary->typed_member_slots_updated;
  }

  for (const auto& root : root_slots) {
    if (!root.retarget_callback) continue;
    const HeapObjectHeader& target_hdr =
        HeapObjectHeader::FromObject(root.target_base);
    if (!target_hdr.IsForwarded()) continue;
    root.retarget_callback(root.root_slot, target_hdr.GetForwardedAddress() +
                                               root.interior_offset);
    ++summary->persistent_roots_updated;
  }
}

void ObjectCompactor::RebaseLegacyCompactionWorklist() {
  CompactionWorklists* cw = heap_.compactor().compaction_worklists();
  if (!cw || !cw->movable_slots_worklist()) return;
  auto* worklist = cw->movable_slots_worklist();

  std::vector<CompactionWorklists::MovableReference*> items;
  {
    CompactionWorklists::MovableReferencesWorklist::Local local(*worklist);
    CompactionWorklists::MovableReference* slot = nullptr;
    while (local.Pop(&slot)) items.push_back(slot);
  }
  if (items.empty()) return;

  for (auto*& slot_ptr : items) {
    BasePage* page = BasePage::FromInnerAddress(&heap_, slot_ptr);
    if (!page || page->is_large() ||
        !NormalPage::From(page)->is_evacuation_candidate()) {
      continue;
    }
    if (const HeapObjectHeader* hdr =
            page->TryObjectHeaderFromInnerAddress(slot_ptr);
        hdr && hdr->IsForwarded()) {
      const size_t offset =
          reinterpret_cast<ConstAddress>(slot_ptr) - hdr->ObjectStart();
      slot_ptr = reinterpret_cast<CompactionWorklists::MovableReference*>(
          hdr->GetForwardedAddress() + offset);
    }
  }

  CompactionWorklists::MovableReferencesWorklist::Local local(*worklist);
  for (auto* slot_ptr : items) local.Push(slot_ptr);
  local.Publish();
}

bool ObjectCompactor::ExecuteCompaction(ObjectCompactionSummary* summary) {
  summary->executed = true;
  {
    v8::base::MutexGuard guard(&foreign_edge_mutex_);
    if (!foreign_observation_complete_) return false;
  }

  heap_.object_allocator().ResetLinearAllocationBuffers();
  absl::flat_hash_set<const void*> prefinalizer_or_untraced_payloads;
  ResolveDeferredPinnedTargets(&prefinalizer_or_untraced_payloads);
  PinCrossThreadPersistentTargets();

  const absl::flat_hash_set<NormalPage*> prev_destination_pages =
      current_destination_pages_;
  std::vector<NormalPage*> candidate_pages;
  std::vector<DestinationBuffer> dest_buffers;
  absl::flat_hash_set<NormalPage*> retained_packed_pages;
  SelectEvacuationCandidates(prefinalizer_or_untraced_payloads,
                             prev_destination_pages, &candidate_pages,
                             &dest_buffers, &retained_packed_pages, summary);

  if (candidate_pages.empty()) {
    current_destination_pages_ = std::move(retained_packed_pages);
    summary->succeeded = true;
    return true;
  }

  absl::flat_hash_set<BasePage*> pages_with_recorded_slots;
  std::vector<RootSlotRecord> root_slots;
  std::vector<CustomHeapSlotRecord> custom_slots;
  absl::flat_hash_set<const void*> pinned_by_unretargetable_slot;
  absl::flat_hash_set<const void*> managed_uncompressed_slots;
  RecordRelocatableSlots(prefinalizer_or_untraced_payloads,
                         &pages_with_recorded_slots, &root_slots, &custom_slots,
                         &pinned_by_unretargetable_slot,
                         &managed_uncompressed_slots);

  cppgc::subtle::DisallowGarbageCollectionScope no_gc_scope(heap_);

  std::vector<MovedObjectRecord> moved_objects;
  if (!EvacuateCandidatePages(
          candidate_pages, prefinalizer_or_untraced_payloads,
          pinned_by_unretargetable_slot, managed_uncompressed_slots,
          &dest_buffers, &pages_with_recorded_slots, &moved_objects, summary)) {
    for (const auto& moved : moved_objects) {
      Filler::CreateAt(moved.dest_header_addr, moved.allocated_size);
    }
    for (BasePage* p : pages_with_recorded_slots) {
      p->ResetSlotSet();
      p->ResetCompactionTypedSlotSet();
    }
    for (NormalPage* p : candidate_pages) {
      p->set_is_evacuation_candidate(false);
    }
    for (auto& buf : dest_buffers) {
      if (!buf.is_reused_packed_page && buf.page) {
        buf.page->ResetSlotSet();
        buf.page->ResetCompactionTypedSlotSet();
        NormalPage::Destroy(buf.page);
      }
    }
    return false;
  }

  current_destination_pages_ = retained_packed_pages;
  for (const auto& buf : dest_buffers) {
    current_destination_pages_.insert(buf.page);
  }

  if (moved_objects.empty()) {
    for (BasePage* p : pages_with_recorded_slots) {
      p->ResetSlotSet();
      p->ResetCompactionTypedSlotSet();
    }
    for (NormalPage* p : candidate_pages) {
      p->set_is_evacuation_candidate(false);
    }
    summary->succeeded = true;
    return true;
  }

  RebaseLegacyCompactionWorklist();
#if defined(CPPGC_YOUNG_GENERATION)
  if (heap_.generational_gc_supported()) {
    heap_.remembered_set().RebaseForwardedCustomCallbackParameters();
  }
#endif  // defined(CPPGC_YOUNG_GENERATION)

  UpdatePointers(pages_with_recorded_slots, root_slots, custom_slots, summary);

  for (const auto& [ctx, cb] : address_keyed_containers_) {
    cb(ctx);
    ++summary->address_keyed_rehashes;
  }
  if (heap_.HasMoveListeners()) {
    for (const auto& move : moved_objects) {
      heap_.CallMoveListeners(reinterpret_cast<Address>(move.src_header),
                              move.dest_header_addr, move.allocated_size);
    }
  }

  for (auto& buf : dest_buffers) {
    const size_t tail_bytes = buf.remaining();
    if (tail_bytes > 0) {
      DCHECK_GE(tail_bytes, sizeof(HeapObjectHeader));
      SetMemoryAccessible(buf.frontier, sizeof(Filler));
      Filler::CreateAt(buf.frontier, tail_bytes);
      SetMemoryInaccessible(buf.frontier + sizeof(Filler),
                            tail_bytes - sizeof(Filler));
      if (!buf.page->is_size_class()) {
        buf.page->object_start_bitmap().SetBit<AccessMode::kAtomic>(
            buf.frontier);
      }
    }
    buf.page->SetAllocatedBytesAtLastGC(
        static_cast<size_t>(buf.frontier - buf.page->PayloadStart()));
    if (!buf.is_reused_packed_page) {
      buf.space->AddPage(buf.page);
    }
  }

  absl::flat_hash_set<NormalPage*> touched_source_pages;
  for (const auto& move : moved_objects) {
    Address src_hdr_addr = reinterpret_cast<Address>(move.src_header);
    move.src_page->DecrementMarkedBytes(move.allocated_size);
    SetMemoryAccessible(src_hdr_addr, sizeof(Filler));
    Filler::CreateAt(src_hdr_addr, move.allocated_size);
    SetMemoryInaccessible(src_hdr_addr + sizeof(Filler),
                          move.allocated_size - sizeof(Filler));
    if (!move.src_page->is_size_class()) {
      move.src_page->object_start_bitmap().SetBit<AccessMode::kAtomic>(
          src_hdr_addr);
    }
    touched_source_pages.insert(move.src_page);
  }
  for (NormalPage* p : candidate_pages) {
    p->set_is_evacuation_candidate(false);
  }
  for (NormalPage* sp : touched_source_pages) {
    if (sp->marked_bytes() == 0) ++summary->source_pages_emptied;
  }

  summary->succeeded = true;
  return true;
}

bool ObjectCompactor::CompactIfEnabled(bool is_memory_reducing_major_gc) {
  if (!enabled_ || !is_memory_reducing_major_gc) return false;
  StatsCollector::EnabledScope scope(heap_.stats_collector(),
                                     StatsCollector::kAtomicCompact);
  ObjectCompactionSummary summary;
  const bool ok = ExecuteCompaction(&summary);
  last_summary_ = summary;
  return ok;
}

}  // namespace cppgc::internal
