---
name: perfetto-migration
description: "Migrate V8 legacy Perfetto trace events to modern typed Perfetto macros."
---

# Perfetto Legacy → Modern Trace Event Migration

Use this skill when migrating legacy numbered Perfetto trace macros (e.g. `TRACE_EVENT0`, `TRACE_EVENT_BEGIN1`) to the modern typed Perfetto macros (`TRACE_EVENT`, `TRACE_EVENT_BEGIN`, etc.) in V8.

**Tracking bug:** https://issues.chromium.org/issues/498378089
**Perfetto SDK headers:** `third_party/perfetto/include/perfetto/tracing/`

## Related Skills

- [v8_commands](../v8_commands/SKILL.md) — Build commands (`tools/dev/gm.py`)
- [v8_testing](../v8_testing/SKILL.md) — Running tests (`tools/run-tests.py`)
- [v8_best_practices](../v8_best_practices/SKILL.md) — Code standards, formatting, minimal diffs

## Architecture Overview

### Build Configuration

- `v8_use_perfetto` GN arg → defines `V8_USE_PERFETTO` preprocessor macro
- When `V8_USE_PERFETTO` is set, `src/tracing/perfetto-sdk.h` is included, which currently sets:
  ```cpp
  #define PERFETTO_ENABLE_LEGACY_TRACE_EVENTS 1
  ```
- When `V8_USE_PERFETTO` is **not** set, `src/tracing/trace-event-no-perfetto.h` is included instead. This file contains:
  - Stub (no-op) definitions for **modern** macros (`TRACE_EVENT`, `TRACE_EVENT_BEGIN`, etc.)
  - Full legacy macro definitions (`TRACE_EVENT0`, `TRACE_EVENT_BEGIN1`, etc.) that route through V8's own `TracingController`

### Key Files

| File | Purpose |
|---|---|
| `src/tracing/perfetto-sdk.h` | Sets `PERFETTO_ENABLE_LEGACY_TRACE_EVENTS`, includes Perfetto headers |
| `src/tracing/trace-event.h` | Main include, dispatches to perfetto-sdk.h or trace-event-no-perfetto.h |
| `src/tracing/trace-event-no-perfetto.h` | Legacy macro definitions + modern macro stubs |
| `src/tracing/trace-categories.h` | Perfetto category registry |
| `third_party/perfetto/include/perfetto/tracing/track_event.h` | Modern macro definitions (`TRACE_EVENT`, `TRACE_EVENT_BEGIN`, etc.) |
| `third_party/perfetto/include/perfetto/tracing/track_event_legacy.h` | Legacy→Perfetto compatibility shim (gated by `PERFETTO_ENABLE_LEGACY_TRACE_EVENTS`) |
| `third_party/perfetto/include/perfetto/tracing/track_event_args.h` | `perfetto::Flow` and `perfetto::TerminatingFlow` definitions |
| `third_party/perfetto/include/perfetto/tracing/string_helpers.h` | `perfetto::StaticString` and `perfetto::DynamicString` definitions |
| `third_party/perfetto/include/perfetto/tracing/track.h` | `perfetto::Track`, `perfetto::NamedTrack`, `perfetto::CounterTrack` definitions |

### Category Registry

All trace categories used by V8 are registered in `src/tracing/trace-categories.h`. When adding a new category string to a modern macro, verify it exists in this file. Existing categories:

`cppgc`, `v8`, `v8.console`, `v8.execute`, `v8.memory`, `v8.wasm`,
`devtools.timeline,v8`, and many `disabled-by-default-*` variants.

---

## Modern Macro Signatures (from Perfetto SDK)

All modern macros accept variadic trailing arguments. The general pattern is:

```
TRACE_EVENT*(category, name [, track] [, timestamp]
             [, "key1", value1] [, "key2", value2] ...
             [, flow] [, lambda]);
```

The exact macro signatures from `third_party/perfetto/include/perfetto/tracing/track_event.h`:

