#!/usr/bin/perl
# Smoke: goto LABEL and goto &NAME.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub skipper {
    my $x = 0;
    goto DONE;
    $x = 1;
    DONE:
    return $x;
}
check('goto_label', skipper() == 0);

sub plus1 { $_[0] + 1 }
sub tramp {
    unshift @_, 10;
    goto &plus1;
}
check('goto_sub', tramp() == 11);

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "goto_smoke_done\n";
