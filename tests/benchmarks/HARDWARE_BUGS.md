# Hardware Bugs & Workarounds

Discovered during benchmarking. Fix after evaluation.

---

## BUG-001: SIMD end-of-function store dropped

**Symptom:** A single store at the end of an SIMD kernel (after the last loop iteration) is only committed for core 0. Follower stores are silently dropped.

**Root cause:** The store-done tracking serializes concurrent stores across cores. When a store is the last instruction before `ret` → `SIMD_DISABLE`, the serialization pipeline doesn't have enough instruction-window to complete all 4 cores' stores before SIMD mode ends and follower synchronization is cut.

**Observed in:** `dot_simd.c` — partial sums stored once at end of kernel. Only core 0's value committed; cores 1–3 retained stale init values.

**Proof:** Result = 177 = 168 (core 0 sum) + 2 + 3 + 4 (original Y init values at Y[16], Y[32], Y[48]).

**Workaround:** Store the accumulator **inside the loop body every iteration**. The loop provides an instruction drain window for the store-done serializer to complete across all cores before the next round of stores.

```c
// BAD — last store gets dropped for followers
for (int i = start; i < end; i++) {
    sum += X[i] * Y[i];
}
Y[start] = sum;  // ← only core 0's store commits

// GOOD — per-iteration store keeps serializer flowing
for (int i = start; i < end; i++) {
    sum += X[i] * Y[i];
    Y[start] = sum;  // ← all cores' stores commit
}
```

**Hardware fix (post-eval):** Add a drain/flush mechanism to the store-done tracker that ensures all pending stores complete before SIMD disable takes effect. Alternatively, gate `SIMD_DISABLE` until the store-done tracker reports all cores idle.

---

## BUG-002: SIMD stores to same cache line — only one core wins

**Symptom:** When multiple SIMD cores store to addresses within the same 64-byte cache line, only one core's store is visible after the SIMD region.

**Observed in:** `dot_simd.c` — `results[hartid]` with `results[4]` (16 bytes, all in one cache line). Only core 0's value survived.

**Workaround:** Ensure all SIMD stores target **separate cache lines** (64-byte aligned, stride ≥ 16 int32_t between cores).

```c
// BAD — all 4 stores hit same cache line
volatile int32_t results[4];  // 16 bytes = 1 cache line
results[hartid] = sum;

// GOOD — each core gets its own cache line
volatile int32_t results[4 * 16];  // 256 bytes = 4 cache lines
results[hartid * 16] = sum;
```

**Note:** This is a known constraint documented in POST_MORTEM.md (store-done tracking serializes same-line stores). Not strictly a bug — more of an architectural constraint. The workaround is to pad data structures.

**Hardware fix (post-eval):** Could implement per-word store merging in the store buffer, but this adds complexity. Documenting as a programming constraint may be sufficient.
