// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/cppgc/internal/object-relocation.h"

#include <cstdint>
#include <vector>

#include "include/cppgc/allocation.h"
#include "include/cppgc/cross-thread-persistent.h"
#include "include/cppgc/garbage-collected.h"
#include "include/cppgc/member.h"
#include "include/cppgc/persistent.h"
#include "include/cppgc/pinned-borrow.h"
#include "include/cppgc/tagged-member.h"
#include "include/cppgc/visitor.h"
#include "src/heap/cppgc-internal/compaction-worklists.h"
#include "src/heap/cppgc-internal/heap-object-header.h"
#include "src/heap/cppgc-internal/heap-page.h"
#include "src/heap/cppgc-internal/heap.h"
#include "src/heap/cppgc-internal/object-compactor.h"
#include "test/unittests/heap/cppgc/tests.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cppgc::internal {
namespace {

class ScopedForceCompaction final {
 public:
  ScopedForceCompaction() { SetTestingForceObjectCompaction(true); }
  ~ScopedForceCompaction() { SetTestingForceObjectCompaction(false); }
};

class RelocatableMixin : public GarbageCollectedMixin {
 public:
  explicit RelocatableMixin(uint64_t tag = 0x1122334455667788ULL)
      : mixin_tag_(tag) {}
  void Trace(Visitor* visitor) const override {}
  uint64_t mixin_tag() const { return mixin_tag_; }

 private:
  uint64_t mixin_tag_;
};

class RelocatableLeaf final : public GarbageCollected<RelocatableLeaf>,
                              public RelocatableMixin {
 public:
  CPPGC_DECLARE_EXACT_RELOCATABLE_TYPE(RelocatableLeaf);

  explicit RelocatableLeaf(uint64_t value, RelocatableLeaf* next = nullptr,
                           RelocatableLeaf* weak_peer = nullptr)
      : RelocatableMixin(value ^ 0xA5A5A5A5A5A5A5A5ULL),
        value_(value),
        next_(next),
        weak_peer_(weak_peer) {}

  RelocatableLeaf(RelocatableLeaf&& other) noexcept
      : RelocatableMixin(other.mixin_tag()),
        value_(other.value_),
        next_(other.next_.Get()),
        weak_peer_(other.weak_peer_.Get()) {
    other.value_ = 0xDEADDEADDEADDEADULL;
  }

  void Trace(Visitor* visitor) const override {
    visitor->Trace(next_);
    visitor->Trace(weak_peer_);
    RelocatableMixin::Trace(visitor);
  }

  uint64_t value() const { return value_; }
  RelocatableLeaf* next() const { return next_.Get(); }
  RelocatableLeaf* weak_peer() const { return weak_peer_.Get(); }
  void set_next(RelocatableLeaf* next) { next_ = next; }
  void set_weak_peer(RelocatableLeaf* peer) { weak_peer_ = peer; }

 private:
  uint64_t value_;
  Member<RelocatableLeaf> next_;
  WeakMember<RelocatableLeaf> weak_peer_;
};

const ObjectRelocationDescriptor*
RelocatableLeaf::GetObjectRelocationDescriptor() {
  static constexpr ObjectRelocationDescriptor kDesc = {
      "RelocatableLeaf",
      RelocatableClassId::kGenericTestType,
      alignof(RelocatableLeaf),
      sizeof(RelocatableLeaf),
      false,
      false,
      &DefaultRelocateObject<RelocatableLeaf>,
      nullptr,
      nullptr};
  return &kDesc;
}

class NonRelocatableFiller final
    : public GarbageCollected<NonRelocatableFiller> {
 public:
  explicit NonRelocatableFiller(uint64_t seed) {
    for (size_t i = 0; i < std::size(payload_); ++i) {
      payload_[i] = seed + i;
    }
  }
  void Trace(Visitor*) const {}
  uint64_t first() const { return payload_[0]; }

 private:
  uint64_t payload_[3];
};

class GraphHolder final : public GarbageCollected<GraphHolder> {
 public:
  GraphHolder(RelocatableLeaf* strong_leaf, RelocatableMixin* mixin_ref,
              RelocatableLeaf* weak_leaf)
      : strong_leaf_(strong_leaf),
        mixin_ref_(mixin_ref),
        weak_leaf_(weak_leaf),
        ephemeron_(strong_leaf, weak_leaf) {
    array_[0] = strong_leaf;
    array_[1] = weak_leaf;
  }

