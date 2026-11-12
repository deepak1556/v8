// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/cppgc-internal/liveness-broker.h"

#include "src/heap/cppgc-internal/heap-object-header.h"
#include "src/heap/cppgc-internal/object-compactor.h"

namespace cppgc {

bool LivenessBroker::IsHeapObjectAliveImpl(const void* payload) const {
  const bool alive = internal::HeapObjectHeader::FromObject(payload).IsMarked();
  if (V8_UNLIKELY(pins_raw_queries_) && alive) {
    internal::ObjectCompactor::ObserveGlobalForeignTargetWrite(payload);
  }
  return alive;
}

bool LivenessBroker::IsHeapObjectAliveTypedWeakImpl(const void* payload) const {
  return internal::HeapObjectHeader::FromObject(payload).IsMarked();
}

namespace internal {

// static
cppgc::LivenessBroker LivenessBrokerFactory::Create(bool pins_raw_queries) {
  return cppgc::LivenessBroker(pins_raw_queries);
}

}  // namespace internal

}  // namespace cppgc