| Macro | Signature | Purpose |
|---|---|---|
| `TRACE_EVENT` | `(category, name, ...)` | Scoped slice (BEGIN on construction, END on scope exit) |
| `TRACE_EVENT_BEGIN` | `(category, name, ...)` | Explicit slice begin |
| `TRACE_EVENT_END` | `(category, ...)` | Explicit slice end — **no name argument** |
| `TRACE_EVENT_INSTANT` | `(category, name, ...)` | Zero-duration event |
| `TRACE_EVENT_CATEGORY_ENABLED` | `(category)` | Returns bool — is category enabled? |
| `TRACE_COUNTER` | `(category, track, ...)` | Counter sample; `track` is coerced to `CounterTrack` |

Optional trailing arguments (in any order after required args):
- `perfetto::Track(id)` — override the track
- `perfetto::Flow::ProcessScoped(id)` — attach a flow
- `perfetto::TerminatingFlow::ProcessScoped(id)` — attach a terminating flow
- `"key", value` pairs — debug annotations
- `timestamp` — custom timestamp (must satisfy `TraceTimestampTraits`)
- `[](perfetto::EventContext ctx) { ... }` — lambda for custom proto fields

---

## Conversion Reference

### Rule 1: Scoped Events — `TRACE_EVENT0/1/2` → `TRACE_EVENT`

Both legacy and modern are scoped (emit BEGIN on entry, END on scope exit).

```cpp
// BEFORE (legacy)
TRACE_EVENT0("v8", "V8.DeoptimizeCode");
TRACE_EVENT1("v8.wasm", "wasm.SyncCompile", "id", compilation_id);
TRACE_EVENT2("v8.wasm", "wasm.CompilationAfterDeserialization",
             "func_count", count, "wall_clock_us", us);

// AFTER (modern)
TRACE_EVENT("v8", "V8.DeoptimizeCode");
TRACE_EVENT("v8.wasm", "wasm.SyncCompile", "id", compilation_id);
TRACE_EVENT("v8.wasm", "wasm.CompilationAfterDeserialization",
            "func_count", count, "wall_clock_us", us);
```

**Dynamic event names:** If the name comes from a variable (not a string literal), wrap it:
```cpp
// BEFORE
TRACE_EVENT0("v8", record_gc_phases_info.trace_event_name());

// AFTER — explicit StaticString needed because const char* constructor is explicit
TRACE_EVENT("v8",
            perfetto::StaticString(record_gc_phases_info.trace_event_name()));
```

`perfetto::StaticString` (from `string_helpers.h`) has:
- **Implicit** constructor from string literals (`const char (&)[N]`)
- **Explicit** constructor from `const char*` — you must write `perfetto::StaticString(ptr)`

Use `perfetto::StaticString(ptr)` when the pointer is guaranteed to be long-lived (static/global storage). Use `perfetto::DynamicString(ptr)` when the string may be temporary and needs copying.

### Rule 2: Begin/End Events — `TRACE_EVENT_BEGIN0/1/2`, `TRACE_EVENT_END0/1/2`

```cpp
// BEFORE (legacy)
TRACE_EVENT_BEGIN0("v8.execute", "RunMicrotasks");
TRACE_EVENT_BEGIN1(TRACE_DISABLED_BY_DEFAULT("v8.stack_trace"), __func__,
                   "id_hash", id_hash);
TRACE_EVENT_BEGIN2("devtools.timeline,v8", event_name_,
                   "usedHeapSizeBefore", before, ...);
TRACE_EVENT_END0("v8.execute", "RunMicrotasks");
TRACE_EVENT_END1("v8.execute", "RunMicrotasks", "microtask_count", count);
TRACE_EVENT_END2(kTraceCategory, phase_kind_name(), "kind",
                 TRACE_STR_COPY(phase_kind_name()), "stats", stats);

// AFTER (modern) — note: END does NOT take a name argument
TRACE_EVENT_BEGIN("v8.execute", "RunMicrotasks");
TRACE_EVENT_BEGIN(TRACE_DISABLED_BY_DEFAULT("v8.stack_trace"),
                  perfetto::StaticString(__func__), "id_hash", id_hash);
TRACE_EVENT_BEGIN("devtools.timeline,v8",
                  perfetto::StaticString(event_name_),
                  "usedHeapSizeBefore", before, ...);
TRACE_EVENT_END("v8.execute");
TRACE_EVENT_END("v8.execute", "microtask_count", count);
TRACE_EVENT_END(kTraceCategory, "kind",
                perfetto::DynamicString(phase_kind_name()), "stats", stats);
```

