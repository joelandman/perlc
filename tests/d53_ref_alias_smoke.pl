#!/usr/bin/perl
# Smoke test for D53: taking a reference to a block-scoped `my $x = LITERAL`
# and writing through it didn't reach $x — subsequent reads of $x kept
# showing the original literal value, because $x was placed on the
# unboxed int/float fast path (no real, addressable PerlValue* for the
# reference to point at), so `\$x` boxed a disposable one-off snapshot
# instead of $x's real storage.
# Fast, narrow coverage — see d53_ref_alias.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    my $x = 42;
    my $r = \$x;
    $$r = 99;
    check('smoke_write_through_ref_reaches_var', $x == 99);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d53_ref_alias_smoke_done\n";
