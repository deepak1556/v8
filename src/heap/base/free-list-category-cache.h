// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_BASE_FREE_LIST_CATEGORY_CACHE_H_
#define V8_HEAP_BASE_FREE_LIST_CATEGORY_CACHE_H_

#include <array>
#include <cstddef>

#include "src/base/logging.h"

namespace heap::base {

// Maintains an O(1) lookup table `next_nonempty_[c]` giving the smallest
// non-empty category index `>= c` (or `kNumberOfCategories` if none exist).
// Shared between the V8 JavaScript heap (`FreeListManyCached`) and `cppgc`
// (`cppgc::internal::FreeList`).
template <int kNumberOfCategories>
class FreeListCategoryCache final {
  static_assert(kNumberOfCategories > 0);

 public:
  static constexpr int kSentinel = kNumberOfCategories;

  FreeListCategoryCache() { Reset(); }

  void Reset() { next_nonempty_.fill(kSentinel); }

  void UpdateAfterAddition(int category) {
    DCHECK_GE(category, 0);
    DCHECK_LT(category, kNumberOfCategories);
    for (int i = category; i >= 0 && next_nonempty_[i] > category; --i) {
      next_nonempty_[i] = category;
    }
  }

  void UpdateAfterRemoval(int category) {
    DCHECK_GE(category, 0);
    DCHECK_LT(category, kNumberOfCategories);
    const int replacement = next_nonempty_[category + 1];
    for (int i = category; i >= 0 && next_nonempty_[i] == category; --i) {
      next_nonempty_[i] = replacement;
    }
  }

  int FirstNonEmpty(int min_category) const {
    DCHECK_GE(min_category, 0);
    if (min_category >= kNumberOfCategories) return kSentinel;
    return next_nonempty_[min_category];
  }

  const int* raw_data() const { return next_nonempty_.data(); }

#ifdef DEBUG
  template <typename IsCategoryEmptyFn>
  void CheckIntegrity(int last_category,
                      IsCategoryEmptyFn is_category_empty) const {
    DCHECK_LT(last_category, kNumberOfCategories);
    for (int i = 0; i <= last_category; ++i) {
      const int next = next_nonempty_[i];
      DCHECK(next == kSentinel || !is_category_empty(next));
      for (int j = i; j < std::min(next, last_category + 1); ++j) {
        DCHECK(is_category_empty(j));
      }
    }
  }
#endif

 private:
  // Overallocated by one so `next_nonempty_[category + 1]` is always valid.
  std::array<int, kNumberOfCategories + 1> next_nonempty_;
};

}  // namespace heap::base

#endif  // V8_HEAP_BASE_FREE_LIST_CATEGORY_CACHE_H_