**Critical:** `TRACE_EVENT_END` in modern Perfetto does **not** take a name. It closes the most recent `BEGIN` on the same track. Only category is required, plus optional key-value arguments.

### Rule 3: Instant Events — `TRACE_EVENT_INSTANT0/1/2`

Legacy instant events take an explicit `scope` argument (`TRACE_EVENT_SCOPE_THREAD`, `TRACE_EVENT_SCOPE_PROCESS`, `TRACE_EVENT_SCOPE_GLOBAL`). Modern instant events default to the current thread track.

```cpp
// BEFORE (legacy)
TRACE_EVENT_INSTANT0("v8.console", "V8.ConsoleAPI",
                     TRACE_EVENT_SCOPE_THREAD);
TRACE_EVENT_INSTANT1(TRACE_DISABLED_BY_DEFAULT("v8.gc_stats"), "V8.GC_Stats",
                     TRACE_EVENT_SCOPE_THREAD, "stats",
                     TRACE_STR_COPY(stats.c_str()));
TRACE_EVENT_INSTANT2(TRACE_DISABLED_BY_DEFAULT("v8.gc"), "V8.GC",
                     TRACE_EVENT_SCOPE_THREAD, "k1", v1, "k2", v2);

// AFTER (modern) — thread scope is the default, no scope arg needed
TRACE_EVENT_INSTANT("v8.console", "V8.ConsoleAPI");
TRACE_EVENT_INSTANT(TRACE_DISABLED_BY_DEFAULT("v8.gc_stats"), "V8.GC_Stats",
                    "stats", perfetto::DynamicString(stats.c_str()));
TRACE_EVENT_INSTANT(TRACE_DISABLED_BY_DEFAULT("v8.gc"), "V8.GC",
                    "k1", v1, "k2", v2);
```

**Non-thread scope:** To emit on a non-default track (e.g., process-scoped), pass an explicit `perfetto::Track`:
```cpp
TRACE_EVENT_INSTANT("category", "name", perfetto::Track::Global(0));
```

### Rule 4: Flow Events — `TRACE_EVENT_WITH_FLOW0/1/2`

Legacy flow events use `bind_id` + `flow_flags` (`TRACE_EVENT_FLAG_FLOW_OUT`, `TRACE_EVENT_FLAG_FLOW_IN`, `TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT`). Modern uses `perfetto::Flow` / `perfetto::TerminatingFlow` (defined in `track_event_args.h`).

From the SDK:
- `perfetto::Flow` adds a `flow_id` to `TrackEvent.flow_ids` — the flow continues after this event.
- `perfetto::TerminatingFlow` adds a `flow_id` to `TrackEvent.terminating_flow_ids` — the flow ends at this event.
- Both have `::ProcessScoped(uint64_t)`, `::Global(uint64_t)`, and `::FromPointer(void*)` factory methods.

Flow/TerminatingFlow are passed as extra arguments to `TRACE_EVENT` / `TRACE_EVENT_BEGIN` — they are recognized by type and handled specially.

