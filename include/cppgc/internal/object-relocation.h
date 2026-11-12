// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef INCLUDE_CPPGC_INTERNAL_OBJECT_RELOCATION_H_
#define INCLUDE_CPPGC_INTERNAL_OBJECT_RELOCATION_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include "cppgc/custom-space.h"
#include "cppgc/internal/api-constants.h"
#include "cppgc/internal/logging.h"
#include "cppgc/type-traits.h"
#include "v8config.h"  // NOLINT(build/include_directory)

namespace cppgc {
class HeapHandle;
}  // namespace cppgc

namespace cppgc::internal {

enum class RelocatableClassId : uint16_t {
  kNone = 0,
  kGenericTestType = 1,
  kCSSNumericLiteralValue = 2,
  kNodeRareData = 3,
  kInlineCSSStyleDeclaration = 4,
  kDatasetDOMStringMap = 5,
  kUniqueElementData = 6,
  kMutableCSSPropertyValueSet = 7,
  kSpaceSplitStringData = 8,
  kGenericAutoType = 9,
};

using RelocateCallback = void (*)(void* from, void* to, size_t size);
using PayloadSizeValidator = bool (*)(const void* payload,
                                      size_t allocated_payload_size);
using WrapperAssociationChecker = bool (*)(const void* payload);

struct ObjectRelocationDescriptor final {
  const char* exact_type_name = nullptr;
  RelocatableClassId class_id = RelocatableClassId::kNone;
  uint16_t required_alignment =
      static_cast<uint16_t>(api_constants::kDefaultAlignment);
  uint32_t min_payload_size = 0;
  bool supports_inline_storage_rebasing = false;
  bool may_have_script_wrapper = false;
  RelocateCallback relocate = nullptr;
  PayloadSizeValidator validate_payload_size = nullptr;
  WrapperAssociationChecker has_wrapper_association = nullptr;
};

inline void BitwiseRelocateObject(void* from, void* to, size_t size) {
  if (size > 0) {
    std::memcpy(to, from, size);
  }
}

template <typename T>
void DefaultRelocateObject(void* from, void* to, size_t /* size */) {
  auto* src = static_cast<T*>(from);
  ::new (to) T(std::move(*src));
  if constexpr (!std::is_trivially_destructible_v<T>) {
    src->~T();
  }
}

template <typename T, typename = void>
struct HasRelocationOwnerMarker : std::false_type {};

template <typename T>
struct HasRelocationOwnerMarker<
    T, std::void_t<typename T::CppgcExactRelocatableOwner>> : std::true_type {};

template <typename T, bool = HasRelocationOwnerMarker<T>::value>
struct IsExactRelocationOwnerMatch : std::false_type {};

template <typename T>
struct IsExactRelocationOwnerMatch<T, true>
    : std::bool_constant<std::is_same_v<
          std::remove_cv_t<T>, typename T::CppgcExactRelocatableOwner>> {};

template <typename T, typename = void>
struct HasDisallowObjectRelocationMarker : std::false_type {};

template <typename T>
struct HasDisallowObjectRelocationMarker<
    T, std::void_t<typename T::CppgcDisallowObjectRelocation>>
    : std::true_type {};

template <typename CustomSpace, typename = void>
struct SpaceSupportsObjectCompactionImpl : std::true_type {};

template <typename CustomSpace>
struct SpaceSupportsObjectCompactionImpl<
    CustomSpace, std::void_t<decltype(CustomSpace::kSupportsObjectCompaction)>>
    : std::bool_constant<!CustomSpace::kSupportsCompaction &&
                         CustomSpace::kSupportsObjectCompaction> {};

template <>
struct SpaceSupportsObjectCompactionImpl<void, void> : std::true_type {};

template <typename T>
struct GenericObjectRelocationDescriptorHolder {
  static constexpr uint16_t kRequiredAlignment =
      static_cast<uint16_t>(alignof(T) < api_constants::kDefaultAlignment
                                ? api_constants::kDefaultAlignment
                                : alignof(T));
  static constexpr ObjectRelocationDescriptor kDescriptor = {
      nullptr,
      RelocatableClassId::kGenericAutoType,
      kRequiredAlignment,
      static_cast<uint32_t>(sizeof(T)),
      false,
      false,
      &BitwiseRelocateObject,
      nullptr,
      nullptr,
  };
};

template <typename T, typename = void>
struct ObjectRelocationTrait {
  using DecayedT = std::remove_cv_t<T>;
  static constexpr bool kIsOptedIn =
      IsExactRelocationOwnerMatch<DecayedT>::value;
  static constexpr bool kIsDisallowed =
      HasDisallowObjectRelocationMarker<DecayedT>::value ||
      IsAllocatedOnCompactableSpace<DecayedT>::value ||
      !SpaceSupportsObjectCompactionImpl<
          typename SpaceTrait<DecayedT>::Space>::value;
  static constexpr bool kIsGenericRelocatable =
      IsGarbageCollectedType<DecayedT>::value &&
      !IsGarbageCollectedMixinType<DecayedT>::value &&
      !HasRelocationOwnerMarker<DecayedT>::value && !kIsDisallowed;
  static constexpr bool kIsRelocatable = kIsOptedIn || kIsGenericRelocatable;
  static constexpr bool kPreventGCInfoFolding =
      HasRelocationOwnerMarker<DecayedT>::value;

