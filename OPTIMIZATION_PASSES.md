# Optimization Passes in perlc

## Overview

The perlc compiler implements multiple optimization passes that work together to generate efficient LLVM IR. These passes are organized into stages that can be selectively enabled/disabled.

## Optimization Stages

### Stage 1: Basic Codegen
- No optimizations
- Direct translation of AST to LLVM IR
- Used for debugging and as a baseline

### Stage 2: LTO (Link-Time Optimization)
- Enables `-flto` flag for final linking
- Allows cross-module optimizations
- Improves performance through whole-program analysis

### Stage 3: LLVM Optimization Passes
- `-O2` optimization level for generated IR
- Loop unrolling, vectorization, inlining
- TBAA metadata for alias analysis

### Stage 15: DerefAV Cache for @_ Arguments
- **Purpose**: Cache PerlArray* for array-ref arguments
- **Target**: Function arguments passed as `my ($x, $bodies) = @_` where `$bodies` is only used for ArrowDeref
- **Mechanism**: 
  - Pre-analyze sub body to detect promotable args
  - For `PPKind::DerefAV`: borrow @_ element, cache PerlArray*
  - Eliminates 3 pool ops per call (alloc + assign + free)
- **Data Structure**: `prePromotedArgs_` map with `PPKind` enum
- **Interaction**: Works with FLAT_ARRAY semantics for numeric arrays

### Stage 22: FLAT_ARRAY Fast Path
- **Purpose**: Avoid perl_deref_array conversion for FLAT_ARRAY PVs
- **Target**: FLAT_ARRAY tagged PerlValues (all-numeric arrays)
- **Mechanism**:
  - Check tag field at runtime
  - Branch to fast path (direct double[] access) or normal path
  - Preserves FLAT_ARRAY throughout computation
- **Interaction**: Works with ArrowDeref for 1D and 2D access

### Stage 23: All-Flat Branch Optimization
- **Purpose**: Hoist all-flat check out of loops
- **Target**: Outer loops with FLAT_ARRAY arrays
- **Mechanism**:
  - Check if array is all-numeric once at loop entry
  - Branch on allflat flag
  - Flat path: direct GEP into double[]
  - Norm path: perl_deref_array + perl_array_get_ref
- **Interaction**: Works with Stage 22 for 1D FLAT_ARRAY

### Stage 27: Pre-promotion of @_ Arguments
- **Purpose**: Skip PV alloca for promoted args
- **Target**: Function arguments with simple numeric/arrow usage
- **Mechanism**:
  - `PPKind::Float`: Unboxed double alloca
  - `PPKind::Int`: Unboxed i64 alloca
  - `PPKind::DerefAV`: Cached PerlArray* (Stage 15)
- **Interaction**: Works with FLAT_ARRAY semantics

### Stage 31: SSA Value Caching
- **Purpose**: Hoist expensive operations from inner loops
- **Target**: Loop-invariant PV computations
- **Mechanism**:
  - Track loopExits_ to know when loop ends
  - Cache results in loop-invariant allocas
  - Reuse cached values inside loop
- **Interaction**: Works with DerefAV cache

### Stage 32: Loop-Invariant Dereference Hoisting
- **Purpose**: Hoist perl_deref_array from inner loops
- **Target**: ScalarVars that are loop-invariant
- **Mechanism**:
  - collectDerefTargets: Find ArrowDeref base ScalarVars
  - emitHoistedDerefs: Emit perl_deref_array before loop
  - Cache in loopDerefCache_ alloca
  - Inner ArrowDeref loads from cache
- **Interaction**: Works with DerefAV cache

### Stage 33: Known Tag Type Tracking
- **Purpose**: Enable dispatch elimination via tag knowledge
- **Target**: Variables with known PerlTag
- **Mechanism**:
  - Track knownTagTypes_ per scope
  - Update on assignments with known tags
  - Used for dispatch elimination (future optimization)
- **Interaction**: Works with FLAT_ARRAY, FLOAT_PAIR tags

### Phase 3: Shared Scalar Atomicity
- **Purpose**: Correct atomic operations for threads::shared
- **Target**: Shared scalars with numeric compound assignment
- **Mechanism**:
  - Negate delta for subtraction before perl_atomic_add
  - Works for both boxed and unboxed shared scalars
  - Integer promotion for int/float shared vars
- **Interaction**: Works with atomic RMW primitives

## Data Structures

### CodeGen Class Members

| Member | Purpose |
|--------|---------|
| `derefAVScopes_` | Stack of DerefAV cache allocas |
| `prePromotedArgs_` | Map of variable name → PPKind |
| `loopDerefCache_` | Map of variable name → alloca |
| `loopExits_` | Stack of exit blocks for tracking |
| `knownTagTypes_` | Stack of known PerlTag per scope |
| `sharedScalarNames_` | Set of shared scalar variable names |
| `enabledStages_` | Vector of enabled optimization stages |

