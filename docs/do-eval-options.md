# Runtime `do` / `eval` options (blocker #1)

Working design notes for how a perlc binary handles runtime `do FILE` and
string `eval EXPR`. This is the follow-on to MVP blocker **#1** (the
"generated binary is not self-contained" blocker). Kept here so the design
survives a session reset — do not duplicate it elsewhere; cross-reference
`docs/mvp_blockers.md`.

_Captured 2026-11-16. This document is a design discussion; implementation is
deferred (see "Status")._

## 1. The blocker

A perlc binary re-invokes the compiler at runtime for `do FILE`, string
`eval EXPR`, and dynamic `require` of a non-native module. It bakes
`PERLC_SELF_PATH` into the linked binary (`src/main.cpp`) and, from the
runtime, shells out:

```
system("${PERLC_SELF_PATH} --do-lib  \"<file>\"  -o <so>");   // do FILE
system("${PERLC_SELF_PATH} --eval-lib \"<expr>\" -o <so>");   // eval EXPR
handle = dlopen(soPath, RTLD_NOW);                           // load the .so
entry = dlsym(handle, "__perlc_do_run");                     // run it
```

This needs **perlc + clang-18 + `/bin/sh` + a writable `/tmp`** on the
target, which is exactly why the output is not a standalone native binary.
See `src/runtime.c`: `perl_do_file()` (1347), `perl_exec_compiled_file()`
(1366), `perl_eval_string()` (1652), `perl_runtime_require()` (1269).

## 2. What is ALREADY static (the foundation)

perlc already compiles a slice of `eval` at **build time** — non-spawning,
fully self-contained. In `codegen.cpp` (`eval` Call handler, ~18462):

- `eval "literal"` whose parsed body has **no subs / require / use / do**
  is emitted directly as an **`EvalBlock`** (`codegen.cpp:16400`), native
  code in-process. Outer `my` lexicals are already visible via the eval
  pad (`emitEvalPadFill()`). No `perl_eval_string`, no spawn, no `/tmp`.
- Constant `eval` **with** a sub (`SubDef`/`AnonSub`/`PackageStmt`) or a
  `require`/`use`/`do` falls through to `perl_eval_string` → spawn.
- `do FILE` (all) and `require` of a non-native on-disk module → spawn.

So the "static eval path" is mostly about **expanding what we already do
statically**, not building a new runtime compiler.

## 3. The wall

The generated binary is a plain **C program linked without LLVM, the C
backend, or CodeGen** — those live only in the `perlc` binary. To compile
Perl at runtime you need LLVM→IR, the C backend→C, and a C compiler→code.
Embedding LLVM + the C backend + a C compiler into every binary is
hundreds of MB of bloat. That closes out the "general in-process JIT"
shape.

## 4. Scope options for #1

| # | Option | What it is | Self-contained? | Effort |
|---|--------|------------|-----------------|--------|
| (a) | **Expand the static compile-time net** | Fold more constant eval/`do`/`require` into the existing `EvalBlock` / const-sub machinery so perlc compiles them in. | Yes, for everything the compiler can see. | Medium |
| (b) | **In-process interpreter for the dynamic case** | A small AST/bytecode evaluator in `runtime.c` that *interprets* the parsed fragment against the existing runtime value model. | Yes (no external toolchain). | High (write a Perl subset interpreter) |
| (c) | **Document "needs toolchain"** | Leave the `system()`+`dlopen` spawn, add a clear note. | No. | None |

**Recommendation:** do **(a)** as the default. Keep the dynamic case
resolvable via an opt-in mechanism (see §5) instead of only (c).

## 5. do-eval-options — a compile-time mode flag (deferred)

A compile-time perlc option picking how the binary handles the runtime
dynamic case. Three modes, embedded in the binary like `PERLC_SELF_PATH`:

| `--<eval|do>-mode` | runtime dynamic eval does… | runtime dep |
|---|---|---|
| `clang` *(default)* | spawn `perlc --eval-lib` → clang → `.so` → `dlopen` | perlc + clang + `/bin/sh` + `/tmp` |
| `perl` | spawn a **locally installed `perl`** to run the eval | system perl only |
| `static` | error at compile time ("dynamic eval not supported in this build") | none (pure MVP binary) |

The perl binary is resolved either baked at compile time (`--perl=`) or at
runtime via `PERLC_PERL`/`$PATH` (recommended default, avoids hardcoding a
host path).

### The capability boundary (design-critical)

A local `perl` is a **separate interpreter**, so it can correctly eval
**isolated** strings (a pure function of inputs you pass in) but **cannot**
see perlc-runtime state:

- ❌ outer closure lexicals (unrecoverable across a process boundary)
- ❌ subs registered earlier in perlc's function registry
- ❌ the caller's `$@`, `wantarray`, PV allocator

Real Perl's `eval $str` shares all that; `--eval-mode=perl` cannot by
construction. So it is a **bounded, documented feature**, not a drop-in
`eval` replacement. The compile-time job is a **heuristic classifier**
that flags any eval/`do` touching perlc-runtime state as unsupported in
`perl` mode (compile error), so it never returns a silently-wrong answer.
`static` mode = everything forced compile-time; `clang` mode = current
behavior.

### How the flag maps to #1 / #6

- **#1:** `perl` mode trades the perlc+clang requirement for a **system
  perl** requirement (strictly weaker — perl is on every relevant host);
  `static` mode needs nothing. So the flag *expands* how self-contained
  you can be rather than being a concession.
- **#6:** the `perl` path skips the perlc+clang compile (the cold-start
  cost), paying only a perl-process start (~tens of ms).

## 6. The (a) expansion — first slices (deferred)

