#!/usr/bin/perl
# Smoke test for zero-width regex substitution matches (D-fix: `s/$/text/`
# used to segfault perlc — a size_t underflow in perl_regex_subst when a
# zero-width match lands past the search-start position, e.g. an end
# anchor matching at end-of-string).
# Fast, narrow coverage — see regex_subst_zero_width.pl for the in-depth suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original crashing repro: end-anchor substitution, non-global.
{
    my $s = "hello";
    $s =~ s/$/world/;
    check('smoke_end_anchor_no_crash', $s eq "helloworld");
}

# Start-anchor substitution (was already working, regression check).
{
    my $s = "hello";
    $s =~ s/^/X/;
    check('smoke_start_anchor', $s eq "Xhello");
}

# Global end-anchor substitution with /mg (multiple zero-width matches).
{
    my $s = "ab\ncd";
    $s =~ s/$/!/mg;
    check('smoke_multiline_global', $s eq "ab!\ncd!");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "regex_subst_zero_width_smoke_done\n";
