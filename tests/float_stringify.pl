#!/usr/bin/perl
# In-depth test suite for default float stringification.
#
# Root cause (D31): perl_to_string() and perl_to_string_dup() (runtime.c)
# both formatted PERL_FLOAT values with C's default `%g` (6 significant
# digits) instead of Perl's `%.15g`, causing broad, silent precision loss
# on every default float-to-string conversion (print, string
# interpolation, concatenation, comparison via `eq`, hash keys, etc.):
# `10/3` printed "3.33333" instead of the correct "3.33333333333333".
#
# Fixed by adding a shared perl_format_float() helper (runtime.c) used by
# both conversion functions, using `%.15g` as the base format and adding
# three special cases where %.15g's raw output differs from real Perl's:
#   - negative zero: %.15g gives "-0", Perl always gives plain "0"
#   - +-infinity: %.15g gives "inf"/"-inf" (lowercase), Perl gives
#     "Inf"/"-Inf"
#   - NaN: glibc's %.15g can render a NaN with a set sign bit as "-nan",
#     Perl always gives plain "NaN" regardless of the sign bit
#
# This fix also resolved tests/arith.pl's pre-existing harness failure
# (it does floating-point division and prints the result directly).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — full 15-significant-digit precision ─────
{
    check('div_10_by_3', (10 / 3) eq "3.33333333333333");
    check('div_1_by_3', (1 / 3) eq "0.333333333333333");
    check('sqrt_2', (2 ** 0.5) eq "1.4142135623731");
    check('pi_literal', 3.14159265358979 eq "3.14159265358979");
}

# ── Section 2: regression — short/exact floats print exactly, no ──────────
# ── trailing garbage digits from float imprecision ──────────────────────────
{
    check('short_float_one_tenth', 0.1 eq "0.1");
    check('short_float_sum', (0.1 + 0.2) eq "0.3");
    check('whole_number_float', 100.0 eq "100");
    check('float_with_half', 1000000.5 eq "1000000.5");
}

# ── Section 3: large-magnitude and small-magnitude values (exponential ────
# ── notation) ────────────────────────────────────────────────────────────
{
    check('large_exponential', 1e20 eq "1e+20");
    check('small_exponential', 1e-20 eq "1e-20");
    check('large_precise_int_like_float', 123456789012345.6 eq "123456789012346");
}

# ── Section 4: negative zero ────────────────────────────────────────────────
{
    my $nz1 = 0.0 * -1;
    my $nz2 = -1 * 0.0;
    check('negzero_via_multiply_a', $nz1 eq "0");
    check('negzero_via_multiply_b', $nz2 eq "0");
}

# ── Section 5: infinity ─────────────────────────────────────────────────────
{
    my $inf  = 9**9**9;
    my $ninf = -9**9**9;
    check('positive_infinity', $inf eq "Inf");
    check('negative_infinity', $ninf eq "-Inf");
}

# ── Section 6: NaN ──────────────────────────────────────────────────────────
{
    my $nan = 9**9**9 - 9**9**9;
    check('nan_stringifies', $nan eq "NaN");
}

# ── Section 7: interpolation and concatenation contexts (not just bare ────
# ── print), to confirm the fix applies uniformly ────────────────────────────
{
    my $x = 10 / 3;
    check('interpolated_in_string', "val=$x" eq "val=3.33333333333333");
    check('concatenated', ("val=" . $x) eq "val=3.33333333333333");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "float_stringify_tests_done\n";
