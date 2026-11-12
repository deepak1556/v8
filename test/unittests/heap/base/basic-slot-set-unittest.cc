// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/base/basic-slot-set.h"

#include <limits>
#include <map>
#include <vector>

#include "src/heap/base/free-list-category-cache.h"
#include "src/heap/base/typed-slot-set.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace heap {
namespace base {

static constexpr size_t kTestGranularity = sizeof(void*);
using TestSlotSet = ::heap::base::BasicSlotSet<kTestGranularity>;
static constexpr size_t kTestPageSize = 1 << 17;
static constexpr size_t kBucketsTestPage =
    TestSlotSet::BucketsForSize(kTestPageSize);

TEST(BasicSlotSet, InsertAndLookup1) {
  TestSlotSet* set = TestSlotSet::Allocate(kBucketsTestPage);
  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    EXPECT_FALSE(set->Lookup(i));
  }
  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    set->Insert<TestSlotSet::AccessMode::ATOMIC>(i);
  }
  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    EXPECT_TRUE(set->Lookup(i));
  }
  TestSlotSet::Delete(set);
}

TEST(BasicSlotSet, InsertAndLookup2) {
  TestSlotSet* set = TestSlotSet::Allocate(kBucketsTestPage);
  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    if (i % 7 == 0) {
      set->Insert<TestSlotSet::AccessMode::ATOMIC>(i);
    }
  }
  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    if (i % 7 == 0) {
      EXPECT_TRUE(set->Lookup(i));
    } else {
      EXPECT_FALSE(set->Lookup(i));
    }
  }
  TestSlotSet::Delete(set);
}

TEST(BasicSlotSet, Iterate) {
  TestSlotSet* set = TestSlotSet::Allocate(kBucketsTestPage);

  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    if (i % 7 == 0) {
      set->Insert<TestSlotSet::AccessMode::ATOMIC>(i);
    }
  }

  set->Iterate(
      0, 0, kBucketsTestPage,
      [](uintptr_t slot) {
        if (slot % 3 == 0) {
          return KEEP_SLOT;
        } else {
          return REMOVE_SLOT;
        }
      },
      TestSlotSet::KEEP_EMPTY_BUCKETS);

  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    if (i % 21 == 0) {
      EXPECT_TRUE(set->Lookup(i));
    } else {
      EXPECT_FALSE(set->Lookup(i));
    }
  }

  TestSlotSet::Delete(set);
}

TEST(BasicSlotSet, IterateFromHalfway) {
  TestSlotSet* set = TestSlotSet::Allocate(kBucketsTestPage);

  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    if (i % 7 == 0) {
      set->Insert<TestSlotSet::AccessMode::ATOMIC>(i);
    }
  }

  set->Iterate(
      0, kBucketsTestPage / 2, kBucketsTestPage,
      [](uintptr_t slot) {
        if (slot % 3 == 0) {
          return KEEP_SLOT;
        } else {
          return REMOVE_SLOT;
        }
      },
      TestSlotSet::KEEP_EMPTY_BUCKETS);

  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    if (i < kTestPageSize / 2 && i % 7 == 0) {
      EXPECT_TRUE(set->Lookup(i));
    } else if (i >= kTestPageSize / 2 && i % 21 == 0) {
      EXPECT_TRUE(set->Lookup(i));
    } else {
      EXPECT_FALSE(set->Lookup(i));
    }
  }

  TestSlotSet::Delete(set);
}

TEST(BasicSlotSet, Remove) {
  TestSlotSet* set = TestSlotSet::Allocate(kBucketsTestPage);

  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    if (i % 7 == 0) {
      set->Insert<TestSlotSet::AccessMode::ATOMIC>(i);
    }
  }

  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    if (i % 3 != 0) {
      set->Remove(i);
    }
  }

  for (size_t i = 0; i < kTestPageSize; i += kTestGranularity) {
    if (i % 21 == 0) {
      EXPECT_TRUE(set->Lookup(i));
    } else {
      EXPECT_FALSE(set->Lookup(i));
    }
  }

  TestSlotSet::Delete(set);
}