  static constexpr const ObjectRelocationDescriptor* GetDescriptor() {
    if constexpr (kIsDisallowed) {
      return nullptr;
    } else if constexpr (kIsOptedIn) {
      return DecayedT::GetObjectRelocationDescriptor();
    } else if constexpr (kIsGenericRelocatable) {
      return &GenericObjectRelocationDescriptorHolder<DecayedT>::kDescriptor;
    } else {
      return nullptr;
    }
  }
};

#define CPPGC_DECLARE_EXACT_RELOCATABLE_TYPE(Type)            \
  using CppgcExactRelocatableOwner = Type;                    \
  static const ::cppgc::internal::ObjectRelocationDescriptor* \
  GetObjectRelocationDescriptor()

#define CPPGC_DISALLOW_OBJECT_RELOCATION() \
  using CppgcDisallowObjectRelocation = void

enum class RelocationStorageCodec : uint8_t {
  kCompressedMember = 0,
  kUncompressedMember = 1,
  kCompressedWeakMember = 2,
  kUncompressedWeakMember = 3,
  kTaggedUncompressedMember = 4,
  kStrongPersistent = 5,
  kWeakPersistent = 6,
  kCrossThreadPersistent = 7,
  kCustomAdapter = 8,
};

enum class RelocationReferenceStrength : uint8_t {
  kStrong = 0,
  kWeak = 1,
  kEphemeronKey = 2,
  kEphemeronValue = 3,
};

using TypedSlotRetargetCallback = void (*)(const void* slot,
                                           void* new_pointee_address);

struct PinToken final {
  void* normal_page = nullptr;
  void* heap_base = nullptr;

  constexpr bool IsValid() const { return normal_page != nullptr; }
};

struct ObjectCompactionSummary final {
  bool executed = false;
  bool succeeded = false;
  size_t candidate_objects = 0;
  size_t moved_objects = 0;
  size_t moved_allocated_bytes = 0;
  size_t retained_packed_objects = 0;
  size_t pinned_pages = 0;
  size_t source_pages_emptied = 0;
  size_t new_destination_pages = 0;
  size_t reused_packed_destination_pages = 0;
  size_t typed_member_slots_updated = 0;
  size_t persistent_roots_updated = 0;
  size_t address_keyed_rehashes = 0;
};

class V8_EXPORT ObjectPinningApi final {
 public:
  ObjectPinningApi() = delete;

  static PinToken AcquireBorrowPin(const void* payload);
  static void ReleaseBorrowPin(PinToken token) noexcept;
  static void EnterNoSafepointBorrowScope(const void* payload);
  static void LeaveNoSafepointBorrowScope(const void* payload) noexcept;
  static bool IsRelocatableObject(const void* payload) noexcept;
  static void ObserveForeignTargetWrite(const void* payload) noexcept;
  static void RecordUntracedMemberTarget(const void* payload) noexcept;

  static void RegisterAddressKeyedContainer(cppgc::HeapHandle& heap_handle,
                                            void* context,
                                            void (*callback)(void*));
  static void UnregisterAddressKeyedContainer(cppgc::HeapHandle& heap_handle,
                                              void* context);
  static void SetCompactionEnabled(cppgc::HeapHandle& heap_handle,
                                   bool enabled);
  static ObjectCompactionSummary GetLastCompactionSummary(
      const cppgc::HeapHandle& heap_handle);
};

// Test-only override allowing unit tests to exercise relocation on small
// single-page synthetic setups that the multi-page profitability guard would
// otherwise skip.
V8_EXPORT void SetTestingForceObjectCompaction(bool enabled);
V8_EXPORT bool GetTestingForceObjectCompaction();

}  // namespace cppgc::internal

#endif  // INCLUDE_CPPGC_INTERNAL_OBJECT_RELOCATION_H_
