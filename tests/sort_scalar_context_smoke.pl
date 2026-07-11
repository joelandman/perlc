#!/usr/bin/perl
# Smoke test for `sort` in scalar context (D29: perlc returned the element
# count — matching grep/map's scalar-context behavior — but real Perl
# returns undef for sort in scalar context, and doesn't evaluate its list
# argument or comparator block at all).
# Fast, narrow coverage — see sort_scalar_context.pl for the in-depth suite.
# Deliberately no `use warnings` — real Perl's "Useless use of sort in
# scalar context" warning under -w would land on stderr and break the
# byte-for-byte harness comparison; the semantics under test (the return
# value) are unaffected by the warnings pragma.
use strict;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: plain sort assigned to a scalar.
{
    my @a = (3, 1, 2);
    my $x = sort @a;
    check('smoke_sort_scalar_is_undef', !defined($x));
}

# Block comparator in scalar context is also undef.
{
    my @a = (3, 1, 2);
    my $x = sort { $a <=> $b } @a;
    check('smoke_sort_block_scalar_is_undef', !defined($x));
}

# List context still works — regression check.
{
    my @a = (3, 1, 2);
    my @s = sort @a;
    check('smoke_sort_list_regression', join(",", @s) eq "1,2,3");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sort_scalar_context_smoke_done\n";
