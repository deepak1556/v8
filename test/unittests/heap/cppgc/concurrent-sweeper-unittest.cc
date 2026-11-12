// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <optional>
#include <set>
#include <vector>

#include "include/cppgc/allocation.h"
#include "include/cppgc/platform.h"
#include "include/v8-platform.h"
#include "src/heap/cppgc-internal/globals.h"
#include "src/heap/cppgc-internal/heap-object-header.h"
#include "src/heap/cppgc-internal/heap-page.h"
#include "src/heap/cppgc-internal/heap-space.h"
#include "src/heap/cppgc-internal/heap-visitor.h"
#include "src/heap/cppgc-internal/page-memory.h"
#include "src/heap/cppgc-internal/raw-heap.h"
#include "src/heap/cppgc-internal/stats-collector.h"
#include "src/heap/cppgc-internal/sweeper.h"
#include "test/unittests/heap/cppgc/test-platform.h"
#include "test/unittests/heap/cppgc/tests.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cppgc {
namespace internal {

namespace {

size_t g_destructor_callcount;

template <size_t Size>
class Finalizable : public GarbageCollected<Finalizable<Size>> {
 public:
  Finalizable() : creation_thread_{v8::base::OS::GetCurrentThreadId()} {}

  virtual ~Finalizable() {
    ++g_destructor_callcount;
    EXPECT_EQ(creation_thread_, v8::base::OS::GetCurrentThreadId());
  }

  virtual void Trace(cppgc::Visitor*) const {}

 private:
  char array_[Size];
  int creation_thread_;
};

using NormalFinalizable = Finalizable<32>;
using LargeFinalizable = Finalizable<kLargeObjectSizeThreshold * 2>;

template <size_t Size>
class NonFinalizable : public GarbageCollected<NonFinalizable<Size>> {
 public:
  virtual void Trace(cppgc::Visitor*) const {}

 private:
  char array_[Size];
  int padding_to_make_size_the_same_as_finalizible_;
};

using NormalNonFinalizable = NonFinalizable<32>;
using LargeNonFinalizable = NonFinalizable<kLargeObjectSizeThreshold * 2>;

}  // namespace

class ConcurrentSweeperTest : public testing::TestWithHeap {
 public:
  ConcurrentSweeperTest() { g_destructor_callcount = 0; }

  void StartSweeping() {
    Heap* heap = Heap::From(GetHeap());
    ResetLinearAllocationBuffers();
    // Pretend do finish marking as StatsCollector verifies that Notify*
    // methods are called in the right order.
    heap->stats_collector()->NotifyMarkingStarted(
        CollectionType::kMajor, GCConfig::MarkingType::kAtomic,
        GCConfig::IsForcedGC::kNotForced);
    heap->stats_collector()->NotifyMarkingCompleted(0);
    Sweeper& sweeper = heap->sweeper();
    const SweepingConfig sweeping_config{
        SweepingConfig::SweepingType::kIncrementalAndConcurrent,
        SweepingConfig::CompactableSpaceHandling::kSweep};
    sweeper.Start(sweeping_config);
  }

  void WaitForConcurrentSweeping() {
    Heap* heap = Heap::From(GetHeap());
    Sweeper& sweeper = heap->sweeper();
    sweeper.WaitForConcurrentSweepingForTesting();
  }

  void FinishSweeping() {
    Heap* heap = Heap::From(GetHeap());
    Sweeper& sweeper = heap->sweeper();
    sweeper.FinishIfRunning();
  }

  const RawHeap& GetRawHeap() const {
    const Heap* heap = Heap::From(GetHeap());
    return heap->raw_heap();
  }

  void CheckFreeListEntries(const std::vector<void*>& objects) {
    const Heap* heap = Heap::From(GetHeap());
    const PageBackend* backend = heap->page_backend();

    for (auto* object : objects) {
      // The corresponding page could be removed.
      if (!backend->Lookup(static_cast<ConstAddress>(object))) continue;

      const auto* header =
          BasePage::FromPayload(object)->TryObjectHeaderFromInnerAddress(
              object);
      // TryObjectHeaderFromInnerAddress returns nullptr for freelist entries.
      EXPECT_EQ(nullptr, header);
    }
  }

