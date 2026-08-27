#!/usr/bin/perl
# Smoke: typeglob stringify and *alias = \&sub.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub orig { $_[0] * 2 }
*alias = \&orig;
check('code_alias', alias(7) == 14);

my $g = *orig;
check('stringify', $g =~ /^\*main::orig$/);

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "glob_smoke_done\n";
