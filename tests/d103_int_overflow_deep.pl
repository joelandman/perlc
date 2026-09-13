#!/usr/bin/perl
# Deep test for D103: 64-bit signed-integer arithmetic overflow used to
# silently wrap (native i64 add/sub/mul with no check at all) instead of
# auto-promoting the way real Perl does: exact UV (0..UINT64_MAX) when
# the true result fits, NV (double) otherwise. Root causes fixed:
#
# 1. `src/runtime.c` perl_add/perl_sub/perl_mul (the boxed/slow path)
#    detected overflow by computing the native wrapped result and
#    inferring overflow from its sign — a pattern that relies on
#    signed-integer-overflow-is-UB, which -O2 can (and, verified
#    empirically here, sometimes did) optimize away entirely, silently
#    defeating the very check it implemented. Replaced with
#    __builtin_{add,sub,mul}_overflow, which is well-defined at any
#    optimization level.
# 2. On detected overflow, instead of always falling to `double`
#    (irrecoverably losing exactness for any magnitude beyond 2^53),
#    the exact result is first computed via mini-gmp and, if it fits
#    Perl's own UV range, returned as a new UNBLESSED PerlValue (tag
#    PERL_BIGINT, blessed_class=NULL) — deliberately distinct from a
#    real, user-declared (blessed) Math::BigInt, which keeps its
#    existing unbounded-range behavior untouched via the overload
#    dispatch checked first in each op. Chained arithmetic on an
#    auto-promoted value re-derives exactly and re-checks the UV bound
#    on every subsequent op (falling to double once it would exceed
#    UINT64_MAX), matching real Perl's own per-operation UV-vs-NV
#    decision instead of staying "exact forever".
# 3. `src/codegen.cpp` emitBinOp's raw, unboxed i64 fast path
#    (emitExprI64, used to avoid boxing overhead for hot arithmetic)
#    had literally zero overflow checking. A new
#    emitI64OverflowCheckedBinOp wraps just the OUTERMOST operator of a
#    top-level +/-/* with LLVM's overflow-checked intrinsics, falling
#    back to the (now overflow-aware) boxed runtime op only on the rare
#    overflow branch — the common non-overflowing case is unchanged.
# 4. `src/parser.cpp`'s integer-literal parsing previously always fell
#    straight to a `double` for any literal beyond INT64_MAX (D78),
#    losing exactness for the entire UV window, not just already-round
#    numbers. Literals fitting 0..UINT64_MAX now build an unblessed
#    auto-BigInt directly instead.
# 5. `perl_negate` gained an unblessed-BigInt case so negating a
#    literal like `-2**63` demotes back to a plain (exact) int when
#    the negated value fits signed 64-bit, instead of a lossy double.
#
# NB: a residual, narrower gap was found (not fixed) while verifying
# this: `emitBinOp`'s separate F64 "stay unboxed" fast path can convert
# a bigint-tagged SCALAR VARIABLE straight to double and add natively,
# bypassing perl_add's BigInt-aware logic entirely — this only ever
# shows up as a 1-ULP divergence in the narrow case of a value sitting
# exactly at the UINT64_MAX literal boundary undergoing further
# arithmetic that itself crosses beyond it. Logged as D132; not
# exercised by this test.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# exact-repro from the original D103 write-up
my $big = 9223372036854775807;
check('add_overflow_exact', ($big + 1) eq "9223372036854775808");

# the write-up's second repro: a power-of-two literal beyond INT64_MAX,
# negated — exact digits, not scientific notation
check('negated_pow2_literal', (-9223372036854775808) eq "-9223372036854775808");

# chained overflow: two additions in a row both still fit the UV window
my $x = 9223372036854775807;
$x = $x + 1;
$x = $x + 1;
check('chained_add_exact', $x eq "9223372036854775809");

# multiplication overflow, exact
my $y = 9223372036854775807;
check('mul_overflow_exact', ($y * 2) eq "18446744073709551614");

# subtraction that overflows into the positive-UV direction
check('sub_overflow_exact', ($y - (-5)) eq "9223372036854775812");

# ordinary (non-overflowing) arithmetic is completely unaffected
my $small = 5;
check('no_overflow_unaffected', ($small + 3) == 8);

# negative-direction overflow (no negative UV in real Perl) still
# falls to NV/double, exactly as before this fix
my $negover = -9223372036854775807 - 2;
check('negative_direction_falls_to_float', $negover == -9.22337203685478e+18);

# comparisons against an overflowed value stay exact (not lossy via
# double, which would round both sides to the same value here)
check('exact_eq_comparison', (9223372036854775807 + 1) == 9223372036854775808);
check('exact_lt_comparison', (9223372036854775807 + 1) < 9223372036854775810);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d103_int_overflow_done\n";
