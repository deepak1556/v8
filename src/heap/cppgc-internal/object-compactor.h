// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_CPPGC_INTERNAL_OBJECT_COMPACTOR_H_
#define V8_HEAP_CPPGC_INTERNAL_OBJECT_COMPACTOR_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "include/cppgc/internal/object-relocation.h"
#include "src/base/macros.h"
#include "src/base/platform/mutex.h"
#include "src/heap/cppgc-internal/globals.h"

namespace cppgc::internal {

class BasePage;
class HeapBase;
class HeapObjectHeader;
class NormalPage;
class NormalPageSpace;

using AddressKeyedContainerRehashCallback = void (*)(void* context);

// Pause-time object compactor for `cppgc` `NormalPage`s, unified with V8's
// `MarkCompactCollector` design:
//   1. Selects sparse non-pinned `NormalPage` evacuation candidates per space.
//   2. Records incoming slots to candidate pages using `BasePage`'s shared
//      `::heap::base::BasicSlotSet` and `::heap::base::BasicTypedSlotSet`.
//   3. Evacuates eligible objects into per-space bump buffers, stores
//      forwarding addresses directly in `HeapObjectHeader`, and migrates
//      outgoing recorded slots onto destination pages.
//   4. Updates recorded slots & `Persistent` roots in O(1) via forwarded
//      headers and retires forwarded source headers as free fillers prior to
//      `Sweeper::Start()`.
class V8_EXPORT_PRIVATE ObjectCompactor final {
 public:
  explicit ObjectCompactor(HeapBase& heap);
  ~ObjectCompactor();

  ObjectCompactor(const ObjectCompactor&) = delete;
  ObjectCompactor& operator=(const ObjectCompactor&) = delete;

  void SetEnabled(bool enabled);
  void SyncWithFlagsIfNotOverridden(bool flag_enabled);
  bool is_enabled() const { return enabled_; }

  // Foreign V8-to-CppHeap & untraced-slot observation epoch.
  void BeginForeignEdgeEpoch(bool is_major_gc);
  void EndForeignEdgeEpoch();
  void ObserveForeignTarget(const void* target_address);
  static void ObserveGlobalForeignTargetWrite(const void* target_address);
  static void RecordGlobalUntracedMemberTarget(const void* target_address);
  void RecordUntracedMemberTarget(const void* target_address);
  void FilterUntracedTargetsAfterMarking();
  void MarkForeignObservationIncomplete(const char* reason);
  bool IsForeignTargetObserved(const void* payload) const;

  // Conservative stack & borrow page-pin epoch.
  uint64_t BeginConservativePinEpoch();
  uint64_t current_pin_epoch() const {
    return current_pin_epoch_.load(std::memory_order_relaxed);
  }
  void RecordConservativePageHit(BasePage* page);
  void PinCrossThreadPersistentTargets();

  // Address-keyed container adapter registration.
  void RegisterAddressKeyedContainer(
      void* context, AddressKeyedContainerRehashCallback callback);
  void UnregisterAddressKeyedContainer(void* context);

  // Executes pause-time object compaction before sweeping starts on
  // memory-reducing major GCs.
  bool CompactIfEnabled(bool is_memory_reducing_major_gc);

  const ObjectCompactionSummary& last_summary() const { return last_summary_; }

 private:
  struct DestinationBuffer {
    NormalPage* page = nullptr;
    NormalPageSpace* space = nullptr;
    Address frontier = nullptr;
    Address limit = nullptr;
    bool is_reused_packed_page = false;

    size_t remaining() const {
      return limit >= frontier ? static_cast<size_t>(limit - frontier) : 0u;
    }
  };

  struct RootSlotRecord {
    const void* root_slot = nullptr;
    Address target_base = nullptr;
    size_t interior_offset = 0;
    TypedSlotRetargetCallback retarget_callback = nullptr;
  };

  struct CustomHeapSlotRecord {
    HeapObjectHeader* owner_header = nullptr;
    size_t offset_in_owner = 0;
    HeapObjectHeader* target_header = nullptr;
    size_t interior_offset = 0;
    TypedSlotRetargetCallback retarget_callback = nullptr;
  };

  struct MovedObjectRecord {
    HeapObjectHeader* src_header = nullptr;
    NormalPage* src_page = nullptr;
    Address dest_header_addr = nullptr;
    size_t allocated_size = 0;
  };

  bool ExecuteCompaction(ObjectCompactionSummary* summary);
  bool CanRelocateObject(
      const HeapObjectHeader& header, const NormalPage& page,
      const ObjectRelocationDescriptor& desc, uint64_t pin_epoch,
      const absl::flat_hash_set<const void*>& prefinalizer_or_untraced) const;
  void ResolveDeferredPinnedTargets(
      absl::flat_hash_set<const void*>* prefinalizer_or_untraced_payloads);
  void SelectEvacuationCandidates(
      const absl::flat_hash_set<const void*>& prefinalizer_or_untraced_payloads,
      const absl::flat_hash_set<NormalPage*>& prev_destination_pages,
      std::vector<NormalPage*>* candidate_pages,
      std::vector<DestinationBuffer>* dest_buffers,
      absl::flat_hash_set<NormalPage*>* retained_packed_pages,
      ObjectCompactionSummary* summary);
  void RecordRelocatableSlots(
      const absl::flat_hash_set<const void*>& prefinalizer_payloads,
      absl::flat_hash_set<BasePage*>* pages_with_recorded_slots,
      std::vector<RootSlotRecord>* root_slots,
      std::vector<CustomHeapSlotRecord>* custom_slots,
      absl::flat_hash_set<const void*>* pinned_by_unretargetable_slot,
      absl::flat_hash_set<const void*>* managed_uncompressed_slots);
  bool EvacuateCandidatePages(
      const std::vector<NormalPage*>& candidate_pages,
      const absl::flat_hash_set<const void*>& prefinalizer_or_untraced_payloads,
      const absl::flat_hash_set<const void*>& pinned_by_unretargetable_slot,
      const absl::flat_hash_set<const void*>& managed_uncompressed_slots,
      std::vector<DestinationBuffer>* dest_buffers,
      absl::flat_hash_set<BasePage*>* pages_with_recorded_slots,
      std::vector<MovedObjectRecord>* moved_objects,
      ObjectCompactionSummary* summary);
  void UpdatePointers(
      const absl::flat_hash_set<BasePage*>& pages_with_recorded_slots,
      const std::vector<RootSlotRecord>& root_slots,
      const std::vector<CustomHeapSlotRecord>& custom_slots,
      ObjectCompactionSummary* summary);
  void RebaseLegacyCompactionWorklist();

  HeapBase& heap_;
  bool enabled_ = false;
  bool explicitly_configured_ = false;

  std::atomic<uint64_t> current_pin_epoch_{1};

  mutable v8::base::Mutex foreign_edge_mutex_;
  bool foreign_epoch_active_ = false;
  bool foreign_observation_complete_ = true;
  std::vector<const void*> pending_foreign_raw_addresses_;
  absl::flat_hash_set<const void*> observed_foreign_targets_;
  std::vector<const void*> pending_untraced_raw_addresses_;
  absl::flat_hash_set<HeapObjectHeader*> sticky_untraced_headers_;

  absl::flat_hash_set<NormalPage*> current_destination_pages_;
  std::vector<std::pair<void*, AddressKeyedContainerRehashCallback>>
      address_keyed_containers_;
  ObjectCompactionSummary last_summary_;
};

}  // namespace cppgc::internal

#endif  // V8_HEAP_CPPGC_INTERNAL_OBJECT_COMPACTOR_H_
