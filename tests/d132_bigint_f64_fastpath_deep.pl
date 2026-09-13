#!/usr/bin/perl
# Deep test for D132: the F64 "stay unboxed" fast path in emitBinOp must not
# commit to native fadd/fsub/fmul when a scalar-variable operand is
# BigInt-tagged at runtime (the tag isn't statically known — D103's
# auto-promotion can put an exact UINT64_MAX-boundary value into an ordinary
# variable). The fix branches on perl_is_bigint_pv(operand) and falls back to
# the boxed D103-aware perl_add/perl_sub/perl_mul; the common non-BigInt path
# keeps identical native instructions.
#
# A second, related pre-existing bug surfaced while verifying: mini-gmp's
# mpz_get_d TRUNCATES toward zero instead of rounding to nearest, so any NV
# conversion of a BigInt at/above the 2^53 mantissa boundary (e.g. plain
# stringification "$big", or "$big / 2") sat 1 ULP below real Perl's (NV)
# cast. perl_to_float now converts via the exact decimal string + strtod
# (round-to-nearest), so these cases match byte-for-byte too.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# the original repro: var + literal crossing the boundary
my $chain = 18446744073709551615;
$chain = $chain + 1;
check('chain_add1', "$chain" eq "1.84467440737096e+19");

# multiplication on the already-promoted value
$chain = $chain * 2;
check('chain_mul2', "$chain" eq "3.68934881474191e+19");

# compound-assign forms route through the same guard
my $chain2 = 18446744073709551615;
$chain2 += 1;
check('compound_add', "$chain2" eq "1.84467440737096e+19");
$chain2 *= 2;
check('compound_mul', "$chain2" eq "3.68934881474191e+19");

# chained ops stay exact through multiple crossings
my $c3 = 18446744073709551615;
$c3 = $c3 + 1 + 1;
check('chained_add', "$c3" eq "1.84467440737096e+19");

# BigInt var mixed with a plain float var — must go through the boxed
# (D103-aware) op, and ordinary float arithmetic must not degrade
my $big = 18446744073709551615;
my $fv  = 0.5;
check('bigint_plus_float_r', $big + $fv == 1.8446744073709552e+19);
check('bigint_plus_float_l', $fv + $big == 1.8446744073709552e+19);

# plain stringification of a BigInt var (NV conversion round-to-nearest)
my $str = 18446744073709551615;
check('bigint_stringify', "$str" eq "18446744073709551615");
my $div = 18446744073709551615;
$div = $div / 2;
check('bigint_div2', "$div" eq "9.22337203685478e+18");

# subtraction within the UV window stays exact (no BigInt involved yet)
my $n = 18446744073709551615;
$n = $n - 1;
check('sub_within_uv', $n eq "18446744073709551614");
$n = $n - 1;
check('sub_within_uv2', $n eq "18446744073709551613");

# ordinary non-BigInt numeric code is completely unaffected
my $plain = 1.5;
my $y = $plain + 2.25;
check('plain_float_add', $y == 3.75);
my $i = 100;
my $z = $i + 23;
check('plain_int_add', $z == 123);
my $m = 3;
my $p = $m * 1.5;
check('plain_mixed_mul', $p == 4.5);

# huge-literal operand itself (the D103 __auto_bigint_lit path) is already
# boxed and unaffected by the F64 guard, but verify it still works
check('huge_lit_add', (18446744073709551615 + 1) == 1.8446744073709552e+19);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d132_bigint_f64_fastpath_done\n";