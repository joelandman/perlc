#!/usr/bin/perl
# Deep test for D117: perl_atof_decimal (src/runtime.c) was a hand-rolled
# digit accumulator that accumulated rounding error, worst on large
# exponents (a repeated-multiply loop instead of a single correctly-
# rounded power) but also just generally less precise than a real
# decimal-to-double conversion. Also covers real Perl's Inf/Infinity/NaN
# string-coercion recognition (not implemented at all before this fix,
# discovered while designing it) and the "not hex, not underscored"
# grammar constraints that must be preserved (real Perl's implicit
# string->number coercion does NOT auto-detect 0x.../"1_000"-style
# literals the way C's strtod/atof alone would if handed the whole
# string unfiltered).
use strict;
use warnings;
no warnings 'numeric'; # deliberate non-numeric-string coercions below

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# the exact real-world-adjacent repro: extreme small exponent
check('extreme_small_exponent', ("2.2250738585072014e-300"+0) eq 2.2250738585072014e-300);
check('extreme_large_exponent', ("1.7976931348623157e300"+0) eq 1.7976931348623157e300);

# ordinary decimals, still must be exact
check('simple_decimal', ("3.14"+0) == 3.14);
check('leading_dot', (".5"+0) == 0.5);
check('negative_leading_dot', ("-.5"+0) == -0.5);
check('trailing_garbage', ("3.14abc"+0) == 3.14);
check('leading_whitespace', ("  42.5"+0) == 42.5);

# grammar constraints that must NOT change (real Perl doesn't recognize
# these in implicit string->number coercion, unlike raw strtod/atof)
check('no_hex_autodetect', ("0x10"+0) == 0);
check('no_underscore_grouping', ("1_000"+0) == 1);
check('bare_sign_no_digits', ("-abc"+0) == 0);

# Inf/Infinity/NaN string coercion (real Perl recognizes these; found
# to be completely unhandled while designing this fix)
check('inf_lowercase', ("inf"+0) == 9**9**9);
check('inf_uppercase', ("INF"+0) == 9**9**9);
check('infinity_word', ("Infinity"+0) == 9**9**9);
check('negative_inf', ("-inf"+0) == -(9**9**9));
my $n = "nan" + 0;
check('nan_is_nan', $n != $n);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d117_atof_precision_done\n";
