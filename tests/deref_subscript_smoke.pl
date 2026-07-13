#!/usr/bin/perl
# Smoke test for D63: `$$ref[idx]`/`$$ref{key}` (real Perl's shorthand for
# `$ref->[idx]`/`$ref->{key}`) produced the wrong (empty/undef) result —
# not string-interpolation-specific; the bare expression form itself was
# broken. Root cause: the parser's adjacent-subscript handling treated a
# `$$ref` base as an already-fully-dereferenced value and applied a
# *second* dereference on top of it for the `[idx]`/`{key}`, instead of
# recognizing that `$$ref[idx]` has only one dereference total (`$ref`
# itself is the array/hash ref to index).
# Fast, narrow coverage — see deref_subscript.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: $$aref[idx] silently returned undef/empty instead of
# the correct element.
{
    my $aref = [10, 20, 30];
    check('smoke_deref_array_subscript', $$aref[1] == 20);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "deref_subscript_smoke_done\n";
