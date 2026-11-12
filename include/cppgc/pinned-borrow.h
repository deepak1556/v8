// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef INCLUDE_CPPGC_PINNED_BORROW_H_
#define INCLUDE_CPPGC_PINNED_BORROW_H_

#include <cstddef>
#include <type_traits>

#include "cppgc/internal/object-relocation.h"
#include "cppgc/macros.h"
#include "cppgc/persistent.h"
#include "cppgc/sentinel-pointer.h"
#include "cppgc/source-location.h"
#include "cppgc/type-traits.h"
#include "v8config.h"  // NOLINT(build/include_directory)

namespace cppgc {

/**
 * Stack-only scoped native borrow that roots a managed allocation via an
 * off-heap strong Persistent and pins its owning normal page at a stable
 * virtual address until the borrow scope ends.
 */
template <typename T>
class PinnedBorrow final {
  CPPGC_STACK_ALLOCATED();
  static_assert(sizeof(T), "Pointee type must be fully defined.");
  static_assert(internal::IsGarbageCollectedOrMixinType<T>::value,
                "PinnedBorrow<T> requires a GarbageCollected or "
                "GarbageCollectedMixin type.");

 public:
  using PointeeType = T;

  PinnedBorrow() = default;
  explicit PinnedBorrow(std::nullptr_t) {}

  explicit PinnedBorrow(T* ptr,
                        const SourceLocation& loc = SourceLocation::Current())
      : ptr_(ptr) {
    if (!ptr_ || ptr_ == kSentinelPointer) {
      ptr_ = nullptr;
      return;
    }
    internal::ObjectPinningApi::EnterNoSafepointBorrowScope(ptr_);
    pin_token_ = internal::ObjectPinningApi::AcquireBorrowPin(ptr_);
    root_ = Persistent<T>(ptr_, loc);
    internal::ObjectPinningApi::LeaveNoSafepointBorrowScope(ptr_);
  }

  explicit PinnedBorrow(T& ref,
                        const SourceLocation& loc = SourceLocation::Current())
      : PinnedBorrow(&ref, loc) {}

  template <typename U, typename WeaknessPolicy, typename LocationPolicy,
            typename CheckingPolicy,
            typename = std::enable_if_t<std::is_base_of_v<T, U>>>
  explicit PinnedBorrow(
      const internal::BasicPersistent<U, WeaknessPolicy, LocationPolicy,
                                      CheckingPolicy>& persistent,
      const SourceLocation& loc = SourceLocation::Current())
      : PinnedBorrow(persistent.Get(), loc) {}

  template <typename U, typename WeaknessTag, typename WriteBarrierPolicy,
            typename CheckingPolicy, typename StorageType,
            typename = std::enable_if_t<std::is_base_of_v<T, U>>>
  explicit PinnedBorrow(
      const internal::BasicMember<U, WeaknessTag, WriteBarrierPolicy,
                                  CheckingPolicy, StorageType>& member,
      const SourceLocation& loc = SourceLocation::Current())
      : PinnedBorrow(member.Get(), loc) {}

  ~PinnedBorrow() noexcept { Reset(); }

  PinnedBorrow(const PinnedBorrow&) = delete;
  PinnedBorrow& operator=(const PinnedBorrow&) = delete;
  PinnedBorrow(PinnedBorrow&&) = delete;
  PinnedBorrow& operator=(PinnedBorrow&&) = delete;

  void* operator new[](size_t) = delete;

  T* get() const { return ptr_; }
  T* Get() const { return ptr_; }
  T* operator->() const {
    CPPGC_DCHECK(ptr_ != nullptr);
    return ptr_;
  }
  T& operator*() const {
    CPPGC_DCHECK(ptr_ != nullptr);
    return *ptr_;
  }
  explicit operator bool() const { return ptr_ != nullptr; }

  bool is_pinned() const { return pin_token_.IsValid(); }

  void Reset() noexcept {
    if (!ptr_) return;
    const void* raw = ptr_;
    internal::ObjectPinningApi::EnterNoSafepointBorrowScope(raw);
    root_.Clear();
    if (pin_token_.IsValid()) {
      internal::ObjectPinningApi::ReleaseBorrowPin(pin_token_);
      pin_token_ = {};
    }
    ptr_ = nullptr;
    internal::ObjectPinningApi::LeaveNoSafepointBorrowScope(raw);
  }

 private:
  T* ptr_ = nullptr;
  internal::PinToken pin_token_{};
  Persistent<T> root_;
};

}  // namespace cppgc

#endif  // INCLUDE_CPPGC_PINNED_BORROW_H_