```cpp
// BEFORE (legacy)
TRACE_EVENT_WITH_FLOW0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
                       "V8.OptimizeConcurrentPrepare", job->trace_id(),
                       TRACE_EVENT_FLAG_FLOW_OUT);
TRACE_EVENT_WITH_FLOW0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
                       "V8.OptimizeConcurrentMerge", job->trace_id(),
                       TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT);
TRACE_EVENT_WITH_FLOW0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
                       "V8.OptimizeFinalize", job->trace_id(),
                       TRACE_EVENT_FLAG_FLOW_IN);

// AFTER (modern)
TRACE_EVENT(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
            "V8.OptimizeConcurrentPrepare",
            perfetto::Flow::ProcessScoped(job->trace_id()));
TRACE_EVENT(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
            "V8.OptimizeConcurrentMerge",
            perfetto::Flow::ProcessScoped(job->trace_id()));
TRACE_EVENT(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
            "V8.OptimizeFinalize",
            perfetto::TerminatingFlow::ProcessScoped(job->trace_id()));
```

**Mapping:**
| Legacy `flow_flags` | Modern | Semantics |
|---|---|---|
| `TRACE_EVENT_FLAG_FLOW_OUT` | `perfetto::Flow::ProcessScoped(id)` | Flow starts (or continues) at this event |
| `TRACE_EVENT_FLAG_FLOW_IN \| TRACE_EVENT_FLAG_FLOW_OUT` | `perfetto::Flow::ProcessScoped(id)` | Flow passes through this event |
| `TRACE_EVENT_FLAG_FLOW_IN` | `perfetto::TerminatingFlow::ProcessScoped(id)` | Flow terminates at this event |

**Flow with arguments** (`TRACE_EVENT_WITH_FLOW1`):
```cpp
// BEFORE
TRACE_EVENT_WITH_FLOW1(TRACE_GC_CATEGORIES, name, bind_id, flow_flags,
                       "epoch", tracer->CurrentEpoch());

// AFTER — Flow/TerminatingFlow determined by the original flow_flags
TRACE_EVENT(TRACE_GC_CATEGORIES, perfetto::StaticString(name),
            perfetto::Flow::ProcessScoped(bind_id),
            "epoch", tracer->CurrentEpoch());
```

**Note:** When porting wrapper macros like `TRACE_GC_EPOCH_WITH_FLOW` that take `flow_flags` as a parameter, the macro itself must resolve whether to use `Flow` or `TerminatingFlow`. If all callsites use the same flag pattern, substitute directly. If mixed, consider splitting into two macros or parameterizing differently.

### Rule 5: Counter Events — `TRACE_COUNTER1/2`

The modern `TRACE_COUNTER` macro signature is `(category, track, value)`. The `track` argument is coerced to `perfetto::CounterTrack(track)`, so you can pass a string literal directly or construct a `CounterTrack` explicitly for more control (units, parent track, etc.).

```cpp
// BEFORE (legacy)
TRACE_COUNTER1("v8.memory", "V8.HeapSize", size);
TRACE_COUNTER2("v8", "V8.MemoryParts", "part1", v1, "part2", v2);

// AFTER (modern) — simple form: string literal as track name
TRACE_COUNTER("v8.memory", "V8.HeapSize", size);

// AFTER — with explicit CounterTrack for parent/unit control
TRACE_COUNTER(TRACE_DISABLED_BY_DEFAULT("v8.gc"),
              perfetto::CounterTrack("OldGenerationAllocationThroughput",
                                     parent_track_),
              throughput_value);
```

For multi-value counters (`TRACE_COUNTER2`), emit two separate `TRACE_COUNTER` calls:
```cpp
TRACE_COUNTER("v8", "V8.MemoryParts.part1", v1);
TRACE_COUNTER("v8", "V8.MemoryParts.part2", v2);
```

### Rule 6: String Copying — `TRACE_STR_COPY`

Legacy requires explicit `TRACE_STR_COPY()` for non-static strings. Modern Perfetto copies `const char*` by default. Use `perfetto::DynamicString` for runtime strings in the **name** position; for argument values, `const char*` is copied automatically.

```cpp
// BEFORE
TRACE_EVENT_END2(kTraceCategory, phase_kind_name(), "kind",
                 TRACE_STR_COPY(phase_kind_name()), "stats", stats);

// AFTER — DynamicString for the name; const char* args are auto-copied
TRACE_EVENT_END(kTraceCategory, "kind",
                perfetto::DynamicString(phase_kind_name()), "stats", stats);
```