  // Allocates a page full of garbage objects of type `From`, collects them
  // without sweeping, and then allocates an object of type `To` that lives in a
  // different space with a different page layout. The allocation is expected to
  // reuse the empty page, which requires changing its layout.
  template <typename From, typename To>
  void TestEmptyPageIsReusedWithDifferentLayout(bool concurrent_sweeping) {
    static constexpr size_t kFromAllocationSize =
        RoundUp<kAllocationGranularity>(sizeof(From) +
                                        sizeof(HeapObjectHeader));
    static constexpr size_t kToAllocationSize =
        RoundUp<kAllocationGranularity>(sizeof(To) + sizeof(HeapObjectHeader));
    static_assert(testing::CellSizeFor<From>() != testing::CellSizeFor<To>());

    PreciseGC();

    // First, allocate a full page of objects.
    auto* first_object = MakeGarbageCollected<From>(GetAllocationHandle());
    NormalPage* page = NormalPage::From(BasePage::FromPayload(first_object));
    ASSERT_EQ(testing::CellSizeFor<From>(), page->cell_size());
    const BaseSpace* from_space = &page->space();
    const size_t from_objects =
        testing::NormalPagePayloadSizeFor<From>() / kFromAllocationSize;
    for (size_t i = 1; i < from_objects; ++i) {
      ASSERT_EQ(page, BasePage::FromPayload(
                          MakeGarbageCollected<From>(GetAllocationHandle())));
    }

    std::optional<testing::TestPlatform::DisableBackgroundTasksScope>
        no_concurrent_sweep_scope;
    if (!concurrent_sweeping) {
      no_concurrent_sweep_scope.emplace(GetPlatformHandle().get());
    }

    // Start the GC without sweeping.
    static constexpr GCConfig config = {
        CollectionType::kMajor, StackState::kNoHeapPointers,
        GCConfig::MarkingType::kAtomic,
        GCConfig::SweepingType::kIncrementalAndConcurrent};
    Heap::From(GetHeap())->CollectGarbage(config);

    if (concurrent_sweeping) {
      WaitForConcurrentSweeping();
    }

    // Allocate and sweep. The empty page is returned to the space of `To`.
    auto* allocated_after_sweeping =
        MakeGarbageCollected<To>(GetAllocationHandle());
    ASSERT_EQ(page, BasePage::FromPayload(allocated_after_sweeping));
    EXPECT_NE(from_space, &page->space());
    EXPECT_EQ(testing::CellSizeFor<To>(), page->cell_size());
    EXPECT_EQ(testing::CellSizeFor<To>(),
              NormalPageSpace::From(page->space()).cell_size());
    EXPECT_EQ(testing::NormalPagePayloadSizeFor<To>(), page->PayloadSize());
    EXPECT_EQ(page->PayloadStart(),
              reinterpret_cast<Address>(
                  &HeapObjectHeader::FromObject(allocated_after_sweeping)));

    // The whole payload of the page is usable in the new layout.
    const size_t to_objects =
        testing::NormalPagePayloadSizeFor<To>() / kToAllocationSize;
    void* last_object = allocated_after_sweeping;
    for (size_t i = 1; i < to_objects; ++i) {
      last_object = MakeGarbageCollected<To>(GetAllocationHandle());
      ASSERT_EQ(page, BasePage::FromPayload(last_object));
    }
    EXPECT_GT(2 * kToAllocationSize,
              static_cast<size_t>(
                  page->PayloadEnd() -
                  HeapObjectHeader::FromObject(last_object).ObjectStart()));

    // Inner pointers are resolved using the new layout.
    for (void* object :
         {static_cast<void*>(allocated_after_sweeping), last_object}) {
      const HeapObjectHeader& header = HeapObjectHeader::FromObject(object);
      EXPECT_EQ(&header, &page->ObjectHeaderFromInnerAddress(object));
      EXPECT_EQ(&header,
                &page->ObjectHeaderFromInnerAddress(header.ObjectEnd() - 1));
    }

    FinishSweeping();
    // Verifies that the page is iterable in its new layout.
    PreciseGC();
  }

  bool PageInBackend(const BasePage* page) {
    const Heap* heap = Heap::From(GetHeap());
    const PageBackend* backend = heap->page_backend();
    return backend->Lookup(reinterpret_cast<ConstAddress>(page));
  }

  bool FreeListContains(const BaseSpace& space,
                        const std::vector<void*>& objects) {
    const Heap* heap = Heap::From(GetHeap());
    const PageBackend* backend = heap->page_backend();
    const auto& freelist = NormalPageSpace::From(space).free_list();

    for (void* object : objects) {
      // The corresponding page could be removed.
      if (!backend->Lookup(static_cast<ConstAddress>(object))) continue;

      if (!freelist.ContainsForTesting({object, 0})) return false;
    }

    return true;
  }

