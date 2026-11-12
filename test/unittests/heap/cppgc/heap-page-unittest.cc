// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/cppgc-internal/heap-page.h"

#include <algorithm>

#include "include/cppgc/allocation.h"
#include "include/cppgc/explicit-management.h"
#include "include/cppgc/internal/api-constants.h"
#include "include/cppgc/persistent.h"
#include "src/base/macros.h"
#include "src/heap/cppgc-internal/globals.h"
#include "src/heap/cppgc-internal/heap-object-header.h"
#include "src/heap/cppgc-internal/heap-space.h"
#include "src/heap/cppgc-internal/object-allocator.h"
#include "src/heap/cppgc-internal/object-start-bitmap.h"
#include "src/heap/cppgc-internal/page-memory.h"
#include "src/heap/cppgc-internal/raw-heap.h"
#include "test/unittests/heap/cppgc/tests.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cppgc {
namespace internal {

namespace {

class PageTest : public testing::TestWithHeap {
 public:
  RawHeap& GetRawHeap() { return Heap::From(GetHeap())->raw_heap(); }
  PageBackend& GetPageBackend() {
    return *Heap::From(GetHeap())->page_backend();
  }
};

template <size_t Size>
class GCed : public GarbageCollected<GCed<Size>> {
 public:
  virtual void Trace(cppgc::Visitor*) const {}
  char array[Size];
};

}  // namespace

TEST_F(PageTest, SpaceIndexing) {
  RawHeap& heap = GetRawHeap();
  size_t space = 0u;
  for (const auto& ptr : heap) {
    EXPECT_EQ(&heap, ptr.get()->raw_heap());
    EXPECT_EQ(space, ptr->index());
    ++space;
  }
  EXPECT_GE(space, RawHeap::kNumberOfRegularSpaces);
}

TEST_F(PageTest, PredefinedSpaces) {
  using SpaceType = RawHeap::RegularSpaceType;
  RawHeap& heap = GetRawHeap();
  const auto check_size_class_space = [&heap](void* object,
                                              size_t allocation_size) {
    BaseSpace& space = NormalPage::FromPayload(object)->space();
    EXPECT_EQ(heap.Space(RawHeap::SizeClassSpaceForSize(allocation_size)),
              &space);
    EXPECT_EQ((allocation_size - kMinSizeClassSize) / kAllocationGranularity,
              space.index());
    EXPECT_FALSE(space.is_large());
    EXPECT_TRUE(NormalPageSpace::From(space).is_size_class());
    EXPECT_EQ(allocation_size, NormalPageSpace::From(space).cell_size());
    const NormalPage* page = NormalPage::From(BasePage::FromPayload(object));
    EXPECT_TRUE(page->is_size_class());
    EXPECT_EQ(allocation_size, page->cell_size());
    EXPECT_EQ(allocation_size,
              HeapObjectHeader::FromObject(object).AllocatedSize());
  };
  {
    static_assert(testing::CellSizeFor<GCed<1>>() != 0);
    auto* gced = MakeGarbageCollected<GCed<1>>(GetAllocationHandle());
    check_size_class_space(gced, testing::CellSizeFor<GCed<1>>());
    // The object consists of the header, a vtable pointer, and a single byte
    // of padded payload.
    EXPECT_EQ(kMinSizeClassSize + kAllocationGranularity,
              testing::CellSizeFor<GCed<1>>());
    EXPECT_EQ(1u, NormalPage::FromPayload(gced)->space().index());
  }
  {
    using Type = GCed<kSizeClassCutoff / 4>;
    static_assert(testing::CellSizeFor<Type>() != 0);
    auto* gced = MakeGarbageCollected<Type>(GetAllocationHandle());
    check_size_class_space(gced, testing::CellSizeFor<Type>());
  }
  {
    using Type = GCed<kSizeClassCutoff / 2>;
    static_assert(testing::CellSizeFor<Type>() != 0);
    auto* gced = MakeGarbageCollected<Type>(GetAllocationHandle());
    check_size_class_space(gced, testing::CellSizeFor<Type>());
  }
  {
    // Largest object that still fits into a size class.
    using Type = GCed<kSizeClassCutoff - kAllocationGranularity -
                      sizeof(void*) - sizeof(HeapObjectHeader)>;
    static_assert(testing::CellSizeFor<Type>() ==
                  kSizeClassCutoff - kAllocationGranularity);
    auto* gced = MakeGarbageCollected<Type>(GetAllocationHandle());
    check_size_class_space(gced, testing::CellSizeFor<Type>());
    EXPECT_EQ(kNumberOfSizeClasses - 1,
              NormalPage::FromPayload(gced)->space().index());
  }
  {
    // Smallest object that doesn't fit into a size class.
    using Type =
        GCed<kSizeClassCutoff - sizeof(void*) - sizeof(HeapObjectHeader)>;
    static_assert(testing::CellSizeFor<Type>() == 0);
    auto* gced = MakeGarbageCollected<Type>(GetAllocationHandle());
    BaseSpace& space = NormalPage::FromPayload(gced)->space();
    EXPECT_EQ(heap.Space(SpaceType::kNormal), &space);
    EXPECT_EQ(kNumberOfSizeClasses, space.index());
    EXPECT_FALSE(space.is_large());
    EXPECT_FALSE(NormalPageSpace::From(space).is_size_class());
    EXPECT_FALSE(
        NormalPage::From(BasePage::FromPayload(gced))->is_size_class());
  }
  {
    auto* gced = MakeGarbageCollected<GCed<128>>(GetAllocationHandle());
    BaseSpace& space = NormalPage::FromPayload(gced)->space();
    EXPECT_EQ(heap.Space(SpaceType::kNormal), &space);
    EXPECT_EQ(kNumberOfSizeClasses, space.index());
    EXPECT_FALSE(space.is_large());
  }
  {
    auto* gced = MakeGarbageCollected<GCed<2 * kLargeObjectSizeThreshold>>(
        GetAllocationHandle());
    BaseSpace& space = NormalPage::FromPayload(gced)->space();
    EXPECT_EQ(heap.Space(SpaceType::kLarge), &space);
    EXPECT_EQ(kNumberOfSizeClasses + 1, space.index());
    EXPECT_TRUE(space.is_large());
  }
}

TEST_F(PageTest, NormalPageIndexing) {
  constexpr size_t kExpectedNumberOfPages = 10u;
  constexpr size_t kObjectSize = 8u;
  using Type = GCed<kObjectSize>;
  static const size_t kNumberOfObjects =
      (kExpectedNumberOfPages * testing::NormalPagePayloadSizeFor<Type>() /
       (sizeof(Type) + sizeof(HeapObjectHeader))) -
      kExpectedNumberOfPages;

  std::vector<Persistent<Type>> persistents(kNumberOfObjects);
  for (auto& p : persistents) {
    p = MakeGarbageCollected<Type>(GetAllocationHandle());
  }

  const RawHeap& heap = GetRawHeap();
  const BaseSpace* space =
      heap.Space(RawHeap::SizeClassSpaceForSize(testing::CellSizeFor<Type>()));
  EXPECT_EQ(kExpectedNumberOfPages, space->size());

  size_t page_n = 0;
  for (const BasePage* page : *space) {
    EXPECT_FALSE(page->is_large());
    EXPECT_EQ(space, &page->space());
    ++page_n;
  }
  EXPECT_EQ(page_n, space->size());
}

TEST_F(PageTest, LargePageIndexing) {
  using SpaceType = RawHeap::RegularSpaceType;
  constexpr size_t kExpectedNumberOfPages = 10u;
  constexpr size_t kObjectSize = 2 * kLargeObjectSizeThreshold;
  using Type = GCed<kObjectSize>;
  const size_t kNumberOfObjects = kExpectedNumberOfPages;

  std::vector<Persistent<Type>> persistents(kNumberOfObjects);
  for (auto& p : persistents) {
    p = MakeGarbageCollected<Type>(GetAllocationHandle());
  }

  const RawHeap& heap = GetRawHeap();
  const BaseSpace* space = heap.Space(SpaceType::kLarge);
  EXPECT_EQ(kExpectedNumberOfPages, space->size());

  size_t page_n = 0;
  for (const BasePage* page : *space) {
    EXPECT_TRUE(page->is_large());
    ++page_n;
  }
  EXPECT_EQ(page_n, space->size());
}

TEST_F(PageTest, HeapObjectHeaderOnBasePageIndexing) {
  constexpr size_t kObjectSize = 8;
  using Type = GCed<kObjectSize>;
  const size_t kNumberOfObjects = testing::NormalPagePayloadSizeFor<Type>() /
                                  (sizeof(Type) + sizeof(HeapObjectHeader));
  const size_t kLeftSpace = testing::NormalPagePayloadSizeFor<Type>() %
                            (sizeof(Type) + sizeof(HeapObjectHeader));

  std::vector<Persistent<Type>> persistents(kNumberOfObjects);
  for (auto& p : persistents) {
    p = MakeGarbageCollected<Type>(GetAllocationHandle());
  }

  const auto* page =
      static_cast<NormalPage*>(BasePage::FromPayload(persistents[0].Get()));
  size_t size = 0;
  size_t num = 0;
  for (const HeapObjectHeader& header : *page) {
    EXPECT_EQ(reinterpret_cast<Address>(persistents[num].Get()),
              header.ObjectStart());
    size += header.AllocatedSize();
    ++num;
  }
  EXPECT_EQ(num, persistents.size());
  EXPECT_EQ(size + kLeftSpace, page->PayloadSize());
}

TEST_F(PageTest, HeapObjectHeaderOnLargePageIndexing) {
  constexpr size_t kObjectSize = 2 * kLargeObjectSizeThreshold;
  using Type = GCed<kObjectSize>;
  auto* gced = MakeGarbageCollected<Type>(GetAllocationHandle());

  const auto* page = static_cast<LargePage*>(BasePage::FromPayload(gced));
  const size_t expected_payload_size =
      RoundUp(sizeof(Type) + sizeof(HeapObjectHeader), kAllocationGranularity);
  EXPECT_EQ(expected_payload_size, page->PayloadSize());

  const HeapObjectHeader* header = page->ObjectHeader();
  EXPECT_EQ(reinterpret_cast<Address>(gced), header->ObjectStart());
}

TEST_F(PageTest, NormalPageCreationDestruction) {
  RawHeap& heap = GetRawHeap();
  const PageBackend* backend = Heap::From(GetHeap())->page_backend();
  auto* space = static_cast<NormalPageSpace*>(
      heap.Space(RawHeap::RegularSpaceType::kNormal));
  auto* page = NormalPage::TryCreate(GetPageBackend(), *space);
  EXPECT_NE(nullptr, page);
  EXPECT_NE(nullptr, backend->Lookup(page->PayloadStart()));
  EXPECT_FALSE(page->is_size_class());
  EXPECT_EQ(NormalPage::PayloadSizeFor(0), page->PayloadSize());
  EXPECT_EQ(reinterpret_cast<Address>(page) + kPageSize, page->PayloadEnd());

  space->AddPage(page);
  EXPECT_NE(space->end(), std::find(space->begin(), space->end(), page));

  space->free_list().Add({page->PayloadStart(), page->PayloadSize()});
  EXPECT_TRUE(space->free_list().ContainsForTesting(
      {page->PayloadStart(), page->PayloadSize()}));

  space->free_list().Clear();
  EXPECT_FALSE(space->free_list().ContainsForTesting(
      {page->PayloadStart(), page->PayloadSize()}));
  space->RemovePage(page);
  EXPECT_EQ(space->end(), std::find(space->begin(), space->end(), page));
  // The payload start depends on the layout of the page and must thus be
  // retrieved before destroying the page.
  const ConstAddress payload_start = page->PayloadStart();
  NormalPage::Destroy(page);
  EXPECT_EQ(nullptr, backend->Lookup(payload_start));
}

TEST_F(PageTest, LargePageCreationDestruction) {
  constexpr size_t kObjectSize = 2 * kLargeObjectSizeThreshold;
  RawHeap& heap = GetRawHeap();
  const PageBackend* backend = Heap::From(GetHeap())->page_backend();
  auto* space = static_cast<LargePageSpace*>(
      heap.Space(RawHeap::RegularSpaceType::kLarge));
  auto* page = LargePage::TryCreate(GetPageBackend(), *space, kObjectSize);
  EXPECT_NE(nullptr, page);
  EXPECT_NE(nullptr, backend->Lookup(page->PayloadStart()));

  space->AddPage(page);
  EXPECT_NE(space->end(), std::find(space->begin(), space->end(), page));

  space->RemovePage(page);
  EXPECT_EQ(space->end(), std::find(space->begin(), space->end(), page));
  LargePage::Destroy(page);
  EXPECT_EQ(nullptr, backend->Lookup(page->PayloadStart()));
}

#if DEBUG
TEST_F(PageTest, UnsweptPageDestruction) {
  RawHeap& heap = GetRawHeap();
  {
    auto* space = static_cast<NormalPageSpace*>(
        heap.Space(RawHeap::RegularSpaceType::kNormal));
    auto* page = NormalPage::TryCreate(GetPageBackend(), *space);
    EXPECT_NE(nullptr, page);
    space->AddPage(page);
    EXPECT_DEATH_IF_SUPPORTED(NormalPage::Destroy(page), "");
  }
  {
    auto* space = static_cast<LargePageSpace*>(
        heap.Space(RawHeap::RegularSpaceType::kLarge));
    auto* page = LargePage::TryCreate(GetPageBackend(), *space,
                                      2 * kLargeObjectSizeThreshold);
    EXPECT_NE(nullptr, page);
    space->AddPage(page);
    EXPECT_DEATH_IF_SUPPORTED(LargePage::Destroy(page), "");
    // Detach page and really destroy page in the parent process so that sweeper
    // doesn't consider it.
    space->RemovePage(page);
    LargePage::Destroy(page);
  }
}
#endif

TEST_F(PageTest, ObjectHeaderFromInnerAddress) {
  {
    auto* object = MakeGarbageCollected<GCed<64>>(GetAllocationHandle());
    const HeapObjectHeader& expected = HeapObjectHeader::FromObject(object);

    for (auto* inner_ptr = reinterpret_cast<ConstAddress>(object);
         inner_ptr < reinterpret_cast<ConstAddress>(object + 1); ++inner_ptr) {
      const HeapObjectHeader& hoh =
          BasePage::FromPayload(object)->ObjectHeaderFromInnerAddress(
              inner_ptr);
      EXPECT_EQ(&expected, &hoh);
    }
  }
  {
    // Object on a page with objects of mixed sizes.
    auto* object = MakeGarbageCollected<GCed<256>>(GetAllocationHandle());
    ASSERT_FALSE(
        NormalPage::From(BasePage::FromPayload(object))->is_size_class());
    const HeapObjectHeader& expected = HeapObjectHeader::FromObject(object);

    for (auto* inner_ptr = reinterpret_cast<ConstAddress>(object);
         inner_ptr < reinterpret_cast<ConstAddress>(object + 1); ++inner_ptr) {
      const HeapObjectHeader& hoh =
          BasePage::FromPayload(object)->ObjectHeaderFromInnerAddress(
              inner_ptr);
      EXPECT_EQ(&expected, &hoh);
    }
  }
  {
    auto* object = MakeGarbageCollected<GCed<2 * kLargeObjectSizeThreshold>>(
        GetAllocationHandle());
    const HeapObjectHeader& expected = HeapObjectHeader::FromObject(object);

    const HeapObjectHeader& hoh =
        BasePage::FromPayload(object)->ObjectHeaderFromInnerAddress(
            reinterpret_cast<ConstAddress>(object) + kLargeObjectSizeThreshold);
    EXPECT_EQ(&expected, &hoh);
  }
}

TEST_F(PageTest, SizeClassPageLayout) {
  RawHeap& heap = GetRawHeap();
  for (size_t i = 0; i < kNumberOfSizeClasses; ++i) {
    const size_t cell_size = RawHeap::SizeForSizeClassSpace(i);
    auto* space = static_cast<NormalPageSpace*>(
        heap.Space(RawHeap::SizeClassSpaceForSize(cell_size)));
    ASSERT_EQ(i, space->index());
    ASSERT_TRUE(space->is_size_class());
    ASSERT_EQ(cell_size, space->cell_size());
    auto* page = NormalPage::TryCreate(GetPageBackend(), *space);
    ASSERT_NE(nullptr, page);
    EXPECT_TRUE(page->is_size_class());
    EXPECT_EQ(cell_size, page->cell_size());
    // The payload only consists of whole cells.
    EXPECT_EQ(NormalPage::PayloadSizeFor(cell_size), page->PayloadSize());
    EXPECT_EQ(0u, page->PayloadSize() % cell_size);
    const Address page_end = reinterpret_cast<Address>(page) + kPageSize;
    EXPECT_LE(page->PayloadEnd(), page_end);
    EXPECT_GT(cell_size, static_cast<size_t>(page_end - page->PayloadEnd()));
    // The object in the first cell is aligned to the maximum supported
    // alignment. This way all objects on pages with a cell size that is a
    // multiple of the alignment are aligned as well.
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(page->PayloadStart() +
                                              sizeof(HeapObjectHeader)) %
                      api_constants::kMaxSupportedAlignment);
    // Size-class pages don't have an object start bitmap and use the memory
    // as payload.
    EXPECT_GE(
        NormalPage::PageHeaderSizeFor(0) -
            NormalPage::PageHeaderSizeFor(cell_size),
        sizeof(ObjectStartBitmap) - api_constants::kMaxSupportedAlignment);
    NormalPage::Destroy(page);
  }
}