namespace {
void CheckRemoveRangeOn(uint32_t start, uint32_t end) {
  TestSlotSet* set = TestSlotSet::Allocate(kBucketsTestPage);
  uint32_t first = start == 0 ? 0 : start - kTestGranularity;
  uint32_t last = end == kTestPageSize ? end - kTestGranularity : end;
  for (const auto mode :
       {TestSlotSet::FREE_EMPTY_BUCKETS, TestSlotSet::KEEP_EMPTY_BUCKETS}) {
    for (uint32_t i = first; i <= last; i += kTestGranularity) {
      set->Insert<TestSlotSet::AccessMode::ATOMIC>(i);
    }
    set->RemoveRange(start, end, kBucketsTestPage, mode);
    if (first != start) {
      EXPECT_TRUE(set->Lookup(first));
    }
    if (last == end) {
      EXPECT_TRUE(set->Lookup(last));
    }
    for (size_t i = start; i < end; i += kTestGranularity) {
      EXPECT_FALSE(set->Lookup(i));
    }
  }
  TestSlotSet::Delete(set);
}
}  // namespace

TEST(BasicSlotSet, RemoveRange) {
  CheckRemoveRangeOn(0, kTestPageSize);
  CheckRemoveRangeOn(1 * kTestGranularity, 1023 * kTestGranularity);
  for (uint32_t start = 0; start <= 32; start++) {
    CheckRemoveRangeOn(start * kTestGranularity,
                       (start + 1) * kTestGranularity);
    CheckRemoveRangeOn(start * kTestGranularity,
                       (start + 2) * kTestGranularity);
    const uint32_t kEnds[] = {32, 64, 100, 128, 1024, 1500, 2048};
    for (size_t i = 0; i < sizeof(kEnds) / sizeof(uint32_t); i++) {
      for (int k = -3; k <= 3; k++) {
        uint32_t end = (kEnds[i] + k);
        if (start < end) {
          CheckRemoveRangeOn(start * kTestGranularity, end * kTestGranularity);
        }
      }
    }
  }
  TestSlotSet* set = TestSlotSet::Allocate(kBucketsTestPage);
  for (const auto mode :
       {TestSlotSet::FREE_EMPTY_BUCKETS, TestSlotSet::KEEP_EMPTY_BUCKETS}) {
    set->Insert<TestSlotSet::AccessMode::ATOMIC>(kTestPageSize / 2);
    set->RemoveRange(0, kTestPageSize, kBucketsTestPage, mode);
    for (uint32_t i = 0; i < kTestPageSize; i += kTestGranularity) {
      EXPECT_FALSE(set->Lookup(i));
    }
  }
  TestSlotSet::Delete(set);
}

TEST(BasicSlotSet, MigrateSlotsInRangeCrossesCellsAndBuckets) {
  TestSlotSet* src = TestSlotSet::Allocate(kBucketsTestPage);
  TestSlotSet* dst = TestSlotSet::Allocate(kBucketsTestPage);

  const size_t before = 31 * kTestGranularity;
  const size_t range_start = 32 * kTestGranularity;
  const size_t mid_cell = 45 * kTestGranularity;
  const size_t next_bucket = 1030 * kTestGranularity;
  const size_t range_end = 1040 * kTestGranularity;
  const size_t after = range_end;
  const size_t dst_start = 16 * kTestGranularity;

  src->Insert<TestSlotSet::AccessMode::NON_ATOMIC>(before);
  src->Insert<TestSlotSet::AccessMode::NON_ATOMIC>(range_start);
  src->Insert<TestSlotSet::AccessMode::NON_ATOMIC>(mid_cell);
  src->Insert<TestSlotSet::AccessMode::NON_ATOMIC>(next_bucket);
  src->Insert<TestSlotSet::AccessMode::NON_ATOMIC>(after);

  src->MigrateSlotsInRange(range_start, range_end, dst, dst_start);

  EXPECT_TRUE(src->Lookup(before));
  EXPECT_FALSE(src->Lookup(range_start));
  EXPECT_FALSE(src->Lookup(mid_cell));
  EXPECT_FALSE(src->Lookup(next_bucket));
  EXPECT_TRUE(src->Lookup(after));

  EXPECT_TRUE(dst->Lookup(dst_start));
  EXPECT_TRUE(dst->Lookup(dst_start + (mid_cell - range_start)));
  EXPECT_TRUE(dst->Lookup(dst_start + (next_bucket - range_start)));
  EXPECT_FALSE(dst->Lookup(dst_start + (after - range_start)));

  TestSlotSet::Delete(src);
  TestSlotSet::Delete(dst);
}

