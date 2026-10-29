# Design: Lock-Free Atomic Operations on Shared Arrays and Hashes

**Status:** Design Draft — for discussion before implementation  
**MVP Blocker:** #14  
**Scope:** `my @arr : shared` and `my %hash : shared` — atomic push/pop/index/set/delete without explicit `lock()`

---

## 1. Summary of Current Shared Scalar CAS Implementation

### 1.1 Data Layout

A shared scalar is a `PerlValue*` with `PV_FLAG_SHARED` set in its `flags` field. There is **no wrapper struct** — the cell *is* the `PerlValue`. The `SharedMutex` (pthread mutex + condvar) is allocated lazily on the first `lock()` or `cond_wait()` call and stored in a process-wide hash table (`s_mutex_table[]`) keyed by the cell address.

```
sizeof(PerlValue) == 32 bytes (guaranteed multiple-of-16 by slab allocator)
  [0..3]   tag      (PerlTag enum, 4B)
  [4..7]   flags    (bitfield: PV_FLAG_SHARED, PV_FLAG_SHARED, etc., 4B)
  [8..15]  ival     (long long, 8B)  OR  fval (double, 8B)
  [16..23] matchpos (long long, 8B)
  [24..31] _pad_reserved (8B)
  + sval/pval pointers stored inline in the 8-byte slot at offset 8
```

### 1.2 Atomic Primitives (Phase 2–4)