**When to use which:**
| Position | String type | Wrapper |
|---|---|---|
| Event **name** | String literal (`"foo"`) | None — implicit `StaticString(const char(&)[N])` |
| Event **name** | `const char*` from long-lived storage | `perfetto::StaticString(ptr)` — explicit ctor required |
| Event **name** | `const char*` that may be temporary | `perfetto::DynamicString(ptr)` — copies the string |
| Event **name** | `std::string` | `perfetto::DynamicString(str)` — copies the string |
| Argument **key** | Always string literal | None needed |
| Argument **value** | `const char*` | None needed (debug annotations copy by default) |
| Argument **value** | `std::string` | None needed (has `TracedValue` support) |

### Rule 7: Category Enabled Check — `TRACE_EVENT_CATEGORY_GROUP_ENABLED`

The modern `TRACE_EVENT_CATEGORY_ENABLED(category)` macro expands to `PERFETTO_INTERNAL_CATEGORY_ENABLED(category)` (defined in `track_event_macros.h`). It returns a bool directly, unlike the legacy macro that writes through a pointer.

```cpp
// BEFORE (legacy) — writes through pointer
bool enabled;
TRACE_EVENT_CATEGORY_GROUP_ENABLED(TRACE_DISABLED_BY_DEFAULT("v8.turbofan"),
                                   &enabled);
if (enabled) { ... }

// AFTER (modern) — evaluates to bool
if (TRACE_EVENT_CATEGORY_ENABLED(TRACE_DISABLED_BY_DEFAULT("v8.turbofan"))) {
  ...
}
```

**Important:** `TRACE_EVENT_CATEGORY_GROUP_ENABLED` (old, pointer-out) vs `TRACE_EVENT_CATEGORY_ENABLED` (new, returns bool). The new macro is already defined in `trace-event-no-perfetto.h` as a no-op stub for non-Perfetto builds.

**Performance — static vs dynamic categories:** The SDK implements two paths inside `PERFETTO_INTERNAL_CATEGORY_ENABLED`:
- **Static categories** (string literals registered in `trace-categories.h`): Calls `TrackEvent::IsCategoryEnabled(index)` which is a single `atomic<uint8_t>::load(relaxed)` — very cheap.
- **Dynamic categories** (`perfetto::DynamicCategory`): Calls `TrackEvent::IsDynamicCategoryEnabled()` which acquires a lock, walks the per-trace-writer cache (`incr_state->dynamic_categories` map), and on cache miss grabs the data source lock to check the trace config. This is significantly more expensive.

All V8 `TRACE_EVENT_CATEGORY_GROUP_ENABLED` callsites (in `tracing-category-observer.cc`, `compiler.cc`, `pipeline.cc`, `maglev-concurrent-dispatcher.cc`, `cpu-profiler.cc`) use static category strings like `TRACE_DISABLED_BY_DEFAULT("v8.turbofan")`, so they take the fast path. The only dynamic category usage is in `builtins-trace.cc` which explicitly constructs a `perfetto::DynamicCategory`.

### Rule 8: Async Events — `TRACE_EVENT_ASYNC_BEGIN/END`, `TRACE_EVENT_NESTABLE_ASYNC_*`

Legacy async events use an explicit `id` parameter. Modern Perfetto models async events as slices on a separate `perfetto::Track`. The track's UUID is derived from the `id`, so matching BEGIN/END pairs use the same track.

From `third_party/perfetto/include/perfetto/tracing/track.h`:
```cpp
// perfetto::Track(id) creates a track parented to the current process.
// Use for async events that span threads or don't belong to a single thread.
TRACE_EVENT_BEGIN("category", "AsyncEvent", perfetto::Track(8086));
...
TRACE_EVENT_END("category", perfetto::Track(8086));
```