Expanding the static net, roughly in value order:

1. **Constant `eval` that defines a sub** — compile the sub into the
   binary (const-sub / registry) instead of spawning.
2. **`do "literal"`** — compile the target file's top-level into the
   binary, registering its subs in-process.
3. **Constant `require "NonNative.pm"`** — compile the on-disk module's
   subs into the binary (overlaps module-availability blocker #3).

### Recommended first step

Survey the current spawn footprint — quantify which `do`/`eval`/`require`
currently force a spawn (and across how many real scripts) so (a) is
scoped from data, not guesses. See `docs/mvp_blockers.md` for the
blocker table and current gate state.

## Status

- 2026-11-16: notes written. (a) is the agreed default direction; the
  `--eval-mode`/`--do-mode` local-perl hatch (§5) and its classifier are
  deferred to a later session.
- 2026-11-16: **First slice of (a) implemented — constant eval that defines
  self-contained subs (`codegen.cpp`, eval Call handler ~18533).**
  - Spawn footprint (measured with runtime markers on the compiled binary,
    not strace — the `/bin/sh` execve is lost to the fork/exec attach race):
    of 125 tests referencing `do`/`eval`, **19 spawn at runtime (~47
    toolchain-needing calls)**.
  - **Reclaimable** (compile-time-constant content): constant-string evals
    that *define a sub*. **Not reclaimable** (genuinely runtime → perl-mode
    hatch, not (a)): `do $libfile`, `eval $src`, `eval $c`, interpolated
    `eval "sub $cls"`, `eval "require $m"`.
  - **`do "./literal.pl"` has NO reclaimable cases in the corpus**: every
    `do` in `tests/` uses a runtime-generated file (`open <<'EOF'` written at
    runtime) or a variable path, so the target content is unknown at compile
    time. Confirmed: zero static `do "./x.pl"` of a pre-existing file.
- 2026-11-16: **(a.1) eval-sub reclaim slice — DONE and verified.**
  - Constant `eval 'sub evaled_add { ... }'` and `eval 'sub dyn_sub { my
    ($x) = @_; ... }'` now compile the sub into the binary via the existing
    `emitSub` machinery instead of spawning `perl_eval_string`.
  - **Self-containment guard** (correctness-critical): the sub is only
    reclaimed if it references *only its own local lexicals and `@_`*.
    Outer-lexical closures (`eval 'sub g { $n++ }'`, `$v`) can't cross the
    compile boundary and still spawn via `perl_eval_string` (which has the
    eval-pad machinery for them). Detected via `collectLocalDecls()` — a
    new static helper near `collectAllScalarNames`.
  - **Implementation** (`src/codegen.cpp` + `src/codegen.h`):
    - New member `pendingEvalBodies_` (cleared at `compile()` start) keeps
      the parsed eval AST alive — `collectSubDefs` pushes raw `&node`
      pointers into `subs_`, which must outlive the `emitSub` loop.
    - When the parsed body is *pure sub-defs* (every top-level stmt is a
      `SubDef`) and *self-contained* and has *no name collision*, the subs
      are registered into `subs_`, their LLVM `Function`s declared here
      (matching the compile-time pre-declare loop ~7037 — the pre-declare
      loop already ran before the main body was emitted), and the eval
      emits nothing (a pure-sub-def eval yields `undef` in void context).
  - **Verification**: `evaled_add`/`dyn_sub` spawn count dropped **19 → 16**
    (the two reclaimed subs no longer call `perl_eval_string`). All **751**
    harness tests pass vs real perl at `-O0`; opt levels 0–5 pass for the
    eval/do family.
  - **Next (a) slices**: `do "literal"` of a *pre-existing* file (no corpus
    coverage today — would need a test with a static `do` of a file present
    at compile time, plus compile-time file read + recursive `cg.compile`);
    constant `require "NonNative.pm"` (overlaps blocker #3).
- 2026-11-16: **#3 native-module override hook (task A) — DONE and verified.**
  - `PERLC_NATIVE_OVERRIDE=0` (falsey token: `0`/`false`/`off`/`no`/`none`/
    `disable`/`disabled`, case-insensitive) opts out of the hard-coded
    native-module short-circuit so a user-supplied `<lib>/<Module>.pm` in
    @INC loads instead. Unset or any other value keeps "native wins"
    (self-contained default). Env is read once, cached.
  - **Implementation** (`src/runtime.c`): new `perl_native_override_enabled()`
    helper before `perl_native_module()`; the `require` short-circuit in
    `perl_runtime_require()` (line 1308) gated with
    `&& !perl_native_override_enabled()`.
  - **Scope decision — `require` only.** The `eval "require …"` path
    (`perl_eval_string`) spawns a *separate* `perlc --eval-lib` subprocess
    that has its **own** @INC (not the parent's `-I`), so it cannot load the
    user's module there and instead hits the host-perl copy (parse error on
    advanced syntax). Gating that path was net-negative, so it was left as the
    clean native stub: `eval "require NativeMod"` always uses the stub, no
    spawn, regardless of the flag. The override is the well-defined `require`
    surface.
  - **Verification** (compile to `-o` then run the binary — in-process runs
    have a stdio-flush quirk): `require "File::Copy"` → default `copied=-1
    detected=-1` (stub); `PERLC_NATIVE_OVERRIDE=0` with `-I<projlib>` →
    `copied=999 detected=42` (user module); a second `-I` dir yields
    `copied=777 detected=7` (proves the chosen `-I` wins, native stub
    bypassed). `eval "require"` stays `copied=-1` (clean stub) in both modes.
    Full gate **751/0** — no regression (default native short-circuit
    unchanged when the env var is unset).