### PPKind Enum

| Value | Meaning |
|-------|---------|
| `DerefAV` | Cache PerlArray*, borrow @_ element |
| `Float` | Unboxed double alloca |
| `Int` | Unboxed i64 alloca |

## Interaction with FLAT_ARRAY Semantics

### Current Design Issues

1. **FLAT_ARRAY as Inline Storage**
   - FLAT_ARRAY uses PerlValue's pval field for double*
   - Tag=10 (PERL_FLAT_ARRAY) indicates inline storage
   - Conversion to REF_ARRAY is lazy and in-place
   - Problem: Confuses alias analysis when mixed with REF_ARRAY

2. **1D ArrowDeref Fast Path**
   - FLAT_ARRAY fast path: direct GEP into double[]
   - Normal path: perl_deref_array + perl_array_get_ref
   - Branch on tag field at runtime
   - Problem: Two separate code paths increase complexity

3. **2D ArrowDeref**
   - Inner ArrowDeref returns PerlValue*
   - Write path: perl_deref_array inner result
   - Problem: Inner result might be undef, needs autovivification

### Proposed Architecture

#### Symbol Table with Separate Descriptors

```
Symbol Table Entry:
├── name: string
├── descriptor: SymbolDescriptor*
└── storage: union {
    - ScalarStorage*    # for scalars
    - ArrayStorage*     # for arrays
    - HashStorage*      # for hashes
  }

SymbolDescriptor:
├── type: SymbolType
├── flags: uint32_t
│   ├── FLAT_ARRAY: 1 bit
│   ├── SHARED: 1 bit
│   └── PROMOTED: 2 bits
└── cache: CacheDescriptor*
```

#### Storage Layer Separation

```
ArrayStorage:
├── base: void*           # pointer to actual data
├── layout: StorageLayout # describes data layout
├── size: size_t          # current element count
└── capacity: size_t      # allocated capacity

StorageLayout:
├── element_type: ElementType
├── is_flat: bool         # inline storage
├── is_shared: bool       # shared across threads
├── alignment: size_t
└── metadata: Metadata*   # optional TBAA, etc.

ElementType:
├── tag: PerlTag
├── size: size_t
└── access: AccessPattern # how elements are accessed
```

#### Access Pattern Abstraction

```
class ArrayAccessor {
public:
    virtual PerlValue* get(ArrayStorage* storage, size_t idx) = 0;
    virtual void set(ArrayStorage* storage, size_t idx, PerlValue* val) = 0;
    virtual size_t len(ArrayStorage* storage) = 0;
    
    // Fast path for FLAT_ARRAY
    virtual double* get_flat_ptr(ArrayStorage* storage) = 0;
};

class FlatArrayAccessor : public ArrayAccessor {
    // Direct access to double*
};

class RefArrayAccessor : public ArrayAccessor {
    // Through PerlValue* indirection
};

class SharedArrayAccessor : public ArrayAccessor {
    // With atomic operations
};
```

## Plan for Optimization Pass Consolidation

### Phase 1: Documentation and Analysis (Current)
1. ✅ Document all optimization passes
2. ✅ Identify issues with current design
3. ⏳ Analyze interaction between passes
4. ⏳ Identify redundant or conflicting passes

### Phase 2: Simplification
1. ⏳ Consolidate FLAT_ARRAY handling
2. ⏳ Simplify DerefAV cache logic
3. ⏳ Remove redundant tag checks
4. ⏳ Unify 1D and 2D ArrowDeref paths

### Phase 3: New Architecture
1. ⏳ Design SymbolDescriptor hierarchy
2. ⏳ Implement StorageLayer abstraction
3. ⏳ Create ArrayAccessor interface
4. ⏳ Implement flat and ref accessors

### Phase 4: Integration
1. ⏳ Integrate new architecture
2. ⏳ Test with nb.pl and mbs.pl
3. ⏳ Verify correctness with test suite
4. ⏳ Measure performance improvements

## Recommendations

### Immediate Actions
1. **Fix 2D ArrowDeref SEGV**: The SEGV in nb.pl indicates a bug in the 2D ArrowDeref write path
2. **Isolate FLAT_ARRAY**: Consider making FLAT_ARRAY a separate type rather than a tag
3. **Simplify Cache Logic**: Reduce complexity in DerefAV and loopDerefCache handling

### Long-term Improvements
1. **Symbol Table**: Implement separate symbol table with descriptor hierarchy
2. **Storage Abstraction**: Create accessors that hide FLAT_ARRAY vs REF_ARRAY differences
3. **Optimization Pipeline**: Reorganize passes into clear pipeline with well-defined stages