```cpp
// BEFORE (legacy)
TRACE_EVENT_ASYNC_BEGIN0("category", "name", id);
TRACE_EVENT_ASYNC_END0("category", "name", id);

// AFTER (modern)
TRACE_EVENT_BEGIN("category", "name", perfetto::Track(id));
TRACE_EVENT_END("category", perfetto::Track(id));
```

For `COPY` variants with timestamps:
```cpp
// BEFORE
TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP1(
    category, name, id, timestamp, arg_name, arg_val);
TRACE_EVENT_COPY_NESTABLE_ASYNC_END_WITH_TIMESTAMP0(
    category, name, id, timestamp);

// AFTER
TRACE_EVENT_BEGIN("category", perfetto::DynamicString(name),
                  perfetto::Track(id), timestamp, arg_name, arg_val);
TRACE_EVENT_END("category", perfetto::Track(id), timestamp);
```

### Rule 9: Mark Events — `TRACE_EVENT_MARK_WITH_TIMESTAMP*`

Legacy mark events (`TRACE_EVENT_PHASE_MARK`) are always emitted on the **global track** (`Track::Global(0)`) — see `track_event_legacy.h` line 131. This is different from thread-scoped instant events.

```cpp
// BEFORE
TRACE_EVENT_MARK_WITH_TIMESTAMP2(category, name, timestamp,
                                 arg1_name, arg1_val, arg2_name, arg2_val);

// AFTER — must specify Track::Global(0) to preserve global-track semantics
TRACE_EVENT_INSTANT(category, name, perfetto::Track::Global(0), timestamp,
                    arg1_name, arg1_val, arg2_name, arg2_val);
```

### Rule 10: Instant With Timestamp — `TRACE_EVENT_INSTANT_WITH_TIMESTAMP*`

Legacy instant events with timestamps still carry a `scope` argument. Map the scope to the correct track:

```cpp
// BEFORE — thread-scoped (most common in V8)
TRACE_EVENT_INSTANT_WITH_TIMESTAMP0(category, name,
                                    TRACE_EVENT_SCOPE_THREAD, timestamp);
TRACE_EVENT_INSTANT_WITH_TIMESTAMP1(category, name,
                                    TRACE_EVENT_SCOPE_THREAD, timestamp,
                                    arg_name, arg_val);

// AFTER — thread scope is the default, just pass timestamp
TRACE_EVENT_INSTANT(category, name, timestamp);
TRACE_EVENT_INSTANT(category, name, timestamp, arg_name, arg_val);

// BEFORE — global-scoped
TRACE_EVENT_INSTANT_WITH_TIMESTAMP0(category, name,
                                    TRACE_EVENT_SCOPE_GLOBAL, timestamp);

// AFTER — must specify Track::Global(0)
TRACE_EVENT_INSTANT(category, name, perfetto::Track::Global(0), timestamp);

// BEFORE — process-scoped
TRACE_EVENT_INSTANT_WITH_TIMESTAMP0(category, name,
                                    TRACE_EVENT_SCOPE_PROCESS, timestamp);

// AFTER — must specify ProcessTrack::Current()
TRACE_EVENT_INSTANT(category, name, perfetto::ProcessTrack::Current(),
                    timestamp);
```

### Rule 11: Copy Variants — `TRACE_EVENT_COPY_*`

Modern Perfetto copies string arguments by default, so all `COPY` variants map to their non-COPY modern equivalents. For event names that come from dynamic strings, use `perfetto::DynamicString`:

```cpp
// BEFORE
TRACE_EVENT_COPY_INSTANT0(category, name, scope);

// AFTER
TRACE_EVENT_INSTANT(category, perfetto::DynamicString(name));
```

### Rule 12: Rich Argument Values via Lambda

Modern Perfetto supports lambda-based argument serialization for complex data. Use this when migrating callsites that emit structured or multi-field data — it produces more efficient and semantically richer traces than flat key-value pairs.