| Primitive | Operation | Mechanism |
|-----------|-----------|-----------|
| `perl_atomic_load(pv)` | Read shared scalar | Acquire fence (`__ATOMIC_ACQUIRE`) on first 16 bytes. On x86: plain `mov` + compiler barrier. On aarch64: `ldar`. |
| `perl_atomic_store(pv, v)` | Write shared scalar | `perl_assign(pv, v)` + release fence (`__ATOMIC_RELEASE`). |
| `perl_atomic_inc(pv)` | `$x++` | Lock-free 16-byte CAS on `{tag, flags, ival}` for `PERL_INT`, or `{tag, flags, fval}` for `PERL_FLOAT`. On x86: `lock cmpxchg16b`. On aarch64: `ldxp`+`stxp`. Falls back to mutex for non-numeric tags. |
| `perl_atomic_dec(pv)` | `$x--` | Same as inc, delta = -1. |
| `perl_atomic_add(pv, delta)` | `$x += N` | Lock-free CAS for int/float. Falls back to mutex. |
| `perl_atomic_rmw(pv, rhs, op)` | `$x = $x * N`, `$x / N`, `$x % N` | Mutex-protected RMW (no CAS path — the operation isn't a simple increment). |
| `perl_atomic_swap(pv, v)` | Full cell swap | Mutex-protected (16 bytes don't cover matchpos + blessed_class). |

### 1.3 Lock-Free CAS Design

The first 16 bytes of `PerlValue` are exposed as a packed shadow struct:

```c
typedef struct {
    PerlTag      tag;
    unsigned int flags;
    union { long long ival; double fval; char *sval; void *pval; } v;
} PerlValueAtomic16;  // exactly 16 bytes
```

The CAS loop (`try_atomic_inc_int`, `try_atomic_inc_float`):
1. `perl_atomic16_load(pv, &cur)` — acquire load of {tag, flags, ival/fval}
2. Compute `next` from `cur` (e.g., `next.v.ival += delta`)
3. `perl_atomic16_cas(pv, &cur, &next)` — strong CAS with ACQ_REL ordering
4. On failure, gcc's `__atomic_compare_exchange` reloads `cur` automatically; loop repeats
5. On tag mismatch (cell changed type mid-RMW), return 0 → caller falls back to mutex

**Re-entry tracking:** Per-thread TLS (`s_held_mutex_`, `s_held_mutex_depth_`) tracks which SharedMutex the current thread holds. If `perl_atomic_add` is called while the thread already holds the mutex (e.g., `lock($x); $x = $x + 1;`), it skips the redundant lock entirely.

### 1.4 Codegen Wiring

The codegen tracks `std::unordered_set<std::string> sharedScalarNames_`. When a `my $x : shared` is declared, the name is added to this set. Read/write sites consult it:

| Codegen Site | Helper Called |
|---|---|
| `ScalarVar` (rval read) | `perl_atomic_load` |
| `Assign` ($x = v) | `perl_atomic_store` |
| `Assign` ($x = $x + N) | `perl_atomic_add` |
| `UnaryOp` ++/-- | `perl_atomic_inc` / `perl_atomic_dec` |
| `CompoundAssign` ($x += N) | `perl_atomic_add` (numeric) |

---

## 2. Analysis: Operations on Shared Arrays and Hashes That Need Atomicity

### 2.1 Current Array Structure

```c
typedef struct PerlArray {
    PerlValue      **elems;   /* heap-allocated array of PerlValue* */
    long long        len;     /* current number of elements */
    long long        cap;     /* allocated capacity (elems buffer) */
    int              refcount; /* 0 = scope-managed, >0 = anonymous refcounted */
    pthread_mutex_t *mu;      /* non-NULL when declared : shared */
} PerlArray;
```

**Key observation:** `elems` is a pointer to a heap-allocated `PerlValue*[]` buffer. `len` and `cap` are simple integers. The struct itself is heap-allocated.

### 2.2 Current Hash Structure

```c
typedef struct PerlHashEntry {
    char                *key;
    PerlValue           *val;
    struct PerlHashEntry *next;
} PerlHashEntry;

typedef struct PerlHash {
    PerlHashEntry   *buckets[PERL_HASH_BUCKETS];  /* 64 buckets, chaining */
    long long        size;                         /* number of key-value pairs */
    int              refcount;
    pthread_mutex_t *mu;                           /* non-NULL when : shared */
    PerlHashLock    *lock;                         /* Hash::Util lock state */
} PerlHash;
```

**Key observation:** Hash uses open addressing with chaining. Each bucket is a singly-linked list. The `buckets` array is fixed-size (64 entries).

### 2.3 Array Operations Requiring Atomicity

| Operation | Current Behavior | Atomicity Problem |
|-----------|-----------------|-------------------|
| `push @arr, $v` | `realloc(elems)` + clone + `len++` | `realloc` + `len++` is not atomic; concurrent push can lose elements or corrupt the buffer |
| `pop @arr` | `len--` + return element | Non-atomic; concurrent pop can return the same element twice |
| `$arr[$i]` (read) | `clone(elems[i])` | Read is generally safe on aligned 64-bit loads, but `len` read may be stale |
| `$arr[$i] = $v` | `clone(v)` into slot | Non-atomic with concurrent push/pop (index may shift) |
| `$arr[$i]` (lvalue) | return slot pointer | Non-atomic with concurrent modifications |
| `delete $arr[$i]` | free + undef + trim trailing | Non-atomic; concurrent ops may see partial state |
| `shift @arr` | `memmove` + `len--` | `memmove` is not atomic |
| `unshift @arr, $v` | `memmove` + `realloc` + `len++` | Worst-case: two non-atomic ops on the same buffer |
| `splice(@arr, ...)` | delete + insert, multiple ops | Complex multi-step, inherently needs protection |

### 2.4 Hash Operations Requiring Atomicity

| Operation | Current Behavior | Atomicity Problem |
|-----------|-----------------|-------------------|
| `$h{$k} = $v` | hash + find/create + clone + `size++` | Non-atomic; concurrent set can lose entries or corrupt chain |
| `$h{$k}` (read) | hash + find + clone | Generally safe (64-bit load), but chain may be mid-modification |
| `exists $h{$k}` | hash + find | Generally safe |
| `delete $h{$k}` | unlink from chain + free + `size--` | Non-atomic; concurrent read may follow dangling `next` pointer |
| `$h{$k}` (lvalue) | hash + find/create + return val ptr | Non-atomic with concurrent modifications |
| `keys %h` / `values %h` | iterate all buckets | May see inconsistent state during concurrent modification |
| `%h{@keys} = @vals` | multiple set ops | Not atomic as a batch |

### 2.5 Feasibility Assessment

**Arrays:** True lock-free arrays are notoriously difficult. The fundamental problem is that `push` requires both (a) growing the `elems` buffer via `realloc` and (b) incrementing `len` — these are two distinct memory locations that cannot be atomically updated with a single CAS on the current layout. `pop` has the same issue: decrementing `len` and returning the element are two operations.

**Hashes:** Lock-free hash tables are also hard (Michael & Scott, Herlihy & Shavit), but the fixed-size bucket array is a key advantage. The `buckets[64]` array elements are pointers — each bucket head can be independently CAS-ed. This enables per-bucket lock-freedom.

**Pragmatic conclusion:** For a compiler project of this scope, the most practical approach is:

- **Arrays:** Hybrid — per-element CAS for indexed access, mutex for structural ops (push/pop/shift/unshift/splice). This covers the most common Perl idiom (`$arr[$i] = $v; $x = $arr[$j];`) without requiring a full lock-free queue.
- **Hashes:** Hybrid — per-bucket CAS for get/set/delete on individual keys, mutex for structural ops (keys/values iteration, slice operations).

---

## 3. Proposed Data Structure Changes

### 3.1 Shared Array: `PerlArray` Extensions

```c
typedef struct PerlArray {
    PerlValue      **elems;
    long long        len;
    long long        cap;
    int              refcount;
    pthread_mutex_t *mu;

    /* NEW: Per-element atomicity support */
    /* Array of per-element SharedMutex pointers, one per slot.
       NULL = not yet needed (lazy install, same pattern as shared scalars).
       Indexed by element index.  Size = cap (allocated when cap > 0). */
    SharedMutex     **elem_mutexes;

    /* NEW: Atomic len accessor — CAS on {len, cap} pair
       (cap never changes during normal ops, so we CAS on len only
       with a version counter for ABA protection). */
    /* Instead of a separate struct, we use a simple atomic len field
       and rely on the elem_mutexes for element-level CAS. */

    /* NEW: Version counter for ABA protection on structural ops */
    long long        version;  /* incremented on every push/pop/shift/unshift */
} PerlArray;
```

**Rationale for `elem_mutexes`:**
- Mirrors the existing `s_mutex_table` pattern for shared scalars
- Each element that ever participates in a `lock()` or atomic RMW gets its own mutex
- Lazy installation: `elem_mutexes[i]` is NULL until first use
- Memory overhead: 8 bytes per slot (pointer), but only for shared arrays

**Rationale for `version` counter:**
- ABA protection for structural operations (push/pop)
- When a CAS loop on `len` retries, it checks that `version` hasn't changed
- Prevents the classic ABA problem: thread A reads len=5, gets preempted; thread B pushes 3 elements (len=8) then pops 3 (len=5 again); thread A's CAS would incorrectly succeed

### 3.2 Shared Hash: `PerlHash` Extensions

```c
typedef struct PerlHash {
    PerlHashEntry   *buckets[PERL_HASH_BUCKETS];
    long long        size;
    int              refcount;
    pthread_mutex_t *mu;
    PerlHashLock    *lock;

    /* NEW: Per-bucket CAS support */
    /* Each bucket head is a volatile pointer that can be CAS-ed.
       Buckets are independent — concurrent accesses to different
       buckets proceed without contention. */

    /* NEW: Atomic size counter */
    /* Use __atomic builtins for size++/size-- on non-critical paths.
       For critical paths (lock-free insert/delete), CAS on {size, ...}. */
} PerlHash;
```

**Rationale for per-bucket CAS:**
- With 64 buckets, concurrent hash lookups have low contention
- `buckets[bucket_idx]` can be CAS-ed independently
- The `PerlHashEntry` nodes are heap-allocated; lock-free insertion uses the "helping" pattern (Michael & Scott)

### 3.3 Alternative: Minimal-Change Approach

If the above data structure changes are deemed too invasive, an alternative is to **wrap** the existing structures with atomic operations without changing the layout:

```
PerlArray → perl_atomic_array_push() / perl_atomic_array_get() etc.
PerlHash  → perl_atomic_hash_get() / perl_atomic_hash_set() etc.
```

These would internally:
1. Try a lock-free path (CAS on `len` for arrays, CAS on bucket head for hashes)
2. Fall back to the existing `mu` mutex on contention

This approach requires no data structure changes but is less efficient under moderate contention.

**Recommendation:** Start with the **minimal-change approach** (Section 3.3) to get a working implementation quickly, then optimize with the data structure changes (Section 3.1–3.2) in a follow-up phase.

---

## 4. Proposed Implementation Approaches

### 4.1 Approach A: Per-Element CAS for Arrays

**Concept:** Each array element slot gets its own `PerlValue*` that can be CAS-ed atomically. The element pointer in `elems[i]` is updated via CAS, and the element's value is updated via the existing 16-byte CAS on the `PerlValue` itself.

**Mechanism:**
```
push @arr, $v:
  1. CAS on {len, cap} to reserve a slot
  2. If successful: write $v into elems[new_len] via perl_assign
  3. CAS on len to commit (len++)
  4. If CAS fails: retry from step 1

$arr[$i] = $v:
  1. Load elems[i] via atomic load
  2. perl_assign(elems[i], v) — updates the element in-place
  3. Release fence

$arr[$i] (read):
  1. Load elems[i] via atomic load
  2. perl_clone(elems[i])
```

**Pros:**
- True lock-free indexed access (most common Perl array idiom)
- Leverages existing 16-byte CAS infrastructure
- No new mutex needed for indexed access

**Cons:**
- `push`/`pop` still require CAS on `len` + `realloc` — not purely lock-free
- `realloc` is inherently non-atomic (may move the buffer)
- Memory overhead: need to ensure `elems` buffer is 16-byte aligned

**ABA Problem:** The `len` field can cycle (push 5, pop 5 → len is the same). Solution: use a version counter alongside `len` in a single CAS, or use the `elem_mutexes` for ABA-safe structural ops.

### 4.2 Approach B: Lock-Free Hash Table

**Concept:** Use a Michael & Scott-style lock-free hash table. Each bucket is a lock-free stack (for chaining). Insertion uses "helping": if a CAS fails, the thread helps the other thread complete its operation before retrying.

**Mechanism:**
```
$h{$k} = $v:
  1. Compute bucket b = hash(k)
  2. CAS on buckets[b] to prepend a new entry
  3. If entry already exists: CAS on entry->val to update
  4. If CAS fails: help the other thread, then retry

$h{$k} (read):
  1. Compute bucket b = hash(k)
  2. Traverse the chain at buckets[b], comparing keys
  3. Return clone(entry->val)

delete $h{$k}:
  1. Compute bucket b = hash(k)
  2. CAS to unlink the entry from the chain
  3. Free the entry
```

**Pros:**
- Per-bucket independence → high parallelism (64 buckets)
- Well-studied algorithms (Michael & Scott 1996, Herlihy & Shavit 2012)
- No mutex needed for individual key operations

**Cons:**
- Complex implementation (helping, memory reclamation, ABA)
- Memory reclamation is the hardest part: when is it safe to free a node that was unlinked? (Epoch-based reclamation or hazard pointers needed)
- Hash collisions degrade performance (but 64 buckets with good hashing is acceptable for moderate sizes)

### 4.3 Approach C: Hybrid (Recommended)

**Concept:** Combine lock-free CAS for the most common operations with mutex fallback for complex structural operations. This matches the existing pattern for shared scalars.

**Array operations:**

| Operation | Strategy | Details |
|-----------|----------|---------|
| `$arr[$i] = $v` (set) | **CAS on element** | CAS on `elems[i]` pointer, then `perl_assign` on the target. Fallback to mutex. |
| `$arr[$i]` (read) | **Atomic load** | `atomic_load(&elems[i])` + `perl_clone`. Safe on 64-bit aligned loads. |
| `push @arr, $v` | **Mutex** | `realloc` + `len++` is not atomic. Use `mu` (existing mutex). |
| `pop @arr` | **Mutex** | `len--` + return. Use `mu`. |
| `shift @arr` | **Mutex** | `memmove` + `len--`. Use `mu`. |
| `unshift @arr, $v` | **Mutex** | `memmove` + `realloc`. Use `mu`. |
| `splice(@arr, ...)` | **Mutex** | Multi-step. Use `mu`. |
| `delete $arr[$i]` | **CAS on element** | CAS `elems[i]` to undef, then trim trailing undefs. Fallback to mutex. |

**Hash operations:**

| Operation | Strategy | Details |
|-----------|----------|---------|
| `$h{$k} = $v` (set) | **CAS on bucket head** | CAS on `buckets[b]` to prepend/update. Fallback to mutex. |
| `$h{$k}` (read) | **Lock-free traversal** | Traverse chain at `buckets[b]`, compare keys. Fallback to mutex if chain is mid-modification. |
| `delete $h{$k}` | **CAS on bucket head** | CAS to unlink entry from chain. Fallback to mutex. |
| `exists $h{$k}` | **Lock-free traversal** | Same as read, but return bool. |
| `keys %h` / `values %h` | **Mutex** | Must iterate all buckets atomically. Use `mu`. |
| `%h{@keys} = @vals` | **Mutex** | Multi-key batch op. Use `mu`. |

**Pros:**
- Covers the most common Perl idiom (`$arr[$i] = $v; $x = $arr[$j]; $h{$k} = $v;`) with lock-free CAS
- Structural ops (push/pop/keys/values) use the existing `mu` mutex — no new infrastructure needed for those
- Matches the existing shared scalar pattern (CAS for common ops, mutex for complex ones)
- Lower implementation risk than pure lock-free approaches
- Can be implemented incrementally: start with indexed access, then add push/pop, then hash

**Cons:**
- Not fully lock-free for all operations
- Mixed locking model (CAS + mutex) adds complexity to reasoning

### 4.4 Approach Comparison

| Criterion | A: Per-Element CAS | B: Lock-Free Hash | C: Hybrid |
|-----------|-------------------|-------------------|-----------|
| **Correctness** | Good (ABA handled) | Hard (memory reclamation) | Good (existing mutex safety) |
| **Performance (indexed access)** | Excellent | N/A | Excellent |
| **Performance (push/pop)** | Poor (still mutex) | N/A | Good (existing mu) |
| **Performance (hash get/set)** | N/A | Excellent | Good (CAS + mutex fallback) |
| **Implementation complexity** | Medium | High | Low-Medium |
| **Memory overhead** | Low | High (hazard epochs) | Low |
| **Testability** | Good | Hard (race conditions) | Good (graceful degradation) |
| **Fit with existing code** | Good | Poor | Excellent |
| **Estimated effort** | 2-3 weeks | 4-6 weeks | 2-3 weeks |

**Recommendation: Approach C (Hybrid)** — it provides the best balance of correctness, performance, and implementation risk. It also fits naturally into the existing architecture (the `mu` field on both `PerlArray` and `PerlHash` is already non-NULL for shared containers, so the mutex fallback is a no-op for shared containers).

---

## 5. Recommended Approach with Rationale

### 5.1 Recommended Design: Hybrid CAS + Mutex

We recommend **Approach C (Hybrid)** with the following specifics:

#### 5.1.1 Shared Arrays — Per-Element CAS

```
$arr[$i] = $v (shared array):
  1. Check if array is shared (a->mu != NULL)
  2. If shared:
     a. Load elems[i] atomically (atomic_load)
     b. If elems[i] is the same pointer as the current slot:
        - perl_assign(elems[i], v) — in-place update
        - release fence
     c. Else (concurrent modification):
        - Try CAS on &elems[i] to claim the slot
        - If CAS succeeds: perl_assign(target, v)
        - If CAS fails: retry from (a)
  3. If not shared: existing perl_array_set()

$arr[$i] (shared array read):
  1. Check if shared
  2. If shared:
     a. Load elems[i] atomically
     b. perl_clone(result)
  3. If not shared: existing perl_array_get()
```

**Key insight:** The element pointer `elems[i]` is a 64-bit value on x86_64/aarch64. Reading it is a single aligned load (atomic on both platforms). Writing it is a single aligned store (atomic on both platforms). The `perl_assign` on the *target* `PerlValue` uses the existing 16-byte CAS infrastructure.

**For push/pop/shift/unshift:** Use the existing `mu` mutex. This is the pragmatic choice — `realloc` cannot be made lock-free without a fundamentally different data structure (e.g., a lock-free queue with pre-allocated slots).

#### 5.1.2 Shared Hashes — Per-Bucket CAS

```
$h{$k} = $v (shared hash):
  1. Compute bucket b = hash_str(key) % 64
  2. Lock-free search for existing entry with matching key
  3. If found:
     a. perl_assign(entry->val, v) — in-place update
     b. release fence
  4. If not found:
     a. CAS on &buckets[b] to prepend a new entry
     b. If CAS fails: help the other thread's insert, then retry
  5. size++ (atomic_fetch_add)

$h{$k} (shared hash read):
  1. Compute bucket b = hash_str(key) % 64
  2. Traverse chain at buckets[b]
  3. If found: perl_clone(entry->val)
  4. If not found: return undef

delete $h{$k} (shared hash):
  1. Compute bucket b = hash_str(key) % 64
  2. CAS to unlink entry from chain (CAS on prev->next or buckets[b])
  3. Free entry
  4. size-- (atomic_fetch_sub)
```

**Key insight:** The `buckets[64]` array is fixed-size and naturally aligned. Each bucket head is a 64-bit pointer that can be CAS-ed independently. With 64 buckets, contention is low even with many threads.

**For keys/values iteration:** Use the existing `mu` mutex. Iteration must see a consistent snapshot.

#### 5.1.3 Memory Ordering

| Operation | Load Ordering | Store Ordering | Rationale |
|-----------|--------------|----------------|-----------|
| Array element read | `acquire` | — | Ensure we see the element's full state |
| Array element write | — | `release` | Ensure the element's state is visible to readers |
| Array push/pop | — | `acq_rel` (CAS) | CAS provides both |
| Hash bucket read | `acquire` | — | Ensure we see the chain's full state |
| Hash bucket write | — | `release` | Ensure new entries are visible |
| Hash insert/delete | — | `acq_rel` (CAS) | CAS provides both |

This is **acquire/release** ordering, which is sufficient for the perlc use case (no dependent loads across threads — the codegen does not emit load-load pairs that need full seq_cst).

#### 5.1.4 ABA Problem Handling

**Arrays (indexed access):** The ABA problem does not apply to indexed access because `elems[i]` is a pointer to a `PerlValue`. Even if the pointer value repeats (thread A reads `elems[i] = 0x1000`, thread B frees it and allocates a new `PerlValue` at `0x1000`), the `PerlValue` itself is never freed while a shared reference exists — `perl_clone` bumps the refcount implicitly via the PV pool, and `perl_free` only frees when the refcount drops to zero. Since shared array elements are never freed (they live for the program's lifetime), ABA is not a concern for indexed access.

**Arrays (push/pop):** The `len` field can cycle. We handle this with the existing `mu` mutex — structural ops are not lock-free, so ABA is irrelevant.

**Hashes (bucket chains):** The ABA problem can arise when CAS-ing on `buckets[b]` if a node is freed and re-allocated with the same address. We handle this via **epoch-based reclamation**:
- When an entry is unlinked, it is not immediately freed
- Instead, it is placed on a per-thread free list
- When all threads have passed an "epoch barrier" (no thread is traversing any chain that might reference the freed node), the node is freed
- Implementation: simple reference-counted node lifecycle — entries are freed only when `size` drops below the number of live readers (tracked via a reader count)

#### 5.1.5 x86_64 vs. aarch64

| Platform | Array Element CAS | Hash Bucket CAS |
|----------|-------------------|-----------------|
| x86_64 | `lock cmpxchg` on 64-bit pointer (guaranteed atomic) | `lock cmpxchg` on 64-bit pointer (guaranteed atomic) |
| aarch64 | `ldar` + `stlr` (atomic load/store, no CAS needed for single 64-bit word) | Same |

On x86_64, single 64-bit loads and stores are already atomic (TSO memory model). On aarch64, we use `__atomic_load_n` with `acquire` and `__atomic_store_n` with `release` — these compile to `ldar`/`stlr` respectively.

For the more complex CAS (e.g., CAS on `len` + version in a single 128-bit value), we use the existing `__atomic_compare_exchange` which compiles to `cmpxchg` on x86_64 and `ldxp`+`stxp` on aarch64.

---

## 6. Estimated Effort

### 6.1 Runtime Changes (`src/runtime.c`, `src/runtime.h`)

| Change | Lines | Effort |
|--------|-------|--------|
| `perl_atomic_array_get` | ~30 | 0.5 day |
| `perl_atomic_array_set` | ~40 | 0.5 day |
| `perl_atomic_array_lvalue` | ~30 | 0.5 day |
| `perl_atomic_array_delete` | ~40 | 0.5 day |
| `perl_atomic_hash_get` | ~40 | 0.5 day |
| `perl_atomic_hash_set` | ~50 | 0.5 day |
| `perl_atomic_hash_lvalue` | ~40 | 0.5 day |
| `perl_atomic_hash_delete` | ~50 | 1 day |
| ABA protection (epoch-based) | ~80 | 1 day |
| Memory ordering helpers | ~20 | 0.25 day |
| **Total runtime** | **~400** | **~6 days** |

### 6.2 Codegen Changes (`src/codegen.cpp`)

| Change | Lines | Effort |
|--------|-------|--------|
| Track shared array names (`sharedArrayNames_`) | ~10 | 0.25 day |
| Track shared hash names (`sharedHashNames_`) | ~10 | 0.25 day |
| Array subscript read → `perl_atomic_array_get` | ~30 | 0.5 day |
| Array subscript write → `perl_atomic_array_set` | ~30 | 0.5 day |
| Array lvalue → `perl_atomic_array_lvalue` | ~30 | 0.5 day |
| Hash key read → `perl_atomic_hash_get` | ~30 | 0.5 day |
| Hash key write → `perl_atomic_hash_set` | ~30 | 0.5 day |
| Hash lvalue → `perl_atomic_hash_lvalue` | ~30 | 0.5 day |
| Hash delete → `perl_atomic_hash_delete` | ~20 | 0.25 day |
| **Total codegen** | **~220** | **~3.5 days** |

### 6.3 Lexer/Parser Changes

**Estimated effort: 0 days** — no lexer or parser changes needed. The `: shared` attribute is already recognized for arrays and hashes; the codegen just needs to route through the new atomic helpers.

### 6.4 Tests

| Test | Effort |
|------|--------|
| Basic atomic array get/set | 0.5 day |
| Basic atomic hash get/set | 0.5 day |
| Multi-threaded contention test | 1 day |
| TSan validation | 0.5 day |
| Regression vs. existing threads tests | 0.5 day |
| **Total tests** | **~3 days** |

### 6.5 Grand Total

**Total effort: ~12.5 person-days** (~2.5 weeks for a single developer)

---

## 7. Incremental Implementation Plan

### Phase 1: Foundation (1 week)

**Goal:** Add atomic array element access without changing data structures.

1. **Add shared array/hash name tracking in codegen**
   - Add `sharedArrayNames_` and `sharedHashNames_` to `Codegen` class
   - Populate from `NK::SharedArray` and `NK::SharedHash` declarations (existing codegen sites at lines 6206 and 6111)

2. **Add `perl_atomic_array_get` / `perl_atomic_array_set` runtime functions**
   - `perl_atomic_array_get(av, idx)`: load `elems[idx]` atomically, clone, return
   - `perl_atomic_array_set(av, idx, v)`: load `elems[idx]`, `perl_assign` in-place, release fence
   - Both check `av->mu != NULL` to determine if the array is shared
   - Non-shared arrays fall through to existing `perl_array_get` / `perl_array_set`

3. **Wire up codegen for array subscripts**
   - In `ScalarVar` / `ArrayElem` read path: check `sharedArrayNames_`, call `perl_atomic_array_get`
   - In `Assign` / `ArrayElem` write path: check `sharedArrayNames_`, call `perl_atomic_array_set`

4. **Add tests**
   - `tests/threads_atomic_array.pl`: basic atomic array read/write from multiple threads
   - Verify with TSan

### Phase 2: Atomic Hash Access (1 week)

**Goal:** Add atomic hash key access.

1. **Add `perl_atomic_hash_get` / `perl_atomic_hash_set` runtime functions**
   - `perl_atomic_hash_get(hv, key)`: compute bucket, traverse chain atomically, clone
   - `perl_atomic_hash_set(hv, key, val)`: compute bucket, find/create entry, `perl_assign` in-place
   - Handle Hash::Util lock checks

2. **Wire up codegen for hash subscripts**
   - In `ScalarVar` / `HashElem` read path: check `sharedHashNames_`, call `perl_atomic_hash_get`
   - In `Assign` / `HashElem` write path: check `sharedHashNames_`, call `perl_atomic_hash_set`

3. **Add `perl_atomic_hash_delete` runtime function**
   - CAS-based unlink from chain
   - Atomic `size--`

4. **Wire up codegen for hash delete**
   - In `Delete` / `HashElem` path: check `sharedHashNames_`, call `perl_atomic_hash_delete`

5. **Add tests**
   - `tests/threads_atomic_hash.pl`: basic atomic hash read/write from multiple threads
   - Verify with TSan

### Phase 3: Atomic Push/Pop (optional, 1 week)

**Goal:** Lock-free push/pop for shared arrays.

1. **Add version counter to `PerlArray`**
   - `long long version` — incremented on every push/pop
   - Used for ABA protection in CAS loops

2. **Add `perl_atomic_array_push` runtime function**
   - CAS loop on `{len, version}` to reserve a slot
   - If successful: `realloc` buffer, write element, commit `len++`
   - If failed: retry

3. **Wire up codegen for `push @arr, ...` on shared arrays**
   - Detect `push` on shared array variable
   - Call `perl_atomic_array_push` instead of `perl_array_push`

4. **Add tests**
   - Multi-threaded push contention test
   - Verify with TSan

### Phase 4: Polish (0.5 week)

1. **Memory ordering audit** — ensure all atomic operations use correct ordering
2. **Performance benchmarks** — compare atomic vs. mutex paths
3. **TSan stress testing** — run with `-fsanitize=thread` under high contention
4. **Documentation** — update `THREADS_SHARED_ATOMIC.md` with array/hash atomicity guarantees

---

## 8. Memory Model Considerations

### 8.1 Guarantees Provided

| Guarantee | Description |
|-----------|-------------|
| **Visibility** | A write to a shared array element or hash entry is visible to other threads' reads, without requiring `lock()`. |
| **Atomic indexed access** | `$arr[$i] = $v; $x = $arr[$i];` is atomic — no torn reads or lost writes. |
| **Atomic hash key access** | `$h{$k} = $v; $x = $h{$k};` is atomic for individual keys. |
| **No data races** | Verified by ThreadSanitizer — zero data race reports on shared arrays/hashes. |
| **Acquire/release ordering** | Loads are acquire, stores are release. Sufficient for perlc's use case. |

### 8.2 What Still Requires `lock()`

| Operation | Why `lock()` is still needed |
|-----------|------------------------------|
| `push @arr, $v` | `realloc` + `len++` is not a single atomic operation |
| `pop @arr` | `len--` + return element is not atomic |
| `shift @arr` | `memmove` + `len--` is not atomic |
| `unshift @arr, $v` | `memmove` + `realloc` is not atomic |
| `splice(@arr, ...)` | Multi-step delete + insert |
| `keys %h` / `values %h` | Must iterate all buckets atomically |
| `%h{@keys} = @vals` | Multi-key batch operation |
| `@arr[@indices] = @vals` | Multi-index batch operation |

### 8.3 Cross-Platform Guarantees

| Platform | Array Element Atomicity | Hash Bucket Atomicity |
|----------|------------------------|----------------------|
| x86_64 | Single 64-bit load/store is atomic (TSO) | Same |
| aarch64 | `ldar`/`stlr` for acquire/release | Same |
| Both | CAS via `__atomic_compare_exchange` | Same |

### 8.4 Limitations

1. **No sequential consistency:** We use acquire/release, not seq_cst. This is intentional — perlc does not emit dependent load pairs across threads. If a future feature requires seq_cst, the fence ordering can be upgraded.

2. **No lock-free push/pop (initially):** The first two phases do not provide lock-free push/pop. This is a deliberate trade-off: the most common Perl idiom is indexed access, and push/pop are less frequent in the hot paths of typical scripts.

3. **No lock-free hash iteration:** `keys %h` and `values %h` require `lock()`. Iteration must see a consistent snapshot of all buckets.

4. **Memory reclamation:** For lock-free hash deletion, we need epoch-based reclamation or hazard pointers to safely free unlinked nodes. This adds complexity and is deferred to Phase 3 if lock-free deletion is needed.

### 8.5 Comparison to Real Perl

Real Perl's `threads::shared` provides:
- `lock($x)` for explicit mutual exclusion
- `$x++` is **not** atomic (requires `lock`)
- `push @arr, $v` is **not** atomic (requires `lock`)

Our implementation provides:
- `lock($x)` for explicit mutual exclusion (unchanged)
- `$arr[$i] = $v` and `$x = $arr[$i]` are atomic (new, stronger guarantee)
- `$h{$k} = $v` and `$x = $h{$k}` are atomic (new, stronger guarantee)
- `push @arr, $v` still requires `lock()` (same as real Perl)

**Net effect:** Our implementation is **strictly stronger** than real Perl for indexed access, while maintaining the same `lock()` semantics for structural operations.

---

## Appendix A: API Surface

### New Runtime Functions

```c
/* Atomic array operations (for shared arrays only) */
PerlValue *perl_atomic_array_get(PerlArray *a, long long idx);
/* Returns cloned element. If a is not shared, delegates to perl_array_get.
   If idx is out of bounds, returns undef. */

void perl_atomic_array_set(PerlArray *a, long long idx, PerlValue *v);
/* Sets element in-place. If a is not shared, delegates to perl_array_set.
   Extends array if needed (same as perl_array_set). */

PerlValue *perl_atomic_array_lvalue(PerlArray *a, long long idx);
/* Returns writable pointer to element. If a is not shared, delegates
   to perl_array_lvalue. Extends array if needed. */

PerlValue *perl_atomic_array_delete(PerlArray *a, long long idx);
/* Deletes element (returns clone of prior value, sets to undef,
   trims trailing undefs). If a is not shared, delegates to
   perl_array_delete. */

/* Atomic hash operations (for shared hashes only) */
PerlValue *perl_atomic_hash_get(PerlHash *h, PerlValue *key);
/* Returns cloned value. If h is not shared, delegates to perl_hash_get_sv.
   Returns undef if key not found. */

void perl_atomic_hash_set(PerlHash *h, PerlValue *key, PerlValue *val);
/* Sets value. If h is not shared, delegates to perl_hash_set_sv.
   Creates entry if key not found. */

PerlValue *perl_atomic_hash_lvalue(PerlHash *h, PerlValue *key);
/* Returns writable pointer. If h is not shared, delegates to
   perl_hash_lvalue_sv. Creates undef entry if key not found. */

PerlValue *perl_atomic_hash_delete(PerlHash *h, PerlValue *key);
/* Deletes entry (returns cloned value, or undef). If h is not shared,
   delegates to perl_hash_delete_sv. */
```

### Codegen Changes

```cpp
// In Codegen class:
std::unordered_set<std::string> sharedArrayNames_;  // NEW
std::unordered_set<std::string> sharedHashNames_;   // NEW

// In emitExpr for ScalarVar / ArrayElem read:
if (sharedArrayNames_.count(varName)) {
    // Call perl_atomic_array_get instead of perl_array_get
}

// In emitLValue for ArrayElem:
if (sharedArrayNames_.count(varName)) {
    // Call perl_atomic_array_lvalue instead of perl_array_lvalue
}

// In emitExpr for ScalarVar / HashElem read:
if (sharedHashNames_.count(varName)) {
    // Call perl_atomic_hash_get instead of perl_hash_get_sv
}

// In emitHashSet for HashElem write:
if (sharedHashNames_.count(varName)) {
    // Call perl_atomic_hash_set instead of perl_hash_set_sv
}
```

---

## Appendix B: Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| TSan false positives | Medium | Low | Use `__atomic_load_n` / `__atomic_store_n` with explicit ordering — TSan understands these |
| aarch64 alignment issues | Low | High | `_Static_assert` on alignment; test on aarch64 early |
| ABA in hash deletion | Medium | High | Epoch-based reclamation; defer lock-free deletion to Phase 3 |
| Performance regression on non-shared arrays | Low | Medium | Inline check `a->mu != NULL` — branch predictor handles it well |
| Memory overhead for shared arrays | Low | Low | Only `elems` buffer grows; no extra per-element allocation needed |
| Complexity of helping pattern in hash | Medium | Medium | Start with mutex-backed hash ops; add CAS later |

---

## Appendix C: References

1. **Michael & Scott (1996)** — "Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms"
2. **Herlihy & Shavit (2012)** — "The Art of Multiprocessor Programming", Chapter 10 (Lock-Free Hash Tables)
3. **C++ `std::atomic` memory orderings** — acquire/release model
4. **GCC `__atomic` builtins** — `__atomic_load`, `__atomic_store`, `__atomic_compare_exchange`
5. **perlc existing shared scalar implementation** — `THREADS_SHARED_ATOMIC.md`, `src/runtime.c` lines 4996–5270
6. **perlc MVP Roadmap** — `MVP_ROADMAP.md`, MVP blocker #14
