#!/usr/bin/perl
# Smoke test bundling 3 small, low-risk Low/Cosmetic-tier defect fixes:
#   D32 - $. (input line number) wasn't reset to 0 when its filehandle
#         was closed.
#   D33 - Scalar::Util::looks_like_number returned integer 0 for false
#         instead of Perl's empty string "".
#   D43 - wantarray() called at top level (outside any sub) returned 0
#         instead of undef (and read one element past the end of an
#         empty context stack).
# Fast, narrow coverage — see low_risk_fixes.pl for the in-depth suite.
use strict;
use warnings;
use Scalar::Util qw(looks_like_number);

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# D32
{
    my $tmpfile = "/tmp/perlc_low_risk_fixes_smoke_$$.txt";
    open(my $out, '>', $tmpfile) or die "write: $!";
    print $out "line1\nline2\n";
    close($out);

    open(my $fh, '<', $tmpfile) or die "open: $!";
    my $line = <$fh>;
    my $during = $.;
    close($fh);
    my $after = $.;
    unlink($tmpfile);
    check('smoke_dollar_dot_nonzero_during_read', $during == 1);
    check('smoke_dollar_dot_reset_after_close', $after == 0);
}

# D33
{
    my $r = looks_like_number("not a number");
    check('smoke_looks_like_number_false_is_empty_string', $r eq "");
    check('smoke_looks_like_number_true_is_one', looks_like_number("42") == 1);
}

# D43
{
    my $w = wantarray();
    check('smoke_wantarray_toplevel_is_undef', !defined($w));
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "low_risk_fixes_smoke_done\n";