namespace {
enum class TestTypedSlotKind : uint8_t {
  kKindA = 0,
  kKindB = 1,
  kCleared = 2,
  kLast = kCleared,
};
}  // namespace

TEST(BasicSlotSet, TypedSlotSetMigrateSlotsInRange) {
  constexpr uintptr_t kSrcBase = 0x10000;
  constexpr uintptr_t kDstBase = 0x20000;
  BasicTypedSlotSet<TestTypedSlotKind> src(kSrcBase);
  BasicTypedSlotSet<TestTypedSlotKind> dst(kDstBase);

  src.Insert(TestTypedSlotKind::kKindA, 64);
  src.Insert(TestTypedSlotKind::kKindB, 128);
  src.Insert(TestTypedSlotKind::kKindA, 160);
  src.Insert(TestTypedSlotKind::kKindB, 256);

  src.MigrateSlotsInRange(120, 200, &dst, 32);

  std::vector<std::pair<TestTypedSlotKind, uintptr_t>> remaining_src;
  src.Iterate(
      [&](TestTypedSlotKind kind, uintptr_t addr) {
        remaining_src.emplace_back(kind, addr - kSrcBase);
        return KEEP_SLOT;
      },
      BasicTypedSlotSet<TestTypedSlotKind>::FREE_EMPTY_CHUNKS);
  ASSERT_EQ(2u, remaining_src.size());
  EXPECT_EQ(64u, remaining_src[0].second);
  EXPECT_EQ(256u, remaining_src[1].second);

  std::vector<std::pair<TestTypedSlotKind, uintptr_t>> migrated_dst;
  dst.Iterate(
      [&](TestTypedSlotKind kind, uintptr_t addr) {
        migrated_dst.emplace_back(kind, addr - kDstBase);
        return KEEP_SLOT;
      },
      BasicTypedSlotSet<TestTypedSlotKind>::FREE_EMPTY_CHUNKS);
  ASSERT_EQ(2u, migrated_dst.size());
  EXPECT_EQ(TestTypedSlotKind::kKindB, migrated_dst[0].first);
  EXPECT_EQ(40u, migrated_dst[0].second);
  EXPECT_EQ(TestTypedSlotKind::kKindA, migrated_dst[1].first);
  EXPECT_EQ(72u, migrated_dst[1].second);
}

TEST(BasicSlotSet, FreeListCategoryCacheUpdatesAndResets) {
  FreeListCategoryCache<16> cache;
  EXPECT_EQ(16, cache.FirstNonEmpty(0));

  cache.UpdateAfterAddition(11);
  cache.UpdateAfterAddition(4);
  EXPECT_EQ(4, cache.FirstNonEmpty(0));
  EXPECT_EQ(4, cache.FirstNonEmpty(4));
  EXPECT_EQ(11, cache.FirstNonEmpty(5));
  EXPECT_EQ(16, cache.FirstNonEmpty(12));

  cache.UpdateAfterRemoval(4);
  EXPECT_EQ(11, cache.FirstNonEmpty(0));
  cache.UpdateAfterRemoval(11);
  EXPECT_EQ(16, cache.FirstNonEmpty(0));
}

}  // namespace base
}  // namespace heap