  void MarkObject(void* payload) {
    HeapObjectHeader& header = HeapObjectHeader::FromObject(payload);
    header.TryMarkAtomic();
    BasePage* page = BasePage::FromPayload(&header);
    page->IncrementMarkedBytes(page->is_large()
                                   ? LargePage::From(page)->PayloadSize()
                                   : header.AllocatedSize());
  }
};

TEST_F(ConcurrentSweeperTest, BackgroundSweepOfNormalPage) {
  // Non finalizable objects are swept right away.
  using GCedType = NormalNonFinalizable;

  auto* unmarked_object = MakeGarbageCollected<GCedType>(GetAllocationHandle());
  auto* marked_object = MakeGarbageCollected<GCedType>(GetAllocationHandle());
  MarkObject(marked_object);

  auto* page = BasePage::FromPayload(unmarked_object);
  auto& space = page->space();

  // The test requires objects to be allocated on the same page;
  ASSERT_EQ(page, BasePage::FromPayload(marked_object));

  StartSweeping();

  // Wait for concurrent sweeping to finish.
  WaitForConcurrentSweeping();

  const auto& hoh = HeapObjectHeader::FromObject(marked_object);
  if (Heap::From(GetHeap())->generational_gc_supported()) {
    // Check that the marked object is still marked.
    EXPECT_TRUE(hoh.IsMarked());
  } else {
    // Check that the marked object was unmarked.
    EXPECT_FALSE(hoh.IsMarked());
  }

  // Check that free list entries are created right away for non-finalizable
  // objects, but not immediately returned to the space's freelist.
  CheckFreeListEntries({unmarked_object});
  EXPECT_FALSE(FreeListContains(space, {unmarked_object}));

  FinishSweeping();

  // Check that finalizable objects are swept and put into the freelist of the
  // corresponding space.
  EXPECT_TRUE(FreeListContains(space, {unmarked_object}));
}

TEST_F(ConcurrentSweeperTest, BackgroundSweepOfLargePage) {
  // Non finalizable objects are swept right away but the page is only returned
  // from the main thread.
  using GCedType = LargeNonFinalizable;

  auto* unmarked_object = MakeGarbageCollected<GCedType>(GetAllocationHandle());
  auto* marked_object = MakeGarbageCollected<GCedType>(GetAllocationHandle());
  MarkObject(marked_object);

  auto* unmarked_page = BasePage::FromPayload(unmarked_object);
  auto* marked_page = BasePage::FromPayload(marked_object);
  auto& space = unmarked_page->space();

  ASSERT_EQ(&space, &marked_page->space());

  StartSweeping();

  // Wait for concurrent sweeping to finish.
  WaitForConcurrentSweeping();

  const auto& hoh = HeapObjectHeader::FromObject(marked_object);
  if (Heap::From(GetHeap())->generational_gc_supported()) {
    // Check that the marked object is still marked.
    EXPECT_TRUE(hoh.IsMarked());
  } else {
    // Check that the marked object was unmarked.
    EXPECT_FALSE(hoh.IsMarked());
  }

  // The page should not have been removed on the background threads.
  EXPECT_TRUE(PageInBackend(unmarked_page));

  FinishSweeping();

  // Check that free list entries are created right away for non-finalizable
  // objects, but not immediately returned to the space's freelist.
  EXPECT_FALSE(PageInBackend(unmarked_page));

  // Check that marked pages are returned to space right away.
  EXPECT_NE(space.end(), std::find(space.begin(), space.end(), marked_page));
}

TEST_F(ConcurrentSweeperTest, DeferredFinalizationOfNormalPage) {
  static constexpr size_t kNumberOfObjects = 10;
  // Finalizable types are left intact by concurrent sweeper.
  using GCedType = NormalFinalizable;

  std::set<BasePage*> pages;
  std::vector<void*> objects;

  BaseSpace* space = nullptr;
  for (size_t i = 0; i < kNumberOfObjects; ++i) {
    auto* object = MakeGarbageCollected<GCedType>(GetAllocationHandle());
    objects.push_back(object);
    auto* page = BasePage::FromPayload(object);
    pages.insert(page);
    if (!space) space = &page->space();
  }

  StartSweeping();

  // Wait for concurrent sweeping to finish.
  WaitForConcurrentSweeping();

  // Check that pages are not returned right away.
  for (auto* page : pages) {
    EXPECT_EQ(space->end(), std::find(space->begin(), space->end(), page));
  }
  // Check that finalizable objects are left intact in pages.
  EXPECT_FALSE(FreeListContains(*space, objects));
  // No finalizers have been executed.
  EXPECT_EQ(0u, g_destructor_callcount);

  FinishSweeping();

  // Check that finalizable objects are swept and turned into freelist entries.
  CheckFreeListEntries(objects);
  // Check that space's freelist contains these entries.
  EXPECT_TRUE(FreeListContains(*space, objects));
  // Check that finalizers have been executed.
  EXPECT_EQ(kNumberOfObjects, g_destructor_callcount);
}

TEST_F(ConcurrentSweeperTest, DeferredFinalizationOfLargePage) {
  using GCedType = LargeFinalizable;

  auto* object = MakeGarbageCollected<GCedType>(GetAllocationHandle());

  auto* page = BasePage::FromPayload(object);
  auto& space = page->space();

  StartSweeping();

  // Wait for concurrent sweeping to finish.
  WaitForConcurrentSweeping();

  // Check that the page is not returned to the space.
  EXPECT_EQ(space.end(), std::find(space.begin(), space.end(), page));
  // Check that no destructors have been executed yet.
  EXPECT_EQ(0u, g_destructor_callcount);

  FinishSweeping();

  // Check that the destructor was executed.
  EXPECT_EQ(1u, g_destructor_callcount);
  // Check that page was unmapped.
  EXPECT_FALSE(PageInBackend(page));
}

TEST_F(ConcurrentSweeperTest, DestroyLargePageOnMainThread) {
  // This test fails with TSAN when large pages are destroyed concurrently
  // without proper support by the backend.
  using GCedType = LargeNonFinalizable;

  auto* object = MakeGarbageCollected<GCedType>(GetAllocationHandle());
  auto* page = BasePage::FromPayload(object);

  StartSweeping();

  // Allocating another large object should not race here.
  MakeGarbageCollected<GCedType>(GetAllocationHandle());

  // Wait for concurrent sweeping to finish.
  WaitForConcurrentSweeping();

  FinishSweeping();

  // Check that page was unmapped.
  EXPECT_FALSE(PageInBackend(page));
}

TEST_F(ConcurrentSweeperTest, IncrementalSweeping) {
  testing::TestPlatform::DisableBackgroundTasksScope disable_concurrent_sweeper(
      &GetPlatform());

  auto task_runner =
      GetPlatform().GetForegroundTaskRunner(TaskPriority::kUserBlocking);

  // Create two unmarked objects.
  MakeGarbageCollected<NormalFinalizable>(GetAllocationHandle());
  MakeGarbageCollected<LargeFinalizable>(GetAllocationHandle());

  // Create two marked objects.
  auto* marked_normal_object =
      MakeGarbageCollected<NormalFinalizable>(GetAllocationHandle());
  auto* marked_large_object =
      MakeGarbageCollected<LargeFinalizable>(GetAllocationHandle());

  auto& marked_normal_header =
      HeapObjectHeader::FromObject(marked_normal_object);
  auto& marked_large_header = HeapObjectHeader::FromObject(marked_large_object);

  MarkObject(marked_normal_object);
  MarkObject(marked_large_object);

  StartSweeping();

  EXPECT_EQ(0u, g_destructor_callcount);
  EXPECT_TRUE(marked_normal_header.IsMarked());
  // Live large objects are eagerly swept.
  if (Heap::From(GetHeap())->generational_gc_supported()) {
    EXPECT_TRUE(marked_large_header.IsMarked());
  } else {
    EXPECT_FALSE(marked_large_header.IsMarked());
  }

  // Wait for incremental sweeper to finish.
  GetPlatform().RunAllForegroundTasks();

  EXPECT_EQ(2u, g_destructor_callcount);

  if (Heap::From(GetHeap())->generational_gc_supported()) {
    EXPECT_TRUE(marked_normal_header.IsMarked());
    EXPECT_TRUE(marked_large_header.IsMarked());
  } else {
    EXPECT_FALSE(marked_normal_header.IsMarked());
    EXPECT_FALSE(marked_large_header.IsMarked());
  }

  FinishSweeping();
}

TEST_F(ConcurrentSweeperTest, SweepOnAllocationReturnEmptyPage) {
  PreciseGC();

  // First, allocate the full page of finalizable objects.
  const size_t objects_to_allocated =
      testing::NormalPagePayloadSizeFor<NormalFinalizable>() /
      (sizeof(HeapObjectHeader) + sizeof(NormalFinalizable));
  auto* first_obj =
      MakeGarbageCollected<NormalFinalizable>(GetAllocationHandle());
  auto* finalizable_page =
      NormalPage::FromInnerAddress(&HeapBase::From(GetHeapHandle()), first_obj);
  for (size_t i = 1; i < objects_to_allocated; ++i) {
    MakeGarbageCollected<NormalFinalizable>(GetAllocationHandle());
  }

  // Then, allocate a new unfinalizable object on a fresh page. We do that so
  // that the sweeper on allocation doesn't allocate a new page.
  auto* non_finalizable =
      MakeGarbageCollected<NormalNonFinalizable>(GetAllocationHandle());
  auto* non_finalizable_page = NormalPage::FromInnerAddress(
      &HeapBase::From(GetHeapHandle()), non_finalizable);
  ASSERT_NE(finalizable_page, non_finalizable_page);

  // Start the GC without sweeping.
  static constexpr GCConfig config = {
      CollectionType::kMajor, StackState::kNoHeapPointers,
      GCConfig::MarkingType::kAtomic,
      GCConfig::SweepingType::kIncrementalAndConcurrent};
  Heap::From(GetHeap())->CollectGarbage(config);

  WaitForConcurrentSweeping();

  // Allocate and sweep.
  auto* allocated_after_sweeping =
      MakeGarbageCollected<NormalFinalizable>(GetAllocationHandle());
  // Check that the empty page of finalizable objects was returned.
  EXPECT_EQ(finalizable_page,
            NormalPage::FromInnerAddress(&HeapBase::From(GetHeapHandle()),
                                         allocated_after_sweeping));
}

namespace {

// Objects of different sizes that are allocated on size-class pages.
using SmallFinalizable = Finalizable<8>;
using OtherSmallFinalizable = Finalizable<24>;
using SmallNonFinalizable = NonFinalizable<8>;
using OtherSmallNonFinalizable = NonFinalizable<24>;
static_assert(testing::CellSizeFor<SmallFinalizable>() != 0);
static_assert(testing::CellSizeFor<OtherSmallFinalizable>() != 0);
static_assert(testing::CellSizeFor<SmallNonFinalizable>() != 0);
static_assert(testing::CellSizeFor<OtherSmallNonFinalizable>() != 0);

// Objects that are allocated on pages with objects of mixed sizes.
using MixedFinalizable = Finalizable<256>;
using MixedNonFinalizable = NonFinalizable<256>;
static_assert(testing::CellSizeFor<MixedFinalizable>() == 0);
static_assert(testing::CellSizeFor<MixedNonFinalizable>() == 0);

}  // namespace

TEST_F(ConcurrentSweeperTest, EmptyPageChangesCellSizeOnFinalization) {
  TestEmptyPageIsReusedWithDifferentLayout<SmallFinalizable,
                                           OtherSmallFinalizable>(true);
}

TEST_F(ConcurrentSweeperTest, EmptyPageChangesCellSizeOnSweeping) {
  TestEmptyPageIsReusedWithDifferentLayout<SmallNonFinalizable,
                                           OtherSmallNonFinalizable>(false);
}

TEST_F(ConcurrentSweeperTest, EmptySizeClassPageBecomesMixedOnFinalization) {
  TestEmptyPageIsReusedWithDifferentLayout<SmallFinalizable, MixedFinalizable>(
      true);
}

TEST_F(ConcurrentSweeperTest, EmptySizeClassPageBecomesMixedOnSweeping) {
  TestEmptyPageIsReusedWithDifferentLayout<SmallNonFinalizable,
                                           MixedNonFinalizable>(false);
}

TEST_F(ConcurrentSweeperTest, EmptyMixedPageBecomesSizeClassOnFinalization) {
  TestEmptyPageIsReusedWithDifferentLayout<MixedFinalizable, SmallFinalizable>(
      true);
}

TEST_F(ConcurrentSweeperTest, EmptyMixedPageBecomesSizeClassOnSweeping) {
  TestEmptyPageIsReusedWithDifferentLayout<MixedNonFinalizable,
                                           SmallNonFinalizable>(false);
}

}  // namespace internal
}  // namespace cppgc
