#!/usr/bin/perl
# Smoke test for D58: `our $pkgvar` did not persist/update across
# *repeated* `do FILE` calls on the same file within one process — each
# `do` call compiles the target into an independent shared library
# (D24's `--do-lib` mode) and dlopen()s it separately, so a package
# scalar's storage was a fresh, disconnected instance every time instead
# of the single persistent slot real Perl's package variables imply.
# Fast, narrow coverage — see do_file_persistence.pl for the in-depth
# suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: a bare `our $counter` should accumulate across
# repeated `do` calls on the same file.
# (Uses a fixed temp filename rather than PID-based uniqueness — `$$`
# doesn't interpolate correctly inside a double-quoted string, D59,
# unrelated to this defect; worked around here rather than fixed.)
{
    my $libfile = "/tmp/perlc_do_persist_smoke.pl";
    open(my $fh, '>', $libfile) or die "write: $!";
    print $fh 'our $counter; $counter++; return $counter;' . "\n";
    close($fh);

    my $r1 = do $libfile;
    my $r2 = do $libfile;
    my $r3 = do $libfile;
    unlink($libfile);

    check('smoke_counter_accumulates', $r1 == 1 && $r2 == 2 && $r3 == 3);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "do_file_persistence_smoke_done\n";