  void Trace(Visitor* visitor) const {
    visitor->Trace(strong_leaf_);
    visitor->Trace(mixin_ref_);
    visitor->Trace(weak_leaf_);
    visitor->Trace(ephemeron_);
    visitor->TraceMultiple(array_, 2);
  }

  RelocatableLeaf* strong_leaf() const { return strong_leaf_.Get(); }
  RelocatableMixin* mixin_ref() const { return mixin_ref_.Get(); }
  RelocatableLeaf* weak_leaf() const { return weak_leaf_.Get(); }
  RelocatableLeaf* array_at(size_t idx) const { return array_[idx].Get(); }
  const EphemeronPair<RelocatableLeaf, RelocatableLeaf>& ephemeron() const {
    return ephemeron_;
  }

 private:
  Member<RelocatableLeaf> strong_leaf_;
  Member<RelocatableMixin> mixin_ref_;
  WeakMember<RelocatableLeaf> weak_leaf_;
  EphemeronPair<RelocatableLeaf, RelocatableLeaf> ephemeron_;
  Member<RelocatableLeaf> array_[2];
};

class ObjectRelocationTest : public testing::TestWithHeap {
 protected:
  Heap* internal_heap() { return Heap::From(GetHeap()); }
  ObjectCompactor& object_compactor() {
    return internal_heap()->object_compactor();
  }

  void RunMemoryReducingGC(
      StackState stack_state = StackState::kNoHeapPointers) {
    internal_heap()->CollectGarbage(
        {CollectionType::kMajor, stack_state, GCConfig::MarkingType::kAtomic,
         GCConfig::SweepingType::kAtomic,
         GCConfig::FreeMemoryHandling::kReleaseMemory,
         GCConfig::IsForcedGC::kForced});
  }
};

}  // namespace

TEST_F(ObjectRelocationTest,
       RelocatesTypedMixinAndEphemeronGraphAndRebasesSlots) {
  ScopedForceCompaction force_compaction;
  object_compactor().SetEnabled(true);

  Persistent<RelocatableLeaf> leaf_a =
      MakeGarbageCollected<RelocatableLeaf>(GetAllocationHandle(), 101);
  for (int i = 0; i < 4500; ++i) {
    MakeGarbageCollected<NonRelocatableFiller>(GetAllocationHandle(), i);
  }
  Persistent<RelocatableLeaf> leaf_b = MakeGarbageCollected<RelocatableLeaf>(
      GetAllocationHandle(), 202, leaf_a.Get(), leaf_a.Get());
  leaf_a->set_weak_peer(leaf_b.Get());
  for (int i = 0; i < 4500; ++i) {
    MakeGarbageCollected<NonRelocatableFiller>(GetAllocationHandle(),
                                               10000 + i);
  }

  WeakPersistent<RelocatableLeaf> weak_root(leaf_b.Get());
  Persistent<GraphHolder> holder = MakeGarbageCollected<GraphHolder>(
      GetAllocationHandle(), leaf_a.Get(),
      static_cast<RelocatableMixin*>(leaf_b.Get()), leaf_b.Get());

  const void* old_a = leaf_a.Get();
  const void* old_b = leaf_b.Get();

  size_t rehash_calls = 0;
  object_compactor().RegisterAddressKeyedContainer(
      &rehash_calls, [](void* ctx) { ++(*static_cast<size_t*>(ctx)); });

  RunMemoryReducingGC(StackState::kNoHeapPointers);

  object_compactor().UnregisterAddressKeyedContainer(&rehash_calls);

  const auto& summary = object_compactor().last_summary();
  ASSERT_TRUE(summary.executed);
  ASSERT_TRUE(summary.succeeded);
  EXPECT_GE(summary.moved_objects + summary.retained_packed_objects, 2u);
  EXPECT_GE(summary.moved_objects, 1u);
  EXPECT_EQ(1u, rehash_calls);

  if (summary.retained_packed_objects == 0) {
    EXPECT_NE(old_a, leaf_a.Get());
  } else {
    EXPECT_EQ(BasePage::FromPayload(leaf_a.Get()),
              BasePage::FromPayload(leaf_b.Get()));
  }
  EXPECT_NE(old_b, leaf_b.Get());
  EXPECT_EQ(101u, leaf_a->value());
  EXPECT_EQ(202u, leaf_b->value());
  EXPECT_EQ(leaf_a.Get(), leaf_b->next());
  EXPECT_EQ(leaf_b.Get(), leaf_a->weak_peer());
  EXPECT_EQ(leaf_b.Get(), weak_root.Get());
  EXPECT_EQ(leaf_a.Get(), holder->strong_leaf());
  EXPECT_EQ(static_cast<RelocatableMixin*>(leaf_b.Get()), holder->mixin_ref());
  EXPECT_EQ(202u ^ 0xA5A5A5A5A5A5A5A5ULL, holder->mixin_ref()->mixin_tag());
  EXPECT_EQ(leaf_b.Get(), holder->weak_leaf());
  EXPECT_EQ(leaf_a.Get(), holder->array_at(0));
  EXPECT_EQ(leaf_b.Get(), holder->array_at(1));
  EXPECT_EQ(leaf_a.Get(), holder->ephemeron().key.Get());
  EXPECT_EQ(leaf_b.Get(), holder->ephemeron().value.Get());
}

