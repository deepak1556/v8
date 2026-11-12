// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_CPPGC_INTERNAL_RAW_HEAP_H_
#define V8_HEAP_CPPGC_INTERNAL_RAW_HEAP_H_

#include <iterator>
#include <memory>
#include <vector>

#include "include/cppgc/heap.h"
#include "src/base/logging.h"
#include "src/base/macros.h"
#include "src/heap/cppgc-internal/globals.h"

namespace cppgc {
namespace internal {

class HeapBase;
class BaseSpace;

// RawHeap is responsible for space management.
class V8_EXPORT_PRIVATE RawHeap final {
 public:
  // Regular spaces are used to store objects depending on their allocation
  // size (which includes the header):
  // - [0, kNumberOfSizeClasses): Size-class spaces for objects smaller than
  //   `kSizeClassCutoff`. There is one space per `kAllocationGranularity` step
  //   and all objects in such a space have the exact same size. See
  //   `SizeClassSpaceForSize()`.
  // - kNormal: Objects of mixed sizes that are at least `kSizeClassCutoff`.
  // - kLarge: Objects of size greater than 2^16.
  //
  // Users can override where objects are allocated via cppgc::CustomSpace to
  // force allocation in a custom space. Custom spaces hold objects of mixed
  // sizes.
  enum class RegularSpaceType : uint8_t {
    kFirstSizeClass = 0,
    kNormal = kNumberOfSizeClasses,
    kLarge,
  };

  static constexpr size_t kNumberOfRegularSpaces =
      static_cast<size_t>(RegularSpaceType::kLarge) + 1;

  // Returns the size-class space for an allocation of `allocation_size` bytes.
  // `allocation_size` includes the header and must be smaller than
  // `kSizeClassCutoff`.
  static constexpr RegularSpaceType SizeClassSpaceForSize(
      size_t allocation_size) {
    DCHECK_LE(kMinSizeClassSize, allocation_size);
    DCHECK_GT(kSizeClassCutoff, allocation_size);
    DCHECK_EQ(0u, allocation_size & kAllocationMask);
    return static_cast<RegularSpaceType>((allocation_size - kMinSizeClassSize) /
                                         kAllocationGranularity);
  }

  // Returns the size of objects in the size-class space with index
  // `space_index`.
  static constexpr size_t SizeForSizeClassSpace(size_t space_index) {
    DCHECK_GT(kNumberOfSizeClasses, space_index);
    return kMinSizeClassSize + space_index * kAllocationGranularity;
  }

  using Spaces = std::vector<std::unique_ptr<BaseSpace>>;
  using iterator = Spaces::iterator;
  using const_iterator = Spaces::const_iterator;

  RawHeap(HeapBase* heap,
          const std::vector<std::unique_ptr<CustomSpaceBase>>& custom_spaces);

  RawHeap(const RawHeap&) = delete;
  RawHeap& operator=(const RawHeap&) = delete;

  ~RawHeap();

  // Space iteration support.
  iterator begin() { return spaces_.begin(); }
  const_iterator begin() const { return spaces_.begin(); }
  iterator end() { return spaces_.end(); }
  const_iterator end() const { return spaces_.end(); }

  iterator custom_begin() { return std::next(begin(), kNumberOfRegularSpaces); }
  iterator custom_end() { return end(); }

  size_t size() const { return spaces_.size(); }

  BaseSpace* Space(RegularSpaceType type) {
    const size_t index = static_cast<size_t>(type);
    DCHECK_GT(kNumberOfRegularSpaces, index);
    return Space(index);
  }
  const BaseSpace* Space(RegularSpaceType space) const {
    return const_cast<RawHeap&>(*this).Space(space);
  }

  BaseSpace* CustomSpace(CustomSpaceIndex space_index) {
    return Space(SpaceIndexForCustomSpace(space_index));
  }
  const BaseSpace* CustomSpace(CustomSpaceIndex space_index) const {
    return const_cast<RawHeap&>(*this).CustomSpace(space_index);
  }

  HeapBase* heap() { return main_heap_; }
  const HeapBase* heap() const { return main_heap_; }

 private:
  size_t SpaceIndexForCustomSpace(CustomSpaceIndex space_index) const {
    DCHECK_LT(space_index.value, spaces_.size() - kNumberOfRegularSpaces);
    return kNumberOfRegularSpaces + space_index.value;
  }

  BaseSpace* Space(size_t space_index) {
    DCHECK_GT(spaces_.size(), space_index);
    BaseSpace* space = spaces_[space_index].get();
    DCHECK(space);
    return space;
  }
  const BaseSpace* Space(size_t space_index) const {
    return const_cast<RawHeap&>(*this).Space(space_index);
  }

  HeapBase* main_heap_;
  Spaces spaces_;
};

}  // namespace internal
}  // namespace cppgc

#endif  // V8_HEAP_CPPGC_INTERNAL_RAW_HEAP_H_
