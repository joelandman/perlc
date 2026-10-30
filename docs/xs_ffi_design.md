# XS / FFI Design & Limitations

## Overview

perlc provides a **DynaLoader-compatible FFI surface** that mirrors real
Perl's DynaLoader/XSLoader API. This enables loading perlc-compiled `.so`
files and resolving native constants (Fcntl/POSIX) via lookup tables.

## What works

### 1. Native constant lookup (Fcntl/POSIX/Errno/File::Glob)

A generated table (`src/native_constants.h`) maps constant names to integer
values. Calls like `Fcntl::SEEK_SET`, `POSIX::LC_ALL`, or bare `SEEK_SET`
after `use Fcntl qw(...)` are resolved via this table.

**Implementation:** `perl_native_constant()` in `src/native_stdlib.c`

### 2. DynaLoader-compatible FFI surface

The following functions mirror real DynaLoader's API:

```c
PerlValue *perl_dl_load_file(path, flags)   → libref (1-based int)
PerlValue *perl_dl_find_symbol(libref, sym) → symref (1-based int)
PerlValue *perl_dl_install_xsub(name, symref) → 1 (success)
PerlValue *perl_dl_error()                  → error string
PerlValue *perl_dl_bootstrap(module, version) → 1 (success)
```

**Implementation:** `src/runtime.c` (lines ~17613-17950)

### 3. Bootstrap flow

`bootstrap()` loads the module's `.so` via `dlopen()`, then calls the
module's `boot_<Module>` sub (underscores substituted for non-word chars,
matching real DynaLoader's `$bootname` mangling). For perlc-built modules,
the subs are already registered and `boot_` is free to do setup.

### 4. perlc-compiled XS modules

Modules compiled by perlc via `perlc --do-lib` use perlc's own calling
convention:

```c
PerlValue *(*fn)(PerlArray *args, int ctx)
```

These work through `dl_install_xsub` → `perl_register_method()`.

## Fundamental limitation: SV-ABI gap

### What real XS modules expect

Real XS modules compiled against real Perl headers use **Perl's internal
SV (Scalar Value) ABI**:

```c
// Real XS macro (from XSUB.h)
dXSARGS;                    // Sets up: SV **sp, **mark, I32 items
ST(0) = sv_2mortal(newSViv(42));  // Direct SV manipulation
PUSHs(sv_2mortal(obj));
XSRETURN(1);
```

This requires:
- `SV*` (Scalar Value) structs with `sv_flags`, `sv_u`, `sv_refcnt`
- `XPVAV*`, `XPVHV*`, `XPVBM*` (array/hash/blessing internals)
- `ST(i)`, `PUSHs()`, `XSRETURN()` macros for stack manipulation
- `sv_2mortal()`, `newSViv()`, `sv_bless()`, `sv_refcnt()` etc.
- `gv_stashpv()`, `sv_refcnt()`, etc. for glob/stash access
- Mortal stack and eval stack management

### What perlc provides

perlc's `PerlValue` is a **tagged union** (not an SV):

```c
typedef struct PerlValue {
    PerlTag   tag;        // PERL_UNDEF, PERL_INT, PERL_STRING, etc.
    unsigned  flags;      // PV_FLAG_SHARED, PV_FLAG_UTF8, etc.
    union {
        long long ival;   // integer value
        double   fval;    // float value
        char    *sval;    // string value
        void    *pval;    // reference target
    };
    long long matchpos;
    char     *blessed_class;
    long long slen;
    long long _pad_reserved;
} PerlValue;
```

perlc's FFI calling convention is:
```c
PerlValue *(*fn)(PerlArray *args, int ctx)
```

### The gap

| Real XS ABI | perlc FFI |
|-------------|-----------|
| `SV*` with `sv_flags`, `sv_refcnt`, `sv_u` | `PerlValue*` tagged union |
| `XPVAV*`, `XPVHV*` internals | `PerlArray*`, `PerlHash*` |
| `ST(i)`, `PUSHs()`, `XSRETURN()` macros | `args->elems[i]`, `perl_array_push()` |
| Stack pointer (`sp`), mark pointer (`mark`) | `PerlArray*` + `int ctx` |
| `sv_2mortal()`, `newSViv()`, `sv_bless()` | `perl_alloc_int()`, `perl_free()` |
| Mortal stack, eval stack | Not implemented |
| `gv_stashpv()`, `sv_refcnt()` | Not implemented |

### What doesn't work

- ❌ Real XS modules compiled against real Perl headers (DBI XS, JSON::PP
  XS backend, etc.)
- ❌ Modules that directly manipulate `SV*`, `HV*`, `AV*` internals
- ❌ Modules that use `dXSARGS`/`dXSI32`/etc. macros
- ❌ Modules that use `sv_2mortal()`, `sv_bless()`, `newRV()` etc.

### What would be needed for full XS support

An **SV-ABI emulation layer** would need to:

1. Map real `SV*` ↔ perlc `PerlValue*` (bidirectional)
2. Implement `sv_2mortal()`, `newSViv()`, `sv_bless()`, etc.
3. Implement `ST(i)`, `PUSHs()`, `XSRETURN()` macros
4. Implement `gv_stashpv()`, `sv_refcnt()`, etc.
5. Handle real Perl's `sv_flags` bitfield semantics
6. Handle real Perl's mortal stack and eval stack

This is essentially reimplementing Perl's SV layer on top of perlc's
`PerlValue` — a significant undertaking comparable to reimplementing
Perl's core SV infrastructure.

## Implementation files

| File | Role |
|------|------|
| `src/runtime.c` (lines ~17613-17950) | DynaLoader FFI implementation |
| `src/native_stdlib.c` | Native constant lookup table |
| `src/codegen.cpp` (lines ~15460-15510) | Codegen for DynaLoader/XSLoader calls |
| `src/native_constants.h` | Generated constant lookup table |

## Future work

Full XS support requires the SV-ABI emulation layer described above. This
is a Low priority item due to the scope of work involved. The current FFI
layer is sufficient for:

- perlc-compiled modules (via `perlc --do-lib`)
- Native constant lookups (Fcntl/POSIX/Errno)
- Pure Perl modules with DynaLoader/XSLoader bootstrap calls