TEST_F(ObjectRelocationTest,
       PinnedBorrowAndCrossThreadRootPinPageWithoutPreventingOtherPages) {
  ScopedForceCompaction force_compaction;
  object_compactor().SetEnabled(true);

  Persistent<RelocatableLeaf> pinned_by_borrow =
      MakeGarbageCollected<RelocatableLeaf>(GetAllocationHandle(), 11);
  while (BasePage::FromPayload(
             MakeGarbageCollected<RelocatableLeaf>(GetAllocationHandle(), 1)) ==
         BasePage::FromPayload(pinned_by_borrow.Get())) {
  }
  Persistent<RelocatableLeaf> pinned_by_ctp =
      MakeGarbageCollected<RelocatableLeaf>(GetAllocationHandle(), 22);
  while (BasePage::FromPayload(
             MakeGarbageCollected<RelocatableLeaf>(GetAllocationHandle(), 2)) ==
         BasePage::FromPayload(pinned_by_ctp.Get())) {
  }
  Persistent<RelocatableLeaf> movable_leaf =
      MakeGarbageCollected<RelocatableLeaf>(GetAllocationHandle(), 33);

  ASSERT_NE(BasePage::FromPayload(pinned_by_borrow.Get()),
            BasePage::FromPayload(movable_leaf.Get()));
  ASSERT_NE(BasePage::FromPayload(pinned_by_ctp.Get()),
            BasePage::FromPayload(movable_leaf.Get()));

  const void* orig_borrow_addr = pinned_by_borrow.Get();
  const void* orig_ctp_addr = pinned_by_ctp.Get();
  const void* orig_movable_addr = movable_leaf.Get();

  subtle::CrossThreadPersistent<RelocatableLeaf> ctp_root(pinned_by_ctp.Get());
  PinnedBorrow<RelocatableLeaf> borrow(pinned_by_borrow.Get());

  RunMemoryReducingGC(StackState::kNoHeapPointers);

  EXPECT_EQ(orig_borrow_addr, pinned_by_borrow.Get());
  EXPECT_EQ(orig_ctp_addr, pinned_by_ctp.Get());
  EXPECT_NE(orig_movable_addr, movable_leaf.Get());
  EXPECT_EQ(33u, movable_leaf->value());
}

namespace {

struct GenericParentTag {};
struct GenericAltTag {};

class GenericEmbedderNode : public GarbageCollected<GenericEmbedderNode> {
 public:
  GenericEmbedderNode(uint64_t id, GenericEmbedderNode* parent = nullptr,
                      bool use_alt_tag = false)
      : id_(id),
        tagged_parent_(
            use_alt_tag
                ? subtle::TaggedUncompressedMember<
                      GenericEmbedderNode, GenericParentTag, GenericAltTag>(
                      GenericAltTag{}, parent)
                : subtle::TaggedUncompressedMember<
                      GenericEmbedderNode, GenericParentTag, GenericAltTag>(
                      GenericParentTag{}, parent)) {
    ++live_instances_;
  }
  GenericEmbedderNode(const GenericEmbedderNode&) = delete;
  GenericEmbedderNode& operator=(const GenericEmbedderNode&) = delete;
  virtual ~GenericEmbedderNode() { --live_instances_; }

  virtual void Trace(Visitor* visitor) const {
    visitor->Trace(tagged_parent_);
    visitor->Trace(child_);
  }

  uint64_t id() const { return id_; }
  void set_child(GenericEmbedderNode* c) { child_ = c; }
  GenericEmbedderNode* child() const { return child_.Get(); }
  GenericEmbedderNode* parent_untagged() const {
    return tagged_parent_.GetUntagged();
  }
  bool is_alt_tagged() const { return tagged_parent_.Is<GenericAltTag>(); }

  static int live_instances() { return live_instances_; }

