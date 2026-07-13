#!/usr/bin/perl
# Smoke test for D59: `$$` (the PID special variable) never interpolated
# correctly inside a double-quoted string — the interpolation scanner had
# no case for `$` followed by a second `$` at all, so the first `$` fell
# through to plain literal text and the second `$` got rescanned from
# scratch, landing on whatever *other* rule its own following character
# happened to match (e.g. `"file_$$.pl"` garbled into `"file_$0pl"` via
# an accidental match against the unrelated `$.` special variable).
# `$$` as a bare expression (`my $p = $$;`) always worked correctly —
# only string interpolation was broken.
# Fast, narrow coverage — see dollar_dollar_interp.pl for the in-depth
# suite.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: $$ inside a double-quoted string must match the bare
# $$ expression's own value (the real PID), not be left as literal text.
{
    my $bare = $$;
    my $interp = "$$";
    check('smoke_dollar_dollar_interpolates', $interp eq $bare);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "dollar_dollar_interp_smoke_done\n";