**As a debug annotation value** (key + lambda):
```cpp
TRACE_EVENT_INSTANT(
    TRACE_DISABLED_BY_DEFAULT("v8.gc"), "V8.GCPretenuringFeedback", "value",
    [&](perfetto::TracedValue ctx) {
      auto dict = std::move(ctx).WriteDictionary();
      dict.Add("created", create_count);
      dict.Add("found", found_count);
      dict.Add("ratio", ratio);
    });
```

**As an EventContext lambda** (direct proto field access):
```cpp
TRACE_EVENT("category", "name",
    [&](perfetto::EventContext ctx) {
      auto* debug = ctx.event()->add_debug_annotations();
      debug->set_name("details");
      debug->set_string_value(some_string);
    });
```

**Combining with other arguments** — lambdas taking `EventContext&` (by reference) can appear alongside key-value pairs and Flow objects:
```cpp
TRACE_EVENT(TRACE_DISABLED_BY_DEFAULT("v8.inspector"),
            "v8::Debugger::AsyncTaskScheduled",
            "taskName", TRACE_STR_COPY(name.c_str()),
            perfetto::Flow::ProcessScoped(reinterpret_cast<uintptr_t>(task)),
            [&](perfetto::EventContext& ctx) {
              // Additional proto fields
            });
```

**Non-Perfetto builds:** Lambdas that reference Perfetto-specific types (e.g., `perfetto::TracedValue`, `perfetto::EventContext`, `ctx.event()->add_debug_annotations()`) will not compile against the no-op stubs. Wrap such callsites in `#if defined(V8_USE_PERFETTO)` guards, or ensure the lambda body only uses types that are stubbed.

---

## Non-Perfetto Build Path

When `V8_USE_PERFETTO` is not defined, `src/tracing/trace-event-no-perfetto.h` provides:
- **No-op stubs** for modern macros (`TRACE_EVENT`, `TRACE_EVENT_BEGIN`, etc.)
- **Full legacy definitions** for numbered macros that route through V8's own `TracingController`

---

## Wrapper Macros

Some legacy macros are used **inside V8 wrapper macros** rather than directly. Migrating these wrappers has multiplicative impact:

| Wrapper | Defined in | Legacy inside |
|---|---|---|
| `TRACE_GC` | `src/heap/gc-tracer.h` | `TRACE_EVENT0` |
| `TRACE_GC_ARG1` | `src/heap/gc-tracer.h` | `TRACE_EVENT1` |
| `TRACE_GC_WITH_FLOW` | `src/heap/gc-tracer.h` | `TRACE_EVENT_WITH_FLOW0` |
| `TRACE_GC1` | `src/heap/gc-tracer.h` | `TRACE_EVENT0` |
| `TRACE_GC1_WITH_FLOW` | `src/heap/gc-tracer.h` | `TRACE_EVENT_WITH_FLOW0` |
| `TRACE_GC_EPOCH_WITH_FLOW` | `src/heap/gc-tracer.h` | `TRACE_EVENT_WITH_FLOW1` |
| `TRACE_GC_NOTE` | `src/heap/gc-tracer.h` | `TRACE_EVENT0` |
| `TRACE_GC_NOTE_WITH_FLOW` | `src/heap/gc-tracer.h` | `TRACE_EVENT_WITH_FLOW0` |
| builtin trace | `src/builtins/builtins-utils.h:139` | `TRACE_EVENT0` |
| runtime call trace | `src/execution/arguments.h:123` | `TRACE_EVENT0` |

`TRACE_GC_EPOCH` is already migrated and uses `TRACE_EVENT(...)` — use it as a reference.

---

## Step-by-Step Migration Workflow

1. **Read surrounding code** to understand whether the name is a literal or variable, whether `TRACE_STR_COPY` is used, and what the flow flags mean.
2. **Apply the conversion rule** from this skill. Keep the same category string.
6. **Format:** Run `git cl format` per [v8_best_practices](../v8_best_practices/SKILL.md).
7. **Build & test** per the section below.

---

## Building & Testing

Refer to [v8_commands](../v8_commands/SKILL.md) and [v8_testing](../v8_testing/SKILL.md) for full details.

### Build with Perfetto (the primary path)

