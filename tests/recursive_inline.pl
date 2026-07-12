#!/usr/bin/perl
# In-depth test suite for D57: recursive subs matching the AST-level
# inliner's shape used to hang/crash the compiler at compile time.
#
# Root cause: CodeGen::compile()'s inlineable-sub detector (codegen.cpp)
# recognizes any sub whose body is exactly `my (@params) = @_; return
# expr;` and records it in inlineSubs_ so tryEmitInline() can expand it
# directly at call sites (binding args to temp allocas, skipping @_
# construction). tryEmitInline() had no self-reference or cycle guard:
# when the return expression it's inlining contains another call to a
# sub that's *also* in inlineSubs_, it recurses into tryEmitInline()
# again for that nested call. For an ordinary (non-recursive) call chain
# this terminates normally (bounded by the finite number of subs in the
# program). But when the callee is the *same* sub being inlined (direct
# self-recursion) — or, transitively, a sub that eventually calls back
# to it (mutual recursion) — the AST node being inlined never changes
# between recursive tryEmitInline() calls, so the recursion never
# terminates: this is static, compile-time infinite recursion in the
# compiler's own C++ call stack, not a runtime loop, and was observed to
# crash with an LLVM "please submit a bug report" stack dump. Confirmed
# to reproduce at plain file scope with zero block nesting (found while
# testing D45, but an unrelated root cause).
#
# Fixed with a two-phase approach in compile(): inlineable-shaped subs
# are first collected into a `candidates` map (not yet "live" for
# tryEmitInline()), then a call graph is built restricted to edges
# between candidates (collectCalls() finds every NK::Call in a body
# expression; edges to non-candidate subs are dropped, since those just
# become ordinary bounded calls and can't be part of a cycle). Standard
# 3-color DFS cycle detection (dfsFindCycles()) finds every candidate
# that's part of a cycle — including a 1-node self-loop for direct
# recursion — and only candidates NOT in a cycle become "live" entries
# in inlineSubs_. A cyclic sub still compiles and runs correctly; it
# just becomes an ordinary (non-inlined, safe, bounded-stack-per-call)
# function call at its own recursive call site instead.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — direct self-recursion (factorial) ───────
sub factorial { my ($n) = @_; return $n <= 1 ? 1 : $n * factorial($n - 1); }
check('factorial_base_case', factorial(1) == 1);
check('factorial_small', factorial(5) == 120);
check('factorial_larger', factorial(10) == 3628800);

# ── Section 2: direct self-recursion with TWO recursive calls in the ──────
# ── same return expression (fibonacci) — exercises collectCalls() ─────────
# ── finding multiple call sites in one body expression ─────────────────────
sub fib { my ($n) = @_; return $n < 2 ? $n : fib($n - 1) + fib($n - 2); }
check('fibonacci_base_case', fib(1) == 1);
check('fibonacci_small', fib(10) == 55);

# ── Section 3: mutual (2-cycle) recursion between two inlinable subs ──────
sub is_even { my ($n) = @_; return $n == 0 ? 1 : is_odd($n - 1); }
sub is_odd  { my ($n) = @_; return $n == 0 ? 0 : is_even($n - 1); }
check('mutual_recursion_even', is_even(10) == 1);
check('mutual_recursion_odd', is_odd(10) == 0);
check('mutual_recursion_even_of_odd_n', is_even(7) == 0);

# ── Section 4: mutual (3-cycle) recursion among three inlinable subs ──────
sub state_a { my ($n) = @_; return $n <= 0 ? "A" : state_b($n - 1); }
sub state_b { my ($n) = @_; return $n <= 0 ? "B" : state_c($n - 1); }
sub state_c { my ($n) = @_; return $n <= 0 ? "C" : state_a($n - 1); }
check('three_cycle_base', state_a(0) eq "A");
check('three_cycle_one_step', state_a(1) eq "B");
check('three_cycle_two_steps', state_a(2) eq "C");
check('three_cycle_wraps_around', state_a(9) eq "A");

# ── Section 5: regression — a non-recursive, acyclic chain of inlinable ───
# ── subs (A calls B, B doesn't call A) still gets inlined and works ───────
sub square_it { my ($x) = @_; return $x * $x; }
sub sum_squares { my ($a, $b) = @_; return square_it($a) + square_it($b); }
check('acyclic_chain_regression', sum_squares(3, 4) == 25);

# ── Section 6: regression — a plain non-recursive, non-chained inlinable ──
# ── sub still works ─────────────────────────────────────────────────────────
sub double_it { my ($x) = @_; return $x * 2; }
check('plain_inlinable_regression', double_it(21) == 42);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "recursive_inline_tests_done\n";