TEST_F(PageTest, CellIndexForOffset) {
  // Exhaustively checks that the division by multiplying with the reciprocal
  // is exact for all offsets on a page.
  for (size_t i = 0; i < kNumberOfSizeClasses; ++i) {
    const size_t cell_size = RawHeap::SizeForSizeClassSpace(i);
    const uint32_t reciprocal = NormalPage::CellSizeReciprocal(cell_size);
    size_t mismatches = 0;
    for (size_t offset = 0; offset < kPageSize; ++offset) {
      if (NormalPage::CellIndexForOffset(offset, reciprocal) !=
          offset / cell_size) {
        ++mismatches;
      }
    }
    EXPECT_EQ(0u, mismatches) << "cell size: " << cell_size;
  }
}

namespace {

template <typename Type>
void TestHeaderLookupOnSizeClassPage(cppgc::Heap* heap) {
  static_assert(testing::CellSizeFor<Type>() != 0,
                "Type must be allocated on size-class pages");
  SCOPED_TRACE(testing::CellSizeFor<Type>());
  // Three adjacent objects.
  Type* const objects[] = {
      MakeGarbageCollected<Type>(heap->GetAllocationHandle()),
      MakeGarbageCollected<Type>(heap->GetAllocationHandle()),
      MakeGarbageCollected<Type>(heap->GetAllocationHandle())};
  const NormalPage* page = NormalPage::From(BasePage::FromPayload(objects[0]));
  ASSERT_TRUE(page->is_size_class());
  const size_t cell_size = page->cell_size();
  ASSERT_EQ(testing::CellSizeFor<Type>(), cell_size);
  // `TryObjectHeaderFromInnerAddress()` requires that there's no LAB. This
  // returns the remainder of the LAB to the free list.
  Heap::From(heap)->object_allocator().ResetLinearAllocationBuffers();

  for (Type* object : objects) {
    ASSERT_EQ(page, BasePage::FromPayload(object));
    const HeapObjectHeader& expected = HeapObjectHeader::FromObject(object);
    ASSERT_EQ(cell_size, expected.AllocatedSize());
    for (auto* inner_ptr = reinterpret_cast<ConstAddress>(&expected);
         inner_ptr < expected.ObjectEnd(); ++inner_ptr) {
      EXPECT_EQ(&expected, &page->ObjectHeaderFromInnerAddress(inner_ptr));
      EXPECT_EQ(
          &expected,
          &page->ObjectHeaderFromInnerAddressConcurrent<AccessMode::kAtomic>(
              inner_ptr));
      EXPECT_EQ(&expected, page->TryObjectHeaderFromInnerAddress(inner_ptr));
    }
  }

  // Free memory is not considered to be an object. This covers the first cell
  // of a free list entry, which has a header, and the following cells that
  // don't have a header.
  ConstAddress free_start =
      HeapObjectHeader::FromObject(objects[2]).ObjectEnd();
  ASSERT_LT(free_start + 3 * cell_size, page->PayloadEnd());
  for (auto* inner_ptr = free_start; inner_ptr < free_start + 3 * cell_size;
       ++inner_ptr) {
    EXPECT_EQ(nullptr, page->TryObjectHeaderFromInnerAddress(inner_ptr));
  }
  EXPECT_EQ(nullptr,
            page->TryObjectHeaderFromInnerAddress(page->PayloadEnd() - 1));
  // Addresses outside of the payload.
  EXPECT_EQ(nullptr,
            page->TryObjectHeaderFromInnerAddress(page->PayloadStart() - 1));
  EXPECT_EQ(nullptr, page->TryObjectHeaderFromInnerAddress(page->PayloadEnd()));
  EXPECT_EQ(nullptr, page->TryObjectHeaderFromInnerAddress(
                         reinterpret_cast<ConstAddress>(page) + kPageSize - 1));

  // Explicitly freed cells are not considered objects.
  const HeapObjectHeader& freed_header =
      HeapObjectHeader::FromObject(objects[1]);
  ConstAddress freed_start = reinterpret_cast<ConstAddress>(&freed_header);
  subtle::FreeUnreferencedObject(heap->GetHeapHandle(), *objects[1]);
  for (auto* inner_ptr = freed_start; inner_ptr < freed_start + cell_size;
       ++inner_ptr) {
    EXPECT_EQ(nullptr, page->TryObjectHeaderFromInnerAddress(inner_ptr));
  }
  // The neighbors are still found.
  EXPECT_EQ(&HeapObjectHeader::FromObject(objects[0]),
            page->TryObjectHeaderFromInnerAddress(freed_start - 1));
  EXPECT_EQ(&HeapObjectHeader::FromObject(objects[2]),
            page->TryObjectHeaderFromInnerAddress(freed_start + cell_size));
}

}  // namespace

TEST_F(PageTest, HeaderLookupOnSizeClassPage) {
  TestHeaderLookupOnSizeClassPage<GCed<1>>(GetHeap());
  TestHeaderLookupOnSizeClassPage<GCed<8>>(GetHeap());
  TestHeaderLookupOnSizeClassPage<GCed<16>>(GetHeap());
  TestHeaderLookupOnSizeClassPage<GCed<24>>(GetHeap());
  TestHeaderLookupOnSizeClassPage<GCed<kSizeClassCutoff / 2>>(GetHeap());
  // Largest object that still fits into a size class.
  TestHeaderLookupOnSizeClassPage<
      GCed<kSizeClassCutoff - kAllocationGranularity - sizeof(void*) -
           sizeof(HeapObjectHeader)>>(GetHeap());
}

}  // namespace internal
}  // namespace cppgc
