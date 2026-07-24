#!/usr/bin/perl
# Deep: D94 — short-circuit ops context propagation.
# Left operand always scalar; right inherits surrounding context.
use strict;
# omit use warnings — stderr diagnostics diverge (D56); values match.

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub t { return wantarray() ? (1, 2, 3) : 9 }
sub z { return 0 }
sub one { return 1 }

{
    my @a = (z() || t());
    check('or_rhs_list', join(",", @a) eq "1,2,3");
}
{
    my @a = (one() || t());
    check('or_lhs_taken', join(",", @a) eq "1");
}
{
    # file-scope log: named subs don't close over block-scoped my @arr under perlc
    our @ctx_log;
    @ctx_log = ();
    sub c_lhs { push @ctx_log, wantarray() ? "L" : "S"; return 0 }
    my @a = (c_lhs() || t());
    check('or_lhs_scalar_ctx', join(",", @ctx_log) eq "S");
    check('or_lhs_scalar_then_rhs_list', join(",", @a) eq "1,2,3");
}
{
    my @a = (one() and t());
    check('and_rhs_list', join(",", @a) eq "1,2,3");
}
{
    my @a = (z() and t());
    check('and_lhs_taken', join(",", @a) eq "0");
}
{
    my @a = (undef // t());
    check('defor_rhs_list', join(",", @a) eq "1,2,3");
}
{
    my @a = (one() // t());
    check('defor_lhs_taken', join(",", @a) eq "1");
}
{
    my @a = (z() or t());
    check('low_or_rhs_list', join(",", @a) eq "1,2,3");
}
{
    my $s = (z() || t());
    check('or_scalar_ctx', $s == 9);
}
{
    sub f { return 0 }
    sub g { f() or return "X"; return "Y" }
    check('or_return_d8a', g() eq "X");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d94_or_list_context_done\n";
