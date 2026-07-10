#!/usr/bin/perl
# Smoke test for scalar/array/hash self-assignment safety (found while
# fixing D35 — Carp::croak — the `eval {...}; die $@ if $@;` idiom passes
# $@ to die() as the SAME cell it just wrote to, and $x = $x segfaulted
# perlc entirely: perl_assign() freed dst's old payload before reading
# src's, and dst==src meant that free() invalidated src too).
# Fast, narrow coverage — see self_assign.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original crash: plain scalar self-assignment.
{
    my $x = "hello";
    $x = $x;
    check('smoke_scalar_self_assign_no_crash', $x eq "hello");
}

# Array-element self-assignment (a related bug in perl_array_set, found
# while fixing the crash above).
{
    my @a = (1, 2, 3);
    $a[0] = $a[0];
    check('smoke_array_elem_self_assign', $a[0] == 1);
}

# Hash-element self-assignment (perl_hash_set_str/perl_hash_set_sv, same
# bug family).
{
    my %h = (a => 1);
    $h{a} = $h{a};
    check('smoke_hash_elem_self_assign', $h{a} == 1);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "self_assign_smoke_done\n";