```bash
# Ensure args.gn contains: v8_use_perfetto = true
tools/dev/gm.py quiet x64.optdebug tests
```

### Build without Perfetto (verify stubs compile)

```bash
# Ensure args.gn contains: v8_use_perfetto = false
tools/dev/gm.py quiet x64.optdebug tests
```

### Run tracing-specific tests

```bash
# Perfetto tracing platform tests (perfetto builds only)
vpython3 tools/run-tests.py --progress dots \
    --outdir=out/arm64.optdebug.perfetto 'unittests/PlatformTracingTest*'

# CCTests for trace events (non-perfetto builds only)
vpython3 tools/run-tests.py --progress dots --outdir=out/arm64.optdebug \
    'cctest/test-trace-event/*'
```

### Run full test suite

```bash
vpython3 tools/run-tests.py --progress dots --exit-after-n-failures=5 \
    --outdir=out/arm64.optdebug
```

---

## Common Pitfalls

1. **Forgetting to remove the name from `TRACE_EVENT_END`** — the modern macro does not accept a name. The compiler will emit a confusing error if you pass one.

2. **Using `TRACE_STR_COPY` with modern macros** — `TRACE_STR_COPY` is only defined for legacy macros. Replace with `perfetto::DynamicString` for names or remove for arg values.

3. **Mismatching flow semantics** — `TRACE_EVENT_FLAG_FLOW_IN` alone means the flow *terminates* here. Use `perfetto::TerminatingFlow`, not `perfetto::Flow`.

4. **Category group strings** — Must match exactly what's registered in `src/tracing/trace-categories.h`. The `TRACE_DISABLED_BY_DEFAULT("x")` macro expands to `"disabled-by-default-x"` (defined in `track_event_legacy.h` when legacy is enabled, and in `trace-event-no-perfetto.h` for non-Perfetto builds).

5. **Non-Perfetto build breakage** — Modern macros are already stubbed in `trace-event-no-perfetto.h`, but any `perfetto::` types used in argument expressions (like `perfetto::StaticString`) must be available. The stubs in the no-perfetto header provide these types as empty classes with matching constructors. If you use new Perfetto types not already stubbed (e.g. `perfetto::CounterTrack` with `.set_unit()`), you must add stubs.

6. **`#ifdef V8_USE_PERFETTO` guards** — Some callsites (e.g., in `v8-debugger.cc`) are guarded by `#ifdef V8_USE_PERFETTO`. After migration, these guards can remain if the code uses Perfetto-specific APIs like `perfetto::TracedValue` lambdas (which would fail to compile against the no-op stubs). For simple macro conversions using only `perfetto::StaticString`, `perfetto::DynamicString`, `perfetto::Flow`, `perfetto::TerminatingFlow`, and `perfetto::Track`, the guards are unnecessary since these are all stubbed.

7. **`devtools.timeline` events are part of a de facto API** — Chrome DevTools consumes these events. Do not change category names, event names, or argument shapes without coordinating with the DevTools team.

8. **`TRACE_EVENT` nesting** — Modern Perfetto requires events to be properly nested on a given track: if you open A then B, you must close B before A. The SDK header explicitly documents this constraint. Legacy events didn't enforce this.

9. **Unused variable warnings in non-Perfetto builds** — Modern macros (`TRACE_EVENT_INSTANT`, `TRACE_EVENT_BEGIN`, etc.) expand to `INTERNAL_TRACE_IGNORE(category, name)` in non-Perfetto builds, which only references the category and name. Any variables used solely as key-value arguments to these macros will trigger `-Werror,-Wunused-variable`. Fix by adding `[[maybe_unused]]` to such variables. The `trace-event-no-perfetto.h` header documents this explicitly (lines 22-25).
    ```cpp
    // Variables only consumed by trace macros need [[maybe_unused]]
    [[maybe_unused]] const base::TimeDelta marking_duration = ...;
    TRACE_EVENT_INSTANT(category, "name", "duration",
                        marking_duration.InMillisecondsF());
    ```