 private:
  static inline int live_instances_ = 0;
  uint64_t id_;
  subtle::TaggedUncompressedMember<GenericEmbedderNode, GenericParentTag,
                                   GenericAltTag>
      tagged_parent_;
  Member<GenericEmbedderNode> child_;
};

class SelfReferentialNode final : public GarbageCollected<SelfReferentialNode> {
 public:
  explicit SelfReferentialNode(uint64_t v)
      : inline_ptr_(reinterpret_cast<uintptr_t>(&inline_buf_[0])) {
    inline_buf_[0] = v;
  }
  void Trace(Visitor*) const {}
  bool points_into_self() const {
    return inline_ptr_ == reinterpret_cast<uintptr_t>(&inline_buf_[0]);
  }

 private:
  uintptr_t inline_ptr_;
  uint64_t inline_buf_[2] = {0, 0};
};

class DisallowedBase : public GarbageCollected<DisallowedBase> {
 public:
  CPPGC_DISALLOW_OBJECT_RELOCATION();
  explicit DisallowedBase(uint64_t v) : v_(v) {}
  virtual ~DisallowedBase() = default;
  virtual void Trace(Visitor*) const {}
  uint64_t v() const { return v_; }

 private:
  uint64_t v_;
};

class DisallowedDerived final : public DisallowedBase {
 public:
  explicit DisallowedDerived(uint64_t v) : DisallowedBase(v) {}
};

}  // namespace

TEST_F(ObjectRelocationTest,
       GenericRelocationCompactsUnannotatedEmbedderClassesSafely) {
  ScopedForceCompaction force_compaction;
  object_compactor().SetEnabled(true);

  Persistent<GenericEmbedderNode> parent_node =
      MakeGarbageCollected<GenericEmbedderNode>(GetAllocationHandle(), 1001);
  for (int i = 0; i < 4500; ++i) {
    MakeGarbageCollected<NonRelocatableFiller>(GetAllocationHandle(), i);
  }
  Persistent<GenericEmbedderNode> child_node =
      MakeGarbageCollected<GenericEmbedderNode>(GetAllocationHandle(), 1002,
                                                parent_node.Get(), true);
  parent_node->set_child(child_node.Get());

  for (int i = 0; i < 4500; ++i) {
    MakeGarbageCollected<NonRelocatableFiller>(GetAllocationHandle(), 5000 + i);
  }
  Persistent<GenericEmbedderNode> untraced_target =
      MakeGarbageCollected<GenericEmbedderNode>(GetAllocationHandle(), 1003);
  UntracedMember<GenericEmbedderNode> untraced_ref(untraced_target.Get());

  Persistent<SelfReferentialNode> self_ref =
      MakeGarbageCollected<SelfReferentialNode>(GetAllocationHandle(), 77);
  Persistent<DisallowedDerived> disallowed =
      MakeGarbageCollected<DisallowedDerived>(GetAllocationHandle(), 88);

  EXPECT_TRUE(ObjectPinningApi::IsRelocatableObject(parent_node.Get()));
  EXPECT_TRUE(ObjectPinningApi::IsRelocatableObject(child_node.Get()));
  EXPECT_FALSE(ObjectPinningApi::IsRelocatableObject(disallowed.Get()));

  const void* orig_parent = parent_node.Get();
  const void* orig_child = child_node.Get();
  const void* orig_untraced = untraced_target.Get();
  const void* orig_self_ref = self_ref.Get();
  const void* orig_disallowed = disallowed.Get();
  const int live_before = GenericEmbedderNode::live_instances();

  RunMemoryReducingGC(StackState::kNoHeapPointers);

  // Unannotated parent & child moved without running destructors prematurely,
  // and TaggedUncompressedMember preserved both the relocated pointer and tag!
  EXPECT_EQ(live_before, GenericEmbedderNode::live_instances());
  EXPECT_NE(orig_child, child_node.Get());
  EXPECT_NE(orig_parent, parent_node.Get());
  EXPECT_EQ(child_node.Get(), parent_node->child());
  EXPECT_EQ(parent_node.Get(), child_node->parent_untagged());
  EXPECT_TRUE(child_node->is_alt_tagged());

  // UntracedMember target, interior self-pointer object, and opted-out derived
  // class all remained pinned in place.
  EXPECT_EQ(orig_untraced, untraced_target.Get());
  EXPECT_EQ(untraced_target.Get(), untraced_ref.Get());
  EXPECT_EQ(orig_self_ref, self_ref.Get());
  EXPECT_TRUE(self_ref->points_into_self());
  EXPECT_EQ(orig_disallowed, disallowed.Get());
  EXPECT_EQ(88u, disallowed->v());
}

}  // namespace cppgc::internal
