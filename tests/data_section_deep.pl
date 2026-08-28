#!/usr/bin/perl
# Deep: __END__ also feeds DATA in the main file; empty extra reads.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $s = "";
    while (<DATA>) { $s .= $_ }
    check('slurp', $s eq "line1\nline2\n");
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "data_section_deep_done\n";
__END__
line1
line2
